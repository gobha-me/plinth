import { normalizeSavedFloatSnapshot, validateFloatSnapshot } from './float-model.js';

const IO_TIMEOUT = 10000;
const SAVE_DELAY = 250;
const nativePromise = Promise;
const nativePromisePrototype = Promise.prototype;
const nativeThen = Promise.prototype.then;
const nativeSpecies = Object.getOwnPropertyDescriptor(Promise, Symbol.species)?.get;
const nativeClock = () => ({
    now: () => globalThis.performance.now(),
    setTimeout: (callback, delay) => globalThis.setTimeout(callback, delay),
    clearTimeout: timer => globalThis.clearTimeout(timer),
});

// Separate factories are intentional: promise/timer continuations capture only
// the document owner and opaque token, not submit's invocation/payload/frame.
const settlement = (owner, token, success) => value => owner.settle(token, success, value);
const deadline = (owner, token) => () => owner.expire(token);
function observe(owner, token, promise) {
    try {
        if (promise !== null && typeof promise === 'object' &&
                Object.getPrototypeOf(promise) === nativePromisePrototype) {
            // Intrinsic then still performs species construction. Refuse an
            // overridden constructor without invoking its accessor.
            if (Object.getOwnPropertyDescriptor(promise, 'constructor') ||
                Object.getOwnPropertyDescriptor(nativePromisePrototype, 'constructor')?.value !== nativePromise ||
                Object.getOwnPropertyDescriptor(nativePromise, Symbol.species)?.get !== nativeSpecies) return;
            nativeThen.call(promise, settlement(owner, token, true), settlement(owner, token, false));
            return;
        }
        // Synchronous plain data is permitted by the reviewed storage port.
        // Unknown async handles provide no physical-settlement proof: retain
        // their opaque lane, never inspect or assimilate a then getter.
        if (promise !== null && (typeof promise === 'object' || typeof promise === 'function')) {
            const prototype = Object.getPrototypeOf(promise);
            if ((prototype !== Object.prototype && prototype !== null &&
                    !(Array.isArray(promise) && prototype === Array.prototype)) ||
                Object.getOwnPropertyDescriptor(promise, 'then')) return;
        }
    } catch { return; } // Unsafe observation cannot release admitted work.
    owner.settle(token, true, promise);
}
// Port results are data, not executable properties or inherited assurances.
function resultField(value, key) {
    try {
        if (value === null || typeof value !== 'object') return undefined;
        const field = Object.getOwnPropertyDescriptor(value, key);
        return field && Object.hasOwn(field, 'value') ? field.value : undefined;
    } catch { return undefined; }
}

/** Document-owned physical lanes, retained across authenticated-frame changes.
 * No payload, response, request promise, frame callback or credential is stored
 * in an outstanding record. Live callbacks are held separately and synchronously
 * removed by retireOwner. A deadline/abort never means physical settlement.
 * Restore preparations have their manager-owned deadline/reservation; at most
 * five opaque preparations and one per frame can be outstanding here.
 */
