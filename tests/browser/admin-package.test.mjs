// Actual admin controller/transport and shipping withCsrf implementation.
// Only platform I/O is controlled; native authority/provenance proof is separate.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const admin = resolve(repo, 'client/admin/client/packages');
const shell = resolve(repo, 'client/shell/client');
const paths = [resolve(admin, 'api.js'), resolve(admin, 'controller.js'),
    ...['sdk.js', 'data-query.js', 'data-controller.js'].map(path => resolve(shell, path))];
const sources = new Map(await Promise.all(paths.map(async path => [path, await readFile(path, 'utf8')])));
const plain = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
function deferred() {
    let resolvePromise, rejectPromise;
    const promise = new Promise((resolveValue, rejectValue) => {
        resolvePromise = resolveValue; rejectPromise = rejectValue;
    });
    return { promise, resolve: resolvePromise, reject: rejectPromise };
}
function response(status = 200, body = {}) {
    return { status, ok: status >= 200 && status < 300, statusText: 'controlled response',
        json: async () => body };
}
async function turns() { for (let index = 0; index < 20; index++) await Promise.resolve(); }

async function fixture(fetch = () => { throw new Error('unexpected mock request'); }) {
    const document = { cookie: 'plinth_csrf=fake-A' };
    const requests = [], timers = new Map();
    let nextTimer = 0;
    const context = vm.createContext({
        URL, URLSearchParams, Headers, Blob, File, FormData, AbortController, DOMException,
        document, window: { location: { href: 'https://plinth.test/app/', origin: 'https://plinth.test',
            protocol: 'https:', host: 'plinth.test' } },
        fetch(url, options = {}) { requests.push({ url, options }); return fetch(url, options); },
        queueMicrotask, console: { error() {} },
        setTimeout(callback, delay) { const id = ++nextTimer; timers.set(id, { callback, delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
    });
    const hooks = new vm.SyntheticModule(['useEffect', 'useRef', 'useState'], function () {
        for (const name of ['useEffect', 'useRef', 'useState']) this.setExport(name,
            () => { throw new Error(`unexpected ${name} in pure transport fixture`); });
    }, { context, identifier: 'platform:preact/hooks' });
    const preact = new vm.SyntheticModule(['h'], function () {
        this.setExport('h', () => { throw new Error('unexpected render in pure transport fixture'); });
    }, { context, identifier: 'platform:preact' });
    const modules = new Map();
    const load = path => {
        assert(sources.has(path), `unapproved module import: ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(sources.get(path), { context, identifier: path }));
        return modules.get(path);
    };
    const link = (specifier, importer) => {
        if (specifier === '@plinth/frontend/sdk') return load(resolve(shell, 'sdk.js'));
        if (specifier === 'preact') return preact;
        if (specifier === 'preact/hooks') return hooks;
        assert(specifier.startsWith('./') || specifier.startsWith('../'), `unexpected external import: ${specifier}`);
        return load(resolve(dirname(importer.identifier), specifier));
    };
    const api = load(resolve(admin, 'api.js')), controller = load(resolve(admin, 'controller.js'));
    await api.link(link); await api.evaluate();
    if (controller.status === 'unlinked') await controller.link(link);
    await controller.evaluate();
    return { api: api.namespace, controller: controller.namespace, requests, document, timers };
}

const id = '11111111-1111-4111-8111-111111111111';
const otherId = '22222222-2222-4222-8222-222222222222';
const row = { id, name: 'example', version: '1.0.0', state: 'ENABLED' };
const session = { user: { id: 'actor-A', username: 'alice' }, session: { id: 'session-A' } };
const listing = (items = [row], limit = 20, offset = 0) => ({ items, limit, offset });
const zip = new File([new Uint8Array([0x50, 0x4b, 3, 4])], 'example.zip', { type: 'application/zip' });

test('A01 actual API reads current session and bounded installed-list page without mutation CSRF', async () => {
    const fixtureResult = await fixture();
    const seen = [];
    const api = fixtureResult.api.createPackagesApi({ fetch: (url, options) => {
        seen.push({ url, options });
        return Promise.resolve(response(200, url === '/api/auth/session' ? session : listing()));
    } });
    assert.deepEqual(plain(await api.session()), session);
    assert.deepEqual(plain(await api.list({ limit: 20, offset: 0, includeFailed: true })), listing());
    assert.deepEqual(seen.map(request => request.url),
        ['/api/auth/session', '/api/packages?limit=20&offset=0&include_failed=1']);
    assert(seen.every(request => !new Headers(request.options.headers).has('X-Plinth-CSRF')));
});

test('A02 API list emits bounded query, current credentials, and validates pagination before dispatch', async () => {
    const seen = [];
    const { api } = await fixture();
    const client = api.createPackagesApi({ fetch: (url, options) => {
        seen.push({ url, options }); return Promise.resolve(response(200, listing([], 2, 4)));
    } });
    assert.deepEqual(plain(await client.list({ limit: 2, offset: 4, includeFailed: true })), listing([], 2, 4));
    assert.equal(seen[0].url, '/api/packages?limit=2&offset=4&include_failed=1');
    assert.equal(seen[0].options.credentials, 'same-origin');
    assert.equal(new Headers(seen[0].options.headers).has('X-Plinth-CSRF'), false);
    for (const page of [{ limit: 0 }, { limit: 201 }, { limit: 1.5 }, { offset: -1 },
        { offset: 2147483648 }, { includeFailed: 'yes' }]) {
        assert.throws(() => client.list(page), error => error.name === 'TypeError');
    }
    assert.equal(seen.length, 1);
});

test('A03 API detail and lifecycle validate UUID/action before dispatch and DELETE 204 skips JSON', async () => {
    const seen = [];
    const { api } = await fixture();
    const client = api.createPackagesApi({ fetch: (url, options) => {
        seen.push({ url, options });
        if (options.method === 'DELETE') return Promise.resolve({ status: 204,
            json() { throw new Error('204 must not parse JSON'); } });
        return Promise.resolve(response(200, options.method === 'PATCH' ? { ...row, action: 'disable' } : row));
    } });
    assert.deepEqual(plain(await client.detail(id)), row);
    assert.deepEqual(plain(await client.transition(id, 'disable')), { ...row, action: 'disable' });
    assert.equal(await client.uninstall(id), null);
    assert.deepEqual(seen.map(entry => [entry.url, entry.options.method]), [
        [`/api/packages/${id}`, 'GET'], [`/api/packages/${id}`, 'PATCH'],
        [`/api/packages/${id}?confirm=true`, 'DELETE']]);
    assert.equal(seen[1].options.body, '{"action":"disable"}');
    assert.equal(new Headers(seen[1].options.headers).get('X-Plinth-CSRF'), 'fake-A');
    assert.equal(new Headers(seen[2].options.headers).get('X-Plinth-CSRF'), 'fake-A');
    assert.throws(() => client.detail('not-a-uuid'), error => error.name === 'TypeError');
    assert.throws(() => client.transition(id, 'restart'), error => error.name === 'TypeError');
    assert.throws(() => client.uninstall('not-a-uuid'), error => error.name === 'TypeError');
    assert.equal(seen.length, 3);
});

test('A04 API uses current CSRF per mutation and exact ZIP multipart dry-run/install shapes', async () => {
    const seen = [];
    const f = await fixture();
    const client = f.api.createPackagesApi({ fetch: (url, options) => {
        seen.push({ url, options });
        return Promise.resolve(response(url.endsWith('dry_run=1') ? 200 : 201,
            url.endsWith('dry_run=1') ? { state: 'VALIDATING', name: 'example', version: '1.0.0',
                validation_report: {} } : row));
    } });
    await client.upload(zip, { dryRun: true });
    f.document.cookie = 'plinth_csrf=fake-B';
    await client.upload(zip, { dryRun: false });
    assert.deepEqual(seen.map(entry => entry.url), ['/api/packages?dry_run=1', '/api/packages']);
    assert.deepEqual(seen.map(entry => new Headers(entry.options.headers).get('X-Plinth-CSRF')),
        ['fake-A', 'fake-B']);
    assert.deepEqual(seen.map(entry => [entry.options.body.get('package').name,
        entry.options.body.get('package').size]), [['example.zip', 4], ['example.zip', 4]]);
    assert.throws(() => client.upload(null), error => error.name === 'TypeError');
    assert.throws(() => client.upload(new File(['x'], 'wrong.txt')), error => error.name === 'TypeError');
    assert.equal(seen.length, 2);
});

test('A05 API distinguishes structured denial from unknown dispatched mutation outcome', async () => {
    const { api } = await fixture();
    const denied = api.createPackagesApi({ fetch: () => Promise.resolve(response(403,
        { error: { code: 'forbidden', message: 'permission denied' } })) });
    await assert.rejects(denied.transition(id, 'disable'), error => error.name === 'PackageApiError' &&
        error.status === 403 && error.code === 'forbidden' && error.unknown === false &&
        error.dispatched === true);
    const lost = api.createPackagesApi({ fetch: () => Promise.reject(new Error('lost reply')) });
    await assert.rejects(lost.transition(id, 'disable'), error => error.name === 'PackageApiError' &&
        error.unknown === true && error.dispatched === true);
    const malformed = api.createPackagesApi({ fetch: () => Promise.resolve({ status: 200,
        json: () => Promise.reject(new Error('truncated JSON')) }) });
    await assert.rejects(malformed.transition(id, 'disable'), error => error.code === 'invalid-response' &&
        error.unknown === true && error.dispatched === true);
});

test('A06 API owner barrier rejects before dispatch and after fetch, JSON, and catch completion', async () => {
    let allowed = false, count = 0;
    const wait = deferred();
    const { api } = await fixture();
    const client = api.createPackagesApi({ fetch: () => { count++; return wait.promise; } });
    await assert.rejects(client.detail(id, { isCurrent: () => allowed }), error =>
        error.name === 'OwnerRetiredError');
    assert.equal(count, 0);
    allowed = true;
    const underway = client.detail(id, { isCurrent: () => allowed });
    assert.equal(count, 1);
    allowed = false; wait.resolve(response(200, row));
    await assert.rejects(underway, error => error.name === 'OwnerRetiredError');
    const jsonWait = deferred(); allowed = true;
    const jsonClient = api.createPackagesApi({ fetch: () => Promise.resolve({ status: 200,
        json: () => jsonWait.promise }) });
    const jsonUnderway = jsonClient.detail(id, { isCurrent: () => allowed });
    await turns(); allowed = false; jsonWait.resolve(row);
    await assert.rejects(jsonUnderway, error => error.name === 'OwnerRetiredError');
    const failWait = deferred(); allowed = true;
    const failClient = api.createPackagesApi({ fetch: () => failWait.promise });
    const failing = failClient.transition(id, 'disable', { isCurrent: () => allowed });
    allowed = false; failWait.reject(new Error('lost'));
    await assert.rejects(failing, error => error.name === 'OwnerRetiredError');
});

test('A07 structured 500 and reported ACTIVE remain unknown; structured 403 is a known denial', async () => {
    const { api } = await fixture();
    const body = { state: 'ACTIVE', failed_at_stage: 'ACTIVE', id,
        kind: 'handoff-failure', message: 'lock release acknowledgement failed' };
    const active = api.createPackagesApi({ fetch: () => Promise.resolve(response(500, body)) });
    await assert.rejects(active.upload(zip), error => error.name === 'PackageApiError' &&
        error.status === 500 && error.unknown === true && error.dispatched === true &&
        error.body.state === 'ACTIVE' && error.body.id === id);
    const structured = api.createPackagesApi({ fetch: () => Promise.resolve(response(503,
        { error: { code: 'db-error', message: 'commit acknowledgement lost' } })) });
    await assert.rejects(structured.transition(id, 'disable'), error => error.unknown === true &&
        error.status === 503 && error.code === 'db-error');
    await assert.rejects(structured.uninstall(id), error => error.unknown === true &&
        error.status === 503 && error.code === 'db-error');
    const denied = api.createPackagesApi({ fetch: () => Promise.resolve(response(403,
        { error: { code: 'rbac_denied', message: 'rule absent' } })) });
    await assert.rejects(denied.upload(zip), error => error.unknown === false &&
        error.status === 403 && error.code === 'rbac_denied');
});

test('C01 controller stays request-free until activate and rechecks actor before list', async () => {
    const calls = [], published = [];
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => { calls.push('session'); return session; },
        list: async () => { calls.push('list'); return listing(); },
    }, publish: snapshot => published.push(snapshot) });
    assert.equal(value.read().phase, 'prepared'); assert.deepEqual(calls, []);
    await value.activate();
    assert.deepEqual(calls, ['session', 'session', 'list', 'session']);
    assert.deepEqual(plain(value.read().actor), { id: 'actor-A', username: 'alice', sessionId: 'session-A' });
    assert.deepEqual(plain(value.read().items), [row]);
    assert.equal(value.read().phase, 'active'); assert(published.length >= 3);
    value.destroy(); assert.equal(value.read().phase, 'destroyed');
});

test('C02 deactivate retires owner before synchronous read abort, old completion inert on reactivation', async () => {
    const old = deferred(), newRead = deferred(), events = [];
    let sessions = 0;
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => { sessions++; return sessions <= 2 ? session :
            { user: { id: 'actor-B', username: 'bob' }, session: { id: 'session-B' } }; },
        list: (_page, options) => {
            options.signal.addEventListener('abort', () => events.push(options.isCurrent()));
            return sessions <= 2 ? old.promise : newRead.promise;
        },
    } });
    const first = value.activate(); await turns();
    assert.equal(value.read().pending.list, true);
    value.deactivate(); assert.deepEqual(events, [false]); assert.equal(value.read().phase, 'inactive');
    const second = value.activate(); await turns();
    old.resolve(listing([{ ...row, name: 'old' }])); await first; await turns();
    assert.equal(value.read().actor.username, 'bob'); assert.deepEqual(plain(value.read().items), []);
    newRead.resolve(listing([{ ...row, name: 'new' }])); await second;
    assert.equal(value.read().items[0].name, 'new'); value.destroy();
});

test('C03 queued write is captured but never dispatched after retirement; current write keeps actor receipt', async () => {
    const first = deferred(), calls = [];
    const { controller } = await fixture();
    const api = { session: async () => session, list: async () => listing(),
        transition: (_id, action) => { calls.push(action); return action === 'disable' ? first.promise : row; } };
    const value = controller.createPackagesController({ api }); await value.activate();
    const admitted = value.transition(id, 'disable'); await turns();
    const queued = value.transition(otherId, 'enable'); await turns();
    assert.deepEqual(calls, ['disable']);
    assert.deepEqual(plain(value.read().attempts.map(item => [item.operation, item.status,
        item.actor.username])), [['disable', 'pending', 'alice'], ['enable', 'queued', 'alice']]);
    value.deactivate(); first.resolve({ ...row, action: 'disable' });
    await Promise.all([admitted, queued]);
    assert.deepEqual(calls, ['disable']); assert.equal(value.read().attempts.length, 0);
    value.destroy();
});

test('C04 lost write remains unknown despite matching observed row and never auto-retries', async () => {
    const calls = [];
    const unknown = Object.assign(new Error('reply lost'), { code: 'transport-error', unknown: true });
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => session,
        list: async (page) => { calls.push(['list', page.includeFailed]); return listing([row]); },
        detail: async () => { calls.push(['detail']); return row; },
        transition: async () => { calls.push(['transition']); throw unknown; },
    } });
    await value.activate(); await value.transition(id, 'disable');
    const attempt = value.read().attempts[0];
    assert.equal(attempt.status, 'unknown');
    assert.deepEqual(plain(attempt.actor), { id: 'actor-A', username: 'alice', sessionId: 'session-A' });
    assert.equal(calls.filter(item => item[0] === 'transition').length, 1);
    assert.equal(attempt.observed.detail.id, id);
    assert.equal(attempt.reconciliation, 'complete');
    assert.equal(value.read().attempts[0].status, 'unknown');
    value.destroy();
});

test('C05 uninstall requires exact selected package name; history is observed rows, not mutation attribution', async () => {
    const calls = [];
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => session,
        list: async (page) => { calls.push(['list', page]); return listing([row, { ...row,
            id: otherId, name: 'unrelated' }]); },
        detail: async () => row,
        uninstall: async () => { calls.push(['uninstall']); return null; },
    } });
    await value.activate(); await value.select(id); await value.loadHistory('example');
    assert.deepEqual(plain(value.read().history.items), [row]);
    assert.equal(await value.uninstall(id, 'wrong'), null);
    assert.equal(await value.uninstall(otherId, 'example'), null);
    assert.equal(calls.filter(item => item[0] === 'uninstall').length, 0);
    await value.uninstall(id, 'example');
    assert.equal(calls.filter(item => item[0] === 'uninstall').length, 1);
    assert.equal(value.read().attempts[0].operation, 'uninstall');
    value.destroy();
});

test('C06 queued mutation rechecks current session; rotated actor refuses write and retires owner', async () => {
    let sessions = 0, writes = 0, terminal = null;
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => ++sessions <= 3 ? session :
            { user: { id: 'actor-B', username: 'bob' }, session: { id: 'session-B' } },
        list: async () => listing(),
        transition: async () => { writes++; return { ...row, action: 'disable' }; },
    }, onSessionEnd: code => { terminal = code; } });
    await value.activate(); await value.transition(id, 'disable');
    assert.equal(writes, 0); assert.equal(sessions, 4);
    assert.equal(terminal, 'session_rotated');
    assert.equal(value.read().phase, 'session-ended');
    assert.equal(value.read().actor, null);
    value.destroy();
});

test('C07 terminal session read retires panel without stale actor; nonterminal read failure stays retryable', async () => {
    let terminal = null;
    const { controller } = await fixture();
    const fatal = Object.assign(new Error('expired'), { status: 401, code: 'session_expired' });
    const value = controller.createPackagesController({ api: {
        session: async () => { throw fatal; }, list: async () => listing(),
    }, onSessionEnd: code => { terminal = code; } });
    await value.activate();
    assert.equal(value.read().phase, 'session-ended'); assert.equal(terminal, 'session_expired');
    assert.equal(value.read().actor, null); value.destroy();
    let sessions = 0;
    const retry = controller.createPackagesController({ api: {
        session: async () => { if (++sessions === 1) throw new Error('offline'); return session; },
        list: async () => listing(),
    } });
    await retry.activate(); assert.equal(retry.read().phase, 'loading');
    assert.equal(retry.read().readErrors.session.message, 'offline');
    await retry.activate(); assert.equal(retry.read().phase, 'active');
    assert.equal(retry.read().actor.username, 'alice'); retry.destroy();
});

test('C08 old detail and reconciliation completions cannot publish into a new owner', async () => {
    const detail = deferred(), observed = deferred(), calls = [];
    let actor = session;
    const { controller } = await fixture();
    const unknown = Object.assign(new Error('lost'), { unknown: true, code: 'transport-error' });
    const value = controller.createPackagesController({ api: {
        session: async () => actor,
        list: async (page) => { calls.push(['list', page.includeFailed]);
            return page.includeFailed ? observed.promise : listing(); },
        detail: async () => detail.promise,
        transition: async () => { throw unknown; },
    } });
    await value.activate(); const oldDetail = value.select(id); await turns();
    const oldMutation = value.transition(id, 'disable'); await turns();
    assert.equal(calls.filter(item => item[1] === true).length, 1);
    value.deactivate(); actor = { user: { id: 'actor-B', username: 'bob' }, session: { id: 'session-B' } };
    await value.activate();
    detail.resolve({ ...row, name: 'old-detail' }); observed.resolve(listing([{ ...row, name: 'old-row' }]));
    await Promise.all([oldDetail, oldMutation]); await turns();
    assert.equal(value.read().actor.username, 'bob');
    assert.equal(value.read().detail, null); assert.equal(value.read().attempts.length, 0);
    assert.equal(value.read().items[0].name, 'example'); value.destroy();
});

test('C09 destroy permanently bars reactivation, stale reads, and queued writes', async () => {
    const pending = deferred(), preflight = deferred(); let writes = 0, sessions = 0;
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => ++sessions === 1 ? session : preflight.promise,
        list: async () => pending.promise,
        transition: async () => { writes++; return { ...row, action: 'disable' }; },
    } });
    const activation = value.activate(); await turns();
    const mutation = value.transition(id, 'disable'); await turns();
    value.destroy(); pending.resolve(listing()); preflight.resolve(session);
    await Promise.all([activation, mutation, value.activate()]);
    assert.equal(value.read().phase, 'destroyed'); assert.equal(writes, 0);
    assert.equal(await value.transition(id, 'disable'), null);
});

test('C10 committed-looking HTTP500 receipt stays UNKNOWN despite matching observed row and one POST', async () => {
    let posts = 0, observations = 0;
    const { api, controller } = await fixture();
    const client = api.createPackagesApi({ fetch: (url, options) => {
        if (url === '/api/auth/session') return Promise.resolve(response(200, session));
        if (url.startsWith('/api/packages?') && options.method === 'GET') {
            observations++; return Promise.resolve(response(200, listing()));
        }
        if (url === '/api/packages' && options.method === 'POST') {
            posts++; return Promise.resolve(response(500, { state: 'ACTIVE',
                failed_at_stage: 'ACTIVE', id, kind: 'handoff-failure',
                message: 'lock release acknowledgement failed' }));
        }
        throw new Error(`unexpected fixture URL ${url}`);
    } });
    const value = controller.createPackagesController({ api: client });
    await value.activate(); await value.install(zip);
    const attempt = value.read().attempts[0];
    assert.equal(attempt.operation, 'install-or-upgrade');
    assert.equal(attempt.status, 'unknown');
    assert.equal(attempt.error.status, 500);
    assert.equal(attempt.error.body.state, 'ACTIVE');
    assert.equal(attempt.reconciliation, 'complete');
    assert.equal(attempt.observed.list.items[0].id, id);
    assert.equal(value.read().attempts[0].status, 'unknown');
    assert.equal(posts, 1);
    assert.equal(observations, 2); // initial list plus one bounded reconciliation list.
    value.destroy();
});

test('C11 stable actor preserves original structured 403 after exactly one error verification probe', async () => {
    let sessions = 0, lists = 0;
    const { controller } = await fixture();
    const denied = Object.assign(new Error('permission denied'), { status: 403,
        code: 'rbac_denied', body: { error: { code: 'rbac_denied', message: 'permission denied' } } });
    const value = controller.createPackagesController({ api: {
        session: async () => { sessions++; return session; },
        list: async () => { if (++lists === 1) return listing(); throw denied; },
    } });
    await value.activate(); await value.loadList();
    assert.equal(sessions, 5); // establish + pre/post initial + pre/error-check failed read.
    assert.equal(lists, 2); assert.equal(value.read().phase, 'active');
    assert.deepEqual(plain(value.read().readErrors.list), {
        code: 'rbac_denied', message: 'permission denied', status: 403, body: denied.body,
    });
    value.destroy();
});

test('C12 B-side failed GET after silent A-to-B rotation retires without exposing B error', async () => {
    const held = deferred(); let currentSession = session, lists = 0, terminal = null;
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => currentSession,
        list: async () => ++lists === 1 ? listing() : held.promise,
    }, onSessionEnd: code => { terminal = code; } });
    await value.activate();
    const refresh = value.loadList(); await turns();
    currentSession = { user: { id: 'actor-B', username: 'bob' }, session: { id: 'session-B' } };
    held.reject(Object.assign(new Error('B-private-error'), { status: 403,
        code: 'forbidden', body: { secret: 'B-private-body' } }));
    await refresh;
    assert.equal(value.read().phase, 'session-ended');
    assert.equal(terminal, 'session_rotated');
    assert.equal(value.read().actor, null);
    assert.equal(value.read().readErrors.list, null);
    assert.doesNotMatch(JSON.stringify(value.read()), /B-private/);
    value.destroy();
});

test('C13 failed post-error actor verification emits one generic error and never repeats probe', async () => {
    let sessions = 0, lists = 0;
    const { controller } = await fixture();
    const value = controller.createPackagesController({ api: {
        session: async () => { if (++sessions === 5) throw new Error('private session failure');
            return session; },
        list: async () => { if (++lists === 1) return listing();
            throw Object.assign(new Error('B-private-error'), { status: 403, code: 'forbidden',
                body: { secret: 'B-private-body' } }); },
    } });
    await value.activate(); await value.loadList();
    assert.equal(sessions, 5); assert.equal(lists, 2);
    assert.equal(value.read().phase, 'active');
    assert.deepEqual(plain(value.read().readErrors.list), { code: 'session-unverified',
        message: 'Current session could not be verified; retry the read.', status: null, body: null });
    assert.doesNotMatch(JSON.stringify(value.read()), /B-private/);
    value.destroy();
});
