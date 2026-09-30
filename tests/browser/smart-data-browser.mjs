// Actual vendored Preact hooks and shipping SDK/private modules. All capability
// responses, authentication ownership and WebSocket frames are local mocks.
// This is not native-server, deployed-backend, sequence or replay evidence.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const client = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const sources = ['sdk.js', 'data-query.js', 'data-controller.js',
    'vendor/preact.module.js', 'vendor/preact-hooks.module.js'];
const channel = 'plinth:system:preferences.changed';
const alternateChannel = 'plinth:system:applications.changed';
const seed = owner => [{ id: `seed-${owner}` }];
const rows = owner => [{ id: owner }];
const sourceIdentity = async () => Object.fromEntries(await Promise.all(sources.map(async path =>
    [path, createHash('sha256').update(await readFile(resolve(client, path))).digest('hex')])));

async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline ${milliseconds}ms`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}

const html = `<!doctype html><meta charset="utf-8"><div id="root"></div>
<script type="importmap">{"imports":{"preact":"/vendor/preact.module.js",
"preact/hooks":"/vendor/preact-hooks.module.js"}}</script>
<script type="module" src="/harness.js"></script>`;
const harness = `
import { h, render, options } from 'preact';
import { useData, subscribe, retireRealtimeSession, activateRealtimeSession } from '/sdk.js';
const root = document.getElementById('root');
const original = { fetch: window.fetch, WebSocket: window.WebSocket,
    raf: options.requestAnimationFrame, microtask: window.queueMicrotask };
