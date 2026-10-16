import assert from 'node:assert/strict';
import test from 'node:test';
import { FloatRequestOwner } from '../../client/shell/client/panels/float-request-owner.js';
import { InstalledFloatController } from '../../client/shell/client/panels/float-controller.js';
import { target, deferred, drain, clock, entry } from './float-production-fixtures.mjs';

test('document physical HTTP lane survives timeout and retired frames until actual settlement', async () => {
    const timer = clock(), io = new FloatRequestOwner({ clock: timer }), first = io.register();
    const wait = deferred(), events = [];
    let signal;
    const token = io.submit(first, 'interactive', current => { signal = current; return wait.promise; }, event => events.push(event));
    assert(token); timer.advance(10000);
    assert.equal(signal.aborted, true); assert.equal(events[0].type, 'timeout');
    const next = io.register(); io.retireOwner(first);
    assert.equal(io.submit(next, 'interactive', async () => ({}), () => {}), null);
    let physicallySettled = false; io.physicalSettlement(token).then(() => { physicallySettled = true; });
    await drain(); assert.equal(physicallySettled, false);
    wait.resolve({ secret: 'fake retired data' }); await drain();
    assert.equal(events.length, 1); assert.equal(physicallySettled, true);
    assert.deepEqual(io.pending().lanes, []);
    assert(io.submit(next, 'interactive', async () => ({}), () => {})); await drain();
    io.retireOwner(next); assert.equal(io.pending().frames, 0);
});

test('unknown async handles never execute then getters or release admitted lanes', () => {
    const timer = clock(), io = new FloatRequestOwner({ clock: timer }), owner = io.register();
    let invoked = 0;
    const unknown = Object.defineProperty({}, 'then', { get() { invoked++; } });
    io.submit(owner, 'catalog', () => unknown, () => {});
    assert.equal(invoked, 0); assert.deepEqual(io.pending().lanes, ['catalog']);
    io.retireOwner(owner); assert.deepEqual(io.pending().lanes, ['catalog']);
});

function managerFixture() {
    const listeners = new Set(), reservationListeners = new Set(), records = new Map(), states = new Map(), calls = [];
    const waits = new Map();
    let current = true, next = 0;
    const notify = () => { for (const fn of [...listeners]) fn(); for (const fn of [...reservationListeners]) fn(); };
    const manager = {
        records, calls, adapter: null,
        reservations: { subscribe(fn) { reservationListeners.add(fn); return () => reservationListeners.delete(fn); },
            inspect(token) { return states.get(token) || null; } },
        isCurrent: () => current,
        subscribe(fn) { listeners.add(fn); return () => listeners.delete(fn); },
        owns: record => !!record && records.get(record.token) === record && !record.retiring,
        currentAttempt(record, attempt) { return this.owns(record) && record.incarnation.token === attempt; },
        readyFloatId(token, attempt) { const record = records.get(token); return this.currentAttempt(record, attempt) && record.readiness === 'ready' ? record.id : null; },
        whenPreparationSettled(token) { return waits.get(token).promise; },
        admit(descriptor, options = {}) {
            calls.push(['admit', descriptor, options]);
            assert(this.adapter.resolve(descriptor));
            const existing = [...records.values()].find(record => JSON.stringify(record.descriptor) === JSON.stringify(descriptor));
            if (existing) return { status: 'deduplicated', token: existing.token, id: existing.id };
            if (records.size === 5) return { status: 'limit-refused' };
            const token = Symbol(), record = { token, descriptor, incarnation: { token: Symbol() }, readiness: 'loading',
                id: 'float-' + ++next, presentation: options.presentation || 'shown', retiring: false, rank: records.size,
                geometry: options.geometry || { x: 0, y: 0, width: 640, height: 480 }, maximized: false };
            records.set(token, record); states.set(token, { pending: true, cleanupComplete: false }); waits.set(token, deferred());
            notify(); return { status: 'admitted', token, id: record.id };
        },
        ready(token) { records.get(token).readiness = 'ready'; notify(); },
        settle(token) { const state = states.get(token); state.pending = false; notify();
            if (records.get(token)?.readiness === 'ready') waits.get(token).resolve({ status: 'ready' }); },
        fail(token) { records.get(token).readiness = 'failed'; states.get(token).cleanupComplete = true; notify(); },
        close(token) { calls.push(['close', token]); const record = records.get(token); if (record) record.retiring = true;
            states.get(token).cleanupComplete = true; records.delete(token); notify();
            if (!states.get(token).pending) waits.get(token).resolve({ status: 'skipped' }); },
        retry(token) { calls.push(['retry', token]); assert(this.adapter.resolve(records.get(token).descriptor)); return true; },
        restore(token, options) { calls.push(['restore', token, options]); records.get(token).presentation = 'shown'; },
        focus(token, options) { calls.push(['focus', token, options]); },
        retire() { current = false; },
    };
    return manager;
}
function fixture(overrides = {}) {
    const timer = clock(), requestOwner = new FloatRequestOwner({ clock: timer }), manager = managerFixture();
    const calls = [], pending = [];
    const transport = {
        async personalDefaults() { calls.push('personal'); return []; },
        async catalog(query) { calls.push(['catalog', query]); return { targets: [target()], next_cursor: null, admin_default_candidate: null }; },
        async admit(requests) { calls.push(['admit', requests]); return requests.map((request, index) => ({ index, status: 'available', target: target() })); },
        ...overrides,
    };
    const statuses = [], controller = new InstalledFloatController({ manager, transport, requestOwner, clock: timer,
        hash: async () => 'a'.repeat(64), document: {}, onStatus: status => statuses.push(status) });
    return { timer, requestOwner, manager, calls, controller, pending, statuses };
}

