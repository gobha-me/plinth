import { floatIdentityKey, normalizeFloatContext, normalizeFloatDescriptor } from './float-model.js';
import { canonicalContentType, logicalFloat, pinnedFloatRequest, projectFloatContext,
    validateFloatContext } from './float-wire.js';
import { createFloatResolver, FloatOpenError } from './float-resolver.js';

const nativeClock = () => ({ setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
    clearTimeout: id => globalThis.clearTimeout(id) });
const error = code => new FloatOpenError(code);
const genericCode = value => ['invalid-input', 'missing-handler', 'limit-refused', 'request-busy',
    'cancelled', 'load-failed', 'target-unavailable', 'reload-required', 'session-ended'].includes(value?.code)
    ? value.code : 'authority-unavailable';
async function contextHash(canonical) {
    const bytes = new TextEncoder().encode(canonical);
    const hash = await crypto.subtle.digest('SHA-256', bytes);
    return [...new Uint8Array(hash)].map(value => value.toString(16).padStart(2, '0')).join('');
}
// Physical operation continuations contain only transport/immutable data, never
// a manager, frame callback, panel context reference or controller.
function opening(resolver, type, context, manual, hash) {
    return async signal => {
        const selected = manual ? await resolver.selectManual(manual, type, { signal })
            : await resolver.resolveAutomatic(type, { signal });
        try { validateFloatContext(context.context, selected.context_schema); }
        catch { throw error('invalid-input'); }
        const target = await resolver.revalidate(selected, { signal });
        let validated;
        try { validated = validateFloatContext(context.context, target.context_schema); }
        catch { throw error('invalid-input'); }
        const key = await hash(validated.canonicalJSON);
        if (signal.aborted) throw error('cancelled');
        return { target, descriptor: normalizeFloatDescriptor({ application_id: target.application_id,
            panel_id: target.panel_id, capability: target.capability, generation: target.generation,
            context: validated.context, context_key: key }) };
    };
}
const checking = (transport, requests) => signal => transport.admit(requests, { signal });
const skippedAfter = promise => promise.then(() => ({ status: 'skipped' }));
function restoring(resolver, entry, original, policy, hash) {
    return async signal => {
        const target = await resolver.revalidate(original, { signal });
        if (target.application_id !== entry.application_id || target.panel_id !== entry.panel_id ||
            target.capability !== entry.capability || target.persistence_contract?.policy_revision !== policy) {
            throw error('target-unavailable');
        }
        const projection = projectFloatContext(entry.context, target);
        const originalContext = normalizeFloatContext(entry.context);
        if (projection === null || normalizeFloatContext(projection).canonicalJSON !== originalContext.canonicalJSON) {
            throw error('target-unavailable');
        }
        const key = await hash(originalContext.canonicalJSON);
        if (signal.aborted) throw error('cancelled');
        return { target, descriptor: normalizeFloatDescriptor({ application_id: target.application_id,
            panel_id: target.panel_id, capability: target.capability, generation: target.generation,
            context: originalContext.context, context_key: key }) };
    };
}
function managerTarget(target) {
    return { applicationId: target.application_id, generation: target.generation, version: target.version,
        panel: { id: target.panel_id, title: target.title, module_url: target.module_url } };
}

/** Frame-owned actor; all network continuations dispatch through the document
 * request owner. It never places asynchronous work inside manager.resolve(). */
