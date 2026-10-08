import assert from 'node:assert/strict';
import test from 'node:test';
import { FloatPreferenceIoOwner, FloatPreferences } from
    '../../client/shell/client/panels/float-preferences.js';

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}
const drain = async () => { for (let index = 0; index < 12; index++) await Promise.resolve(); };
function clock() {
    let now = 0, next = 0;
    const timers = new Map();
    return {
        timers, now: () => now,
        setTimeout(callback, delay) { const id = ++next; timers.set(id, { callback, at: now + delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
        jump(ms) { now += ms; },
        advance(ms) {
            const end = now + ms;
            while (true) {
                const due = [...timers].filter(([, timer]) => timer.at <= end)
                    .sort((a, b) => a[1].at - b[1].at)[0];
                if (!due) break;
                now = due[1].at; timers.delete(due[0]); due[1].callback();
            }
            now = end;
        },
    };
}
function entry(id = 'one', presentation = 'shown') {
    return { application_id: 'notes', panel_id: 'preview', capability: 'notes:1:preview',
        context: { record_id: id }, presentation, maximized: false,
        geometry: { x: 24, y: 24, width: 640, height: 480 } };
}
const snapshot = (...entries) => ({ version: 1, floats: entries });
const plain = value => JSON.parse(JSON.stringify(value));
function fixture({ ioOwner, timer = clock(), overrides = {} } = {}) {
    const value = { timer, io: ioOwner || new FloatPreferenceIoOwner({ clock: timer }),
        reads: [], writes: [], restores: [], statuses: [], policy: Symbol('policy'), ready: true,
        snapshot: snapshot(entry()) };
    const ports = {
        ioOwner: value.io, clock: timer,
        authority: () => ({ ready: value.ready, revision: value.policy }),
        projection: () => value.snapshot,
        storage: {
            read(args) { const call = { ...deferred(), args }; value.reads.push(call); return call.promise; },
            commit(args) { const call = { ...deferred(), args }; value.writes.push(call); return call.promise; },
        },
        restore: { begin(descriptor, options) {
            const call = { ...deferred(), descriptor, options, retired: 0 };
            value.restores.push(call);
            return { settled: call.promise, retire: () => { call.retired++; } };
        } },
        onStatus: status => value.statuses.push(status),
        ...overrides,
    };
    value.preferences = new FloatPreferences(ports);
    return value;
}
async function read(value, data) {
    assert.equal(value.preferences.start(), true);
    value.reads[0].resolve(data);
    await drain();
}
function save(value) {
    assert.equal(value.preferences.layoutChanged(), true);
    value.timer.advance(250);
    return value.writes.at(-1);
}
const acknowledge = call => call.resolve({ saved: true, revision: call.args.revision });

test('absent reviewed storage restore projection ports never call generic preferences', () => {
    let calls = 0;
    for (const missing of ['storage', 'restore', 'projection']) {
        const f = fixture({ overrides: { [missing]: undefined } });
        globalThis.preferences = { get: () => calls++, set: () => calls++ };
        assert.equal(f.preferences.start(), false);
        assert.equal(f.preferences.layoutChanged(), false);
        assert.equal(f.preferences.status().available, false);
        assert.deepEqual(f.io.pending(), { read: false, write: false, restore: 0, liveFrames: 1 });
        f.preferences.retire();
    }
    delete globalThis.preferences;
    assert.equal(calls, 0);
});

test('one automatic read restores at most five entries sequentially in saved background order', async () => {
    const f = fixture();
    const entries = Array.from({ length: 5 }, (_, index) => entry(String(index), index === 2 ? 'minimized' : 'shown'));
    await read(f, snapshot(...entries));
    assert.equal(f.preferences.start(), false);
    assert.equal(f.preferences.retryRestore(), false);
    for (let index = 0; index < 5; index++) {
        assert.equal(f.restores.length, index + 1);
        assert.deepEqual(plain(f.restores[index].descriptor), entries[index]);
        assert.equal(f.restores[index].options.rank, index);
        assert.equal(f.restores[index].options.background, true);
        assert.equal(f.restores[index].options.policyRevision, f.policy);
        f.restores[index].resolve({ status: 'ready' }); await drain();
    }
    assert.equal(f.preferences.status().restore, 'complete');
    assert.equal(f.reads.length, 1); assert.equal(f.writes.length, 0);
});

test('absent empty and malformed envelopes normalize non-destructively without persistence', async () => {
    for (const [data, warning] of [[undefined, false], [snapshot(), false], [null, true],
        [{ version: 2, floats: [] }, true], [snapshot(...Array(6).fill(entry())), true],
        [{ version: 1, floats: [], extra: true }, true]]) {
        const f = fixture(); await read(f, data);
        assert.equal(f.preferences.status().restore, 'complete');
        assert.equal(f.preferences.status().warning, warning);
        assert.equal(f.restores.length, 0); assert.equal(f.writes.length, 0);
    }
});

test('individual invalid entries and canonical duplicates preserve first valid order without getters', async () => {
    let invoked = 0;
    const unsafe = { ...entry('unsafe'), context: Object.defineProperty({}, 'record_id', {
        enumerable: true, get() { invoked++; return 'secret'; },
    }) };
    const first = entry('first');
    const duplicate = { ...entry('first', 'minimized'), capability: 'notes:01:preview' };
    const oversize = { ...entry('too-long'), context: { record_id: 'x'.repeat(513) } };
    const f = fixture(); await read(f, snapshot(first, duplicate, entry('bad', 'invalid'), oversize, entry('last')));
    assert.equal(invoked, 0);
    assert.deepEqual(plain(f.restores[0].descriptor), first);
    f.restores[0].resolve({ status: 'ready' }); await drain();
    assert.equal(f.restores.length, 2);
    assert.equal(f.restores[1].descriptor.context.record_id, 'last');
    assert.equal(f.preferences.status().warning, true);
    const accessors = fixture(); await read(accessors, snapshot(unsafe));
    assert.equal(invoked, 0);
    assert.equal(accessors.restores.length, 0);
    assert.equal(accessors.preferences.status().warning, true);
});

test('reviewed projection is mandatory and strict writes do not serialize arbitrary live state', async () => {
    const live = { ...entry(), dirty: true, component: {}, secret: 'fake-private-value' };
    const f = fixture({ overrides: { projection: () => snapshot(entry('approved-id')) } });
    f.live = live;
    const call = save(f);
    assert.deepEqual(plain(call.args.snapshot), snapshot(entry('approved-id')));
    assert.equal(JSON.stringify(call.args.snapshot).includes('fake-private-value'), false);
    acknowledge(call); await drain(); assert.equal(f.preferences.status().save, 'saved');
    f.snapshot = snapshot(live);
    const invalid = fixture({ overrides: { projection: () => snapshot(live) } });
    assert.equal(invalid.preferences.layoutChanged(), false);
    invalid.timer.advance(1000); assert.equal(invalid.writes.length, 0);
});

test('accepted explicit intent fences a pending read so a late layout never reopens', async () => {
    const f = fixture(); f.preferences.start(); f.preferences.acceptIntent();
    assert.equal(f.reads[0].args.signal.aborted, true);
    f.reads[0].resolve(snapshot(entry('closed'))); await drain();
    assert.equal(f.restores.length, 0);
    assert.equal(f.preferences.status().restore, 'interrupted');
});

test('intent interruption retires only current pending restore and discards remaining descriptors', async () => {
    const f = fixture(); await read(f, snapshot(entry('ready'), entry('pending'), entry('remaining')));
    f.restores[0].resolve({ status: 'ready' }); await drain();
    f.preferences.acceptIntent(); f.preferences.acceptIntent();
    assert.equal(f.restores[0].retired, 0); assert.equal(f.restores[1].retired, 1);
    assert.equal(f.preferences.retryRestore(), false);
    f.restores[1].resolve({ status: 'ready' }); await drain();
    assert.equal(f.restores.length, 2);
    assert.equal(f.preferences.status().restore, 'interrupted');
});

test('policy revision changes fence read restore and admitted writes without rollback claims', async () => {
    const f = fixture(); await read(f, snapshot(entry('pending'), entry('remaining')));
    const call = save(f);
    f.policy = Symbol('new policy'); f.preferences.policyChanged();
    assert.equal(f.restores[0].retired, 1); assert.equal(call.args.signal.aborted, true);
    f.restores[0].resolve({ status: 'ready' }); acknowledge(call); await drain();
    assert.equal(f.restores.length, 1); assert.equal(f.writes.length, 1);
    assert.equal(f.preferences.status().save, 'uncertain');
    assert.equal(f.preferences.retryRestore(), true);
    f.reads[1].resolve(snapshot()); await drain();
    assert.equal(f.preferences.status().restore, 'complete');
});

test('authority read failures and skipped capacity require deliberate retry not polling or eviction', async () => {
    const f = fixture(); f.ready = false;
    assert.equal(f.preferences.start(), false); f.timer.advance(100000);
    assert.equal(f.reads.length, 0); f.ready = true;
    assert.equal(f.preferences.retryRestore(), true);
    f.reads[0].reject(new Error('fake denied detail')); await drain();
    f.timer.advance(100000); assert.equal(f.reads.length, 1);
    assert.equal(f.preferences.retryRestore(), true);
    f.reads[1].resolve(snapshot(entry('denied'), entry('limit'))); await drain();
    f.restores[0].resolve({ status: 'skipped' }); await drain();
    f.restores[1].resolve({ status: 'skipped' }); await drain();
    assert.equal(f.preferences.status().warning, true);
    assert.equal(JSON.stringify(f.statuses).includes('fake denied detail'), false);
    assert.equal(f.writes.length, 0);
    const malformed = fixture(); await read(malformed, snapshot(entry()));
    let accessed = 0;
    malformed.restores[0].resolve(Object.defineProperty({}, 'status', {
        get() { accessed++; throw new Error('not data'); },
    }));
    await drain();
    assert.equal(accessed, 0);
    assert.equal(malformed.preferences.status().restore, 'retry-required');
});

test('restore retry reads fresh data once and never queues overlapping attempts', async () => {
    const f = fixture(); await read(f, snapshot(entry('old')));
    f.preferences.acceptIntent();
    for (let index = 0; index < 8; index++) assert.equal(f.preferences.retryRestore(), false);
    f.restores[0].resolve({ status: 'skipped' }); await drain();
    assert.equal(f.preferences.retryRestore(), true); assert.equal(f.preferences.retryRestore(), false);
    f.reads[1].resolve(snapshot(entry('new'))); await drain();
    assert.equal(f.restores[1].descriptor.context.record_id, 'new');
    assert.equal(f.restores[1].options.passToken === f.restores[0].options.passToken, false);
});

test('burst changes have one 250 millisecond trailing timer and only the latest snapshot', () => {
    const f = fixture(); f.preferences.layoutChanged();
    f.timer.advance(200); f.snapshot = snapshot(entry('latest')); f.preferences.layoutChanged();
    assert.equal(f.timer.timers.size, 1);
    f.timer.advance(249); assert.equal(f.writes.length, 0);
    f.timer.advance(1); assert.equal(f.writes.length, 1);
    assert.equal(f.writes[0].args.snapshot.floats[0].context.record_id, 'latest');
});

test('one physical write coalesces many replacements and honors an existing future timer', async () => {
    const f = fixture(); const first = save(f);
    for (let index = 0; index < 20; index++) {
        f.snapshot = snapshot(entry(String(index))); f.preferences.layoutChanged();
    }
    assert.equal(f.timer.timers.size, 2); // Existing write deadline plus one trailing timer.
    acknowledge(first); await drain();
    assert.equal(f.timer.timers.size, 1);
    f.timer.advance(250); assert.equal(f.writes.length, 2);
    assert.equal(f.writes[1].args.snapshot.floats[0].context.record_id, '19');
    acknowledge(f.writes[1]); await drain(); assert.equal(f.preferences.status().save, 'saved');
});

test('stale and malformed acknowledgements cannot mark a newer or invalid projected layout saved', async () => {
    const f = fixture(); const call = save(f);
    f.snapshot = snapshot({ ...entry(), extra: true });
    assert.equal(f.preferences.layoutChanged(), false);
    acknowledge(call); await drain(); assert.equal(f.preferences.status().save, 'not-saved');
    f.snapshot = snapshot(entry('new')); const next = save(f);
    next.resolve({ saved: true, revision: call.args.revision }); await drain();
    assert.equal(f.preferences.status().save, 'not-saved');
    assert.equal(f.snapshot.floats[0].context.record_id, 'new');
    let accessed = 0;
    const malformed = save(f);
    malformed.resolve(Object.defineProperty({ revision: malformed.args.revision }, 'saved', {
        get() { accessed++; return true; },
    }));
    await drain(); assert.equal(accessed, 0);
    assert.equal(f.preferences.status().save, 'not-saved');
    const inherited = save(f);
    inherited.resolve(Object.create({ saved: true, revision: inherited.args.revision }));
    await drain(); assert.equal(f.preferences.status().save, 'not-saved');
});

test('write failure drops automatic queued work while explicit retry uses fresh current projection', async () => {
    const f = fixture(); const first = save(f);
    f.snapshot = snapshot(entry('queued')); f.preferences.layoutChanged(); f.timer.advance(250);
    first.reject(new Error('fake private storage error')); await drain();
    f.timer.advance(100000); assert.equal(f.writes.length, 1);
    assert.equal(f.preferences.status().save, 'not-saved');
    f.snapshot = snapshot(entry('fresh')); assert.equal(f.preferences.retrySave(), true);
    f.timer.advance(250); assert.equal(f.writes[1].args.snapshot.floats[0].context.record_id, 'fresh');
    assert.equal(JSON.stringify(f.statuses).includes('fake private storage error'), false);
});

test('read and write timeouts abort once retain physical lanes and treat late success honestly', async () => {
    const f = fixture(); f.preferences.start();
    let readAborts = 0; f.reads[0].args.signal.addEventListener('abort', () => readAborts++);
    f.timer.advance(10000); assert.equal(readAborts, 1);
    assert.equal(f.io.pending().read, true); assert.equal(f.preferences.retryRestore(), false);
    f.reads[0].resolve(snapshot(entry('late'))); await drain();
    assert.equal(f.restores.length, 0); assert.equal(f.io.pending().read, false);
    const call = save(f); let writeAborts = 0;
    call.args.signal.addEventListener('abort', () => writeAborts++);
    f.snapshot = snapshot(entry('newer')); f.preferences.layoutChanged();
    f.timer.advance(10000); assert.equal(writeAborts, 1);
    assert.equal(f.io.pending().write, true); assert.equal(f.preferences.retrySave(), false);
    acknowledge(call); await drain();
    assert.equal(f.io.pending().write, false); assert.equal(f.preferences.status().save, 'uncertain');
    assert.equal(f.writes.length, 1); assert.equal(f.preferences.retrySave(), true);
});

test('delayed timers cannot admit a post-deadline synchronous or asynchronous read result', async () => {
    const f = fixture(); f.preferences.start(); f.timer.jump(10001);
    f.reads[0].resolve(snapshot(entry())); await drain();
    assert.equal(f.restores.length, 0); assert.equal(f.preferences.status().restore, 'retry-required');
    const c = clock();
    const sync = fixture({ timer: c, overrides: { storage: {
        read() { c.jump(10001); return snapshot(entry()); }, commit() { throw new Error('not called'); },
    } } });
    sync.preferences.start(); await drain();
    assert.equal(sync.restores.length, 0); assert.equal(sync.io.pending().read, false);
});

test('retirement drops frame dispatch timers snapshots and restore handles without deleting storage', async () => {
    const f = fixture(); await read(f, snapshot(entry('pending'), entry('remaining')));
    const call = save(f); f.snapshot = snapshot(entry('queued')); f.preferences.layoutChanged();
    f.preferences.retire(); f.preferences.retire();
    assert.equal(f.timer.timers.size, 0);
    assert.deepEqual(f.io.pending(), { read: false, write: true, restore: 1, liveFrames: 0 });
    const count = f.statuses.length;
    f.restores[0].resolve({ status: 'ready' }); acknowledge(call); await drain();
    assert.equal(f.statuses.length, count); assert.equal(f.restores.length, 1);
    assert.equal(f.restores[0].retired, 1); assert.equal(f.writes.length, 1);
    assert.deepEqual(f.io.pending(), { read: false, write: false, restore: 0, liveFrames: 0 });
    assert.equal(f.preferences.status().restore, 'retired');
});

test('replacement frames cannot reset stalled physical lanes or inherit another frame layout', async () => {
    const timer = clock(); const io = new FloatPreferenceIoOwner({ clock: timer });
    const old = fixture({ timer, ioOwner: io }); old.preferences.start(); const write = save(old);
    old.preferences.retire();
    for (let index = 0; index < 10; index++) {
        const next = fixture({ timer, ioOwner: io });
        assert.equal(next.preferences.start(), false); assert.equal(next.preferences.retrySave(), false);
        assert.equal(next.reads.length, 0); assert.equal(next.writes.length, 0); next.preferences.retire();
    }
    assert.deepEqual(io.pending(), { read: true, write: true, restore: 0, liveFrames: 0 });
    const current = fixture({ timer, ioOwner: io }); current.preferences.start();
    old.reads[0].resolve(snapshot(entry('old-user'))); acknowledge(write); await drain();
    assert.equal(current.restores.length, 0); assert.equal(current.reads.length, 0);
    assert.equal(current.preferences.retryRestore(), true);
    current.reads[0].resolve(snapshot(entry('current-user'))); await drain();
    assert.equal(current.restores[0].descriptor.context.record_id, 'current-user');
    assert.deepEqual(Object.keys(io), ['clock']);
});

test('synchronous restore retirement still observes settlement and malformed handles remain bounded', async () => {
    let f; const pending = deferred(); let retired = 0;
    f = fixture({ overrides: { restore: { begin() {
        f.preferences.retire();
        return { settled: pending.promise, retire() { retired++; } };
    } } } });
    await read(f, snapshot(entry()));
    assert.equal(retired, 1); assert.equal(f.io.pending().restore, 1);
    assert.equal(f.preferences.status().restore, 'retired');
    pending.resolve({ status: 'ready' }); await drain(); assert.equal(f.io.pending().restore, 0);
    const malformed = fixture({ overrides: { restore: { begin: () => ({}) } } });
    await read(malformed, snapshot(entry()));
    assert.equal(malformed.preferences.status().restore, 'cleanup-pending');
    assert.equal(malformed.preferences.retryRestore(), false);
    malformed.preferences.retire(); assert.equal(malformed.io.pending().restore, 1);
    let accessed = 0;
    const accessor = fixture({ overrides: { restore: { begin: () =>
        Object.defineProperty({}, 'settled', { get() { accessed++; throw new Error('not data'); } }) } } });
    await read(accessor, snapshot(entry()));
    assert.equal(accessed, 0);
    assert.equal(accessor.preferences.status().restore, 'cleanup-pending');
    assert.equal(accessor.io.pending().restore, 1);
});

test('independent documents have no cross-tab CAS and synchronous port failure never fabricates success', async () => {
    const first = fixture(); const second = fixture();
    const a = save(first); const b = save(second);
    assert.notEqual(a.args.owner, b.args.owner); assert.notEqual(a.args.revision, b.args.revision);
    acknowledge(b); await drain(); acknowledge(a); await drain();
    assert.equal(first.preferences.status().save, 'saved'); assert.equal(second.preferences.status().save, 'saved');
    const failure = fixture({ overrides: { storage: {
        read() { throw new Error('fake read failure'); },
        commit() { throw new Error('fake commit failure'); },
    } } });
    failure.preferences.start(); assert.equal(failure.io.pending().read, false);
    failure.preferences.layoutChanged(); failure.timer.advance(250);
    assert.equal(failure.io.pending().write, false); assert.equal(failure.preferences.status().save, 'not-saved');
});

test('authority requires own data ready and revision without invoking accessors', () => {
    let accessed = 0;
    const revision = Symbol('policy');
    const malformed = [Object.create({ ready: true, revision }),
        Object.defineProperty({ revision }, 'ready', { get() { accessed++; return true; } }),
        Object.defineProperty({ ready: true }, 'revision', { get() { accessed++; return revision; } })];
    for (const authority of malformed) {
        const f = fixture({ overrides: { authority: () => authority } });
        assert.equal(f.preferences.start(), false);
        assert.equal(f.preferences.layoutChanged(), false);
        assert.equal(f.reads.length, 0); assert.equal(f.writes.length, 0);
        assert.equal(accessed, 0);
        f.preferences.retire();
    }
});

test('unknown storage thenables never execute getters or reclaim their admitted physical lane', async () => {
    for (const kind of ['read', 'write']) {
        for (const accessor of [false, true]) {
            let invoked = 0;
            const unknown = accessor
                ? Object.defineProperty({}, 'then', { get() { invoked++; throw new Error('not data'); } })
                : { then(resolve) { invoked++; resolve(kind === 'read' ? undefined : { saved: true }); } };
            const io = new FloatPreferenceIoOwner({ clock: clock() });
            const owner = io.register(() => {});
            const token = io.acquire(kind, owner);
            assert.equal(io.submit(token, () => unknown), true);
            await drain();
            assert.equal(invoked, 0, 'unknown async handles cannot execute during observation');
            assert.equal(io.pending()[kind], true, 'no physical settlement proof exists');
            io.retireOwner(owner);
            assert.equal(io.pending()[kind], true, 'retirement cannot release unknown admitted work');
            assert.equal(io.pending().liveFrames, 0);
        }
    }
});

test('native promise observation bypasses then accessors and quarantines unsafe constructor accessors', async () => {
    for (const property of ['then', 'constructor']) {
        let accessed = 0;
        const pending = deferred();
        Object.defineProperty(pending.promise, property, {
            get() { accessed++; throw new Error('not an observation API'); },
        });
        const io = new FloatPreferenceIoOwner({ clock: clock() });
        const events = []; const owner = io.register(event => events.push(event));
        const token = io.acquire('read', owner);
        io.submit(token, () => pending.promise);
        assert.equal(accessed, 0);
        assert.equal(io.pending().read, true);
        pending.resolve(undefined); await drain();
        assert.equal(accessed, 0);
        if (property === 'constructor') {
            assert.equal(io.pending().read, true, 'intrinsic then cannot observe safely through an unsafe species boundary');
            assert.equal(events.length, 0);
        } else {
            assert.equal(io.pending().read, false);
            assert.equal(events.length, 1); assert.equal(events[0].outcome, 'success');
        }
        io.retireOwner(owner);
    }
});

test('synchronous preference clock setup failure leaves no unsubmitted physical lane', () => {
    for (const method of ['setTimeout', 'now']) {
        const timer = clock(); let fail = true;
        const injected = { ...timer, [method](...args) {
            if (fail) { fail = false; throw new Error('controlled preference clock setup failure'); }
            return timer[method](...args);
        } };
        const io = new FloatPreferenceIoOwner({ clock: injected });
        const owner = io.register(() => {});
        assert.equal(io.acquire('read', owner), null);
        assert.equal(io.pending().read, false);
        assert.equal(timer.timers.size, 0);
        const reused = io.acquire('read', owner);
        assert.equal(typeof reused, 'symbol');
        io.submit(reused, () => { throw new Error('no physical request'); });
        assert.equal(io.pending().read, false);
        io.retireOwner(owner);
    }
});

test('synchronous decoded absent and malformed reads physically complete without quarantining data', async () => {
    for (const data of [undefined, null, [], 0, 'malformed', true]) {
        const f = fixture({ overrides: { storage: { read: () => data, commit: () => ({ saved: false }) } } });
        assert.equal(f.preferences.start(), true);
        await drain();
        assert.equal(f.io.pending().read, false);
        assert.equal(f.preferences.status().restore, 'complete');
        assert.equal(f.preferences.status().warning, data !== undefined);
        assert.equal(f.restores.length, 0);
        f.preferences.retire();
    }
});

test('authority reentrant intent or retirement fences a settled read before restoring any entry', async () => {
    for (const action of ['acceptIntent', 'retire']) {
        let f, armed = false;
        f = fixture({ overrides: { authority() {
            if (armed) { armed = false; f.preferences[action](); }
            return { ready: true, revision: f.policy };
        } } });
        assert.equal(f.preferences.start(), true);
        armed = true; f.reads[0].resolve(snapshot(entry())); await drain();
        assert.equal(f.io.pending().read, false);
        assert.equal(f.restores.length, 0);
        assert.equal(f.preferences.status().restore, action === 'retire' ? 'retired' : 'interrupted');
        f.preferences.retire();
    }
});

test('authority reentrant retirement cannot overwrite retired status or start storage work', () => {
    for (const operation of ['start', 'layoutChanged']) {
        let f;
        f = fixture({ overrides: { authority() {
            f.preferences.retire();
            return { ready: true, revision: f.policy };
        } } });
        assert.equal(f.preferences[operation](), false);
        assert.equal(f.preferences.status().retired, true);
        assert.equal(f.preferences.status().restore, 'retired');
        assert.equal(f.preferences.status().save, 'retired');
        assert.equal(f.reads.length, 0); assert.equal(f.writes.length, 0);
        assert.equal(f.io.pending().liveFrames, 0);
    }
});

test('explicit intent accepted within initial authority lookup prevents a stale automatic restore pass', () => {
    let f, armed = true;
    f = fixture({ overrides: { authority() {
        if (armed) { armed = false; f.preferences.acceptIntent(); }
        return { ready: true, revision: f.policy };
    } } });
    assert.equal(f.preferences.start(), false);
    assert.equal(f.reads.length, 0); assert.equal(f.io.pending().read, false);
    f.preferences.retire();
});

test('authority retirement during write admission or acknowledgement cannot dereference stale layout or rewrite retired state', async () => {
    for (const phase of ['admission', 'acknowledgement']) {
        let f, armed = false;
        f = fixture({ overrides: { authority() {
            if (armed) { armed = false; f.preferences.retire(); }
            return { ready: true, revision: f.policy };
        } } });
        assert.equal(f.preferences.layoutChanged(), true);
        if (phase === 'admission') {
            armed = true;
            assert.doesNotThrow(() => f.timer.advance(250));
            assert.equal(f.writes.length, 0);
        } else {
            f.timer.advance(250); armed = true;
            acknowledge(f.writes[0]); await drain();
            assert.equal(f.io.pending().write, false);
        }
        assert.equal(f.preferences.status().restore, 'retired');
        assert.equal(f.preferences.status().save, 'retired');
        assert.equal(f.io.pending().liveFrames, 0);
        assert.equal(f.timer.timers.size, 0);
    }
});

test('authority retirement after restore admission preserves terminal state and physical ownership', async () => {
    let f, lookups = 0;
    f = fixture({ overrides: { authority() {
        if (++lookups === 4) f.preferences.retire();
        return { ready: true, revision: f.policy };
    } } });
    assert.equal(f.preferences.start(), true);
    f.reads[0].resolve(snapshot(entry())); await drain();
    assert.equal(lookups, 4); assert.equal(f.restores.length, 1);
    assert.equal(f.restores[0].retired, 1);
    assert.equal(f.preferences.status().retired, true);
    assert.equal(f.preferences.status().restore, 'retired');
    assert.equal(f.preferences.status().save, 'retired');
    assert.equal(f.io.pending().restore, 1); assert.equal(f.io.pending().liveFrames, 0);
    f.restores[0].resolve({ status: 'ready' }); await drain();
    assert.equal(f.io.pending().restore, 0);
    assert.equal(f.preferences.status().restore, 'retired');
});