test('production actor copies context before await and resolves only ready current floatId', async () => {
    const f = fixture(), original = { record_id: 'fake' };
    const opened = f.controller.openFloat('text/plain', original);
    original.record_id = 'edited';
    const duplicate = f.controller.openFloat('text/plain', { record_id: 'fake' });
    assert.equal(opened, duplicate);
    let resolved = false; opened.then(() => { resolved = true; }); await drain();
    const [record] = f.manager.records.values();
    assert.equal(record.descriptor.context.record_id, 'fake'); assert.equal(resolved, false);
    assert.equal(f.manager.adapter.resolve(record.descriptor), null, 'one-shot evidence was consumed synchronously');
    f.manager.ready(record.token); await drain();
    assert.deepEqual(await opened, { floatId: 'float-1' });
    assert.equal(f.requestOwner.pending().lanes.length, 0);
    f.manager.settle(record.token); f.controller.retire();
});

test('same context hash cannot deduplicate different full canonical contexts', async () => {
    const f = fixture();
    const first = f.controller.openFloat('text/plain', { record_id: 'one' }); await drain();
    let record = [...f.manager.records.values()][0]; f.manager.ready(record.token); f.manager.settle(record.token); await first;
    const second = f.controller.openFloat('text/plain', { record_id: 'two' }); await drain();
    record = [...f.manager.records.values()][1]; f.manager.ready(record.token); f.manager.settle(record.token); await second;
    assert.equal(f.manager.records.size, 2); f.controller.retire();
});

test('interactive queue is one active plus five distinct pending and keeps timed-out physical work owned', async () => {
    const wait = deferred(), f = fixture({ personalDefaults: () => wait.promise });
    const requests = Array.from({ length: 6 }, (_, index) => f.controller.openFloat('text/plain', { record_id: String(index) }));
    const outcomes = requests.map(promise => promise.then(() => 'ready', failure => failure.code));
    await assert.rejects(f.controller.openFloat('text/plain', { record_id: 'seventh' }), { code: 'request-busy' });
    f.timer.advance(10000); await drain();
    assert.deepEqual(f.requestOwner.pending().lanes, ['interactive']);
    f.controller.retire();
    assert.deepEqual(await Promise.all(outcomes), ['authority-unavailable', ...Array(5).fill('cancelled')]);
    wait.resolve([]); await drain(); assert.deepEqual(f.requestOwner.pending().lanes, []);
});

