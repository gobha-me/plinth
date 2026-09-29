// Actual installed-package source, vendored Preact, PanelManager/PanelApi and
// shipping SDK. HTTP responses are explicit local mocks, not native authority,
// persistence, installation, grant, provenance or deployed-backend evidence.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const shell = resolve(repo, 'client/shell/client'), admin = resolve(repo, 'client/admin/client');
const sourcePaths = ['client/admin/client/panels/packages.js', 'client/admin/client/packages/api.js',
    'client/admin/client/packages/controller.js', 'client/shell/client/panels/loader.js',
    'client/shell/client/panels/panel_api.js', 'client/shell/client/sdk.js',
    'client/shell/client/data-query.js', 'client/shell/client/data-controller.js',
    'client/shell/client/vendor/preact.module.js', 'client/shell/client/vendor/preact-hooks.module.js'];
const identity = async () => Object.fromEntries(await Promise.all(sourcePaths.map(async path =>
    [path, createHash('sha256').update(await readFile(resolve(repo, path))).digest('hex')])));
async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
const html = `<!doctype html><meta charset="utf-8"><div id="host"></div>
<script type="importmap">{"imports":{"preact":"/vendor/preact.module.js",
"preact/hooks":"/vendor/preact-hooks.module.js","@plinth/frontend/sdk":"/sdk.js"}}</script>
<script type="module" src="/harness.js"></script>`;
const harness = `
import { options } from 'preact';
import { PanelManager } from '/panels/loader.js';
import { retireRealtimeSession, activateRealtimeSession } from '/sdk.js';
const host = document.getElementById('host');
const original = { fetch: window.fetch, raf: options.requestAnimationFrame };
const requests = [], effects = [], failures = [], aborts = [];
let account = 'A', instance, manager;
options.requestAnimationFrame = callback => { effects.push(callback); };
const makeManager = () => new PanelManager(host, {
  onFailure: (_target, error) => failures.push(error.name),
});
manager = makeManager();
document.cookie = 'plinth_csrf=fake-A; path=/; SameSite=Strict';
function deferred() {
  let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
async function turns() { for (let index = 0; index < 30; index++) await Promise.resolve(); }
window.fetch = (url, settings = {}) => {
  if (!(url === '/api/auth/session' || String(url).startsWith('/api/packages'))) {
    throw new Error('unexpected local-mock API URL');
  }
  const fetch = deferred(), json = deferred();
  const record = { url: String(url), method: settings.method || 'GET', account,
    csrf: new Headers(settings.headers || {}).get('X-Plinth-CSRF'),
    body: typeof settings.body === 'string' ? JSON.parse(settings.body) : null,
    form: settings.body instanceof FormData ? [...settings.body].map(([key, value]) => ({
      key, name: value.name, size: value.size, type: value.type })) : null,
    signal: settings.signal, fetch, json, jsonCalls: 0, settled: false, jsonSettled: false };
  const index = requests.push(record) - 1;
  settings.signal?.addEventListener('abort', () => aborts.push({ index, account,
    method: record.method, text: host.textContent }));
  // Intentionally ignore AbortSignal for completion. Original-owner checks
  // must reject even a transport which continues after cancellation.
  return fetch.promise;
};
function summary() {
  return { requests: requests.map(({ url, method, account, csrf, body, form, signal, jsonCalls }) =>
    ({ url, method, account, csrf, body, form, aborted: signal?.aborted || false, jsonCalls })),
    failures: [...failures], aborts: [...aborts], effects: effects.length,
    text: host.textContent, active: manager.active?.status || null,
    count: manager.instances.size, connected: instance?.container.isConnected || false };
}
window.__admin = {
  async prepare(generation = 'admin-A', context = {}) {
    instance = await manager.prepare({ applicationId: 'admin', generation, version: '0.1.0',
      panel: { id: 'packages', module_url: '/ext/admin/0.1.0/panels/packages.js' },
      tabId: 'admin-tab', paneId: 'admin-pane' }, { context });
    return { ...summary(), context: instance.api.getContext(), status: instance.status,
      hidden: instance.container.hidden };
  },
  commit() { manager.commit(instance); return summary(); },
  home() { manager.deactivateToHome(); return summary(); },
  destroy() { manager.destroyAll(); return summary(); },
  summary,
  async turns() { await turns(); return summary(); },
  async effects() {
    for (let index = 0; effects.length && index < 30; index++) { effects.shift()(); await turns(); }
    if (effects.length) throw new Error('actual hook effects did not quiesce');
    await turns(); return summary();
  },
  async reply(index, status, body, holdJson = false) {
    const request = requests[index]; if (!request || request.settled) throw new Error('invalid fetch settlement');
    request.settled = true;
    request.fetch.resolve({ status, ok: status >= 200 && status < 300, statusText: 'mock HTTP',
      json() { request.jsonCalls++; return request.json.promise; } });
    if (!holdJson) { request.jsonSettled = true; request.json.resolve(body); }
    await turns(); return summary();
  },
  async json(index, body, reject = false) {
    const request = requests[index]; if (!request || request.jsonSettled) throw new Error('invalid JSON settlement');
    request.jsonSettled = true;
    if (reject) request.json.reject(new Error('fake malformed JSON')); else request.json.resolve(body);
    await turns(); return summary();
  },
  async reject(index) {
    const request = requests[index]; request.settled = true;
    request.fetch.reject(new Error('fake lost reply')); await turns(); return summary();
  },
  rotate() { account = 'B'; document.cookie = 'plinth_csrf=fake-B; path=/; SameSite=Strict'; },
  retire() { retireRealtimeSession('session_ended'); return summary(); },
  newRoot() { manager.dispose(); manager = makeManager(); activateRealtimeSession(); instance = null; },
  async cleanup() {
    manager.dispose();
    for (const request of requests) {
      if (!request.settled) { request.settled = true; request.fetch.resolve({ status: 401, ok: false,
        json() { return Promise.resolve({ error: 'not_authenticated' }); } }); }
      if (!request.jsonSettled) { request.jsonSettled = true; request.json.resolve({ error: 'not_authenticated' }); }
    }
    await turns(); effects.length = 0; host.replaceChildren();
    retireRealtimeSession('test_cleanup');
    window.fetch = original.fetch; options.requestAnimationFrame = original.raf;
    return { managerEmpty: manager.instances.size === 0, inactive: manager.active === null,
      unloadRemoved: !manager.beforeUnloadInstalled, domEmpty: !host.childNodes.length,
      restored: window.fetch === original.fetch && options.requestAnimationFrame === original.raf,
      settled: requests.every(request => request.settled && request.jsonSettled) };
  },
};
`;

