import { normalizeFloatContext, normalizeSavedFloatSnapshot, savedFloatIdentityKey,
    saveFloatGeometry, validateFloatSnapshot } from './float-model.js';
import { encodeFloatLayout, projectFloatContext, invalidFloatWire } from './float-wire.js';

// Physical I/O continuations retain only this document dispatch table and an
// opaque token. Retiring a frame removes its callback synchronously.
const dispatch = new Map();
const failed = token => error => {
    const code = error && typeof error === 'object' ? Object.getOwnPropertyDescriptor(error, 'code')?.value : null;
    if (code === 'session-ended' || code === 'reload-required') dispatch.get(token)?.failure(code);
    throw error;
};
function watchedRead(token, transport, signal) {
    return transport.readLayout({ signal }).catch(failed(token));
}
function layoutRead(token, transport, signal, epoch) {
    return watchedRead(token, transport, signal).then(value => {
        const handler = dispatch.get(token);
        if (!handler || !handler.current(epoch)) throw new Error('stale float storage');
        return handler.adopt(value);
    });
}
function layoutCommit(token, transport, snapshot, bindings, signal, epoch, revision) {
    return transport.commitLayout(snapshot, bindings, { signal }).catch(failed(token)).then(result => {
        const handler = dispatch.get(token);
        if (!handler || !handler.current(epoch) || signal.aborted || result.saved !== true) throw new Error('uncertain float save');
        return { saved: true, revision };
    });
}

function restoreIndex(layout) {
    const mapping = new Map(), seen = new Set();
    if (!layout.layout || typeof layout.layout !== 'object' || Array.isArray(layout.layout) ||
        Object.keys(layout.layout).length !== 2 || layout.layout.version !== 1 ||
        !Object.hasOwn(layout.layout, 'floats')) return mapping;
    const rawEntries = layout.layout.floats;
    if (!Array.isArray(rawEntries) || rawEntries.length > 5) return mapping;
    const results = new Map(layout.restore_targets.map(item => [item.index, item]));
    for (let index = 0; index < rawEntries.length; index++) {
        const normalized = normalizeSavedFloatSnapshot({ version: 1, floats: [rawEntries[index]] });
        const result = results.get(index);
        if (!result) invalidFloatWire();
        const entry = normalized.entries[0];
        const identity = entry ? savedFloatIdentityKey(entry) : null;
        if (!entry || seen.has(identity)) {
            if (result.status === 'available') invalidFloatWire();
            continue;
        }
        seen.add(identity);
        if (result.status === 'available') {
            const target = result.target;
            if (target.application_id !== entry.application_id || target.panel_id !== entry.panel_id ||
                target.capability !== entry.capability ||
                target.persistence_contract?.policy_revision !== layout.policy_revision) invalidFloatWire();
            const projected = projectFloatContext(entry.context, target);
            if (projected === null || normalizeFloatContext(projected).canonicalJSON !==
                normalizeFloatContext(entry.context).canonicalJSON) invalidFloatWire();
        }
        mapping.set(identity, { entry, ...result });
    }
    return mapping;
}

/** Real FloatPreferences ports. The local epoch and acknowledgement Symbols
 * never cross the wire. Generic preference APIs are deliberately absent. */
