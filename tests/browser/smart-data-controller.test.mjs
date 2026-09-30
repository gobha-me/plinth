import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import test from 'node:test';

const root = new URL('../../client/shell/client/', import.meta.url);
const context = vm.createContext({ AbortController });
const modules = new Map();
async function load(name) {
    if (modules.has(name)) return modules.get(name);
    const url = new URL(name, root);
    const module = new vm.SourceTextModule(await readFile(url, 'utf8'), {
        context, identifier: fileURLToPath(url),
    });
    modules.set(name, module);
    await module.link(specifier => load(specifier.replace(/^\.\//, '')));
    await module.evaluate();
    return module;
}
const { prepareCapabilityRequest, captureView, sameQuery } = (await load('data-query.js')).namespace;
const { createDataController } = (await load('data-controller.js')).namespace;
const plain = value => JSON.parse(JSON.stringify(value));
const CHANNEL = 'plinth:data:ext_demo.rows';
const deferred = () => {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
};
const flush = async () => { for (let i = 0; i < 12; i++) await Promise.resolve(); };
const frame = (...ops) => ({ type: 'event', channel: CHANNEL,
    payload: { channel: CHANNEL, ops } });
const op = (name, ids) => ({ op: name, count: ids.length, ids });

function harness(options = {}) {
    let now = 0, nextTimer = 0, allowed = options.allowed ?? true, current = true;
    let subscription, unsubscribeCount = 0, draws = 0;
    const timers = new Map(), requests = [], published = [], observers = new Set();
    const query = { channel: CHANNEL, scope: 'fake-owner',
        snapshot: prepareCapabilityRequest('demo.rows', { table: 'rows' }),
        initialData: undefined, view: null, ...options.query };
    const controller = createDataController({ query,
        isCurrent: () => current,
        publish: state => published.push(state),
        request: (prepared, { signal }) => {
            const result = deferred();
            requests.push({ prepared, signal, ...result });
            return result.promise;
        },
        subscribe: (channel, handler, callbacks) => {
            assert.equal(channel, CHANNEL);
            subscription = { handler, ...callbacks };
            options.onSubscribe?.(subscription, () => setAllowed(false));
            return () => { unsubscribeCount++; };
        },
        admission: {
            isAllowed: () => allowed,
            subscribe: callback => {
                observers.add(callback);
                options.onObserve?.(() => setAllowed(false));
                return () => observers.delete(callback);
            },
        },
        clock: () => now,
        random: () => { draws++; return options.random ?? 0; },
        timer: {
            set: (callback, delay) => {
                assert.ok(Number.isFinite(delay) && delay >= 0);
                const id = ++nextTimer;
                timers.set(id, { at: now + delay, callback });
                return id;
            },
            clear: id => timers.delete(id),
        }, AbortController,
    });
    function setAllowed(value) {
        allowed = value;
        for (const callback of [...observers]) callback();
    }
    return { query, controller, requests, published, timers, observers,
        get draws() { return draws; }, get unsubscribeCount() { return unsubscribeCount; },
        async start() { controller.start(); await flush(); },
        async settle(index, value) { requests[index].resolve(value); await flush(); },
        async reject(index, error) { requests[index].reject(error); await flush(); },
        async event(value) { subscription.handler(value); await flush(); },
        async ready(advice) { subscription.onReady(advice); await flush(); },
        async error(error) { subscription.onError(error); await flush(); },
        retireSession: () => setAllowed(false), replaceRender: () => { current = false; },
        async advance(ms) {
            const target = now + ms;
            for (let count = 0; count < 100; count++) {
                const due = [...timers].filter(([, item]) => item.at <= target)
                    .sort((a, b) => a[1].at - b[1].at || a[0] - b[0])[0];
                if (!due) { now = target; await flush(); return; }
                now = due[1].at;
                timers.delete(due[0]);
                due[1].callback();
                await flush();
            }
            assert.fail('unbounded timer loop');
        },
    };
}
async function baseline(rows = [], options = {}) {
    const h = harness(options);
    await h.start();
    await h.settle(0, rows);
    return h;
}

test('C32 query capture preserves call bytes and canonical equivalent JSON identity', () => {
    const a = prepareCapabilityRequest('fake.rows/one', { z: 1, a: [null, true] });
    assert.equal(a.url, '/api/cap/fake.rows%2Fone');
    assert.equal(a.body, '{"args":{"z":1,"a":[null,true]}}');
    assert.equal(prepareCapabilityRequest('fake.rows', undefined).body, '{"args":null}');
    assert.equal(prepareCapabilityRequest('fake.rows', () => {}).body, '{}');
    assert.ok(Object.isFrozen(a));
    const b = prepareCapabilityRequest('fake.rows/one', { a: [null, true], z: 1 });
    assert.ok(sameQuery({ channel: CHANNEL, snapshot: a }, { channel: CHANNEL, snapshot: b }));
    assert.throws(() => prepareCapabilityRequest('fake.rows', 1n), /BigInt/);
    const cyclic = {}; cyclic.self = cyclic;
    assert.throws(() => prepareCapabilityRequest('fake.rows', cyclic), /circular/i);
});

test('C32 request and where capture do not follow later caller mutation', () => {
    const args = { values: [1] }, ids = ['a', 1];
    const snapshot = prepareCapabilityRequest('fake.rows', args);
    const view = captureView({ key: 'id', complete: true, where: { in: ids } });
    args.values.push(2); ids.push('b');
    assert.equal(snapshot.body, '{"args":{"values":[1]}}');
    assert.deepEqual(new Set(view.where.in), new Set(['a', 1]));
    assert.ok(Object.isFrozen(view) && Object.isFrozen(view.where.in));
});

test('C32 query identity distinguishes ownership, request, key predicate and adapter', () => {
    const adapter = () => [];
    const q = { channel: CHANNEL, scope: 'A', snapshot: prepareCapabilityRequest('a', 1),
        view: captureView({ key: 'id', complete: true, where: { in: [1, '1'] }, insertRows: adapter }) };
    assert.ok(sameQuery(q, { ...q, initialData: 'different' }));
    assert.ok(sameQuery(q, { ...q, view: captureView({ key: 'id', complete: true,
        where: { in: ['1', 1] }, insertRows: adapter }) }));
    for (const replacement of [{ channel: 'other' }, { scope: 'B' },
        { snapshot: prepareCapabilityRequest('a', 2) }, { snapshot: prepareCapabilityRequest('b', 1) },
        { view: captureView({ key: 'id', complete: true, where: { eq: 1 }, insertRows: adapter }) },
        { view: captureView({ key: 'id', complete: true, where: { in: [1, '1'] }, insertRows: () => [] }) }]) {
        assert.equal(sameQuery(q, { ...q, ...replacement }), false);
    }
    const failure = () => Object.assign(new Error('fetch failed'), { name: 'NetworkError', cause: new TypeError('cycle') });
    assert.ok(sameQuery({ ...q, preparationError: failure() }, { ...q, preparationError: failure() }));
    assert.equal(sameQuery({ ...q, preparationError: failure() }, q), false);
});

test('C32 unsupported or paginated descriptor disables optimism', () => {
    for (const value of [null, {}, { key: 'id', complete: false }, { key: '', complete: true },
        { key: 'id', complete: true, limit: 10 }, { key: 'id', complete: true, where: { eq: 1, in: [1] } },
        { key: 'id', complete: true, where: { in: [1, 1] } },
        { key: 'id', complete: true, where: { eq: 1.5 } },
        { key: 'id', complete: true, insertRows: true }]) assert.equal(captureView(value), null);
    assert.equal(captureView({ get key() { throw new Error('uncertain descriptor'); } }), null);
});

test('C32 construction inert, start idempotent and initial loading exact', async () => {
    const h = harness({ query: { initialData: 'seed' } });
    assert.equal(h.requests.length, 0); assert.equal(h.observers.size, 0);
    assert.deepEqual(plain(h.controller.read()), { data: 'seed', error: null, loading: true });
    await h.start(); await h.start();
    assert.equal(h.requests.length, 1); assert.equal(h.observers.size, 1);
    await h.settle(0, 'server');
    assert.deepEqual(plain(h.controller.read()), { data: 'server', error: null, loading: false });
});

test('C32 native counts requery once at fixed window with one random draw', async () => {
    const h = await baseline('old', { random: 0.5 });
    const event = frame({ op: 'update', count: 1 });
    await h.event(event); await h.advance(50); await h.event(event);
    assert.equal(h.draws, 1); assert.equal(h.timers.size, 1);
    await h.advance(74); assert.equal(h.requests.length, 1);
    await h.event(event); await h.advance(1);
    assert.equal(h.requests.length, 2); assert.equal(h.draws, 1);
    await h.settle(1, 'new'); assert.equal(h.controller.read().data, 'new');
});

test('C32 zero advice coalesces synchronous events and honors zero', async () => {
    const h = await baseline([]);
    await h.ready({ debounceMs: 0, jitterMs: 0 });
    await h.event(frame({ op: 'update', count: 1 }));
    await h.event(frame({ op: 'update', count: 2 }));
    assert.equal(h.requests.length, 1); assert.equal(h.timers.size, 1); assert.equal(h.draws, 1);
    await h.advance(0); assert.equal(h.requests.length, 2);
});

test('C32 advice independently defaults invalid fields and accepts maximum inclusive jitter', async () => {
    const h = await baseline([], { random: 0.999999 });
    await h.ready({ debounceMs: 60000, jitterMs: 5000 });
    await h.event(frame({ op: 'update', count: 1 }));
    await h.advance(64999); assert.equal(h.requests.length, 1);
    await h.advance(1); assert.equal(h.requests.length, 2);
    await h.settle(1, []);
    await h.ready({ debounceMs: false, jitterMs: 0 });
    await h.event(frame({ op: 'update', count: 1 }));
    await h.advance(99); assert.equal(h.requests.length, 2);
    await h.advance(1); assert.equal(h.requests.length, 3);
    await h.settle(2, []);
    await h.ready({ debounceMs: 0, jitterMs: -1 });
    await h.event(frame({ op: 'update', count: 1 }));
    await h.advance(49); assert.equal(h.requests.length, 3);
    await h.advance(1); assert.equal(h.requests.length, 4);
});

test('C32 invalid advice/rand boundaries use independent defaults and bounded delays', async () => {
    for (const invalid of [undefined, null, true, -1, 0.5, Infinity, NaN, '0', 60001]) {
        const h = await baseline([], { random: 0 });
        await h.ready({ debounceMs: invalid, jitterMs: 0 });
        await h.event(frame({ op: 'update', count: 1 }));
        await h.advance(99); assert.equal(h.requests.length, 1);
        await h.advance(1); assert.equal(h.requests.length, 2);
    }
    for (const invalid of [undefined, null, true, -1, 0.5, Infinity, NaN, '0', 5001]) {
        const h = await baseline([], { random: 0.999999 });
        await h.ready({ debounceMs: 0, jitterMs: invalid });
        await h.event(frame({ op: 'update', count: 1 }));
        await h.advance(49); assert.equal(h.requests.length, 1);
        await h.advance(1); assert.equal(h.requests.length, 2);
    }
    for (const invalid of [NaN, Infinity, -1, 1]) {
        const h = await baseline([], { random: invalid });
        await h.ready({ debounceMs: 0, jitterMs: 50 });
        await h.event(frame({ op: 'update', count: 1 }));
        await h.advance(0); assert.equal(h.requests.length, 2);
    }
});

test('C32 controller captures query object and independent controllers own advice/timers', async () => {
    const a = await baseline([], { query: { view: captureView({ key: 'id', complete: true }) } });
    const b = await baseline([]);
    a.query.snapshot = prepareCapabilityRequest('changed-after-capture', { changed: true });
    a.query.view = null;
    await a.ready({ debounceMs: 0, jitterMs: 0 });
    await a.event(frame(op('delete', [99]))); assert.equal(a.timers.size, 0);
    await a.event(frame({ op: 'update', count: 1 }));
    await b.event(frame({ op: 'update', count: 1 }));
    assert.equal(a.timers.size, 1); assert.equal(b.timers.size, 1);
    await a.advance(0); assert.equal(a.requests.length, 2);
    assert.equal(a.requests[1].prepared.capability, 'demo.rows');
    a.controller.dispose(); assert.equal(b.timers.size, 1);
    await b.advance(99); assert.equal(b.requests.length, 1);
    await b.advance(1); assert.equal(b.requests.length, 2);
});

test('C32 dirty-in-flight serializes one due followup and ordinary storm replies progress', async () => {
    const h = harness(); await h.start();
    await h.event(frame({ op: 'update', count: 1 })); await h.advance(100);
    assert.equal(h.requests.length, 1); assert.equal(h.timers.size, 0);
    await h.event(frame({ op: 'update', count: 3 }));
    assert.equal(h.draws, 1);
    await h.settle(0, 'first-point-in-time');
    assert.equal(h.controller.read().data, 'first-point-in-time'); assert.equal(h.requests.length, 2);
    await h.event(frame({ op: 'update', count: 1 })); await h.advance(100);
    await h.settle(1, 'second-point-in-time');
    assert.equal(h.controller.read().data, 'second-point-in-time'); assert.equal(h.requests.length, 3);
});

test('C32 future dirty deadline retained after current request completes', async () => {
    const h = harness(); await h.start();
    await h.event(frame({ op: 'update', count: 1 })); await h.advance(30);
    await h.settle(0, []); assert.equal(h.requests.length, 1);
    await h.advance(69); assert.equal(h.requests.length, 1);
    await h.advance(1); assert.equal(h.requests.length, 2);
});

test('C32 independent live/query/adapter errors retain last good and correct precedence', async () => {
    const adapterError = new Error('adapter'), liveError = new Error('live denial'), queryError = new Error('query');
    const h = await baseline([], { query: { view: captureView({ key: 'id', complete: true,
        insertRows: () => { throw adapterError; } }) } });
    await h.event(frame(op('insert', [1])));
    assert.equal(h.controller.read().error, adapterError);
    await h.advance(100); await h.reject(1, queryError);
    assert.equal(h.controller.read().error, queryError); assert.deepEqual(plain(h.controller.read().data), []);
    await h.error(liveError); await h.event(frame(op('update', [1])));
    await h.advance(100); await h.settle(2, [{ id: 1 }]);
    assert.equal(h.controller.read().error, liveError);
    await h.ready({ debounceMs: 0, jitterMs: 0 });
    assert.equal(h.controller.read().error, null);
});

test('C32 preparation failure never silently enters raw mode or requests', async () => {
    const error = new Error('typed serialization failure');
    const h = harness({ query: { snapshot: null, preparationError: error, initialData: 'seed' } });
    await h.start(); await h.event(frame({ op: 'update', count: 1 })); await h.advance(1000);
    assert.equal(h.requests.length, 0); assert.equal(h.controller.read().data, 'seed');
    assert.equal(h.controller.read().error, error); assert.equal(h.controller.read().loading, false);
});

test('C32 no-snapshot raw stream preserves full frame and no request', async () => {
    const h = harness({ query: { snapshot: null } }); await h.start();
    const event = frame({ op: 'update', count: 1 }); await h.event(event);
    assert.equal(h.controller.read().data, event); assert.equal(h.requests.length, 0);
});

test('C32 dispose cancels owned timer/subscriber/observer and ignored abort cannot publish', async () => {
    const h = harness(); await h.start(); await h.event(frame({ op: 'update', count: 1 }));
    const count = h.published.length;
    h.controller.dispose(); h.controller.dispose();
    assert.equal(h.timers.size, 0); assert.equal(h.unsubscribeCount, 1); assert.equal(h.observers.size, 0);
    assert.equal(h.requests[0].signal.aborted, true);
    await h.settle(0, 'late'); await h.ready(); await h.advance(1000);
    assert.equal(h.published.length, count); assert.equal(h.requests.length, 1);
});

test('C32 permanent retirement closes admission and cannot be revived by readiness', async () => {
    const h = harness(); await h.start(); h.retireSession();
    const count = h.published.length;
    await h.ready(); await h.event(frame({ op: 'update', count: 1 })); await h.settle(0, 'A');
    assert.equal(h.requests.length, 1); assert.equal(h.published.length, count);
    const denied = harness({ allowed: false }); await denied.start();
    assert.equal(denied.requests.length, 0); assert.equal(denied.observers.size, 0);
});

test('C32 render replacement blocks callbacks before effect disposal', async () => {
    const h = harness(); await h.start();
    const count = h.published.length; h.replaceRender();
    await h.settle(0, 'old'); await h.event(frame({ op: 'update', count: 1 }));
    await h.advance(1000); assert.equal(h.published.length, count); assert.equal(h.requests.length, 1);
    h.controller.dispose(); assert.equal(h.unsubscribeCount, 1);
});

test('C32 recoverable auth retires old requests; fresh same-session grant permits repeated recovery', async () => {
    const h = harness(); await h.start();
    const error = { code: 'auth_failed' }; await h.error(error);
    assert.equal(h.requests[0].signal.aborted, true);
    await h.event(frame({ op: 'update', count: 1 })); await h.advance(1000);
    assert.equal(h.requests.length, 1); assert.equal(h.controller.read().error, error);
    await h.ready(); assert.equal(h.requests.length, 2);
    await h.settle(1, 'fresh'); await h.settle(0, 'obsolete');
    assert.equal(h.controller.read().data, 'fresh'); assert.equal(h.controller.read().error, null);
    await h.error({ code: 'auth_timeout' }); await h.ready(); assert.equal(h.requests.length, 3);
    await h.settle(2, 'fresh-again'); assert.equal(h.controller.read().data, 'fresh-again');
});

test('C32 transient disconnect retains ordinary query admission and live error after snapshot success', async () => {
    const h = await baseline('old'); const error = { code: 'disconnected' };
    await h.error(error); await h.event(frame({ op: 'update', count: 1 })); await h.advance(100);
    await h.settle(1, 'new'); assert.equal(h.controller.read().data, 'new');
    assert.equal(h.controller.read().error, error); await h.ready(); assert.equal(h.controller.read().error, null);
});

test('C32 synchronous admission retirement cleans observer and synchronous subscribe retirement cleans subscriber', async () => {
    const observed = harness({ onObserve: retire => retire() }); await observed.start();
    assert.equal(observed.observers.size, 0); assert.equal(observed.requests.length, 0);
    const subscribed = harness({ onSubscribe: (_, retire) => retire() }); await subscribed.start();
    assert.equal(subscribed.observers.size, 0); assert.equal(subscribed.unsubscribeCount, 1);
    assert.equal(subscribed.requests.length, 0);
});

test('C32 complete deletes immutable, idle absent row skips, numeric/string IDs distinct', async () => {
    const original = [{ id: 1, name: 'number' }, { id: '1', name: 'string' }];
    const h = await baseline(original, { query: { view: captureView({ key: 'id', complete: true }) } });
    await h.event(frame(op('delete', [1])));
    assert.deepEqual(plain(h.controller.read().data), [{ id: '1', name: 'string' }]);
    assert.equal(original.length, 2); assert.notEqual(h.controller.read().data, original);
    await h.event(frame(op('delete', [99]))); await h.advance(1000);
    assert.equal(h.requests.length, 1); assert.equal(h.draws, 0);
});

test('C32 absent-row delete bars in-flight resurrection and reconciles exactly once', async () => {
    const h = await baseline([], { query: { view: captureView({ key: 'id', complete: true }) } });
    await h.event(frame(op('update', ['k']))); await h.advance(100);
    assert.equal(h.requests.length, 2);
    await h.event(frame(op('delete', ['k']))); await h.advance(100);
    await h.settle(1, [{ id: 'k' }]);
    assert.deepEqual(plain(h.controller.read().data), []); assert.equal(h.requests.length, 3);
    await h.settle(2, []); assert.equal(h.requests.length, 3);
});

test('C32 cold absent-delete cannot treat initialData as an authoritative empty baseline', async () => {
    const h = harness({ query: { initialData: [], view: captureView({ key: 'id', complete: true }) } });
    await h.start(); await h.event(frame(op('delete', ['k']))); await h.advance(100);
    await h.settle(0, [{ id: 'k' }]);
    assert.deepEqual(plain(h.controller.read().data), []); assert.equal(h.requests.length, 2);
    await h.settle(1, []); assert.equal(h.requests.length, 2);
});

test('C32 cold absent-delete without any initial data discards obsolete result and obsolete rejection', async () => {
    for (const reject of [false, true]) {
        const h = harness({ query: { view: captureView({ key: 'id', complete: true }) } });
        await h.start(); await h.event(frame(op('delete', ['k']))); await h.advance(100);
        if (reject) await h.reject(0, new Error('obsolete failure'));
        else await h.settle(0, [{ id: 'k' }]);
        assert.equal(h.controller.read().data, undefined); assert.equal(h.controller.read().error, null);
        assert.equal(h.requests.length, 2); await h.settle(1, []);
        assert.deepEqual(plain(h.controller.read().data), []);
    }
});

test('C32 optimistic insertion clones full rows; late snapshot cannot erase it', async () => {
    let supplied = [{ id: 'k', nested: { value: 'new' } }];
    const h = await baseline([], { query: { view: captureView({ key: 'id', complete: true, insertRows: () => supplied }) } });
    await h.event(frame(op('update', ['k']))); await h.advance(100);
    await h.event(frame(op('insert', ['k'])));
    supplied[0].nested.value = 'mutated';
    assert.deepEqual(plain(h.controller.read().data), [{ id: 'k', nested: { value: 'new' } }]);
    await h.advance(100); await h.settle(1, []);
    assert.deepEqual(plain(h.controller.read().data), [{ id: 'k', nested: { value: 'new' } }]);
    assert.equal(h.requests.length, 3); await h.settle(2, [{ id: 'k', nested: { value: 'server' } }]);
    assert.equal(h.controller.read().data[0].nested.value, 'server');
});

test('C32 optimism does not cancel an earlier unknown invalidation', async () => {
    const h = await baseline([{ id: 1 }, { id: 2 }], { query: { view: captureView({ key: 'id', complete: true }) } });
    await h.event(frame({ op: 'update', count: 1 })); await h.event(frame(op('delete', [1])));
    assert.deepEqual(plain(h.controller.read().data), [{ id: 2 }]);
    await h.advance(100); assert.equal(h.requests.length, 2); assert.equal(h.draws, 1);
});

test('C32 immutable-key eq/in filters skip disjoint updates/deletes and filter complete insert rows', async () => {
    for (const where of [{ eq: 'k' }, { in: ['k'] }]) {
        const h = await baseline([], { query: { view: captureView({ key: 'id', complete: true, where,
            insertRows: () => [{ id: 'k', name: 'kept' }, { id: 'other', name: 'outside' }] }) } });
        await h.event(frame(op('update', ['other']))); await h.event(frame(op('delete', ['other'])));
        await h.event(frame(op('insert', ['k', 'other']))); await h.advance(1000);
        assert.deepEqual(plain(h.controller.read().data), [{ id: 'k', name: 'kept' }]);
        assert.equal(h.requests.length, 1); assert.equal(h.draws, 0);
    }
    const typed = await baseline([{ id: 1 }], { query: { view: captureView({ key: 'id', complete: true,
        where: { eq: 1 } }) } });
    await typed.event(frame(op('delete', ['1']))); await typed.event(frame(op('update', ['1'])));
    await typed.advance(1000); assert.equal(typed.requests.length, 1);
    assert.deepEqual(plain(typed.controller.read().data), [{ id: 1 }]);
});

test('C32 malformed summaries always use conservative requery', async () => {
    const fixtures = [frame({ op: 'update', count: 1 }), frame({ op: 'delete', count: 2, ids: [1] }),
        frame({ op: 'delete', count: 1, ids: [1], truncated: true }),
        frame({ op: 'future', count: 1, ids: [1] }), frame({ op: 'delete', count: -1, ids: [] }),
        frame({ op: 'delete', count: true, ids: [1] }), frame(op('delete', [1, 1])),
        frame(op('delete', [1.5])), frame(op('delete', [1]), op('update', [1])),
        frame(op('delete', [1]), op('delete', [2])), frame(),
        { ...frame(op('delete', [1])), channel: 'foreign' },
        { ...frame(op('delete', [1])), payload: { channel: CHANNEL, truncated: true, ops: [op('delete', [1])] } },
        { ...frame(op('delete', [1])), payload: { channel: 'foreign', ops: [op('delete', [1])] } }];
    for (const event of fixtures) {
        const h = await baseline([{ id: 1 }], { query: { view: captureView({ key: 'id', complete: true }) } });
        await h.event(event); await h.advance(100);
        assert.equal(h.requests.length, 2); assert.deepEqual(plain(h.controller.read().data), [{ id: 1 }]);
    }
});

test('C32 incompatible baselines/mixed relevant operations/missing rows/collisions requery', async () => {
    for (const rows of [null, {}, [{ id: 1 }, { id: 1 }], [{ id: 1.5 }], [{ noKey: 1 }]]) {
        const h = await baseline(rows, { query: { view: captureView({ key: 'id', complete: true }) } });
        await h.event(frame(op('delete', [1]))); await h.advance(100);
        assert.equal(h.requests.length, 2);
    }
    for (const supplied of [undefined, [], [{ id: 2 }], [{ id: 1 }, { id: 1 }], [{ id: 1 }]]) {
        const h = await baseline([{ id: 1 }], { query: { view: captureView({ key: 'id', complete: true,
            insertRows: () => supplied }) } });
        await h.event(frame(op('insert', [1]))); await h.advance(100);
        assert.equal(h.requests.length, 2); assert.deepEqual(plain(h.controller.read().data), [{ id: 1 }]);
    }
    const mixed = await baseline([{ id: 1 }], { query: { view: captureView({ key: 'id', complete: true }) } });
    await mixed.event(frame(op('delete', [1]), op('insert', [2]))); await mixed.advance(100);
    assert.equal(mixed.requests.length, 2); assert.deepEqual(plain(mixed.controller.read().data), [{ id: 1 }]);
});