let browser;
async function scenario(name, check) {
    let server, context, page, failure;
    const connections = new Set(), fixtureErrors = [], pageErrors = [], cleanupErrors = [];
    try {
        server = createServer(async (request, response) => {
            try {
                const path = new URL(request.url, 'http://localhost').pathname;
                if (path === '/favicon.ico') return response.writeHead(204).end();
                if (path === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
                if (path === '/harness.js') return response.writeHead(200,
                    { 'Content-Type': 'application/javascript' }).end(harness);
                const adminPrefix = '/ext/admin/0.1.0/';
                const root = path.startsWith(adminPrefix) ? admin : shell;
                const relative = path.startsWith(adminPrefix) ? path.slice(adminPrefix.length) : path.slice(1);
                const file = resolve(root, relative); assert(file.startsWith(root + sep));
                response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(await readFile(file));
            } catch (error) {
                fixtureErrors.push(error.message);
                if (!response.headersSent) response.writeHead(500);
                response.end();
            }
        });
        server.on('connection', socket => { connections.add(socket); socket.on('close', () => connections.delete(socket)); });
        await bounded(() => new Promise((done, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', done);
        }), `${name} server listen`);
        context = await bounded(() => browser.newContext(), `${name} context`);
        page = await context.newPage(); page.setDefaultTimeout(5000);
        page.on('pageerror', error => pageErrors.push(error.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        await page.waitForFunction(() => Boolean(window.__admin));
        await check(page);
        assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []);
    } catch (error) { failure = error; }
    finally {
        try {
            if (page && !page.isClosed()) assert.deepEqual(await bounded(
                () => page.evaluate(() => window.__admin?.cleanup()), `${name} fixture cleanup`),
            { managerEmpty: true, inactive: true, unloadRemoved: true, domEmpty: true, restored: true, settled: true });
        } catch (error) { cleanupErrors.push(error); }
        try { await bounded(() => context?.close(), `${name} context cleanup`); }
        catch (error) { cleanupErrors.push(error); }
        if (page && !page.isClosed()) cleanupErrors.push(new Error(`${name}: page remained open`));
        try {
            await bounded(() => new Promise((done, reject) => {
                if (!server) return done();
                server.close(error => error && error.code !== 'ERR_SERVER_NOT_RUNNING' ? reject(error) : done());
                server.closeAllConnections(); for (const connection of connections) connection.destroy();
            }), `${name} server cleanup`);
            assert.equal(server?.listening || false, false);
            assert([...connections].every(connection => connection.destroyed));
        } catch (error) { cleanupErrors.push(error); }
    }
    if (failure || cleanupErrors.length) throw new AggregateError(
        [...(failure ? [failure] : []), ...cleanupErrors], `${name} failed`);
    return name;
}

const sessionA = { user: { id: 'actor-A', username: 'alice' }, session: { id: 'session-A' } };
const sessionB = { user: { id: 'actor-B', username: 'bob' }, session: { id: 'session-B' } };
const packageId = '11111111-1111-4111-8111-111111111111';
const example = { id: packageId, name: 'example', version: '1.0.0', state: 'ENABLED' };
const list = (items = [example], limit = 20, offset = 0) => ({ items, limit, offset });
const evaluate = (page, expression, arg) => page.evaluate(expression, arg);
async function activated(page, rows = [example]) {
    await evaluate(page, () => window.__admin.prepare());
    await evaluate(page, () => window.__admin.commit());
    await evaluate(page, () => window.__admin.effects());
    await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
    await evaluate(page, () => window.__admin.effects());
    assert.equal((await evaluate(page, () => window.__admin.summary())).requests[1].url,
        '/api/auth/session');
    await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
    await evaluate(page, () => window.__admin.effects());
    await evaluate(page, body => window.__admin.reply(2, 200, body), list(rows));
    await evaluate(page, () => window.__admin.effects());
    await evaluate(page, body => window.__admin.reply(3, 200, body), sessionA);
    await evaluate(page, () => window.__admin.effects());
    assert.equal(await page.locator('#admin-phase').textContent(), 'active');
}

const checks = [
    ['B01 PREPARED is hidden and request-free; current activation captures actor before list', async page => {
        const prepared = await evaluate(page, () => window.__admin.prepare());
        assert.equal(prepared.hidden, true);
        assert.equal(prepared.status, 'prepared');
        assert.deepEqual(prepared.requests, []);
        assert.equal(await page.locator('#admin-phase').textContent(), 'prepared');
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        assert.deepEqual((await evaluate(page, () => window.__admin.summary())).requests.map(item => item.url),
            ['/api/auth/session']);
        await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.deepEqual((await evaluate(page, () => window.__admin.summary())).requests.map(item => item.url),
            ['/api/auth/session', '/api/auth/session']);
        await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[2].url,
            '/api/packages?limit=20&offset=0');
        await evaluate(page, body => window.__admin.reply(2, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[3].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(3, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'active');
        assert.match(await page.locator('#admin-actor').textContent(), /alice \(actor-A\), session session-A/);
        assert.equal(await page.locator('#admin-package-list [data-package-id]').count(), 1);
    }],
    ['B02 deactivate aborts owned read before callbacks; reactivation rejects old fetch completion', async page => {
        await evaluate(page, () => window.__admin.prepare());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[1].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 3);
        const home = await evaluate(page, () => window.__admin.home());
        assert.equal(home.requests[2].aborted, true);
        assert.deepEqual(home.aborts.map(item => [item.index, item.method]),
            [[1, 'GET'], [2, 'GET']]);
        await evaluate(page, () => window.__admin.rotate());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(2, 200, body), list([{ ...example,
            name: 'old-owner' }]));
        await evaluate(page, () => window.__admin.effects());
        assert.doesNotMatch(await page.locator('#admin-packages-panel').textContent(), /old-owner/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[3].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(3, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[4].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[5].url,
            '/api/packages?limit=20&offset=0');
        await evaluate(page, body => window.__admin.reply(5, 200, body), list([{ ...example,
            name: 'new-owner' }]));
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.match(await page.locator('#admin-actor').textContent(), /bob \(actor-B\)/);
        assert.match(await page.locator('#admin-package-list').textContent(), /new-owner/);
        assert.doesNotMatch(await page.locator('#admin-package-list').textContent(), /old-owner/);
    }],
    ['B03 old JSON rejection after home cannot contaminate reactivated owner', async page => {
        await evaluate(page, () => window.__admin.prepare());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, () => window.__admin.reply(2, 200, {}, true));
        await evaluate(page, () => window.__admin.home());
        await evaluate(page, () => window.__admin.rotate());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, () => window.__admin.json(2, {}, true));
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'loading');
        assert.equal((await page.locator('#admin-list-error').count()), 0);
        await evaluate(page, body => window.__admin.reply(3, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-list-error').textContent(), 'none');
        assert.match(await page.locator('#admin-actor').textContent(), /bob \(actor-B\)/);
    }],
    ['B04 real UI ZIP validation/install and lifecycle use fresh admission, CSRF and explicit uninstall', async page => {
        await activated(page);
        await page.locator('#admin-package-list [data-package-id]').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), example);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await page.locator('#admin-package-file').setInputFiles({ name: 'example.zip',
            mimeType: 'application/zip', buffer: Buffer.from([0x50, 0x4b, 3, 4]) });
        await page.locator('#admin-dry-run').click();
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[7].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(7, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const dry = (await evaluate(page, () => window.__admin.summary())).requests[8];
        assert.equal(dry.url, '/api/packages?dry_run=1'); assert.equal(dry.method, 'POST');
        assert.equal(dry.csrf, 'fake-A');
        assert.deepEqual(dry.form.map(value => [value.key, value.name, value.size]),
            [['package', 'example.zip', 4]]);
        await evaluate(page, body => window.__admin.reply(8, 200, body),
            { state: 'VALIDATING', name: 'example', version: '1.0.0', validation_report: {} });
        await evaluate(page, () => window.__admin.effects());
        assert.match(await page.locator('#admin-validation').textContent(), /VALIDATING/);
        assert.equal(await page.locator('#admin-attempts [data-outcome="succeeded"]').count(), 1);
        await page.locator('#admin-install').click();
        await evaluate(page, body => window.__admin.reply(9, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const install = (await evaluate(page, () => window.__admin.summary())).requests[10];
        assert.equal(install.url, '/api/packages'); assert.equal(install.method, 'POST');
        assert.equal(install.csrf, 'fake-A');
        await evaluate(page, body => window.__admin.reply(10, 201, body), example);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(11, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(12, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(13, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-attempts [data-outcome="succeeded"]').count(), 2);
        assert.doesNotMatch(await page.locator('#admin-attempts').textContent(), /[0-9]+%|streaming stage/i);
        await page.locator('#admin-disable').click();
        await evaluate(page, body => window.__admin.reply(14, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const disable = (await evaluate(page, () => window.__admin.summary())).requests[15];
        assert.equal(disable.method, 'PATCH'); assert.deepEqual(disable.body, { action: 'disable' });
        await evaluate(page, body => window.__admin.reply(15, 200, body),
            { ...example, action: 'disable', state: 'DISABLED' });
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(16, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(17, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(18, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-uninstall').isDisabled(), true);
        await page.locator('#admin-uninstall-name').fill('wrong');
        assert.equal(await page.locator('#admin-uninstall').isDisabled(), true);
        await page.locator('#admin-uninstall-name').fill('example');
        assert.equal(await page.locator('#admin-uninstall').isEnabled(), true);
        await page.locator('#admin-uninstall').click();
        await evaluate(page, body => window.__admin.reply(19, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const uninstall = (await evaluate(page, () => window.__admin.summary())).requests[20];
        assert.equal(uninstall.url, `/api/packages/${packageId}?confirm=true`);
        assert.equal(uninstall.method, 'DELETE');
        await evaluate(page, () => window.__admin.reply(20, 204, {}));
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(21, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(22, 200, body), list([]));
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(23, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-attempts [data-outcome="succeeded"]').count(), 4);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 24);
    }],
    ['B05 lost PATCH remains unknown despite matching observed row; no automatic mutation retry', async page => {
        await activated(page);
        await page.locator('#admin-package-list [data-package-id]').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), example);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await page.locator('#admin-disable').click();
        await evaluate(page, body => window.__admin.reply(7, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[8].method, 'PATCH');
        await evaluate(page, () => window.__admin.reject(8));
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[9].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(9, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const observation = (await evaluate(page, () => window.__admin.summary())).requests[10];
        assert.equal(observation.method, 'GET');
        assert.equal(observation.url, '/api/packages?limit=20&offset=0&include_failed=1');
        await evaluate(page, body => window.__admin.reply(10, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(11, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(12, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[13].url,
            `/api/packages/${packageId}`);
        await evaluate(page, body => window.__admin.reply(13, 200, body), example);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(14, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-attempts [data-outcome="unknown"]').count(), 1);
        assert.match(await page.locator('#admin-attempts').textContent(),
            /Observed rows cannot prove this request committed/);
        assert.match(await page.locator('#admin-attempts').textContent(), /Server observation: complete/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests
            .filter(request => request.method === 'PATCH').length, 1);
        await page.locator('#admin-package-file').setInputFiles({ name: 'example.zip',
            mimeType: 'application/zip', buffer: Buffer.from([0x50, 0x4b, 3, 4]) });
        await page.locator('#admin-install').click();
        await evaluate(page, body => window.__admin.reply(15, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[16].method, 'POST');
        await evaluate(page, body => window.__admin.reply(16, 500, body), {
            state: 'ACTIVE', failed_at_stage: 'ACTIVE', id: packageId,
            kind: 'handoff-failure', message: 'lock release acknowledgement failed',
        });
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(17, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(18, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(19, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-attempts [data-outcome="unknown"]').count(), 2);
        assert.match(await page.locator('#admin-attempts [data-server-report]').last().textContent(),
            /"state": "ACTIVE"/);
        assert.match(await page.locator('#admin-attempts [data-outcome="unknown"]').last().textContent(),
            /Observed rows cannot prove this request committed/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests
            .filter(request => request.method === 'POST').length, 1);
    }],
    ['B06 SDK retirement and root disposal bar queued write; fresh root takes new actor', async page => {
        await activated(page);
        await page.locator('#admin-package-list [data-package-id]').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), example);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await page.locator('#admin-disable').click();
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[7].url,
            '/api/auth/session');
        await evaluate(page, () => window.__admin.retire());
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'session-ended');
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[7].aborted, true);
        await evaluate(page, body => window.__admin.reply(7, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 8);
        await evaluate(page, () => { window.__admin.rotate(); window.__admin.newRoot(); });
        const prepared = await evaluate(page, () => window.__admin.prepare('admin-B'));
        assert.deepEqual(prepared.requests.filter(request => request.account === 'B'), []);
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[8].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(8, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(9, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(10, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(11, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.match(await page.locator('#admin-actor').textContent(), /bob \(actor-B\)/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests
            .filter(request => request.method === 'PATCH').length, 0);
    }],
    ['B07 visible panel does not imply API grant; structured 403 remains explicit and retryable', async page => {
        await evaluate(page, () => window.__admin.prepare());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(2, 403, body),
            { error: { code: 'forbidden', message: 'permission denied' } });
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[3].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(3, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'active');
        assert.match(await page.locator('#admin-packages-panel').textContent(),
            /Panel visibility does not grant package API permissions/);
        assert.equal(await page.locator('#admin-list-error').textContent(), 'forbidden: permission denied');
        assert.equal(await page.locator('#admin-package-list [data-package-id]').count(), 0);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests
            .filter(request => request.method !== 'GET').length, 0);
        await page.locator('#admin-refresh').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-list-error').textContent(), 'none');
        assert.equal(await page.locator('#admin-package-list [data-package-id]').count(), 1);
    }],
    ['B08 inactive instance destruction and rejected old transport cannot replay into new generation', async page => {
        await evaluate(page, () => window.__admin.prepare());
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(0, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(1, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        const inactive = await evaluate(page, () => window.__admin.home());
        assert.equal(inactive.requests[2].aborted, true);
        const destroyed = await evaluate(page, () => window.__admin.destroy());
        assert.equal(destroyed.count, 0);
        await evaluate(page, () => window.__admin.reject(2));
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, () => window.__admin.rotate());
        const prepared = await evaluate(page, () => window.__admin.prepare('admin-B'));
        assert.equal(prepared.status, 'prepared');
        assert.equal(prepared.requests.length, 3);
        await evaluate(page, () => window.__admin.commit());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(3, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(5, 200, body), list());
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.match(await page.locator('#admin-actor').textContent(), /bob \(actor-B\)/);
        assert.equal(await page.locator('#admin-list-error').textContent(), 'none');
        assert.deepEqual((await evaluate(page, () => window.__admin.summary())).failures, []);
    }],
    ['B09 silent cookie rotation before a package read retires A without dispatching B read', async page => {
        await activated(page);
        await evaluate(page, () => window.__admin.rotate());
        await page.locator('#admin-refresh').click();
        await evaluate(page, () => window.__admin.effects());
        const requests = (await evaluate(page, () => window.__admin.summary())).requests;
        assert.equal(requests[4].url, '/api/auth/session');
        assert.equal(requests[4].account, 'B');
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'session-ended');
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 5);
        assert.doesNotMatch(await page.locator('#admin-packages-panel').textContent(), /bob|actor-B/);
    }],
    ['B10 rotation during package JSON cannot render B rows under A after post-read probe', async page => {
        await activated(page);
        await page.locator('#admin-refresh').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[5].url,
            '/api/packages?limit=20&offset=0');
        await evaluate(page, () => window.__admin.rotate());
        await evaluate(page, body => window.__admin.reply(5, 200, body),
            list([{ ...example, name: 'B-private-row' }]));
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[6].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'session-ended');
        assert.doesNotMatch(await page.locator('#admin-packages-panel').textContent(), /B-private-row/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 7);
    }],
    ['B11 rotated B-side 403 is never surfaced under the old A panel', async page => {
        await activated(page);
        await page.locator('#admin-refresh').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[5].url,
            '/api/packages?limit=20&offset=0');
        await evaluate(page, () => window.__admin.rotate());
        await evaluate(page, body => window.__admin.reply(5, 403, body),
            { error: { code: 'B-private-denial', message: 'B-private-error' } });
        await evaluate(page, () => window.__admin.effects());
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests[6].url,
            '/api/auth/session');
        await evaluate(page, body => window.__admin.reply(6, 200, body), sessionB);
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'session-ended');
        assert.doesNotMatch(await page.locator('#admin-packages-panel').textContent(), /B-private/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 7);
    }],
    ['B12 failed post-error actor probe emits only generic error with one bounded session GET', async page => {
        await activated(page);
        await page.locator('#admin-refresh').click();
        await evaluate(page, body => window.__admin.reply(4, 200, body), sessionA);
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, () => window.__admin.rotate());
        await evaluate(page, body => window.__admin.reply(5, 403, body),
            { error: { code: 'B-private-denial', message: 'B-private-error' } });
        await evaluate(page, () => window.__admin.effects());
        await evaluate(page, body => window.__admin.reply(6, 503, body),
            { error: { code: 'db-error', message: 'session verification failed' } });
        await evaluate(page, () => window.__admin.effects());
        assert.equal(await page.locator('#admin-phase').textContent(), 'active');
        assert.equal(await page.locator('#admin-list-error').textContent(),
            'session-unverified: Current session could not be verified; retry the read.');
        assert.doesNotMatch(await page.locator('#admin-packages-panel').textContent(), /B-private/);
        assert.equal((await evaluate(page, () => window.__admin.summary())).requests.length, 7);
    }],
];

const initial = await identity();
const completed = [];
try {
    browser = await bounded(() => chromium.launch({ headless: true }), 'Chromium launch', 10000);
    assert.equal(checks.length, 12, 'all twelve named browser controls must be registered');
    for (const [name, check] of checks) completed.push(await scenario(name, check));
    assert.equal(completed.length, 12, 'all twelve browser controls must execute');
    assert.deepEqual(await identity(), initial, 'actual shipped sources changed during browser proof');
    console.log(JSON.stringify({ cases: completed, count: completed.length,
        cleanup: 'PASS', actualSources: initial }));
} finally {
    await bounded(() => browser?.close(), 'Chromium cleanup', 10000);
}
