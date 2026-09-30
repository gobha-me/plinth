import { isViewId } from './data-query.js';

const BLOCKING_ERRORS = new Set(['auth_failed', 'auth_timeout', 'already_connected',
    'not_authenticated', 'session_expired', 'session_revoked']);

function adviceValue(value, fallback, maximum) {
    return Number.isSafeInteger(value) && value >= 0 && value <= maximum ? value : fallback;
}

function belongs(view, id) {
    return !view.where || (Object.hasOwn(view.where, 'eq')
        ? id === view.where.eq : view.where.in.includes(id));
}

function rowsById(data, view) {
    if (!Array.isArray(data)) return null;
    const rows = new Map();
    for (const row of data) {
        if (!row || typeof row !== 'object' || Array.isArray(row) ||
            !Object.hasOwn(row, view.key) || !isViewId(row[view.key]) ||
            !belongs(view, row[view.key]) || rows.has(row[view.key])) return null;
        rows.set(row[view.key], row);
    }
    return rows;
}

function operations(event, channel) {
    const payload = event?.payload;
    if (event?.type !== 'event' || event.channel !== channel || !payload ||
        typeof payload !== 'object' || Array.isArray(payload) || payload.channel !== channel ||
        (payload.truncated !== undefined && payload.truncated !== false) ||
        !Array.isArray(payload.ops) || !payload.ops.length) return null;
    const names = new Set();
    const ids = new Set();
    for (const op of payload.ops) {
        if (!op || !['insert', 'update', 'delete'].includes(op.op) || names.has(op.op) ||
            (op.truncated !== undefined && op.truncated !== false) ||
            !Number.isSafeInteger(op.count) || op.count < 0 ||
            (op.count > 0 && !Array.isArray(op.ids)) ||
            (op.ids !== undefined && (!Array.isArray(op.ids) || op.ids.length !== op.count))) return null;
        names.add(op.op);
        for (const id of op.ids || []) {
            if (!isViewId(id) || ids.has(id)) return null;
            ids.add(id);
        }
    }
    return payload.ops;
}

