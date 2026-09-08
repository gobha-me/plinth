import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const source = await readFile(new URL('../../client/shell/client/sdk.js', import.meta.url), 'utf8');
async function fixture() {
    const sockets = [];
    const timers = new Map();
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
        window: { location: { protocol: 'https:', host: 'plinth.test' } },
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
    return { sdk: module.namespace, sockets, timers,
        runTimer() {
            assert.equal(timers.size, 1);
            const [id, { fn }] = [...timers][0];
            timers.delete(id);
            fn();
        },
    };
}

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
