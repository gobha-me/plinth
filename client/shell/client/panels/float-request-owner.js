// Document lifetime, not authenticated frame lifetime. A timeout/abort fences
// dispatch, but does not free a physical lane before its operation settles.
const nativeThen = Promise.prototype.then;
const nativePromise = Promise;
const nativeSpecies = Object.getOwnPropertyDescriptor(Promise, Symbol.species)?.get;
const settled = (owner, token, success) => value => owner.settle(token, success, value);
const expired = (owner, token) => () => owner.expire(token);
const nativeClock = () => ({ now: () => performance.now(),
    setTimeout: (callback, delay) => globalThis.setTimeout(callback, delay),
    clearTimeout: timer => globalThis.clearTimeout(timer) });

export class FloatRequestOwner {
    #owners = new Map(); #requests = new Map(); #dispatch = new Map();
    constructor({ clock = nativeClock() } = {}) { this.clock = clock; }
    register() { const owner = Symbol('float requests frame'); this.#owners.set(owner, true); return owner; }
    pending() {
        return { frames: this.#owners.size,
            lanes: [...this.#requests.values()].map(record => record.lane) };
    }
    submit(owner, lane, operation, callback) {
        if (!this.#owners.has(owner) || !['interactive', 'catalog', 'watch'].includes(lane) ||
            this.#requests.size >= 3 || [...this.#requests.values()].some(record => record.lane === lane)) return null;
        const token = Symbol('float request');
        let complete;
        const physical = new Promise(resolve => { complete = resolve; });
        const record = { owner, lane, controller: new AbortController(), timer: null, physical, complete,
            expired: false, invalid: false, deadline: this.clock.now() + 10000 };
        this.#requests.set(token, record);
        this.#dispatch.set(token, callback);
        try {
            record.timer = this.clock.setTimeout(expired(this, token), 10000);
            const promise = operation(record.controller.signal);
            // Do not assimilate arbitrary thenables or execute their accessors.
            if (!promise || Object.getPrototypeOf(promise) !== Promise.prototype ||
                Object.getOwnPropertyDescriptor(promise, 'constructor') ||
                Object.getOwnPropertyDescriptor(Promise.prototype, 'constructor')?.value !== nativePromise ||
                Object.getOwnPropertyDescriptor(Promise, Symbol.species)?.get !== nativeSpecies) {
                this.expire(token); // Unknown physical work stays quarantined.
                return token;
            }
            nativeThen.call(promise, settled(this, token, true), settled(this, token, false));
        } catch { this.settle(token, false); }
        return token;
    }
    #abort(record) { try { record.controller.abort(); } catch { /* Retain physical ownership. */ } }
    #deliver(token, event) {
        const callback = this.#dispatch.get(token);
        if (callback) callback(event);
    }
    expire(token) {
        const record = this.#requests.get(token);
        if (!record || record.expired || record.invalid) return;
        record.expired = true;
        this.clock.clearTimeout(record.timer); record.timer = null;
        this.#abort(record);
        this.#deliver(token, { type: 'timeout', token });
    }
    cancel(token) {
        const record = this.#requests.get(token);
        this.#dispatch.delete(token);
        if (!record) return;
        record.invalid = true;
        this.clock.clearTimeout(record.timer); record.timer = null;
        this.#abort(record);
    }
    physicalSettlement(token) { return this.#requests.get(token)?.physical || Promise.resolve(); }
    settle(token, success, value) {
        const record = this.#requests.get(token);
        if (!record) return;
        if (this.clock.now() >= record.deadline) this.expire(token);
        this.clock.clearTimeout(record.timer);
        this.#requests.delete(token);
        record.complete();
        const callback = this.#dispatch.get(token);
        this.#dispatch.delete(token);
        const failureCode = !success && value && typeof value === 'object'
            ? Object.getOwnPropertyDescriptor(value, 'code')?.value : undefined;
        if (callback && this.#owners.has(record.owner) && !record.invalid) {
            callback({ type: 'settled', token, outcome: record.expired ? 'uncertain' : success ? 'success' : 'failure',
                ...(success && !record.expired ? { value } : {}),
                ...(typeof failureCode === 'string' ? { code: failureCode } : {}) });
        }
    }
    retireOwner(owner) {
        this.#owners.delete(owner);
        for (const [token, record] of this.#requests) if (record.owner === owner) this.cancel(token);
    }
}