// Construction is inert. Every effect, timer, callback and request is owned by
// this controller; the SDK supplies session admission and render ownership.
export function createDataController({ query, isCurrent, publish, request, subscribe,
    admission, clock, random, timer, AbortController }) {
    query = Object.freeze({ ...query });
    let started = false;
    let retired = false;
    let generation = 0;
    let mutation = 0;
    let data = query.initialData;
    let queryError = query.preparationError || null;
    let liveError = null;
    let adapterError = null;
    const snapshotMode = !!query.snapshot || !!query.preparationError;
    let loading = snapshotMode && !queryError;
    let baseline = false;
    let blocked = false;
    let debounce = 100;
    let jitter = 50;
    let dirty = false;
    let deadline = null;
    let timerId = null;
    let inFlight = null;
    let unsubscribe = null;
    let unobserve = null;

    const active = () => started && !retired && isCurrent() && admission.isAllowed();
    const read = () => Object.freeze({ data, error: liveError || queryError || adapterError, loading });
    const emit = () => { if (active()) publish(read()); };
    function cancelTimer() {
        if (timerId !== null) timer.clear(timerId);
        timerId = null;
    }
    function invalidateRequests() {
        generation++;
        cancelTimer();
        dirty = false;
        deadline = null;
        const previous = inFlight;
        inFlight = null;
        previous?.abort.abort();
    }
    function retire() {
        if (retired) return;
        retired = true;
        invalidateRequests();
        const stop = unsubscribe;
        const stopObservation = unobserve;
        unsubscribe = null;
        unobserve = null;
        try { stop?.(); } finally { stopObservation?.(); }
    }
    function arrange(dueImmediately = false) {
        if (!active() || blocked || !dirty || timerId !== null) return;
        if (deadline <= clock() && (inFlight || dueImmediately)) {
            if (!inFlight) runRequest();
            return;
        }
        timerId = timer.set(() => {
            timerId = null;
            if (active() && !blocked && dirty && !inFlight) runRequest();
        }, Math.max(0, deadline - clock()));
    }
    function invalidate() {
        if (!active() || blocked || !query.snapshot || query.preparationError) return;
        if (!dirty) {
            dirty = true;
            const draw = random();
            const fraction = Number.isFinite(draw) && draw >= 0 && draw < 1 ? draw : 0;
            deadline = clock() + debounce + Math.floor(fraction * (jitter + 1));
        }
        arrange();
    }
    function runRequest() {
        if (!active() || blocked || inFlight || !query.snapshot || query.preparationError) return;
        cancelTimer();
        dirty = false;
        deadline = null;
        const operation = { generation, mutation, abort: new AbortController() };
        inFlight = operation;
        Promise.resolve().then(() => {
            if (!active() || blocked || inFlight !== operation || generation !== operation.generation) return;
            return request(query.snapshot, { signal: operation.abort.signal });
        }).then(value => {
            if (!active() || inFlight !== operation || generation !== operation.generation) return;
            if (operation.mutation === mutation) {
                data = value;
                baseline = true;
                queryError = null;
                adapterError = null;
            } else invalidate();
        }, error => {
            if (!active() || inFlight !== operation || generation !== operation.generation) return;
            if (operation.mutation === mutation) queryError = error;
            else invalidate();
        }).finally(() => {
            if (!active() || inFlight !== operation || generation !== operation.generation) return;
            inFlight = null;
            loading = false;
            emit();
            arrange(true);
        });
    }
    function change(event) {
        if (!active() || blocked) return;
        if (!snapshotMode) {
            data = event;
            loading = false;
            emit();
            return;
        }
        const view = query.view;
        const ops = view && operations(event, query.channel);
        if (!ops) { invalidate(); return; }
        const relevant = ops.filter(op => op.count && op.ids.some(id => belongs(view, id)));
        if (!relevant.length) return;
        const deletes = relevant.filter(op => op.op === 'delete');
        // Absence is authoritative only after a valid baseline. Even an absent
        // cached row (or a cold view) must bar an older in-flight resurrection.
        const deleteBarrier = deletes.length > 0 && inFlight !== null;
        if (deleteBarrier) mutation++;
        const rows = baseline && rowsById(data, view);
        if (!rows || relevant.some(op => op.op === 'update') ||
            new Set(relevant.map(op => op.op)).size !== 1) { invalidate(); return; }
        if (deletes.length) {
            const ids = new Set(deletes.flatMap(op => op.ids.filter(id => belongs(view, id))));
            const next = data.filter(row => !ids.has(row[view.key]));
            if (next.length !== data.length) {
                if (!deleteBarrier) mutation++;
                data = next;
                emit();
            }
            if (deleteBarrier) invalidate();
            return;
        }
        let inserts;
        try { inserts = view.insertRows?.(event); }
        catch (error) { adapterError = error; emit(); invalidate(); return; }
        if (!Array.isArray(inserts)) { invalidate(); return; }
        const expected = new Set(ops.filter(op => op.op === 'insert').flatMap(op => op.ids || []));
        let captured;
        try { captured = JSON.parse(JSON.stringify(inserts)); }
        catch { invalidate(); return; }
        const allRows = rowsById(captured, { ...view, where: null });
        if (!allRows || allRows.size !== expected.size ||
            [...allRows.keys()].some(id => !expected.has(id))) { invalidate(); return; }
        const additions = captured.filter(row => belongs(view, row[view.key]));
        if (additions.some(row => rows.has(row[view.key]))) { invalidate(); return; }
        mutation++;
        data = [...data, ...additions];
        emit();
        if (inFlight) invalidate();
    }
    function ready(advice = {}) {
        if (!active()) return;
        debounce = adviceValue(advice?.debounceMs, 100, 60000);
        jitter = adviceValue(advice?.jitterMs, 50, 5000);
        const recovering = blocked;
        blocked = false;
        liveError = null;
        emit();
        if (recovering) runRequest();
    }
    function error(value) {
        if (!active()) return;
        liveError = value;
        if (BLOCKING_ERRORS.has(value?.code)) {
            blocked = true;
            invalidateRequests();
        }
        loading = false;
        emit();
    }
    function start() {
        if (started || retired) return;
        started = true;
        if (!active()) { retire(); return; }
        const stopObservation = admission.subscribe(() => {
            if (!admission.isAllowed()) retire();
        });
        if (retired) { stopObservation(); return; }
        unobserve = stopObservation;
        if (!active()) { retire(); return; }
        const stop = subscribe(query.channel, change, { onReady: ready, onError: error });
        if (retired) { stop(); return; }
        unsubscribe = stop;
        if (!active()) { retire(); return; }
        emit();
        runRequest();
    }
    return Object.freeze({ start, retire, dispose: retire, read });
}
