// Actual transport, Panel API, and ONE shipping SDK module. Native I/O only is
// controlled; these unit cases are not deployed session/RBAC/browser proof.
import assert from 'node:assert/strict';
import test from 'node:test';
import vm from 'node:vm';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { target, snapshot, location, deferred, drain, plain } from './float-production-fixtures.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const paths = ['sdk.js', 'data-query.js', 'data-controller.js', 'panels/float-model.js',
    'panels/float-wire.js', 'panels/float-transport.js', 'panels/panel_api.js'];
const sources = new Map(await Promise.all(paths.map(async path => [resolve(root, path), await readFile(resolve(root, path), 'utf8')])));
async function fixture() {
    const document = { cookie: 'plinth_csrf=fake-A' };
    const sdkRequests = [];
    const context = vm.createContext({ URL, URLSearchParams, Headers, TextEncoder, TextDecoder, AbortController,
        document, window: { location }, location, console, setTimeout, clearTimeout,
        fetch(url, init) { sdkRequests.push({ url, init }); return Promise.resolve(response({ ok: true, value: {},
            resolved_tier: 'tier2', provider_type: 'extension' })); } });
    const modules = new Map();
    const preact = new vm.SyntheticModule(['h'], function () { this.setExport('h', () => { throw new Error('unexpected render'); }); }, { context });
    const hooks = new vm.SyntheticModule(['useEffect', 'useRef', 'useState'], function () {
        for (const name of ['useEffect', 'useRef', 'useState']) this.setExport(name, () => { throw new Error('unexpected hook'); });
    }, { context });
    const load = path => {
        assert(sources.has(path), `unexpected shipping module ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(sources.get(path), { context, identifier: path }));
        return modules.get(path);
    };
    const link = (specifier, importer) => {
        if (specifier === '@plinth/frontend/sdk') return load(resolve(root, 'sdk.js'));
        if (specifier === 'preact') return preact;
        if (specifier === 'preact/hooks') return hooks;
        assert(specifier.startsWith('.')); return load(resolve(dirname(importer.identifier), specifier));
    };
    const transport = load(resolve(root, 'panels/float-transport.js'));
    await transport.link(link); await transport.evaluate();
    const panel = load(resolve(root, 'panels/panel_api.js')); await panel.link(link); await panel.evaluate();
    assert.equal(modules.size, 7);
    const realm = value => {
        context.fixture = JSON.stringify(value);
        try { return vm.runInContext('JSON.parse(fixture)', context); } finally { delete context.fixture; }
    };
    return { transport: transport.namespace, panel: panel.namespace, realm, document, sdkRequests,
        sdk: modules.get(resolve(root, 'sdk.js')).namespace };
}
const response = (body, { profile = 'fixed-profile', status = 200 } = {}) => new Response(JSON.stringify(body), {
    status, headers: { 'Content-Type': 'application/json', ...(profile === null ? {} : { 'X-Plinth-Platform-Profile': profile }) } });

test('actual canonical SDK supplies fresh CSRF and profile is response-only compatibility', async () => {
    const f = await fixture(), requests = [];
    const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
        fetch: async (url, init) => { requests.push({ url, init }); return response({ schema_version: 1, saved: true }); } });
    assert.deepEqual(plain(await api.commitLayout(f.realm(snapshot()), f.realm([]))), { saved: true });
    assert.equal(new Headers(requests[0].init.headers).get('X-Plinth-CSRF'), 'fake-A');
    assert.equal(new Headers(requests[0].init.headers).has('X-Plinth-Platform-Profile'), false);
    assert.equal(requests[0].init.credentials, 'same-origin'); assert.equal(requests[0].init.redirect, 'error');
    f.document.cookie = 'plinth_csrf=fake-B'; await api.commitLayout(f.realm(snapshot()), f.realm([]));
    assert.equal(new Headers(requests[1].init.headers).get('X-Plinth-CSRF'), 'fake-B');
    assert.equal(requests[0].url, '/api/frontend/float-layout');
});

test('actual typed transport bounds raw UTF8 rejects duplicate keys and preserves uncertain mutation ACK', async () => {
    const f = await fixture();
    for (const raw of ['{"schema_version":1,"schema_version":1,"saved":true}',
        '{"schema_version":1,"saved":false}', 'x'.repeat(2049)]) {
        const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
            fetch: async () => new Response(raw, { headers: { 'X-Plinth-Platform-Profile': 'fixed-profile' } }) });
        await assert.rejects(api.commitLayout(f.realm(snapshot()), f.realm([])), failure => failure.uncertain === true);
    }
    for (const body of [new Uint8Array([0xff]), new Uint8Array([0xef, 0xbb, 0xbf, 0x7b, 0x7d])]) {
        await assert.rejects(f.transport.readBoundedFloatResponse(new Response(body), 128));
    }
    const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
        fetch: async () => response({ schema_version: 1, present: false, policy_revision: null, restore_targets: [] }, { profile: null }) });
    await assert.rejects(api.readLayout(), { code: 'reload-required' });
});

test('stream overflow retains body cancellation until it physically settles', async () => {
    const f = await fixture(), cancellation = deferred();
    let cancelled = false, completed = false;
    const body = new ReadableStream({ start(stream) { stream.enqueue(new Uint8Array(9)); },
        cancel() { cancelled = true; return cancellation.promise; } });
    const promise = f.transport.readBoundedFloatResponse(new Response(body), 8);
    const result = promise.catch(() => { completed = true; });
    await drain(); assert.equal(cancelled, true); assert.equal(completed, false);
    cancellation.resolve(); await result; assert.equal(completed, true);
});

test('late cancelled responses preserve terminal identity/profile only after physical body settlement', async () => {
    const f = await fixture();
    for (const [status, profile, raw, code] of [
        [401, 'fixed-profile', '{}', 'session-ended'],
        [401, 'fixed-profile', 'not-json', 'session-ended'],
        [200, 'other-profile', '{}', 'reload-required'],
        [200, 'other-profile', 'not-json', 'reload-required'],
        [200, 'fixed-profile', '{}', 'cancelled'],
    ]) {
        const held = deferred(), abort = new AbortController(); let complete = false;
        const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
            fetch: () => held.promise });
        const result = api.readLayout({ signal: abort.signal });
        const checked = assert.rejects(result, { code }).then(() => { complete = true; });
        abort.abort(); await drain(); assert.equal(complete, false);
        held.resolve(new Response(raw, { status, headers: { 'X-Plinth-Platform-Profile': profile } }));
        await checked;
    }
    // A malformed body's still-owned cancel Promise must not be bypassed even
    // though the already observed HTTP status is terminal.
    const cancellation = deferred(), abort = new AbortController(); let finished = false;
    const body = new ReadableStream({ start(stream) { stream.enqueue(new Uint8Array(131073)); },
        cancel() { return cancellation.promise; } });
    const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
        fetch: async () => new Response(body, { status: 401 }) });
    const checked = assert.rejects(api.readLayout({ signal: abort.signal }), { code: 'session-ended' })
        .then(() => { finished = true; });
    abort.abort(); await drain(); assert.equal(finished, false);
    cancellation.resolve(); await checked;
});

test('typed catalog exact decoder and defaults use existing own preference capability, not admin management', async () => {
    const f = await fixture(), requests = [];
    const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
        fetch: async (url, init) => {
            requests.push({ url, init });
            return response(url.startsWith('/api/cap/') ? { ok: true, value: {}, resolved_tier: 'tier2', provider_type: 'extension' }
                : { schema_version: 1, kind: 'float', targets: [target()], next_cursor: null });
        } });
    assert.deepEqual(plain(await api.personalDefaults()), []);
    assert.equal(requests[0].url, '/api/cap/shell.preferences.get');
    assert.deepEqual(JSON.parse(requests[0].init.body), { args: { key: 'shell.handler_defaults' } });
    // Independently compare with the existing shipping SDK's normal call path,
    // not just another hardcoded expectation of the new transport.
    await f.sdk.call('shell.preferences.get', { key: 'shell.handler_defaults' });
    assert.equal(requests[0].url, f.sdkRequests[0].url);
    assert.deepEqual(JSON.parse(requests[0].init.body), JSON.parse(f.sdkRequests[0].init.body));
    assert.equal((await api.catalog({ content_type: 'Text/Plain' })).targets.length, 1);
    assert.equal(requests[1].url.includes('include_admin_default'), false);
    assert.equal(new Headers(requests[1].init.headers).has('X-Plinth-CSRF'), false);
});

test('own preference response uses the actual native four-key envelope and closed metadata types', async () => {
    const f = await fixture();
    const source = await readFile(resolve(root, '../../../src/kernel/cap/api_cap.cpp'), 'utf8');
    const success = source.slice(source.indexOf('auto make_success('), source.indexOf('auto make_error('));
    assert.deepEqual([...success.matchAll(/body\["([a-z_]+)"\]/g)].map(match => match[1]),
        ['ok', 'value', 'resolved_tier', 'provider_type']);
    for (const body of [{ ok: true, value: {} }, { ok: true, value: {}, resolved_tier: 2, provider_type: 'extension' },
        { ok: true, value: {}, resolved_tier: 'tier2', provider_type: null },
        { ok: true, value: {}, resolved_tier: 'tier9', provider_type: 'extension' }]) {
        const api = f.transport.createFloatTransport({ platformProfile: 'fixed-profile', location,
            fetch: async () => response(body) });
        await assert.rejects(api.personalDefaults(), { code: 'invalid-response' });
    }
});

test('private Panel API activation preserves context identity Promise shape and unbind fences actions', async () => {
    const f = await fixture(), calls = [], context = { record_id: 'initial' };
    const api = f.panel.makePanelApi({ panel: { id: 'preview' }, context, floatActions: {
        openFloat(type, value) { calls.push(['open', type, value]); return Promise.resolve({ floatId: 'float-ready' }); },
        requestFocus() { calls.push(['focus']); },
    } });
    assert.equal(api.getContext(), context);
    assert.deepEqual(await api.openFloat('text/plain', context), { floatId: 'float-ready' });
    assert.equal(api.requestFocus(), undefined); assert.equal(calls.length, 2);
    api.__shell_internal.unbind();
    await assert.rejects(api.openFloat('text/plain', context), { name: 'PanelUnboundError' });
    assert.equal(api.requestFocus(), undefined); assert.equal(calls.length, 2);
    const legacy = f.panel.makePanelApi({ panel: { id: 'primary' } });
    await assert.rejects(legacy.openFloat('text/plain'), { name: 'NotImplementedError' });
    assert.throws(() => legacy.requestFocus(), { name: 'NotImplementedError' });
});

test('trusted document profile reader rejects absence duplicates and malformed profiles', async () => {
    const f = await fixture(), profile = 'v1:' + target().generation + ':' + 'a'.repeat(64);
    const document = contents => ({ querySelectorAll(selector) {
        assert.equal(selector, 'meta[name="plinth-platform-profile"]');
        return contents.map(content => ({ getAttribute(name) { assert.equal(name, 'content'); return content; } }));
    } });
    assert.equal(f.transport.readFloatPlatformProfile(document([profile])), profile);
    for (const contents of [[], [profile, profile], [null], ['fixed-profile'], [profile + '\n']]) {
        assert.throws(() => f.transport.readFloatPlatformProfile(document(contents)), { code: 'reload-required' });
    }
});
