import assert from 'node:assert/strict';
import test from 'node:test';
import { createFloatPersistenceAdapter } from '../../client/shell/client/panels/float-persistence-adapter.js';
import { FloatPreferenceIoOwner, FloatPreferences } from '../../client/shell/client/panels/float-preferences.js';
import { target, entry, snapshot, deferred, drain, clock, plain } from './float-production-fixtures.mjs';

function fixture(options = {}) {
    const { results, policy = '1', read, commit } = options;
    const layout = Object.hasOwn(options, 'layout') ? options.layout : snapshot(entry());
    const timer = clock(), ioOwner = new FloatPreferenceIoOwner({ clock: timer });
    const restores = [], reads = [], commits = [], statuses = [], records = new Map(), evidence = new Map();
    const transport = {
        readLayout(options) {
            reads.push(options);
            return read ? read(options) : Promise.resolve({ present: layout !== undefined, layout,
                policy_revision: policy, restore_targets: results || (layout?.floats || []).map((_, index) =>
                    ({ index, status: 'available', target: target() })) });
        },
        commitLayout(value, bindings, options) {
            commits.push({ value, bindings, options });
            return commit ? commit(value, bindings, options) : Promise.resolve({ saved: true });
        },
    };
    const controller = {
        recordEvidence: token => evidence.get(token),
        beginRestore(value, selected, options) { restores.push({ value, selected, options });
            return { settled: Promise.resolve({ status: 'ready' }), retire() {} }; },
    };
    const manager = { records, owns: record => !!record && !record.retiring };
    const adapter = createFloatPersistenceAdapter({ transport, controller, manager, ioOwner,
        onStatus: status => statuses.push(status) });
    const preferences = new FloatPreferences({ ioOwner, clock: timer, ...adapter });
    return { timer, ioOwner, transport, records, evidence, adapter, preferences, restores, reads, commits, statuses,
        addRecord(id = 'fake-one', selected = target(), overrides = {}) {
            const token = Symbol(), record = { token, descriptor: { application_id: 'notes', panel_id: 'preview',
                generation: selected.generation, capability: 'notes:1:preview', context: { record_id: id } },
                readiness: 'ready', retiring: false, rank: records.size, presentation: 'shown', maximized: false,
                geometry: entry().geometry, ...overrides };
            records.set(token, record); evidence.set(token, { target: selected }); return token;
        },
        retire() { preferences.retire(); adapter.retire(); },
    };
}

test('guarded prepare occupies existing physical read lane and automatic read is one-shot', async () => {
    const wait = deferred(), f = fixture({ read: () => wait.promise });
    assert.equal(f.adapter.authority().ready, false);
    const preparation = f.adapter.prepare();
    assert.equal(f.ioOwner.pending().read, true);
    assert.equal(f.preferences.start(), false, 'no fabricated local readiness');
    wait.resolve({ present: true, layout: snapshot(entry()), policy_revision: '1',
        restore_targets: [{ index: 0, status: 'available', target: target() }] });
    await preparation;
    assert.equal(f.ioOwner.pending().read, false); assert.equal(f.adapter.authority().ready, true);
    assert.equal(f.preferences.retryRestore(), true); await drain();
    assert.equal(f.reads.length, 1, 'prepared layout consumed without duplicate GET');
    assert.equal(f.restores.length, 1); assert.equal(f.restores[0].options.policyRevision, '1');
    assert.equal(f.preferences.status().restore, 'complete'); f.retire();
});

test('original raw indexes survive malformed drops and dedup without shifting restore target', async () => {
    const first = entry('first'), last = entry('last');
    const f = fixture({ layout: snapshot(null, first, { ...first, presentation: 'minimized' }, last),
        results: [{ index: 0, status: 'unavailable' }, { index: 1, status: 'unavailable' },
            { index: 2, status: 'unavailable' }, { index: 3, status: 'available', target: target() }] });
    await f.adapter.prepare(); f.preferences.start(); await drain();
    assert.equal(f.restores.length, 1); assert.equal(f.restores[0].value.context.record_id, 'last');
    assert.equal(f.preferences.status().warning, true); f.retire();
    const forged = fixture({ layout: snapshot(null, first), results: [
        { index: 0, status: 'available', target: target() }, { index: 1, status: 'unavailable' }] });
    await assert.rejects(forged.adapter.prepare()); assert.equal(forged.adapter.authority().ready, false); forged.retire();
});

test('present stored null is warning not absence; absent needs no restore lookup', async () => {
    for (const [layout, warning] of [[null, true], [undefined, false], [{ version: 2, floats: [entry()] }, true]]) {
        const f = fixture({ layout, results: [] });
        await f.adapter.prepare(); f.preferences.start(); await drain();
        assert.equal(f.preferences.status().warning, warning); assert.equal(f.restores.length, 0);
        assert.equal(f.commits.length, 0); f.retire();
    }
});