export class InstalledFloatController {
    #retired = false; #manager = null; #owner; #active = null; #queue = [];
    #evidence = new Map(); #targets = new Map(); #watch = null; #watchLatest = false;
    #poll = null; #subscriptions = []; #waitingActions = new Map(); #catalog = null; #restores = new Set();
    constructor({ manager, transport, requestOwner, resolver = createFloatResolver(transport),
        document: doc = globalThis.document, clock = nativeClock(), hash = contextHash,
        onStatus = () => {}, onIntent = () => {}, onAuthorityChange = () => {}, isPersistable = () => false } = {}) {
        if (!transport || !requestOwner) throw new TypeError('missing float production ports');
        Object.assign(this, { transport, requestOwner, resolver, document: doc, clock, hash,
            onStatus, onIntent, onAuthorityChange, isPersistable });
        this.#owner = requestOwner.register();
        this.adapter = Object.freeze({ resolve: descriptor => this.#consume(descriptor),
            actions: token => this.panelActions(token), liveOnly: token => {
                const evidence = this.#targets.get(token);
                return !evidence || !this.isPersistable(this.#manager?.records.get(token), evidence.target);
            } });
        this.#subscriptions.push(() => doc?.removeEventListener?.('visibilitychange', this.visibility));
        this.visibility = () => this.refresh('visibility');
        doc?.addEventListener?.('visibilitychange', this.visibility);
        if (manager) this.attachManager(manager);
    }
    attachManager(manager) {
        if (this.#manager || this.#retired || manager.records.size || (manager.adapter && manager.adapter !== this.adapter)) {
            throw new TypeError('invalid float manager attachment');
        }
        this.#manager = manager; manager.adapter = this.adapter;
        this.#subscriptions.push(manager.subscribe(() => this.#changed()),
            manager.reservations.subscribe(() => this.#changed()));
    }
    isCurrent() { return !this.#retired && !!this.#manager?.isCurrent(); }
    #status(code) { if (!this.#retired) { try { this.onStatus(code); } catch { /* Status is not authority. */ } } }
    #authorityChanged() { if (!this.#retired) this.onAuthorityChange(); }
    #consume(input) {
        if (!this.isCurrent()) return null;
        const identity = floatIdentityKey(input), evidence = this.#evidence.get(identity);
        this.#evidence.delete(identity); // One turn, one use, even on refusal.
        if (!evidence || evidence.identity !== identity) return null;
        return { descriptor: evidence.descriptor, target: managerTarget(evidence.target) };
    }
    #publish(descriptor, target, action) {
        const identity = floatIdentityKey(descriptor);
        this.#evidence.set(identity, { identity, descriptor, target });
        try { return action(); } finally { this.#evidence.delete(identity); }
    }
    #retireOtherGenerations(target) {
        for (const [token, evidence] of this.#targets) {
            if (evidence.target.application_id === target.application_id && evidence.target.panel_id === target.panel_id &&
                (evidence.target.generation !== target.generation || evidence.target.capability !== target.capability)) {
                this.#authorityChanged();
                this.#manager.close(token); this.#targets.delete(token);
            }
        }
    }
    panelActions(token = null) {
        return Object.freeze({ openFloat: (type, context) => this.openFloat(type, context),
            requestFocus: () => { if (token !== null) this.raise(token); } });
    }
    openFloat(contentType, input) { return this.#enqueue(contentType, input, null); }
    openManual(choice, contentType, input) { return this.#enqueue(contentType, input, choice); }
    #enqueue(contentType, input, manual) {
        if (!this.isCurrent()) return Promise.reject(error('cancelled'));
        let type, context, key;
        try {
            type = canonicalContentType(contentType);
            context = normalizeFloatContext(input === undefined ? {} : input);
            // Capture raw caller data synchronously before the first await.
            if (manual) manual = logicalFloat(manual);
            key = JSON.stringify([type, context.canonicalJSON, manual]);
        } catch { return Promise.reject(error('invalid-input')); }
        const duplicate = [this.#active, ...this.#queue].find(item => item?.key === key);
        if (duplicate) return duplicate.promise;
        if (this.#active && this.#queue.length >= 5) return Promise.reject(error('request-busy'));
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        const intent = { key, type, context, manual, promise, resolve, reject, request: null,
            token: null, attempt: null, answered: false, timedOut: false };
        try { this.onIntent(); } catch { reject(error('cancelled')); return promise; }
        if (!this.isCurrent()) { reject(error('cancelled')); return promise; }
        this.#queue.push(intent); this.#pump(); return promise;
    }
    #pump() {
        if (!this.isCurrent() || this.#active || !this.#queue.length) return;
        const intent = this.#queue.shift(); this.#active = intent;
        intent.request = this.requestOwner.submit(this.#owner, 'interactive',
            opening(this.resolver, intent.type, intent.context, intent.manual, this.hash), event => {
                if (!this.isCurrent() || this.#active !== intent) return;
                if (event.type === 'timeout') { intent.timedOut = true; this.#answer(intent, 'authority-unavailable'); return; }
                intent.request = null;
                if (event.code === 'session-ended' || event.code === 'reload-required') {
                    // Timeout already answered the caller, not the document's
                    // terminal identity fence. Exact current intent/owner was
                    // checked above; never admit late success or touch a
                    // successor frame from a retired request.
                    if (intent.answered) this.#status(event.code);
                    else this.#answer(intent, event.code);
                    this.#finish(intent); return;
                }
                if (event.outcome !== 'success' || intent.timedOut) {
                    this.#answer(intent, event.outcome === 'uncertain' ? 'authority-unavailable' : genericCode(event));
                    this.#finish(intent); return;
                }
                const { descriptor, target } = event.value;
                this.#retireOtherGenerations(target);
                const result = this.#publish(descriptor, target, () => this.#manager.admit(descriptor));
                if (!['admitted', 'deduplicated'].includes(result.status)) {
                    this.#answer(intent, result.status === 'limit-refused' ? result.status : 'load-failed');
                    this.#finish(intent); return;
                }
                intent.token = result.token;
                intent.attempt = this.#manager.records.get(result.token)?.incarnation?.token;
                this.#targets.set(result.token, { descriptor, target });
                this.#changed(); this.#schedulePoll();
            });
        if (!intent.request && this.#active === intent) {
            this.#answer(intent, 'request-busy'); this.#finish(intent);
        }
    }
    #answer(intent, code = null, floatId = null) {
        if (intent.answered) return;
        intent.answered = true;
        if (code) { intent.reject(error(code)); this.#status(code); }
        else intent.resolve({ floatId });
        // No caller context retained after result, even if import is still owned.
        intent.context = null; intent.manual = null;
    }
    #finish(intent) {
        if (this.#active !== intent) return;
        this.#active = null; this.#pump();
    }
    #changed() {
        if (!this.isCurrent()) return;
        for (const token of this.#targets.keys()) if (!this.#manager.owns(this.#manager.records.get(token))) this.#targets.delete(token);
        const intent = this.#active;
        if (intent?.token) {
            const record = this.#manager.records.get(intent.token);
            const physical = this.#manager.reservations.inspect(intent.token);
            const current = this.#manager.currentAttempt(record, intent.attempt);
            if (current && record.readiness === 'ready') this.#answer(intent, null,
                this.#manager.readyFloatId(intent.token, intent.attempt));
            else if (!current || record.readiness === 'failed') this.#answer(intent, current ? 'load-failed' : 'cancelled');
            if (!physical || (!physical.pending && (current && record.readiness === 'ready' || physical.cleanupComplete))) this.#finish(intent);
        }
        if (!this.#targets.size && this.#poll !== null) {
            this.clock.clearTimeout(this.#poll); this.#poll = null;
        }
    }
    recordEvidence(token) { return this.#targets.get(token) || null; }
    beginRestore(entry, target, { policyRevision, background = true } = {}) {
        let resolve, reject;
        const settled = new Promise((done, failure) => { resolve = done; reject = failure; });
        const job = { resolve, request: null, token: null, attempt: null, created: false, retired: false,
            physical: Promise.resolve() };
        const retire = () => {
            if (job.retired) return;
            job.retired = true; this.#restores.delete(job);
            if (job.request) this.requestOwner.cancel(job.request);
            const record = this.#manager?.records.get(job.token);
            if (job.created && this.#manager?.owns(record) && record.readiness !== 'ready') this.#manager.close(job.token);
            resolve(skippedAfter(job.physical));
        };
        if (!this.isCurrent() || background !== true) { resolve({ status: 'skipped' }); return { settled, retire }; }
        this.#restores.add(job);
        job.request = this.requestOwner.submit(this.#owner, 'interactive',
            restoring(this.resolver, entry, target, policyRevision, this.hash), event => {
                if (!this.isCurrent() || job.retired) return;
                if (event.type === 'timeout') { this.#status('authority-unavailable'); return; }
                job.request = null;
                if (event.outcome !== 'success') {
                    this.#restores.delete(job);
                    if (event.code === 'target-unavailable') resolve({ status: 'skipped' });
                    else { const code = genericCode(event); this.#status(code); reject(error(code)); }
                    return;
                }
                const { descriptor, target: fresh } = event.value;
                this.#retireOtherGenerations(fresh);
                const result = this.#publish(descriptor, fresh, () => this.#manager.admit(descriptor, {
                    background: true, presentation: entry.presentation, geometry: entry.geometry,
                    maximized: entry.maximized }));
                if (!['admitted', 'deduplicated'].includes(result.status)) {
                    this.#restores.delete(job); resolve({ status: 'skipped' }); return;
                }
                job.token = result.token; job.created = result.status === 'admitted';
                job.attempt = this.#manager.records.get(result.token)?.incarnation?.token;
                this.#targets.set(result.token, { descriptor, target: fresh });
                job.physical = this.#manager.whenPreparationSettled(result.token, job.attempt);
                this.#restores.delete(job); resolve(job.physical); this.#schedulePoll();
            });
        if (job.request) job.physical = this.requestOwner.physicalSettlement(job.request);
        else { this.#restores.delete(job); resolve({ status: 'skipped' }); }
        return { settled, retire };
    }
    listManual(type, cursor = null) {
        if (!this.isCurrent() || this.#catalog) return Promise.reject(error('request-busy'));
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        const query = { type: canonicalContentType(type), cursor };
        const transport = this.transport;
        this.#catalog = { resolve, reject, token: null };
        const call = this.#catalog;
        call.token = this.requestOwner.submit(this.#owner, 'catalog',
            signal => transport.catalog({ content_type: query.type, cursor: query.cursor, limit: 64 }, { signal }), event => {
                if (this.#catalog !== call) return;
                if (event.type === 'timeout') { reject(error('authority-unavailable')); return; }
                this.#catalog = null;
                if (event.outcome === 'success') resolve(event.value);
                else { const code = genericCode(event); this.#status(code); reject(error(code)); }
            });
        if (!call.token) { this.#catalog = null; reject(error('request-busy')); }
        return promise;
    }
    refresh(_reason = 'explicit') {
        if (!this.isCurrent()) return false;
        if (this.#watch) { this.#watchLatest = true; return true; }
        const entries = [...this.#targets.entries()].filter(([token]) => this.#manager.owns(this.#manager.records.get(token)));
        if (!entries.length) { this.#performActions(); return true; }
        // Different contexts of the same logical panel share a single tuple:
        // the protocol forbids duplicate logical app/panel/kind requests.
        const groups = [], byTuple = new Map();
        for (const item of entries) {
            const request = pinnedFloatRequest(item[1].target), identity = JSON.stringify(request);
            if (!byTuple.has(identity)) { const group = { request, entries: [] }; byTuple.set(identity, group); groups.push(group); }
            byTuple.get(identity).entries.push(item);
        }
        const requests = groups.map(group => group.request);
        const watch = { entries, token: null }; this.#watch = watch;
        watch.token = this.requestOwner.submit(this.#owner, 'watch', checking(this.transport, requests), event => {
            if (!this.isCurrent() || this.#watch !== watch) return;
            if (event.type === 'timeout') { this.#authorityChanged(); this.#denyActions(); this.#status('authority-unavailable'); return; }
            this.#watch = null;
            if (event.outcome === 'success') {
                // Transport already validated the ENTIRE complete indexed batch.
                for (const result of event.value) {
                    for (const [token, original] of groups[result.index].entries) {
                        if (this.#targets.get(token) !== original) continue;
                        if (result.status !== 'available') {
                            this.#authorityChanged();
                            this.#manager.close(token); this.#targets.delete(token);
                        } else {
                            if (JSON.stringify(original.target.persistence_contract) !== JSON.stringify(result.target.persistence_contract)) this.#authorityChanged();
                            this.#targets.set(token, { ...original, target: result.target });
                        }
                    }
                }
                this.#performActions();
            } else { this.#authorityChanged(); this.#denyActions(); this.#status(genericCode(event)); }
            const again = this.#watchLatest; this.#watchLatest = false;
            if (again) this.refresh('coalesced');
            else this.#schedulePoll();
        });
        if (!watch.token) { this.#watch = null; this.#denyActions(); this.#status('request-busy'); return false; }
        return true;
    }
    #schedulePoll() {
        if (!this.isCurrent() || !this.#targets.size || this.#poll !== null) return;
        this.#poll = this.clock.setTimeout(() => { this.#poll = null; this.refresh('poll'); },
            this.document?.visibilityState === 'hidden' ? 60000 : 30000);
    }
    #denyActions() { this.#waitingActions.clear(); }
    #performActions() {
        const actions = [...this.#waitingActions]; this.#waitingActions.clear();
        for (const [token, action] of actions) {
            const evidence = this.#targets.get(token), record = this.#manager.records.get(token);
            if (!evidence || !this.#manager.owns(record)) continue;
            if (action.kind === 'retry') this.#publish(record.descriptor, evidence.target, () => this.#manager.retry(token));
            else this.#manager.restore(token, { focusChrome: action.focusChrome });
        }
    }
    retry(token) { return this.#action(token, { kind: 'retry' }); }
    raise(token, { focusChrome = true, acceptIntent = true } = {}) {
        return this.#action(token, { kind: 'raise', focusChrome }, acceptIntent);
    }
    #action(token, action, acceptIntent = true) {
        if (!this.isCurrent() || !this.#targets.has(token)) return false;
        const pending = this.#waitingActions.get(token);
        if (pending?.kind === action.kind && pending.focusChrome === action.focusChrome) return true;
        try { if (acceptIntent) this.onIntent(); } catch { return false; }
        this.#waitingActions.set(token, action); return this.refresh(action.kind);
    }
    retire() {
        if (this.#retired) return;
        this.#retired = true; this.requestOwner.retireOwner(this.#owner);
        if (this.#poll !== null) this.clock.clearTimeout(this.#poll);
        for (const intent of [this.#active, ...this.#queue]) if (intent) this.#answer(intent, 'cancelled');
        this.#active = null; this.#queue = []; this.#evidence.clear(); this.#targets.clear();
        this.#waitingActions.clear(); this.#catalog?.reject(error('cancelled')); this.#catalog = null;
        for (const job of this.#restores) { job.retired = true; job.resolve(skippedAfter(job.physical)); }
        this.#restores.clear();
        for (const unsubscribe of this.#subscriptions) unsubscribe();
        this.#subscriptions = []; this.#manager = null;
        this.onStatus = this.onIntent = this.onAuthorityChange = this.isPersistable = null;
    }
}