const paints = [], requests = [], sockets = [], records = [], ready = [], deliveries = [];
let props, holdReady = false;
const readyTasks = [];
options.requestAnimationFrame = callback => { paints.push(callback); };
window.queueMicrotask = callback => holdReady ? readyTasks.push(callback) : original.microtask.call(window, callback);
class MockSocket extends EventTarget {
    static OPEN = 1;
    constructor(url) { super(); this.url = url; this.readyState = 0; this.sent = []; sockets.push(this); }
    send(value) { if (this.readyState !== 1) throw new Error('mock send before open'); this.sent.push(JSON.parse(value)); }
    close(code = 1000) { this.readyState = 3; this.dispatchEvent(new CloseEvent('close', { code })); }
    frame(value) { this.dispatchEvent(new MessageEvent('message', { data: JSON.stringify(value) })); }
}
window.WebSocket = MockSocket;
window.fetch = (url, settings = {}) => {
    if (!String(url).startsWith('/api/cap/')) throw new Error('unexpected mock capability URL');
    let resolveRequest, rejectRequest;
    const promise = new Promise((resolveValue, rejectValue) => {
        resolveRequest = resolveValue; rejectRequest = rejectValue;
    });
    // Deliberately ignore settings.signal: ownership must also reject replies
    // from a transport which does not cooperate with AbortController.
    requests.push({ url, body: JSON.parse(settings.body), signal: settings.signal,
        resolve: resolveRequest, reject: rejectRequest });
    return promise;
};
activateRealtimeSession(); // Explicit local-mock sign-in, not scope-based authentication.
const plain = value => value === undefined ? null : JSON.parse(JSON.stringify(value));
function Probe(current) {
    const opts = { initialData: current.initialData, scope: current.scope, view: current.view };
    if (!current.raw) opts.snapshot = { capability: 'mock.snapshot', args: current.args };
    const state = useData(current.channel, opts);
    records.push({ label: current.label, data: plain(state.data), loading: state.loading,
        error: state.error ? { name: state.error.name, code: state.error.code || null,
            message: state.error.message } : null });
    return h('pre', { id: 'state' }, JSON.stringify(records.at(-1)));
}
async function settle() { for (let index = 0; index < 20; index++) await Promise.resolve(); }
const raw = new Map();
window.__smart = {
    records, requests, sockets, ready, deliveries,
    mount(current) { props = current; render(h(Probe, props), root); return plain(records.at(-1)); },
    async settle() { await settle(); return plain(records.at(-1)); },
    async flushEffects() {
        for (let index = 0; paints.length && index < 20; index++) { paints.shift()(); await settle(); }
        if (paints.length) throw new Error('effect scheduler did not quiesce');
        await settle();
    },
    async resolve(index, value, status = 200) {
        const body = status === 200 ? { ok: true, value }
            : { ok: false, error: { code: 'mock_failure', message: 'fake snapshot failure' } };
        requests[index].resolve(new Response(JSON.stringify(body), { status,
            headers: { 'Content-Type': 'application/json' } }));
        await settle();
    },
    async reject(index) { requests[index].reject(new Error('fake transport failure')); await settle(); },
    summary() { return { requests: requests.map(request => ({ args: plain(request.body.args),
        aborted: request.signal?.aborted || false })), records: plain(records),
        ready: plain(ready), deliveries: plain(deliveries), paints: paints.length, pendingReady: readyTasks.length }; },
    grant(index, channels) {
        const socket = sockets[index];
        if (socket.readyState === 0) socket.readyState = 1;
        socket.frame({ type: 'connected' });
        socket.frame({ type: 'subscribed', channels,
            recommended_debounce_ms: 0, recommended_jitter_ms: 0 });
    },
    event(index, currentChannel, marker = 'mock') {
        sockets[index].frame({ type: 'event', channel: currentChannel, payload: {
            marker, ops: [{ op: 'update', count: 1 }], truncated: false } });
    },
    customDelete(index, currentChannel, id) {
        sockets[index].frame({ type: 'event', channel: currentChannel, payload: {
            channel: currentChannel, ops: [{ op: 'delete', count: 1, ids: [id] }], truncated: false } });
    },
    raw(name, currentChannel) {
        raw.set(name, subscribe(currentChannel, value => deliveries.push([name, plain(value)]), {
            onReady: advice => ready.push([name, plain(advice)]), onError() {} }));
    },
    unsubscribe(name) { raw.get(name)?.(); },
    holdReady(value) { holdReady = value; },
    flushReady() { holdReady = false; while (readyTasks.length) readyTasks.shift()(); },
    retire() { retireRealtimeSession('session_ended'); },
    activate() { activateRealtimeSession(); },
    unmount() { render(null, root); },
    cleanup() {
        render(null, root);
        for (const unsubscribe of raw.values()) unsubscribe();
        retireRealtimeSession('test_cleanup');
        paints.length = 0; readyTasks.length = 0;
        window.fetch = original.fetch; window.WebSocket = original.WebSocket;
        window.queueMicrotask = original.microtask;
        options.requestAnimationFrame = original.raf;
        return { rootEmpty: root.childNodes.length === 0,
            socketsClosed: sockets.every(socket => socket.readyState === 3),
            restored: window.fetch === original.fetch && window.WebSocket === original.WebSocket &&
                window.queueMicrotask === original.microtask && options.requestAnimationFrame === original.raf };
    },
};
`;

let browser;
async function scenario(name, check) {
    let server, context, page, failure;
    const connections = new Set(), cleanupErrors = [], fixtureErrors = [], pageErrors = [];
    try {
        server = createServer(async (request, response) => {
            try {
                const path = new URL(request.url, 'http://localhost').pathname;
                if (path === '/favicon.ico') return response.writeHead(204).end();
                if (path === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
                if (path === '/harness.js') return response.writeHead(200,
                    { 'Content-Type': 'application/javascript' }).end(harness);
                const file = resolve(client, `.${path}`);
                assert(file.startsWith(client + sep));
                response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(await readFile(file));
            } catch (error) {
                fixtureErrors.push(error.message);
                if (!response.headersSent) response.writeHead(500);
                response.end();
            }
        });
        server.on('connection', socket => {
            connections.add(socket); socket.on('close', () => connections.delete(socket));
        });
        await bounded(() => new Promise((done, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', done);
        }), `${name} server listen`);
        context = await bounded(() => browser.newContext(), `${name} context`);
        page = await context.newPage(); page.setDefaultTimeout(5000);
        page.on('pageerror', error => pageErrors.push(error.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        await page.waitForFunction(() => Boolean(window.__smart));
        await check(page);
        assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []);
    } catch (error) { failure = error; }
    finally {
        try {
            if (page && !page.isClosed()) assert.deepEqual(await bounded(
                () => page.evaluate(() => window.__smart?.cleanup()), `${name} fixture cleanup`),
            { rootEmpty: true, socketsClosed: true, restored: true });
        } catch (error) { cleanupErrors.push(error); }
        try { await bounded(() => context?.close(), `${name} context cleanup`); }
        catch (error) { cleanupErrors.push(error); }
        try {
            await bounded(() => new Promise((done, reject) => {
                if (!server) return done();
                server.close(error => error && error.code !== 'ERR_SERVER_NOT_RUNNING' ? reject(error) : done());
                server.closeAllConnections();
                for (const connection of connections) connection.destroy();
            }), `${name} server cleanup`);
            assert.equal(server?.listening || false, false);
            assert([...connections].every(connection => connection.destroyed));
        } catch (error) { cleanupErrors.push(error); }
        if (page && !page.isClosed()) cleanupErrors.push(new Error(`${name}: page remained open`));
    }
    if (failure || cleanupErrors.length) throw new AggregateError(
        [...(failure ? [failure] : []), ...cleanupErrors], `${name} failed`);
    return name;
}
const mount = (page, owner, extra = {}) => page.evaluate(current => window.__smart.mount(current), {
    label: owner, channel, scope: owner, args: { owner }, initialData: seed(owner), ...extra,
});
const summary = page => page.evaluate(() => window.__smart.summary());
const effects = page => page.evaluate(() => window.__smart.flushEffects());
const settle = page => page.evaluate(() => window.__smart.settle());
const resolveReply = (page, index, value, status = 200) => page.evaluate(
    ({ index, value, status }) => window.__smart.resolve(index, value, status), { index, value, status });
const expectState = (actual, data, loading, errorCode = null) => {
    assert.deepEqual(actual.data, data); assert.equal(actual.loading, loading);
    if (errorCode === null) assert.equal(actual.error, null);
    else { assert.equal(actual.error?.name, 'CapabilityError'); assert.equal(actual.error?.code, errorCode); }
};
const grant = (page, index = 0, channels = [channel]) => page.evaluate(
    ({ index, channels }) => window.__smart.grant(index, channels), { index, channels });
const event = (page, index = 0, currentChannel = channel, marker = 'mock') => page.evaluate(
    ({ index, currentChannel, marker }) => window.__smart.event(index, currentChannel, marker),
    { index, currentChannel, marker });
const requestCount = (page, count) => page.waitForFunction(expected =>
    window.__smart.requests.length === expected, count);

const cases = [
    ['H32.01 exact current loading/data/stale-error/recovery', async page => {
        expectState(await mount(page, 'A'), seed('A'), true);
        assert.equal((await summary(page)).requests.length, 0, 'initial query is effect-owned');
        await effects(page); await requestCount(page, 1);
        await resolveReply(page, 0, rows('A')); expectState(await settle(page), rows('A'), false);
        await grant(page); await event(page); await requestCount(page, 2);
        await resolveReply(page, 1, null, 503);
        expectState(await settle(page), rows('A'), false, 'mock_failure');
        await event(page); await requestCount(page, 3);
        await resolveReply(page, 2, rows('A2')); expectState(await settle(page), rows('A2'), false);
        const records = (await summary(page)).records;
        assert(records.some(record => record.error?.code === 'mock_failure'));
        const initialDone = records.findIndex(record => record.data?.[0]?.id === 'A' && !record.loading);
        assert(initialDone >= 0);
        assert(records.slice(initialDone).every(record => !record.loading), 'background refresh must not flash initial loading');
    }],
    ['H32.02 replacement first render and pre-cleanup late reply', async page => {
        await mount(page, 'A'); await effects(page); await resolveReply(page, 0, rows('A'));
        await grant(page); await event(page); await requestCount(page, 2);
        await resolveReply(page, 1, null, 503); expectState(await settle(page), rows('A'), false, 'mock_failure');
        await event(page); await requestCount(page, 3);
        const before = (await summary(page)).records.length;
        expectState(await mount(page, 'B', { channel: alternateChannel }), seed('B'), true);
        assert((await summary(page)).paints > 0, 'real after-paint effect flush must be held');
        await resolveReply(page, 2, rows('late-A'));
        const pending = await summary(page);
        assert.equal(pending.requests.length, 3, 'B has rendered but its effect has not started');
        for (const record of pending.records.slice(before)) expectState(record, seed('B'), true);
        await effects(page); await requestCount(page, 4);
        assert.equal((await summary(page)).requests[2].aborted, true);
        await resolveReply(page, 3, rows('B')); expectState(await settle(page), rows('B'), false);
    }],
    ['H32.03 equivalent inline args and immutable prepared requeries', async page => {
        await page.evaluate(({ channel, initialData }) => {
            window.__originalArgs = { owner: 'C', nested: { key: 'captured' } };
            window.__smart.mount({ label: 'C', channel, scope: 'C', args: window.__originalArgs, initialData });
            window.__originalArgs.owner = 'mutated'; window.__originalArgs.nested.key = 'mutated';
        }, { channel, initialData: seed('C') });
        await effects(page); await requestCount(page, 1);
        assert.deepEqual((await summary(page)).requests[0].args, { owner: 'C', nested: { key: 'captured' } });
        await resolveReply(page, 0, rows('C'));
        await mount(page, 'C', { args: { owner: 'C', nested: { key: 'captured' } } });
        await effects(page); assert.equal((await summary(page)).requests.length, 1, 'equivalent inline args preserve owner');
        await grant(page); await event(page); await requestCount(page, 2);
        assert.deepEqual((await summary(page)).requests[1].args, { owner: 'C', nested: { key: 'captured' } });
        await resolveReply(page, 1, rows('C2')); expectState(await settle(page), rows('C2'), false);
        expectState(await mount(page, 'args-only', { scope: 'C', args: { owner: 'D' } }), seed('args-only'), true);
        await effects(page); await requestCount(page, 3);
        await resolveReply(page, 2, rows('D')); expectState(await settle(page), rows('D'), false);
        expectState(await mount(page, 'scope-only', { scope: 'D', args: { owner: 'D' } }), seed('scope-only'), true);
        await effects(page); await requestCount(page, 4);
        await resolveReply(page, 3, rows('scope-D')); expectState(await settle(page), rows('scope-D'), false);
        const invalid = await page.evaluate(currentChannel => {
            const circular = {}; circular.self = circular;
            return window.__smart.mount({ label: 'invalid', channel: currentChannel, scope: 'invalid',
                args: circular, initialData: [{ id: 'invalid-seed' }] });
        }, channel);
        assert.deepEqual(invalid.data, [{ id: 'invalid-seed' }]); assert.equal(invalid.loading, false);
        assert.equal(invalid.error?.name, 'NetworkError', 'serialization must be a typed query failure, not raw mode/render throw');
        await effects(page); assert.equal((await summary(page)).requests.length, 4);
    }],
    ['H32.04 raw envelope retained and unmount ignores late transport', async page => {
        expectState(await mount(page, 'raw', { raw: true }), seed('raw'), false);
        await effects(page); await grant(page); await event(page, 0, channel, 'raw-control');
        const rawState = await settle(page);
        assert.equal(rawState.data.type, 'event'); assert.equal(rawState.data.channel, channel);
        assert.equal(rawState.data.payload.marker, 'raw-control'); assert.equal(rawState.loading, false);
        assert.equal((await summary(page)).requests.length, 0, 'raw stream must not create snapshot queries');
        await mount(page, 'pending'); await effects(page); await requestCount(page, 1);
        await page.evaluate(() => window.__smart.unmount());
        const before = await summary(page); assert.equal(before.requests[0].aborted, true);
        await resolveReply(page, 0, rows('retired'));
        assert.deepEqual((await summary(page)).records, before.records, 'ignored abort completion must not render');
        assert.equal((await summary(page)).requests.length, 1);
    }],
    ['H32.05 session rotation retires old ready/result/unsubscribe owners', async page => {
        await mount(page, 'A'); await effects(page); await requestCount(page, 1);
        await page.evaluate(currentChannel => {
            window.__smart.raw('A', currentChannel); window.__smart.holdReady(true);
        }, alternateChannel);
        await grant(page, 0, [channel, alternateChannel]);
        assert.equal((await summary(page)).pendingReady, 2, 'actual A hook and raw readiness must both be pending');
        await page.evaluate(() => window.__smart.retire()); await settle(page);
        await mount(page, 'B'); await effects(page);
        assert.equal((await summary(page)).requests.length, 1, 'terminal session admits no new query');
        await page.evaluate(() => window.__smart.activate());
        const before = (await summary(page)).records.length;
        expectState(await mount(page, 'B'), seed('B'), true);
        await page.evaluate(({ currentChannel, smartChannel }) => {
            window.__smart.raw('B', currentChannel); window.__smart.unsubscribe('A');
            window.__smart.grant(0, [smartChannel, currentChannel]);
            window.__smart.event(0, currentChannel, 'old-A'); window.__smart.flushReady();
        }, { currentChannel: alternateChannel, smartChannel: channel });
        const socketIndex = await page.evaluate(() => window.__smart.sockets.length - 1);
        assert(socketIndex > 0); await grant(page, socketIndex, [channel, alternateChannel]);
        await event(page, socketIndex, alternateChannel, 'current-B'); await settle(page);
        let state = await summary(page);
        assert.equal(state.ready.filter(([name]) => name === 'A').length, 0);
        assert.deepEqual(state.ready.filter(([name]) => name === 'B'), [['B', { debounceMs: 0, jitterMs: 0 }]]);
        assert.deepEqual(state.deliveries.map(([name, frame]) => [name, frame.payload.marker]), [['B', 'current-B']]);
        await resolveReply(page, 0, rows('late-A'));
        state = await summary(page);
        for (const record of state.records.slice(before)) expectState(record, seed('B'), true);
        await effects(page); await requestCount(page, 2);
        assert.equal((await summary(page)).requests[0].aborted, true);
        await resolveReply(page, 1, rows('B')); expectState(await settle(page), rows('B'), false);
    }],
    ['H32.06 captured original view and explicit new descriptor', async page => {
        await page.evaluate(currentChannel => {
            window.__originalView = { key: 'id', complete: true, where: { in: ['k'] } };
            window.__smart.mount({ label: 'view', channel: currentChannel, scope: 'view',
                args: { owner: 'view' }, initialData: [{ id: 'view-seed' }], view: window.__originalView });
            window.__originalView.where.in.splice(0, 1, 'outside');
        }, channel);
        await effects(page); await requestCount(page, 1);
        await resolveReply(page, 0, rows('k')); expectState(await settle(page), rows('k'), false);
        await grant(page);
        // These ID-rich frames are custom client fixtures, NOT native counts-
        // only producer evidence. Visible deletion proves which predicate the
        // actual controller captured; query count alone cannot distinguish it.
        await page.evaluate(currentChannel => window.__smart.customDelete(0, currentChannel, 'k'), channel);
        expectState(await settle(page), [], false);
        assert.equal((await summary(page)).requests.length, 1);
        expectState(await mount(page, 'new-view', { scope: 'view', args: { owner: 'view' },
            view: { key: 'id', complete: true, where: { in: ['j'] } } }), seed('new-view'), true);
        await effects(page); await requestCount(page, 2);
        await resolveReply(page, 1, rows('j')); expectState(await settle(page), rows('j'), false);
        const currentSocket = await page.evaluate(() => window.__smart.sockets.length - 1);
        await grant(page, currentSocket);
        await page.evaluate(({ index, currentChannel }) => window.__smart.customDelete(index, currentChannel, 'j'),
            { index: currentSocket, currentChannel: channel });
        expectState(await settle(page), [], false);
        assert.equal((await summary(page)).requests.length, 2);
    }],
];

const before = await sourceIdentity();
const results = [];
let failure;
try {
    browser = await bounded(() => chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] }), 'Chromium launch', 15000);
    for (const [name, check] of cases) results.push(await scenario(name, check));
    assert.equal(results.length, 6, 'all named real-hook cases must complete');
    assert.deepEqual(await sourceIdentity(), before, 'shipping sources changed during hook proof');
} catch (error) { failure = error; }
finally {
    try {
        await bounded(() => browser?.close(), 'browser cleanup');
        assert.equal(browser?.isConnected() || false, false);
    } catch (error) { failure = failure ? new AggregateError([failure, error], 'case and browser cleanup failed') : error; }
}
if (failure) throw failure;
for (const name of results) console.log(`PASS ${name}`);
console.log(JSON.stringify({ realHookCases: results.length, expectedCases: 6, cleanup: 'PASS',
    sourceIdentity: before, scope: 'actual vendored Preact/SDK; mock local capability/auth/WS; no native/deployed backend' }));