test('projection binds every required reopen key and never copies unsafe component state', async () => {
    const f = fixture({ layout: undefined }); await f.adapter.prepare();
    const approved = target();
    approved.context_schema.fields.ephemeral = { required: false, max_bytes: 32, meaning: 'not approved' };
    f.addRecord('fake-one', approved, { descriptor: { application_id: 'notes', panel_id: 'preview',
        capability: 'notes:1:preview', context: { record_id: 'fake-one', ephemeral: 'never persisted' } }, dirty: true });
    const live = target(); live.context_schema.persist_keys = []; live.persistence_contract = null;
    f.addRecord('fake-two', live);
    const snapshotValue = f.adapter.projection({ policyRevision: f.adapter.authority().revision });
    assert.equal(snapshotValue.floats.length, 1);
    assert.deepEqual(plain(snapshotValue.floats[0].context), { record_id: 'fake-one' });
    assert.equal(Object.hasOwn(snapshotValue.floats[0], 'generation'), false);
    assert.equal(Object.hasOwn(snapshotValue.floats[0], 'dirty'), false);
    assert.equal(f.statuses.at(-1), 'live-only'); f.retire();
});

test('real commit serializes complete bindings not local revision, with stale ACK fences and guarded empty reset', async () => {
    const wait = deferred(), f = fixture({ layout: undefined, commit: () => wait.promise });
    await f.adapter.prepare(); f.addRecord();
    assert.equal(f.preferences.layoutChanged(), true); f.timer.advance(250); await drain();
    const write = f.commits[0]; assert.equal(write.bindings.length, 1);
    assert.deepEqual(Object.keys(write.bindings[0]), ['index', 'generation', 'artifact_digest', 'schema_digest', 'projection_contract', 'policy_revision']);
    assert.equal(JSON.stringify(write.bindings).includes('Symbol'), false);
    f.adapter.invalidate(); f.preferences.policyChanged();
    wait.resolve({ saved: true }); await drain();
    assert.notEqual(f.preferences.status().save, 'saved'); f.retire();
    const empty = fixture({ layout: undefined }); await empty.adapter.prepare();
    const revision = Symbol('ACK only'), epoch = empty.adapter.authority().revision;
    const value = empty.adapter.projection({ policyRevision: epoch });
    const result = await empty.adapter.storage.commit({ snapshot: value, revision, policyRevision: epoch,
        signal: new AbortController().signal });
    assert.equal(result.revision, revision);
    assert.deepEqual(plain(empty.commits[0].value), snapshot()); assert.deepEqual(empty.commits[0].bindings, []); empty.retire();
});

test('timeout and retirement abort but never free a blocked prepare read physically', async () => {
    const wait = deferred(), f = fixture({ read: () => wait.promise });
    const preparation = f.adapter.prepare();
    const rejected = assert.rejects(preparation);
    f.timer.advance(10000); await rejected; f.retire();
    assert.equal(f.reads[0].signal.aborted, true); assert.equal(f.ioOwner.pending().read, true);
    wait.resolve({ present: false, layout: undefined, policy_revision: null, restore_targets: [] }); await drain();
    assert.equal(f.ioOwner.pending().read, false); assert.equal(f.adapter.authority().ready, false);
});

test('null policy keeps existing descriptors intact and live-only semantic changes cannot synthesize an empty reset', async () => {
    const stored = snapshot(entry('keep-existing'));
    const f = fixture({ layout: stored, policy: null, results: [{ index: 0, status: 'unavailable' }] });
    await assert.rejects(f.adapter.prepare());
    assert.equal(f.adapter.authority().ready, false);
    const live = target(); live.persistence_contract = null;
    const token = f.addRecord('new-live-only', live);
    assert.equal(f.adapter.isPersistable(f.records.get(token), live), false);
    assert.equal(f.preferences.start(), false);
    assert.equal(f.preferences.layoutChanged(), false);
    assert.equal(f.preferences.retrySave(), false);
    f.timer.advance(1000); await drain();
    assert.equal(f.preferences.status().save, 'unavailable');
    assert.equal(f.commits.length, 0);
    assert.equal(f.restores.length, 0);
    assert.deepEqual(plain(stored), snapshot(entry('keep-existing')));
    assert.equal(f.statuses.at(-1), 'authority-unavailable');
    const epoch = f.adapter.authority().revision;
    assert.throws(() => f.adapter.projection({ policyRevision: epoch }));
    assert.throws(() => f.adapter.storage.commit({ snapshot: snapshot(), revision: Symbol(),
        policyRevision: epoch, signal: new AbortController().signal }));
    f.retire();
});

test('new null policy invalidates an earlier projection and stale save without altering stored layout', async () => {
    let reads = 0;
    const f = fixture({ read: () => Promise.resolve(++reads === 1
        ? { present: true, layout: snapshot(entry()), policy_revision: '1',
            restore_targets: [{ index: 0, status: 'available', target: target() }] }
        : { present: true, layout: snapshot(entry()), policy_revision: null,
            restore_targets: [{ index: 0, status: 'unavailable' }] }) });
    await f.adapter.prepare(); f.addRecord();
    const epoch = f.adapter.authority().revision;
    const projected = f.adapter.projection({ policyRevision: epoch });
    await assert.rejects(f.adapter.prepare());
    assert.equal(f.adapter.authority().ready, false);
    assert.notEqual(f.adapter.authority().revision, epoch);
    assert.throws(() => f.adapter.storage.commit({ snapshot: projected, revision: Symbol(),
        policyRevision: epoch, signal: new AbortController().signal }));
    assert.equal(f.commits.length, 0); f.retire();
});
