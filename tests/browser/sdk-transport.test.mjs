import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

const client = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const sources = new Map(await Promise.all(['sdk.js', 'data-query.js', 'data-controller.js'].map(async path =>
    [resolve(client, path), await readFile(resolve(client, path), 'utf8')])));
async function fixture(options = {}) {
    const sockets = [];
    const timers = new Map();
    const document = { cookie: options.cookie || '' };
    let nextTimer = 0;
    class Socket {
        static OPEN = 1;
        constructor(url) {
            this.url = url;
            this.readyState = 0;
            this.listeners = new Map();
            this.frames = [];
            sockets.push(this);
        }
        addEventListener(type, listener) {
            const list = this.listeners.get(type) || [];
            list.push(listener);
            this.listeners.set(type, list);
        }
        emit(type, value = {}) { for (const listener of this.listeners.get(type) || []) listener(value); }
        open() { this.readyState = 1; this.emit('open'); }
        receive(value) { this.emit('message', { data: JSON.stringify(value) }); }
        send(value) { assert.equal(this.readyState, 1); this.frames.push(JSON.parse(value)); }
        close(code = 1000) {
            this.readyState = this.deferClose ? 2 : 3;
            if (!this.deferClose) this.emit('close', { code, reason: '' });
        }
        finishClose() { this.readyState = 3; this.emit('close', { code: 1000, reason: '' }); }
    }
    const context = vm.createContext({
        window: { location: {
            protocol: 'https:',
            host: 'plinth.test',
            origin: 'https://plinth.test',
            href: 'https://plinth.test/app/',
        } },
        document, URL, Headers, AbortController,
        fetch: options.fetch || (() => { throw new Error('unexpected fetch'); }),
        WebSocket: Socket, console: { error() {} }, queueMicrotask,
        setTimeout(fn, delay) { const id = ++nextTimer; timers.set(id, { fn, delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
    });
    // Evaluate unchanged SDK source with real ES-module import linkage. These
    // tests isolate transport ownership; actual Preact hooks run in Chromium.
    const preact = new vm.SyntheticModule(['h'], function () {
        this.setExport('h', () => { throw new Error('unexpected Preact rendering in transport fixture'); });
    }, { context, identifier: 'platform:preact' });
    const hooks = new vm.SyntheticModule(['useEffect', 'useState', 'useRef'], function () {
        for (const name of ['useEffect', 'useState', 'useRef']) {
            this.setExport(name, () => { throw new Error(`unexpected ${name} in transport fixture`); });
        }
    }, { context, identifier: 'platform:preact/hooks' });
    const modules = new Map();
    function load(path) {
        assert(sources.has(path), `unexpected private module ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(sources.get(path),
            { context, identifier: path }));
        return modules.get(path);
    }
    const module = load(resolve(client, 'sdk.js'));
    await module.link((specifier, importer) => {
        if (specifier === 'preact') return preact;
        if (specifier === 'preact/hooks') return hooks;
        assert(specifier.startsWith('.'), `unexpected external module ${specifier}`);
        return load(resolve(dirname(importer.identifier), specifier));
    });
    await module.evaluate();
    assert.equal(modules.size, 3, 'must link the actual SDK/query/controller modules');
    return { sdk: module.namespace, sockets, timers, document,
        runTimer() {
            assert.equal(timers.size, 1);
            const [id, { fn }] = [...timers][0];
            timers.delete(id);
            fn();
        },
    };
}

test('unsafe same-origin requests read the current CSRF cookie without forwarding it cross-origin', async () => {
    const { sdk, document } = await fixture({ cookie: 'other=x; plinth_csrf=first%2Dtoken' });
    const first = sdk.withCsrf('/api/auth/logout', { method: 'post', headers: { Accept: 'x' } });
    assert.equal(first.headers.get('X-Plinth-CSRF'), 'first-token');
    assert.equal(first.headers.get('Accept'), 'x');

    document.cookie = 'plinth_csrf=rotated-token';
    const rotated = sdk.withCsrf('/api/auth/logout', { method: 'POST' });
    assert.equal(rotated.headers.get('X-Plinth-CSRF'), 'rotated-token');
    assert.equal(sdk.withCsrf('/api/auth/session').headers, undefined);
    const crossOrigin = sdk.withCsrf('https://attacker.test/api', {
        method: 'POST', headers: { 'X-Plinth-CSRF': 'must-not-leak', Accept: 'x' },
    });
    assert.equal(crossOrigin.headers.get('X-Plinth-CSRF'), null);
    assert.equal(crossOrigin.headers.get('Accept'), 'x');

    document.cookie = 'plinth_csrf=%not-valid';
    assert.equal(sdk.withCsrf('/api/auth/logout', { method: 'DELETE' }).headers, undefined);
    document.cookie = '';
    const noCookie = sdk.withCsrf('/api/auth/login', {
        method: 'POST', headers: { 'X-Plinth-CSRF': 'stale' },
    });
    assert.equal(noCookie.headers.get('X-Plinth-CSRF'), null);
});

test('capability calls attach the fresh CSRF token after session rotation', async () => {
    const requests = [];
    const { sdk, document } = await fixture({
        cookie: 'plinth_csrf=before-login',
        fetch: async (url, options) => {
            requests.push({ url, options });
            return { ok: true, json: async () => ({ ok: true, value: requests.length }) };
        },
    });
    assert.equal(await sdk.call('shell.preferences.set', { key: 'x' }), 1);
    document.cookie = 'plinth_csrf=after-relogin';
    assert.equal(await sdk.call('shell.preferences.set', { key: 'y' }), 2);
    assert.equal(requests[0].options.headers.get('X-Plinth-CSRF'), 'before-login');
    assert.equal(requests[1].options.headers.get('X-Plinth-CSRF'), 'after-relogin');
    assert.equal(requests[1].options.headers.get('Content-Type'), 'application/json');
});

test('capability calls normalize pre-dispatch and capability error envelopes', async () => {
    const responses = [
        { error: 'csrf_failed', message: 'Request validation failed' },
        { ok: false, error: { code: 'rbac_denied', message: 'Denied' } },
    ];
    const { sdk } = await fixture({
        cookie: 'plinth_csrf=token',
        fetch: async () => ({
            ok: false,
            statusText: 'Forbidden',
            json: async () => responses.shift(),
        }),
    });
    await assert.rejects(sdk.call('shell.preferences.set', {}), error =>
        error.name === 'CapabilityError' && error.code === 'csrf_failed' &&
        error.message === 'Request validation failed');
    await assert.rejects(sdk.call('shell.preferences.set', {}), error =>
        error.name === 'CapabilityError' && error.code === 'rbac_denied' &&
        error.message === 'Denied');
});

test('wait for authentication, multiplex once, acknowledge grants, unsubscribe arrays', async () => {
    const { sdk, sockets } = await fixture();
    const events = [];
    const a = sdk.subscribe('a', value => events.push(['first', value.payload]));
    const b = sdk.subscribe('a', value => events.push(['second', value.payload]));
    const keep = sdk.subscribe('keep', () => {});
    sdk.subscribe('removed-before-auth', () => assert.fail('removed handler ran'))();
    assert.equal(sockets.length, 1);
    const socket = sockets[0];
    assert.equal(socket.url, 'wss://plinth.test/ws/events');
    socket.open();
    assert.deepEqual(socket.frames, []);
    socket.receive({ type: 'connected' });
    assert.deepEqual(socket.frames, [{ type: 'subscribe', channels: ['a', 'keep'] }]);
    socket.receive({ type: 'subscribed', channels: ['a', 'keep'] });
    socket.receive({ type: 'ping', timestamp: 1234 });
    assert.deepEqual(socket.frames.at(-1), { type: 'pong', timestamp: 1234 });
    socket.receive({ type: 'event', channel: 'a', payload: 42 });
    assert.deepEqual(events, [['first', 42], ['second', 42]]);
    a();
    assert.equal(socket.frames.length, 2);
    b();
    assert.deepEqual(socket.frames.at(-1), { type: 'unsubscribe', channels: ['a'] });
    socket.receive({ type: 'unsubscribed', channels: ['a'] });
    socket.receive({ type: 'event', channel: 'a', payload: 99 });
    assert.equal(events.length, 2);
    keep();
    assert.equal(socket.readyState, 3);
    assert.equal(sdk.getRealtimeState().status, 'idle');
});

test('same-channel handler A failure is isolated while B and C receive one exact envelope', async () => {
    const { sdk, sockets, timers } = await fixture();
    let attemptsA = 0;
    const receivedB = [], receivedC = [];
    const removeA = sdk.subscribe('isolated', () => {
        attemptsA++;
        throw new Error('fake subscriber A failure');
    });
    const removeB = sdk.subscribe('isolated', envelope => receivedB.push(envelope));
    const removeC = sdk.subscribe('isolated', envelope => receivedC.push(envelope));
    try {
        assert.equal(sockets.length, 1, 'three listeners must share one physical socket');
        const socket = sockets[0];
        socket.open(); socket.receive({ type: 'connected' });
        assert.deepEqual(socket.frames, [{ type: 'subscribe', channels: ['isolated'] }]);
        socket.receive({ type: 'subscribed', channels: ['isolated'] });
        const envelope = { type: 'event', channel: 'isolated', payload: {
            seq: 32, marker: 'fake event', nested: ['unchanged', { value: true }],
        } };
        assert.doesNotThrow(() => socket.receive(envelope), 'A failure must not escape dispatch');
        assert.equal(attemptsA, 1);
        assert.equal(receivedB.length, 1); assert.equal(receivedC.length, 1);
        assert.deepEqual(JSON.parse(JSON.stringify(receivedB)), [envelope]);
        assert.deepEqual(JSON.parse(JSON.stringify(receivedC)), [envelope]);
        assert.equal(receivedB[0], receivedC[0], 'both listeners receive the same outer envelope');
        assert.equal(socket.frames.length, 1, 'handler failure must not resubscribe or open another socket');
        assert.equal(sockets.length, 1);
        removeA(); removeB();
        assert.equal(socket.readyState, 1, 'remaining C still owns its grant');
        assert.equal(socket.frames.length, 1);
        removeC();
        assert.equal(socket.readyState, 3);
        assert.equal(timers.size, 0);
        assert.equal(sdk.getRealtimeState().status, 'idle');
    } finally { removeA(); removeB(); removeC(); }
});

test('onReady fires once per acknowledged connection epoch and unsubscribe cancels pending delivery', async () => {
    const { sdk, sockets, runTimer } = await fixture();
    const ready = [];
    const remove = sdk.subscribe('a', () => {}, { onReady: () => ready.push('first') });
    const first = sockets[0];
    first.open();
    first.receive({ type: 'connected' });
    assert.deepEqual(ready, []);
    first.receive({ type: 'subscribed', channels: ['a'] });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, ['first']);

    const removeLate = sdk.subscribe('a', () => {}, { onReady: () => ready.push('late') });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, ['first', 'late']);

    const removeCancelled = sdk.subscribe('a', () => {}, {
        onReady: () => assert.fail('unsubscribed readiness callback ran'),
    });
    removeCancelled();
    await new Promise(resolve => queueMicrotask(resolve));

    first.close(1006);
    runTimer();
    const second = sockets[1];
    second.open();
    second.receive({ type: 'connected' });
    assert.deepEqual(ready, ['first', 'late']);
    second.receive({ type: 'subscribed', channels: ['a'] });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, ['first', 'late', 'first', 'late']);
    remove();
    removeLate();
});

test('onReady rejects a non-function option', async () => {
    const { sdk } = await fixture();
    assert.throws(() => sdk.subscribe('a', () => {}, { onReady: true }),
        error => error?.name === 'TypeError' && error.message === 'subscribe onReady must be a function');
});

test('denied and terminal subscriptions never report ready', async () => {
    const { sdk, sockets } = await fixture();
    const ready = [];
    const errors = [];
    const remove = sdk.subscribe('denied', () => {}, {
        onReady: () => ready.push('ready'),
        onError: error => errors.push(error.code),
    });
    sockets[0].open();
    sockets[0].receive({ type: 'connected' });
    sockets[0].receive({ type: 'subscribed', channels: [] });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, []);
    assert.deepEqual(errors, ['subscription_denied']);
    sdk.reconnectRealtime();
    sockets[1].open();
    sockets[1].receive({ type: 'error', error: 'auth_failed' });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, []);
    assert.deepEqual(errors, ['subscription_denied', 'auth_failed']);
    remove();
});

test('a terminal failure cancels readiness queued from the same connection epoch', async () => {
    const { sdk, sockets } = await fixture();
    const ready = [];
    const errors = [];
    const remove = sdk.subscribe('a', () => {}, {
        onReady: () => ready.push('ready'),
        onError: error => errors.push(error.code),
    });
    sockets[0].open();
    sockets[0].receive({ type: 'connected' });
    sockets[0].receive({ type: 'subscribed', channels: ['a'] });
    sockets[0].receive({ type: 'error', error: 'auth_failed' });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(ready, []);
    assert.deepEqual(errors, ['auth_failed']);
    remove();
});

test('serialize removal during subscription acknowledgement and surface denied grants', async () => {
    const { sdk, sockets } = await fixture();
    const denied = [];
    const remove = sdk.subscribe('a', () => assert.fail('removed handler ran'));
    const socket = sockets[0];
    socket.open();
    socket.receive({ type: 'connected' });
    const keep = sdk.subscribe('denied', () => {}, { onError: error => denied.push(error.code) });
    remove();
    assert.equal(socket.frames.length, 1);
    socket.receive({ type: 'subscribed', channels: ['a'] });
    assert.deepEqual(socket.frames.at(-1), { type: 'unsubscribe', channels: ['a'] });
    socket.receive({ type: 'unsubscribed', channels: ['a'] });
    assert.deepEqual(socket.frames.at(-1), { type: 'subscribe', channels: ['denied'] });
    socket.receive({ type: 'subscribed', channels: [] });
    assert.deepEqual(denied, ['subscription_denied']);
    assert.equal(socket.frames.length, 3);
    keep();
});

test('one reconnect timer handles error plus close; removed handlers stay removed', async () => {
    const { sdk, sockets, timers, runTimer } = await fixture();
    const remove = sdk.subscribe('gone', () => {});
    const keep = sdk.subscribe('kept', () => {});
    const first = sockets[0];
    first.open();
    first.receive({ type: 'connected' });
    first.receive({ type: 'subscribed', channels: ['gone', 'kept'] });
    first.emit('error');
    assert.equal(timers.size, 0);
    first.close(1006);
    first.emit('error');
    first.close(1006);
    assert.equal(timers.size, 1);
    assert.equal(sockets.length, 1);
    remove();
    runTimer();
    const second = sockets[1];
    second.open();
    second.receive({ type: 'connected' });
    assert.deepEqual(second.frames, [{ type: 'subscribe', channels: ['kept'] }]);
    first.receive({ type: 'connected' });
    assert.equal(sockets.length, 2);
    second.close(1006);
    assert.equal(timers.size, 1);
    keep();
    assert.equal(timers.size, 0);
});

test('auth failure and displacement are terminal until explicit reconnect', async () => {
    const { sdk, sockets, timers } = await fixture();
    const errors = [];
    const remove = sdk.subscribe('a', () => {}, { onError: error => errors.push(error.code) });
    sockets[0].open();
    sockets[0].receive({ type: 'error', error: 'auth_failed', message: 'Authentication failed' });
    assert.equal(sdk.getRealtimeState().status, 'failed');
    assert.deepEqual(errors, ['auth_failed']);
    assert.equal(timers.size, 0);
    const removeSecond = sdk.subscribe('b', () => {}, { onError: error => errors.push(error.code) });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(errors, ['auth_failed', 'auth_failed']);
    assert.equal(sockets.length, 1);
    sdk.reconnectRealtime();
    assert.equal(sockets.length, 2);
    sockets[1].open();
    sockets[1].close(4003);
    assert.equal(sdk.getRealtimeState().error.code, 'already_connected');
    assert.equal(timers.size, 0);
    remove();
    removeSecond();
});


test('explicit reconnect and resubscribe wait for previous socket to close', async () => {
    const { sdk, sockets } = await fixture();
    const remove = sdk.subscribe('a', () => {});
    const first = sockets[0];
    first.open();
    first.receive({ type: 'connected' });
    first.receive({ type: 'subscribed', channels: ['a'] });
    first.deferClose = true;
    sdk.reconnectRealtime();
    sdk.reconnectRealtime();
    assert.equal(sockets.length, 1);
    first.receive({ type: 'connected' });
    assert.equal(first.frames.length, 1);
    first.finishClose();
    assert.equal(sockets.length, 2);
    const second = sockets[1];
    second.deferClose = true;
    remove();
    const keep = sdk.subscribe('new', () => {});
    assert.equal(sockets.length, 2);
    second.finishClose();
    assert.equal(sockets.length, 3);
    sockets[2].open();
    sockets[2].receive({ type: 'connected' });
    assert.deepEqual(sockets[2].frames, [{ type: 'subscribe', channels: ['new'] }]);
    keep();
});

for (const [name, frame, expected] of [
    ['missing defaults', {}, { debounceMs: 100, jitterMs: 50 }],
    ['explicit zeros', { recommended_debounce_ms: 0, recommended_jitter_ms: 0 },
        { debounceMs: 0, jitterMs: 0 }],
    ['inclusive upper bounds', { recommended_debounce_ms: 60000, recommended_jitter_ms: 5000 },
        { debounceMs: 60000, jitterMs: 5000 }],
    ['invalid debounce alone', { recommended_debounce_ms: -1, recommended_jitter_ms: 0 },
        { debounceMs: 100, jitterMs: 0 }],
    ['invalid jitter alone', { recommended_debounce_ms: 0, recommended_jitter_ms: 5001 },
        { debounceMs: 0, jitterMs: 50 }],
    ['fractional advice', { recommended_debounce_ms: 0.5, recommended_jitter_ms: 1.5 },
        { debounceMs: 100, jitterMs: 50 }],
    ['string and boolean advice', { recommended_debounce_ms: '0', recommended_jitter_ms: false },
        { debounceMs: 100, jitterMs: 50 }],
    ['above debounce bound alone', { recommended_debounce_ms: 60001, recommended_jitter_ms: 5000 },
        { debounceMs: 100, jitterMs: 5000 }],
]) {
    test(`actual subscribed advice validates ${name} independently`, async () => {
        const { sdk, sockets } = await fixture();
        const ready = [];
        const remove = sdk.subscribe('a', () => {}, { onReady: advice => ready.push(advice) });
        sockets[0].open(); sockets[0].receive({ type: 'connected' });
        assert.deepEqual(ready, []);
        sockets[0].receive({ type: 'subscribed', channels: ['a'], ...frame });
        await new Promise(resolve => queueMicrotask(resolve));
        assert.equal(ready.length, 1);
        assert.deepEqual(JSON.parse(JSON.stringify(ready[0])), expected);
        assert.equal(Object.isFrozen(ready[0]), true);
        remove();
    });
}

test('one consumer removal retains actual channel advice for a later consumer', async () => {
    const { sdk, sockets } = await fixture();
    const ready = [];
    const a = sdk.subscribe('a', () => {});
    const b = sdk.subscribe('a', () => {});
    sockets[0].open(); sockets[0].receive({ type: 'connected' });
    sockets[0].receive({ type: 'subscribed', channels: ['a'],
        recommended_debounce_ms: 17, recommended_jitter_ms: 3 });
    a();
    const c = sdk.subscribe('a', () => {}, { onReady: advice => ready.push(advice) });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(JSON.parse(JSON.stringify(ready)), [{ debounceMs: 17, jitterMs: 3 }]);
    assert.equal(sockets[0].frames.length, 1);
    b(); c();
});

test('actual channel unsubscribe retires advice before same-socket regrant', async () => {
    const { sdk, sockets } = await fixture();
    const a = sdk.subscribe('a', () => {});
    const keep = sdk.subscribe('keep', () => {});
    const socket = sockets[0]; socket.open(); socket.receive({ type: 'connected' });
    socket.receive({ type: 'subscribed', channels: ['a', 'keep'],
        recommended_debounce_ms: 17, recommended_jitter_ms: 3 });
    a(); socket.receive({ type: 'unsubscribed', channels: ['a'] });
    const ready = [];
    const fresh = sdk.subscribe('a', () => {}, { onReady: advice => ready.push(advice) });
    await new Promise(resolve => queueMicrotask(resolve)); assert.deepEqual(ready, []);
    socket.receive({ type: 'subscribed', channels: ['a'] });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(JSON.parse(JSON.stringify(ready)), [{ debounceMs: 100, jitterMs: 50 }]);
    fresh(); keep();
});

test('stale ACK cannot seed replacement physical owner advice', async () => {
    const { sdk, sockets } = await fixture();
    const ready = [];
    const remove = sdk.subscribe('a', () => {}, { onReady: advice => ready.push(advice) });
    const first = sockets[0]; first.open(); first.receive({ type: 'connected' });
    sdk.reconnectRealtime();
    first.receive({ type: 'subscribed', channels: ['a'],
        recommended_debounce_ms: 60000, recommended_jitter_ms: 5000 });
    const next = sockets[1]; next.open(); next.receive({ type: 'connected' });
    next.receive({ type: 'subscribed', channels: ['a'],
        recommended_debounce_ms: 0, recommended_jitter_ms: 0 });
    await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(JSON.parse(JSON.stringify(ready)), [{ debounceMs: 0, jitterMs: 0 }]);
    remove();
});

test('managed retirement drops pending A ready and stale unsubscribe cannot erase B', async () => {
    const { sdk, sockets, timers } = await fixture();
    const ready = [];
    const events = [];
    const errors = [];
    const removeA = sdk.subscribe('same', event => events.push(['A', event.payload]), {
        onReady: advice => ready.push(['A', advice]), onError: error => errors.push(error.code),
    });
    const first = sockets[0]; first.open(); first.receive({ type: 'connected' });
    first.receive({ type: 'subscribed', channels: ['same'],
        recommended_debounce_ms: 17, recommended_jitter_ms: 3 });
    first.deferClose = true;
    sdk.retireRealtimeSession();
    sdk.reconnectRealtime();
    assert.equal(sockets.length, 1); assert.equal(timers.size, 0);
    sdk.activateRealtimeSession();
    const removeB = sdk.subscribe('same', event => events.push(['B', event.payload]), {
        onReady: advice => ready.push(['B', advice]),
    });
    removeA();
    first.receive({ type: 'subscribed', channels: ['same'],
        recommended_debounce_ms: 60000, recommended_jitter_ms: 5000 });
    first.receive({ type: 'event', channel: 'same', payload: 'old' });
    first.finishClose();
    const second = sockets[1]; second.open(); second.receive({ type: 'connected' });
    assert.deepEqual(second.frames, [{ type: 'subscribe', channels: ['same'] }]);
    second.receive({ type: 'subscribed', channels: ['same'],
        recommended_debounce_ms: 0, recommended_jitter_ms: 0 });
    await new Promise(resolve => queueMicrotask(resolve));
    second.receive({ type: 'event', channel: 'same', payload: 'new' });
    assert.deepEqual(events, [['B', 'new']]); assert.deepEqual(errors, []);
    assert.deepEqual(JSON.parse(JSON.stringify(ready)), [['B', { debounceMs: 0, jitterMs: 0 }]]);
    removeB();
});

test('retired session denies new subscriptions and explicit reconnect until activation', async () => {
    const { sdk, sockets, timers } = await fixture();
    sdk.retireRealtimeSession('session_ended');
    const errors = [];
    const remove = sdk.subscribe('a', () => assert.fail('retired event'), {
        onReady: () => assert.fail('retired readiness'), onError: error => errors.push(error.code),
    });
    sdk.reconnectRealtime(); await new Promise(resolve => queueMicrotask(resolve));
    assert.deepEqual(errors, ['session_ended']); assert.equal(sockets.length, 0); assert.equal(timers.size, 0);
    sdk.activateRealtimeSession();
    const fresh = sdk.subscribe('a', () => {}); remove();
    assert.equal(sockets.length, 1); fresh();
});

for (const code of ['auth_failed', 'auth_timeout', 'already_connected']) {
    test(`${code} keeps same-session subscribers for explicit current-grant recovery`, async () => {
        const { sdk, sockets, timers } = await fixture();
        const ready = [];
        const errors = [];
        const remove = sdk.subscribe('a', () => {}, {
            onReady: advice => ready.push(advice), onError: error => errors.push(error.code),
        });
        sockets[0].open(); sockets[0].receive({ type: 'error', error: code });
        assert.deepEqual(errors, [code]); assert.equal(timers.size, 0);
        sdk.reconnectRealtime();
        sockets[1].open(); sockets[1].receive({ type: 'connected' });
        sockets[1].receive({ type: 'subscribed', channels: ['a'],
            recommended_debounce_ms: 0, recommended_jitter_ms: 0 });
        await new Promise(resolve => queueMicrotask(resolve));
        assert.deepEqual(JSON.parse(JSON.stringify(ready)), [{ debounceMs: 0, jitterMs: 0 }]);
        remove();
    });
}

for (const code of ['not_authenticated', 'session_expired', 'session_revoked']) {
    test(`${code} permanently retires raw subscribers until a fresh session`, async () => {
        const { sdk, sockets, timers } = await fixture();
        const errors = [];
        const events = [];
        const remove = sdk.subscribe('a', value => events.push(value), { onError: error => errors.push(error.code) });
        sockets[0].open(); sockets[0].receive({ type: 'error', error: code });
        sdk.reconnectRealtime(); assert.equal(sockets.length, 1); assert.equal(timers.size, 0);
        assert.deepEqual(errors, [code]);
        sdk.activateRealtimeSession();
        const next = sdk.subscribe('a', value => events.push(value)); remove();
        sockets[1].open(); sockets[1].receive({ type: 'connected' });
        sockets[1].receive({ type: 'subscribed', channels: ['a'] });
        sockets[1].receive({ type: 'event', channel: 'a', payload: 'new' });
        assert.equal(events.length, 1); next();
    });
}

test('call serialization remains one immutable JSON value with undefined becoming null', async () => {
    const bodies = [];
    const { sdk } = await fixture({ fetch: async (_url, options) => {
        bodies.push(options.body); return { ok: true, json: async () => ({ ok: true, value: 1 }) };
    } });
    let serialized = 0;
    const args = { toJSON() { serialized++; return { key: 'captured' }; } };
    await sdk.call('test.capture', args); await sdk.call('test.empty');
    assert.equal(serialized, 1);
    assert.deepEqual(bodies.map(body => JSON.parse(body)), [{ args: { key: 'captured' } }, { args: null }]);
});

test('serialization and URI preparation failures keep typed NetworkError with zero dispatch', async () => {
    let requests = 0;
    const { sdk } = await fixture({ fetch: () => { requests++; throw new Error('unexpected dispatch'); } });
    const cycle = {}; cycle.self = cycle;
    for (const args of [cycle, 1n]) {
        await assert.rejects(sdk.call('test.serialize', args), error =>
            error.name === 'NetworkError' && error.message === 'fetch failed for test.serialize' &&
            error.cause?.name === 'TypeError');
    }
    await assert.rejects(sdk.call('\uD800', {}), error => error.name === 'NetworkError' && error.cause?.name === 'URIError');
    assert.equal(requests, 0);
});

test('fetch and response JSON failures retain independent typed NetworkError boundaries', async () => {
    const { sdk } = await fixture({ fetch: async url => {
        if (url.endsWith('/test.fetch')) throw new Error('fake network down');
        return { json: async () => { throw new Error('fake malformed JSON'); } };
    } });
    await assert.rejects(sdk.call('test.fetch', {}), error =>
        error.name === 'NetworkError' && error.message === 'fetch failed for test.fetch');
    await assert.rejects(sdk.call('test.json', {}), error =>
        error.name === 'NetworkError' && error.message === 'response is not JSON for test.json');
});
