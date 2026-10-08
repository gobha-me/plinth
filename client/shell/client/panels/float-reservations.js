import { FLOAT_LIMIT } from './float-model.js';

// These maps never store a frame, descriptor, API, component or DOM node.
// Current dispatch and status observers are separate registries whose owners
// must detach/unsubscribe synchronously before retiring their sensitive data.
const controllers = new WeakMap();
const nativeThen = Promise.prototype.then;

export function createFloatReservations() {
    const records = new Map();
    const observers = new Map();
    let poisoned = false;
    let stickyUnsaved = false;
    const status = () => Object.freeze({
        count: records.size,
        pending: [...records.values()].filter(record => record.pending).length,
        retiring: [...records.values()].filter(record => record.retiring).length,
        poisoned,
        unsaved: stickyUnsaved || [...records.values()].some(record => record.dirty),
    });
    const publish = () => {
        const snapshot = status();
        for (const [owner, callback] of [...observers]) {
            if (!observers.has(owner)) continue;
            // An observer cannot veto physical accounting or other observers.
            try { callback(snapshot); } catch { /* status remains authoritative */ }
        }
    };
    const release = (token, record) => {
        if (record.retiring && !record.pending && record.cleanupComplete && !record.poisoned) {
            records.delete(token);
        }
    };
    const budget = Object.freeze({
        status,
        inspect(token) {
            const record = records.get(token);
            return record ? Object.freeze({ pending: record.pending,
                cleanupComplete: record.cleanupComplete, retiring: record.retiring,
                poisoned: poisoned || record.poisoned }) : null;
        },
        subscribe(callback) {
            if (typeof callback !== 'function') throw new TypeError('invalid float observer');
            const owner = Symbol();
            observers.set(owner, callback);
            return () => { observers.delete(owner); };
        },
        reserve() {
            if (poisoned || records.size >= FLOAT_LIMIT) return null;
            const token = Symbol();
            records.set(token, { attempt: null, pending: false, tracked: false,
                cleanupComplete: true, retiring: false, poisoned: false, dirty: false });
            publish();
            return token;
        },
        beginAttempt(token) {
            const record = records.get(token);
            if (!record || poisoned || record.retiring || record.pending || !record.cleanupComplete) return null;
            const attempt = Symbol();
            record.attempt = attempt;
            record.pending = true;
            record.tracked = false;
            record.cleanupComplete = false;
            publish();
            return attempt;
        },
        settleAttempt(token, attempt) {
            const record = records.get(token);
            if (!record || record.attempt !== attempt || !record.pending) return false;
            record.pending = false;
            release(token, record);
            publish();
            return true;
        },
        completeCleanup(token, attempt) {
            const record = records.get(token);
            const matches = record && (record.attempt === attempt || (record.attempt === null && attempt === undefined));
            if (!matches || record.cleanupComplete || record.poisoned) return false;
            record.cleanupComplete = true;
            release(token, record);
            publish();
            return true;
        },
        holdCleanup(token) {
            const record = records.get(token);
            if (!record || record.poisoned) return false;
            record.cleanupComplete = false;
            publish();
            return true;
        },
        poison({ dirty = true } = {}) {
            if (typeof dirty !== 'boolean') throw new TypeError('invalid float dirty state');
            poisoned = true;
            stickyUnsaved ||= dirty;
            publish();
        },
        retire(token, { dirty = false } = {}) {
            if (typeof dirty !== 'boolean') throw new TypeError('invalid float dirty state');
            const record = records.get(token);
            if (!record) return false;
            record.retiring = true;
            record.dirty ||= dirty;
            release(token, record);
            publish();
            return true;
        },
        failCleanup(token, { dirty = false } = {}) {
            if (typeof dirty !== 'boolean') throw new TypeError('invalid float dirty state');
            const record = records.get(token);
            if (!record) return false;
            poisoned = true;
            record.poisoned = true;
            record.retiring = true;
            record.cleanupComplete = false;
            record.dirty ||= dirty;
            publish();
            return true;
        },
    });
    controllers.set(budget, records);
    return budget;
}

export function createFloatDispatch() {
    const handlers = new Map();
    return Object.freeze({
        bind(token, attempt, handler) {
            if (typeof token !== 'symbol' || typeof attempt !== 'symbol' || typeof handler !== 'function') {
                throw new TypeError('invalid float dispatch');
            }
            if (handlers.has(token) || handlers.size >= FLOAT_LIMIT) return false;
            handlers.set(token, { attempt, handler });
            return true;
        },
        detach(token) { return handlers.delete(token); },
        clear() { handlers.clear(); },
        deliver(token, attempt, outcome) {
            const delivery = handlers.get(token);
            if (!delivery || delivery.attempt !== attempt) return false;
            handlers.delete(token);
            delivery.handler(outcome);
            return true;
        },
    });
}

function completeImport(budget, dispatch, token, attempt, outcome) {
    if (!budget.settleAttempt(token, attempt)) return;
    try { dispatch.deliver(token, attempt, outcome); }
    catch {
        // The current handler is shell-owned. An unexpected throw leaves its
        // construction/cleanup uncertain; do not invent reclaimed capacity.
        dispatch.detach(token);
        budget.failCleanup(token, { dirty: true });
    }
}

// This factory's closure scope contains only document objects and opaque
// tokens. In particular it does not close over an importer, URL or old frame.
function settlementHandlers(budget, dispatch, token, attempt) {
    return [
        module => { completeImport(budget, dispatch, token, attempt, { ok: true, module }); },
        error => { completeImport(budget, dispatch, token, attempt, { ok: false, error }); },
    ];
}

export function trackFloatImport(budget, dispatch, token, attempt, promise) {
    const record = controllers.get(budget)?.get(token);
    if (!record || record.attempt !== attempt || !record.pending || record.tracked) {
        throw new TypeError('invalid float preparation ownership');
    }
    record.tracked = true;
    const [fulfilled, rejected] = settlementHandlers(budget, dispatch, token, attempt);
    try {
        // Do not inspect/assimilate arbitrary thenables. Native module import
        // and the controlled test importer supply actual Promise instances.
        return nativeThen.call(promise, fulfilled, rejected);
    } catch {
        dispatch.detach(token);
        budget.failCleanup(token, { dirty: true });
        throw new TypeError('float preparation tracking failed');
    }
}

export function retireFloatRecord(budget, dispatch, token, options) {
    dispatch.detach(token);
    return budget.retire(token, options);
}
