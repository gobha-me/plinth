// Pure original-activation ownership; no browser, framework or SDK dependency.
const clone = value => JSON.parse(JSON.stringify(value));
const page = input => ({ limit: input?.limit ?? 20, offset: input?.offset ?? 0,
    includeFailed: input?.includeFailed ?? false });
const failure = error => ({ code: error.code || error.name || 'error',
    message: error.message || 'Package request failed', status: error.status ?? null,
    body: error.body ?? null });
const unverified = () => ({ code: 'session-unverified',
    message: 'Current session could not be verified; retry the read.', status: null, body: null });
const terminal = error => error?.status === 401;

export function createPackagesController({ api, publish = () => {},
    AbortController: Abort = globalThis.AbortController, onSessionEnd = () => {} }) {
    let destroyed = false;
    let owner = null;
    let nextRequest = 0;
    let state = initial('prepared');
    function initial(phase) {
        return { phase, actor: null, items: [], page: page(), selectedId: null, detail: null,
            history: { name: null, items: [], page: page({ includeFailed: true }) },
            pending: { session: false, list: false, detail: false, history: false },
            readErrors: { session: null, list: null, detail: null, history: null },
            validation: null, attempts: [] };
    }
    const owns = captured => !destroyed && owner === captured && captured?.active;
    const emit = () => publish(clone(state));
    const opts = (captured, signal) => ({ isCurrent: () => owns(captured), ...(signal ? { signal } : {}) });
    function retire(phase) {
        const old = owner;
        if (old) old.active = false; // Admission barrier MUST precede abort callbacks.
        owner = null;
        if (old) for (const read of old.reads.values()) read.abort.abort();
        state = initial(phase);
        emit();
    }
    function sessionEnded() { if (!destroyed) retire('session-ended'); }
    function authError(captured, error) {
        if (!owns(captured) || !terminal(error)) return false;
        sessionEnded();
        onSessionEnd(error.code);
        return true;
    }
    async function actorMatches(captured, options) {
        const session = await api.session(options);
        if (!options.isCurrent()) return false;
        if (session.user.id === captured.actor.id &&
            session.session.id === captured.actor.sessionId) return true;
        sessionEnded();
        onSessionEnd('session_rotated');
        return false;
    }
    async function failedReadActorMatches(captured, options) {
        try { return await actorMatches(captured, options); }
        catch (probeError) {
            if (!options.isCurrent() || authError(captured, probeError)) return false;
            return null; // Do not reveal the package error under unverified ownership.
        }
    }
    async function readRequest(captured, key, request, accept) {
        if (!owns(captured)) return null;
        const old = captured.reads.get(key);
        const entry = { abort: new Abort() };
        captured.reads.set(key, entry); // Invalidate old read before synchronous abort.
        old?.abort.abort();
        const current = () => owns(captured) && captured.reads.get(key) === entry;
        state.pending[key] = true;
        state.readErrors[key] = null;
        emit();
        const options = { signal: entry.abort.signal, isCurrent: current };
        let stage = key === 'session' ? 'session' : 'preflight';
        try {
            if (!current()) return null;
            // Auth session reads establish the actor. Every later package GET
            // rechecks that actor before dispatch, including retained panels
            // whose cookies may have rotated without a realtime notification.
            if (key !== 'session' && !await actorMatches(captured, options)) return null;
            if (!current()) return null;
            stage = 'request';
            const value = await request(options);
            if (!current()) return null;
            stage = 'postflight';
            if (key !== 'session' && !await actorMatches(captured, options)) return null;
            if (!current()) return null;
            stage = 'accept';
            accept(value);
            return value;
        } catch (error) {
            if (!current() || authError(captured, error)) return null;
            if (key !== 'session') {
                if (stage === 'request') {
                    const match = await failedReadActorMatches(captured, options);
                    if (!current() || match === false) return null;
                    if (match === null) {
                        state.readErrors[key] = unverified();
                        return null;
                    }
                } else {
                    // A failed admission probe cannot authenticate the error
                    // it produced. Never recurse into another probe here.
                    state.readErrors[key] = unverified();
                    return null;
                }
            }
            state.readErrors[key] = failure(error);
            return null;
        } finally {
            if (current()) {
                captured.reads.delete(key);
                state.pending[key] = false;
                emit();
            }
        }
    }
    async function activate() {
        if (destroyed) return;
        if (owner?.active && (owner.actor || state.pending.session)) return;
        if (owner?.active) retire('inactive'); // Retry failed actor read under a new owner.
        const captured = { active: true, reads: new Map(), tail: Promise.resolve(), actor: null };
        owner = captured;
        state = initial('loading');
        emit();
        const session = await readRequest(captured, 'session', options => api.session(options), value => {
            captured.actor = Object.freeze({ id: value.user.id, username: value.user.username,
                sessionId: value.session.id });
            state.actor = captured.actor;
            state.phase = 'active';
        });
        if (session && owns(captured)) await loadList();
    }
    function loadList(input = state.page) {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        const requested = page(input);
        state.page = requested;
        return readRequest(captured, 'list', options => api.list(requested, options), value => {
            state.items = value.items;
            state.page = { ...requested, limit: value.limit, offset: value.offset };
        });
    }
    function select(id) {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        const target = String(id);
        state.selectedId = target;
        state.detail = null;
        state.history = { name: null, items: [], page: page({ includeFailed: true }) };
        return readRequest(captured, 'detail', options => api.detail(target, options), value => {
            state.detail = value;
        });
    }
    function loadHistory(name, input = {}) {
        const captured = owner;
        if (!owns(captured) || !captured.actor || typeof name !== 'string' || !name) return Promise.resolve(null);
        const requested = { ...page(input), includeFailed: true };
        state.history = { name, items: [], page: requested };
        return readRequest(captured, 'history', options => api.list(requested, options), value => {
            // This is only a filtered page of observed records, not an ancestry/audit API.
            state.history = { name, items: value.items.filter(row => row.name === name),
                page: { ...requested, limit: value.limit, offset: value.offset },
                hasNext: value.items.length === value.limit };
        });
    }
    function updateAttempt(captured, requestId, patch) {
        if (!owns(captured)) return;
        const attempt = state.attempts.find(item => item.requestId === requestId);
        if (attempt) { Object.assign(attempt, patch); emit(); }
    }
    async function reconcile(requestId) {
        const captured = owner;
        const attempt = state.attempts.find(item => item.requestId === requestId);
        if (!owns(captured) || !attempt) return;
        const key = 'reconcile:' + requestId;
        const previous = captured.reads.get(key);
        const entry = { abort: new Abort() };
        captured.reads.set(key, entry);
        previous?.abort.abort();
        const current = () => owns(captured) && captured.reads.get(key) === entry;
        const options = { isCurrent: current, signal: entry.abort.signal };
        updateAttempt(captured, requestId, { reconciliation: 'pending' });
        // At most two GETs per explicit reconciliation. A matching row never proves this mutation committed.
        const observed = { list: null, detail: null };
        let stage = 'preflight';
        try {
            if (!await actorMatches(captured, options)) return;
            if (!current()) return;
            stage = 'list';
            const list = await api.list({ limit: 20, offset: 0, includeFailed: true }, options);
            if (!current()) return;
            stage = 'postflight';
            if (!await actorMatches(captured, options)) return;
            if (!current()) return;
            observed.list = list;
            if (attempt.target?.id) {
                stage = 'preflight';
                if (!await actorMatches(captured, options)) return;
                if (!current()) return;
                try {
                    stage = 'detail';
                    const detail = await api.detail(attempt.target.id, options);
                    stage = 'postflight';
                    if (!current() || !await actorMatches(captured, options)) return;
                    observed.detail = detail;
                }
                catch (error) {
                    if (!current() || authError(captured, error)) return;
                    if (error.status === 404) {
                        stage = 'postflight';
                        if (!await actorMatches(captured, options)) return;
                        observed.detail = { absent: true };
                    }
                    else throw error;
                }
                if (!current()) return;
            }
            updateAttempt(captured, requestId, { reconciliation: 'complete', observed });
        } catch (error) {
            if (!current() || authError(captured, error)) return;
            if (stage === 'list' || stage === 'detail') {
                const match = await failedReadActorMatches(captured, options);
                if (!current() || match === false) return;
                if (match === null) error = unverified();
            } else {
                // An admission probe failed: do not recycle the error as if
                // it came from the old owner's package read.
                error = unverified();
            }
            updateAttempt(captured, requestId, { reconciliation: 'failed', observed,
                reconciliationError: failure(error) });
        } finally {
            if (current()) captured.reads.delete(key);
        }
    }
    function mutation(operation, target, invoke, validation = false) {
        const captured = owner;
        if (!owns(captured) || !captured.actor) return Promise.resolve(null);
        const requestId = ++nextRequest;
        const actor = clone(captured.actor);
        const intent = clone(target);
        state.attempts.push({ requestId, actor, operation, target: intent, status: 'queued',
            response: null, error: null, reconciliation: 'none', observed: null });
        emit();
        const task = async () => {
            if (!owns(captured)) return null;
            updateAttempt(captured, requestId, { status: 'pending' });
            try {
                // The panel API exposes no session generation. Revalidate the
                // current authenticated session before an already-queued
                // mutation can cross a cookie rotation while this view lives.
                const key = 'admission:' + requestId;
                const entry = { abort: new Abort() };
                captured.reads.set(key, entry);
                try {
                    if (!await actorMatches(captured, {
                        signal: entry.abort.signal,
                        isCurrent: () => owns(captured) && captured.reads.get(key) === entry,
                    })) return null;
                } finally {
                    if (captured.reads.get(key) === entry) captured.reads.delete(key);
                }
                if (!owns(captured)) return null;
                // This preflight reduces stale-owner dispatch; cookies can
                // still rotate before the POST, whose server auth is final.
                const response = await invoke(opts(captured)); // Never abort admitted mutations.
                if (!owns(captured)) return null;
                updateAttempt(captured, requestId, { status: 'succeeded', response });
                if (validation) { state.validation = response; emit(); }
                else {
                    if (operation === 'uninstall' && state.selectedId === intent.id) {
                        state.selectedId = null;
                        state.detail = null;
                        state.history = { name: null, items: [], page: page({ includeFailed: true }) };
                        emit();
                    } else if (['enable', 'disable'].includes(operation) &&
                               state.detail?.id === intent.id) {
                        state.detail = { ...state.detail, state: response.state };
                        emit();
                    }
                    await loadList();
                }
                return response;
            } catch (error) {
                if (!owns(captured) || authError(captured, error)) return null;
                updateAttempt(captured, requestId, { status: error.unknown ? 'unknown' : 'failed',
                    error: failure(error) });
                if (error.unknown && !validation) await reconcile(requestId);
                return null;
            }
        };
        const result = captured.tail.then(task, task);
        captured.tail = result.catch(() => {}); // Rejection cannot poison a later current-owner intent.
        return result;
    }
    const upload = (file, dryRun) => {
        // File is immutable in browsers; copy bytes and metadata before a queued write.
        const capturedFile = file && typeof file.slice === 'function'
            ? { name: file.name, size: file.size, blob: file.slice(0, file.size, file.type) }
            : file;
        const target = { file: file ? { name: file.name, size: file.size } : null };
        return mutation(dryRun ? 'validate' : 'install-or-upgrade', target,
            options => api.upload(capturedFile, { dryRun }, options), dryRun);
    };
    return {
        activate, deactivate: () => { if (!destroyed) retire('inactive'); }, sessionEnded,
        destroy() { if (!destroyed) { retire('destroyed'); destroyed = true; } },
        read: () => clone(state), loadList, select, loadHistory, reconcile,
        dryRun: file => upload(file, true), install: file => upload(file, false),
        transition(id, action) {
            const target = String(id), selectedAction = String(action);
            return mutation(selectedAction, { id: target }, options => api.transition(target, selectedAction, options));
        },
        uninstall(id, confirmedName) {
            const row = state.detail;
            if (!row || row.id !== id || confirmedName !== row.name) return Promise.resolve(null);
            const target = String(id);
            return mutation('uninstall', { id: target, name: row.name }, options => api.uninstall(target, options));
        },
    };
}