export class FloatPreferenceIoOwner {
    #requests = new Map();
    #dispatch = new Map();
    constructor({ clock = nativeClock() } = {}) { this.clock = clock; }
    register(callback) {
        const owner = Symbol('float preference frame');
        this.#dispatch.set(owner, callback);
        return owner;
    }
    pending() {
        const records = [...this.#requests.values()];
        return {
            read: records.some(record => record.kind === 'read'),
            write: records.some(record => record.kind === 'write'),
            restore: records.filter(record => record.kind === 'restore').length,
            liveFrames: this.#dispatch.size,
        };
    }
    acquire(kind, owner) {
        if (!this.#dispatch.has(owner) || !['read', 'write', 'restore'].includes(kind)) return null;
        const records = [...this.#requests.values()];
        if (kind === 'restore') {
            if (records.filter(record => record.kind === kind).length >= 5
                || records.some(record => record.kind === kind && record.owner === owner)) return null;
        } else if (records.some(record => record.kind === kind)) return null;
        const token = Symbol('float preference attempt');
        let record;
        try {
            record = { owner, kind, controller: new AbortController(), aborted: false,
                invalid: false, expired: false, submitted: false, timer: null,
                deadline: kind === 'restore' ? Infinity : this.clock.now() + IO_TIMEOUT };
            this.#requests.set(token, record);
            if (kind !== 'restore') record.timer = this.clock.setTimeout(deadline(this, token), IO_TIMEOUT);
        } catch {
            // No port has been called; rollback only this unsubmitted lane.
            this.#requests.delete(token);
            return null;
        }
        return token;
    }
    submit(token, invoke) {
        const record = this.#requests.get(token);
        if (!record || record.submitted) return false;
        record.submitted = true;
        if (record.invalid || !this.#dispatch.has(record.owner)) {
            this.settle(token, false);
            return false;
        }
        try { observe(this, token, invoke(record.controller.signal)); }
        catch { this.settle(token, false); }
        return true;
    }
    #abort(record) {
        if (record.aborted) return;
        record.aborted = true;
        try { record.controller.abort(); } catch { /* Abort failure cannot free physical ownership. */ }
    }
    #notify(record, event) {
        const callback = this.#dispatch.get(record.owner);
        if (callback) callback(event);
    }
    expire(token) {
        const record = this.#requests.get(token);
        if (!record || record.expired) return;
        record.expired = true;
        this.clock.clearTimeout(record.timer);
        record.timer = null;
        this.#abort(record);
        this.#notify(record, { token, kind: record.kind, type: 'timeout' });
    }
    cancel(token) {
        const record = this.#requests.get(token);
        if (!record) return;
        record.invalid = true;
        this.clock.clearTimeout(record.timer);
        record.timer = null;
        this.#abort(record);
    }
    settle(token, success, value) {
        const record = this.#requests.get(token);
        if (!record) return;
        if (this.clock.now() >= record.deadline) this.expire(token);
        this.clock.clearTimeout(record.timer);
        this.#requests.delete(token);
        const outcome = record.expired ? 'uncertain' : record.invalid ? 'cancelled'
            : success ? 'success' : 'failure';
        this.#notify(record, { token, kind: record.kind, type: 'settled', outcome,
            // Never dispatch/cache a retired, invalidated or rejected response.
            value: outcome === 'success' ? value : undefined });
    }
    retireOwner(owner) {
        this.#dispatch.delete(owner);
        for (const [token, record] of this.#requests) {
            if (record.owner === owner) this.cancel(token);
        }
    }
}

/** Optional reviewed shell-internal ports, NOT extension SDK/storage APIs:
 * authority() -> {ready:true, revision: opaque current policy/generation token}.
 * projection({policyRevision}) -> exact version-1 safe-projected snapshot;
 * it must omit live-only targets and report their shell status separately. Shape
 * validation does not approve context meanings; no generic projection exists.
 * storage.read({owner,signal}) -> decoded value (undefined means absent).
 * storage.commit({owner,signal,snapshot,revision,policyRevision}) ->
 * {saved:true,revision}, only after separately reviewed owner/generation/policy
 * commit checks. Local revisions are acknowledgement fences, not cross-tab CAS.
 * Ports must not retain a frame/callback in pending transport continuations;
 * they may own an already submitted serialized request until physical settlement.
 * restore.begin(entry,{passToken,policyRevision,intentRevision,rank,background:true})
 * -> {settled:Promise<{status:'ready'|'skipped'}>, retire:()=>void}. It revalidates
 * approved projection/fresh authority and uses the manager's shared admission,
 * dedup, reservations, minimized presentation/geometry and no-focus-steal path.
 * settled means readiness/failure AND physical preparation settlement. retire
 * fences only that pass's not-yet-ready candidate, never an already-ready owner.
 * A synchronous port throw means no physical work was admitted. These contracts
 * are fixture-only until their production seams receive separate review.
 */