export function createFloatPersistenceAdapter({ transport, controller, manager, ioOwner,
    onStatus = () => {} } = {}) {
    let retired = false, ready = false, epoch = Symbol('float storage epoch'), policy = null;
    let prepared = null, mapping = new Map(), pendingPrepare = null;
    let projectionBindings = null;
    const dispatchToken = Symbol('float storage dispatch');
    const status = code => { if (!retired) { try { onStatus(code); } catch { /* No authority from status. */ } } };
    function adopt(layout) {
        if (retired) throw new Error('retired float storage');
        if (layout.policy_revision === null) {
            // An affirmative lack of eligible policy is NOT permission to
            // replace existing descriptors with an empty projection. Live
            // panels remain usable, but every persistence port is fenced.
            ready = false; policy = null; prepared = null; mapping.clear(); projectionBindings = null;
            epoch = Symbol('float storage epoch'); status('authority-unavailable');
            throw new Error('float persistence policy unavailable');
        }
        const nextMap = restoreIndex(layout);
        if (!ready || policy !== layout.policy_revision) epoch = Symbol('float storage epoch');
        policy = layout.policy_revision; ready = true; mapping = nextMap;
        return layout.present ? layout.layout : undefined;
    }
    dispatch.set(dispatchToken, { adopt, current: value => !retired && ready && epoch === value,
        failure: status });
    const prepareOwner = ioOwner.register(event => {
        const call = pendingPrepare;
        if (!call || call.token !== event.token || retired) return;
        if (event.type === 'timeout') {
            ready = false; prepared = null; status('authority-unavailable');
            call.reject(new Error('float storage unavailable')); return;
        }
        pendingPrepare = null;
        if (event.outcome !== 'success') {
            ready = false; call.reject(new Error('float storage unavailable')); return;
        }
        try {
            const raw = adopt(event.value);
            prepared = { raw, epoch }; call.resolve({ ready: true, revision: epoch });
        } catch { ready = false; call.reject(new Error('float storage unavailable')); }
    });
    function prepare() {
        if (retired || pendingPrepare) return Promise.reject(new Error('float storage unavailable'));
        let resolve, reject;
        const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
        const token = ioOwner.acquire('read', prepareOwner);
        if (!token) { reject(new Error('float storage cleanup pending')); return promise; }
        pendingPrepare = { token, resolve, reject };
        ioOwner.submit(token, signal => watchedRead(dispatchToken, transport, signal));
        return promise;
    }
    function authority() { return { ready: !retired && ready, revision: epoch }; }
    function isPersistable(record, target) {
        if (retired || !ready || !manager.owns(record) || record.readiness !== 'ready' || !record.geometry ||
            target?.persistence_contract?.policy_revision !== policy) return false;
        try { return projectFloatContext(record.descriptor.context, target) !== null; }
        catch { return false; }
    }
    function projection({ policyRevision }) {
        if (retired || !ready || policyRevision !== epoch) throw new Error('stale float storage');
        const entries = [], bindings = [], seen = new Set();
        let liveOnly = 0;
        for (const record of [...manager.records.values()].sort((left, right) => left.rank - right.rank)) {
            if (!manager.owns(record) || record.readiness !== 'ready') { if (!record.retiring) liveOnly++; continue; }
            const evidence = controller.recordEvidence(record.token), target = evidence?.target;
            if (!target || target.persistence_contract?.policy_revision !== policy || !record.geometry) { liveOnly++; continue; }
            const context = projectFloatContext(record.descriptor.context, target);
            if (context === null) { liveOnly++; continue; }
            const entry = { application_id: record.descriptor.application_id, panel_id: record.descriptor.panel_id,
                capability: record.descriptor.capability, context, presentation: record.presentation,
                maximized: record.maximized, geometry: saveFloatGeometry(record.geometry) };
            const identity = savedFloatIdentityKey(entry);
            if (seen.has(identity)) { liveOnly++; continue; }
            seen.add(identity);
            entries.push(entry);
            const contract = target.persistence_contract;
            bindings.push({ index: bindings.length, generation: target.generation,
                artifact_digest: contract.artifact_digest, schema_digest: contract.schema_digest,
                projection_contract: contract.projection_contract, policy_revision: contract.policy_revision });
        }
        const snapshot = validateFloatSnapshot({ version: 1, floats: entries }).value;
        // Keep only one latest bounded projection; each commit verifies exact
        // bytes against this result, not merely equal number of entries.
        projectionBindings = { canonical: JSON.stringify(snapshot), bindings, epoch };
        status(liveOnly ? 'live-only' : 'restore-approved');
        return snapshot;
    }
    const storage = Object.freeze({
        read({ signal }) {
            if (retired || !ready) throw new Error('float storage unavailable');
            if (prepared && prepared.epoch === epoch) {
                const value = prepared.raw; prepared = null; return value;
            }
            // This Promise is owned by FloatPreferenceIoOwner's existing read
            // lane. It does not create a second transport/readiness lane.
            return layoutRead(dispatchToken, transport, signal, epoch);
        },
        commit({ snapshot, revision, policyRevision, signal }) {
            if (retired || !ready || policyRevision !== epoch) throw new Error('stale float storage');
            const validated = validateFloatSnapshot(snapshot).value;
            const savedProjection = projectionBindings;
            if (!savedProjection || savedProjection.epoch !== epoch ||
                savedProjection.canonical !== JSON.stringify(validated)) throw new Error('stale float projection');
            // Validate before transport; no Symbol or caller/user scope is serialized.
            encodeFloatLayout(validated, savedProjection.bindings);
            return layoutCommit(dispatchToken, transport, validated, savedProjection.bindings, signal, epoch, revision);
        },
    });
    const restore = Object.freeze({
        begin(entry, options) {
            if (retired || !ready || options.policyRevision !== epoch) throw new Error('stale float restore');
            const evidence = mapping.get(savedFloatIdentityKey(entry));
            if (!evidence || evidence.status !== 'available') {
                return { settled: Promise.resolve({ status: 'skipped' }), retire() {} };
            }
            // Mapping is by the complete saved identity from its ORIGINAL raw
            // index. Dropping/deduplicating a prior slot cannot shift targets.
            return controller.beginRestore(entry, evidence.target, { ...options, policyRevision: policy });
        },
    });
    return Object.freeze({ prepare, authority, projection, storage, restore, isPersistable,
        invalidate() {
            ready = false; prepared = null; mapping.clear(); projectionBindings = null; epoch = Symbol('float storage epoch');
            if (pendingPrepare) {
                ioOwner.cancel(pendingPrepare.token); pendingPrepare.reject(new Error('stale float storage')); pendingPrepare = null;
            }
        },
        retire() {
            if (retired) return;
            retired = true; ready = false; ioOwner.retireOwner(prepareOwner);
            dispatch.delete(dispatchToken);
            pendingPrepare?.reject(new Error('retired float storage')); pendingPrepare = null;
            prepared = null; mapping.clear(); projectionBindings = null;
        },
    });
}