test('timed-out exact current actor forwards late terminal failures but never late success or retired owner data', async () => {
    for (const code of ['session-ended', 'reload-required']) {
        for (const retired of [false, true]) {
            const wait = deferred(), f = fixture({ personalDefaults: () => wait.promise });
            const result = assert.rejects(f.controller.openFloat('text/plain', { record_id: 'late' }),
                { code: 'authority-unavailable' });
            f.timer.advance(10000); await result;
            assert.deepEqual(f.requestOwner.pending().lanes, ['interactive']);
            if (retired) f.controller.retire();
            wait.reject(Object.assign(new Error('controlled terminal'), { code }));
            await drain();
            assert.deepEqual(f.statuses, ['authority-unavailable', ...(retired ? [] : [code])]);
            assert.equal(f.manager.records.size, 0);
            assert.deepEqual(f.requestOwner.pending().lanes, []);
            f.controller.retire();
        }
    }
    const wait = deferred(), f = fixture({ personalDefaults: () => wait.promise });
    const result = assert.rejects(f.controller.openFloat('text/plain', { record_id: 'late-success' }),
        { code: 'authority-unavailable' });
    f.timer.advance(10000); await result;
    wait.resolve([]); await drain();
    assert.equal(f.manager.records.size, 0);
    assert.equal(f.statuses.includes('session-ended'), false);
    assert.equal(f.statuses.includes('reload-required'), false);
    f.controller.retire();
});

test('selected schema failure never falls through to another provider or imports', async () => {
    const f = fixture();
    await assert.rejects(f.controller.openFloat('text/plain', {}), { code: 'invalid-input' });
    assert.equal(f.calls.filter(call => call[0] === 'catalog').length, 1);
    assert.equal(f.manager.records.size, 0); f.controller.retire();
});

test('fresh owner batch gates focus/retry and affirmative stale retires without dirty veto', async () => {
    const watch = deferred(); let count = 0;
    const f = fixture({ async admit(requests) {
        if (++count === 1) return [{ index: 0, status: 'available', target: target() }];
        return watch.promise;
    } });
    const opened = f.controller.openFloat('text/plain', { record_id: 'fake' }); await drain();
    const [record] = f.manager.records.values(); f.manager.ready(record.token); f.manager.settle(record.token); await opened;
    assert.equal(f.controller.raise(record.token), true);
    assert.equal(f.manager.calls.some(call => call[0] === 'restore'), false);
    watch.resolve([{ index: 0, status: 'stale' }]); await drain();
    assert.equal(f.manager.records.size, 0);
    assert.equal(f.manager.calls.some(call => call[0] === 'restore'), false); f.controller.retire();
});

test('background restore uses pinned target and retirement never closes an already shared ready owner', async () => {
    const f = fixture();
    const attempt = f.controller.beginRestore(entry('fake'), target(), { policyRevision: '1', background: true });
    await drain(); const [record] = f.manager.records.values();
    assert.equal(f.manager.calls[0][2].background, true);
    assert.equal(f.calls[0][1][0].expected_generation, target().generation);
    f.manager.ready(record.token); f.manager.settle(record.token);
    assert.deepEqual(await attempt.settled, { status: 'ready' });
    attempt.retire(); assert.equal(f.manager.records.size, 1); f.controller.retire();
});

test('owner watch deduplicates shared logical tuples and gates content focus without chrome focus theft', async () => {
    const f = fixture();
    for (const id of ['one', 'two']) {
        const opened = f.controller.openFloat('text/plain', { record_id: id }); await drain();
        const record = [...f.manager.records.values()].at(-1);
        f.manager.ready(record.token); f.manager.settle(record.token); await opened;
    }
    const tokens = [...f.manager.records.keys()];
    f.controller.raise(tokens[0], { focusChrome: false }); await drain();
    const lastAdmission = f.calls.filter(call => call[0] === 'admit').at(-1)[1];
    assert.equal(lastAdmission.length, 1, 'two contexts use one exact pinned logical tuple');
    const restored = f.manager.calls.filter(call => call[0] === 'restore').at(-1);
    assert.equal(restored[1], tokens[0]); assert.equal(restored[2].focusChrome, false);
    f.controller.retire();
});

test('failed owner watch retains records but does not grant queued presentation', async () => {
    let count = 0;
    const f = fixture({ async admit() {
        if (++count > 1) throw new Error('controlled authority outage');
        return [{ index: 0, status: 'available', target: target() }];
    } });
    const opened = f.controller.openFloat('text/plain', { record_id: 'fake' }); await drain();
    const [record] = f.manager.records.values(); f.manager.ready(record.token); f.manager.settle(record.token); await opened;
    f.controller.raise(record.token); await drain();
    assert.equal(f.manager.records.size, 1);
    assert.equal(f.manager.calls.some(call => call[0] === 'restore'), false);
    assert.equal(f.statuses.at(-1), 'authority-unavailable'); f.controller.retire();
});