export class FloatPreferences {
    #io; #clock; #owner; #ports; #retired = false; #started = false;
    #intent = Symbol(); #revision = Symbol(); #pass = null; #read = null;
    #restore = null; #write = null; #latest = null; #timer = null; #due = 0;
    #state = { available: false, restore: 'idle', save: 'idle', warning: false };
    constructor({ ioOwner, clock = ioOwner?.clock, authority, projection,
        storage, restore, onStatus = () => {} } = {}) {
        this.#io = ioOwner || new FloatPreferenceIoOwner();
        this.#clock = clock || this.#io.clock;
        this.#ports = { authority, projection, storage, restore, onStatus };
        this.#owner = this.#io.register(event => this.#event(event));
        this.#state.available = typeof authority === 'function' && typeof projection === 'function'
            && typeof storage?.read === 'function' && typeof storage?.commit === 'function'
            && typeof restore?.begin === 'function';
    }
    status() {
        const pending = this.#io.pending();
        return { ...this.#state, retired: this.#retired,
            readPending: pending.read, writePending: pending.write,
            restorePending: this.#restore !== null };
    }
    #publish() {
        if (this.#retired) return;
        try { this.#ports.onStatus(this.status()); }
        catch { this.#state.warning = true; }
    }
    #authority() {
        if (this.#retired || !this.#state.available) return null;
        try {
            const result = this.#ports.authority();
            const revision = resultField(result, 'revision');
            const ready = resultField(result, 'ready');
            return !this.#retired && ready === true && revision !== undefined
                && revision !== null ? revision : null;
        } catch { return null; }
    }
    #validPass() {
        const pass = this.#pass;
        if (!pass || pass.intent !== this.#intent) return false;
        const policy = this.#authority();
        return !this.#retired && this.#pass === pass && pass.intent === this.#intent
            && policy === pass.policy;
    }
    start() {
        if (this.#started || this.#retired) return false;
        this.#started = true;
        return this.#beginRead();
    }
    retryRestore() {
        if (this.#retired || this.#read || this.#restore || this.#pass) return false;
        return this.#beginRead();
    }
    #beginRead() {
        const intent = this.#intent;
        const policy = this.#authority();
        if (this.#retired) return false;
        if (this.#intent !== intent) {
            this.#state.restore = 'interrupted'; this.#publish(); return false;
        }
        if (policy === null) {
            this.#state.restore = this.#state.available ? 'retry-required' : 'unavailable';
            this.#publish(); return false;
        }
        const token = this.#io.acquire('read', this.#owner);
        if (!token) { this.#state.restore = 'cleanup-pending'; this.#publish(); return false; }
        this.#pass = { token: Symbol('restore pass'), intent: this.#intent, policy, queue: [], rank: 0 };
        this.#read = token;
        this.#state.restore = 'reading';
        this.#io.submit(token, signal => this.#ports.storage.read({ owner: this.#owner, signal }));
        this.#publish(); return true;
    }
    #interrupt() {
        this.#pass = null;
        if (this.#read) this.#io.cancel(this.#read);
        if (this.#restore) {
            this.#io.cancel(this.#restore.token);
            const retire = this.#restore.retire;
            this.#restore.retire = null;
            try { retire?.(); } catch { this.#state.warning = true; }
        }
    }
    acceptIntent() {
        if (this.#retired) return;
        this.#intent = Symbol();
        if (this.#pass) { this.#interrupt(); this.#state.restore = 'interrupted'; }
        this.#publish();
    }
    policyChanged() {
        if (this.#retired) return;
        this.#interrupt();
        this.#dropLatest();
        if (this.#write) this.#io.cancel(this.#write.token);
        this.#state.restore = 'retry-required';
        if (this.#write) this.#state.save = 'uncertain';
        this.#publish();
    }
    #next() {
        if (!this.#validPass()) {
            if (this.#retired) return;
            this.#interrupt(); this.#state.restore = 'interrupted'; this.#publish(); return;
        }
        const pass = this.#pass;
        if (!pass.queue.length) { this.#pass = null; this.#state.restore = 'complete'; this.#publish(); return; }
        const token = this.#io.acquire('restore', this.#owner);
        if (!token) { this.#pass = null; this.#state.restore = 'cleanup-pending'; this.#publish(); return; }
        const entry = pass.queue.shift();
        this.#restore = { token, retire: null };
        this.#state.restore = 'restoring';
        this.#io.submit(token, () => {
            const attempt = this.#ports.restore.begin(entry, { passToken: pass.token,
                policyRevision: pass.policy, intentRevision: pass.intent,
                rank: pass.rank++, background: true });
            const retire = resultField(attempt, 'retire');
            const settled = resultField(attempt, 'settled');
            let validPromise = false;
            try { validPromise = settled instanceof Promise; } catch { /* Malformed handle stays owned. */ }
            if (typeof retire !== 'function' || !validPromise) {
                // A malformed admitted handle gives no physical-settlement
                // proof. Quarantine its opaque slot until document destruction,
                // rather than freeing it to launch unbounded replacement work.
                this.#pass = null;
                if (!this.#retired) this.#state.restore = 'cleanup-pending';
                try { if (typeof retire === 'function') retire(); }
                catch { this.#state.warning = true; }
                return new Promise(() => {});
            }
            if (this.#restore?.token === token && !this.#retired) this.#restore.retire = retire;
            else {
                this.#io.cancel(token);
                try { retire(); } catch { this.#state.warning = true; }
            }
            const valid = !this.#retired && this.#validPass();
            if (!this.#retired && !valid) {
                this.#interrupt(); this.#state.restore = 'interrupted';
            }
            return settled;
        });
        this.#publish();
    }
    #dropLatest() {
        if (this.#timer !== null) this.#clock.clearTimeout(this.#timer);
        this.#timer = null; this.#latest = null;
    }
    layoutChanged() {
        if (this.#retired) return false;
        this.#revision = Symbol('layout revision');
        const policy = this.#authority();
        if (this.#retired) return false;
        if (policy === null) {
            this.#dropLatest(); this.#state.save = 'unavailable'; this.#publish(); return false;
        }
        try {
            const snapshot = validateFloatSnapshot(this.#ports.projection({ policyRevision: policy })).value;
            if (this.#authority() !== policy) throw new TypeError('stale float projection');
            if (this.#retired) return false;
            this.#dropLatest();
            this.#latest = { snapshot, revision: this.#revision, policy };
            this.#due = this.#clock.now() + SAVE_DELAY;
            this.#timer = this.#clock.setTimeout(() => { this.#timer = null; this.#flush(); }, SAVE_DELAY);
            this.#state.save = 'pending'; this.#publish(); return true;
        } catch {
            if (this.#retired) return false;
            this.#dropLatest(); this.#state.save = 'not-saved'; this.#publish(); return false;
        }
    }
    retrySave() {
        if (this.#retired || this.#write || this.#io.pending().write) return false;
        return this.layoutChanged();
    }
    #flush() {
        if (this.#retired || this.#write || !this.#latest) return;
        if (this.#clock.now() < this.#due) {
            if (this.#timer === null) this.#timer = this.#clock.setTimeout(
                () => { this.#timer = null; this.#flush(); }, this.#due - this.#clock.now());
            return;
        }
        const latest = this.#latest;
        const policy = this.#authority();
        if (this.#retired || this.#latest !== latest) return;
        if (policy !== latest.policy) {
            this.#dropLatest(); this.#state.save = 'not-saved'; this.#publish(); return;
        }
        const token = this.#io.acquire('write', this.#owner);
        if (!token) {
            this.#dropLatest(); this.#state.save = 'cleanup-pending'; this.#publish(); return;
        }
        this.#latest = null;
        this.#write = { token, revision: latest.revision, policy: latest.policy };
        this.#state.save = 'writing';
        this.#io.submit(token, signal => this.#ports.storage.commit({ owner: this.#owner, signal,
            snapshot: latest.snapshot, revision: latest.revision, policyRevision: latest.policy }));
        this.#publish();
    }
    #event(event) {
        if (this.#retired) return;
        if (event.kind === 'read' && event.token === this.#read) {
            if (event.type === 'timeout') {
                this.#pass = null; this.#state.restore = 'retry-required';
            } else {
                this.#read = null;
                if (event.outcome !== 'success') {
                    this.#pass = null;
                    if (this.#state.restore !== 'interrupted') this.#state.restore = 'retry-required';
                } else if (!this.#validPass()) {
                    if (this.#retired) return;
                    this.#interrupt(); this.#state.restore = 'interrupted';
                } else {
                    try {
                        const saved = normalizeSavedFloatSnapshot(event.value);
                        this.#state.warning = saved.warning;
                        this.#pass.queue = [...saved.entries];
                        this.#next();
                    } catch {
                        this.#pass = null; this.#state.restore = 'retry-required';
                    }
                }
            }
        } else if (event.kind === 'restore' && event.token === this.#restore?.token) {
            this.#restore = null;
            const status = resultField(event.value, 'status');
            const valid = event.outcome === 'success' && ['ready', 'skipped'].includes(status)
                && this.#validPass();
            if (this.#retired) return;
            if (valid) {
                if (status === 'skipped') this.#state.warning = true;
                this.#next();
            } else {
                this.#pass = null;
                if (this.#state.restore !== 'interrupted') this.#state.restore = 'retry-required';
            }
        } else if (event.kind === 'write' && event.token === this.#write?.token) {
            if (event.type === 'timeout') {
                this.#dropLatest(); this.#state.save = 'uncertain';
            } else {
                const write = this.#write;
                this.#write = null;
                const saved = event.outcome === 'success' && resultField(event.value, 'saved') === true
                    && resultField(event.value, 'revision') === write.revision
                    && this.#authority() === write.policy;
                if (this.#retired) return;
                if (!saved) {
                    this.#dropLatest(); this.#state.save = event.outcome === 'uncertain'
                        || event.outcome === 'cancelled' ? 'uncertain' : 'not-saved';
                } else if (this.#latest) this.#flush();
                else this.#state.save = write.revision === this.#revision ? 'saved' : 'not-saved';
            }
        }
        this.#publish();
    }
    retire() {
        if (this.#retired) return;
        this.#retired = true;
        this.#io.retireOwner(this.#owner);
        this.#interrupt(); this.#dropLatest();
        this.#ports = null; this.#read = null; this.#restore = null; this.#write = null;
        this.#state = { available: false, restore: 'retired', save: 'retired', warning: false };
    }
}
