import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const source = await readFile(new URL('../../client/shell/client/sdk.js', import.meta.url), 'utf8');
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
        document, URL, Headers,
        fetch: options.fetch || (() => { throw new Error('unexpected fetch'); }),
        WebSocket: Socket, console: { error() {} }, queueMicrotask,
        setTimeout(fn, delay) { const id = ++nextTimer; timers.set(id, { fn, delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
    });
    // Evaluate unchanged SDK source with real ES-module import linkage. These
    // tests isolate transport ownership; actual Preact hooks run in Chromium.
    const module = new vm.SourceTextModule(source, { context });
    await module.link(specifier => {
        const exports = specifier === 'preact' ? ['h'] : ['useEffect', 'useState'];
        return new vm.SyntheticModule(exports, function () {
            for (const name of exports) this.setExport(name, () => {});
        }, { context });
    });
    await module.evaluate();
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
