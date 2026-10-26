// Actual groups UI/API/controller, vendored Preact, PanelManager and shipped
// SDK; HTTP is explicitly mocked. No native authority, persistence, install,
// upgrade, permission-grant or deployed-backend evidence is supplied here.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const shell = resolve(repo, 'client/shell/client'), admin = resolve(repo, 'client/admin/client');
const adminVersion = JSON.parse(await readFile(resolve(repo, 'client/admin/manifest.json'), 'utf8')).version;
assert.match(adminVersion, /^\d+\.\d+\.\d+$/);
const adminPrefix = `/ext/admin/${adminVersion}/`;
const sourcePaths = ['client/admin/manifest.json', 'client/admin/client/panels/groups.js',
    'client/admin/client/groups/api.js', 'client/admin/client/groups/controller.js',
    'client/shell/client/panels/loader.js', 'client/shell/client/panels/panel_api.js',
    'client/shell/client/sdk.js', 'client/shell/client/data-query.js', 'client/shell/client/data-controller.js',
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
let account = 'A', instance;
options.requestAnimationFrame = callback => { effects.push(callback); };
const makeManager = () => new PanelManager(host, { onFailure: (_target, error) => failures.push(error.name) });
let manager = makeManager();
document.cookie = 'plinth_csrf=fake-A; path=/; SameSite=Strict';
const deferred = () => {
  let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
async function turns() { for (let i = 0; i < 30; ++i) await Promise.resolve(); }
window.fetch = (url, settings = {}) => {
  if (!(url === '/api/auth/session' || url === '/api/groups' || String(url).startsWith('/api/groups/')))
    throw new Error('unexpected mocked HTTP request');
  const record = { url: String(url), method: settings.method || 'GET', account,
    credentials: settings.credentials, cache: settings.cache,
    csrf: new Headers(settings.headers || {}).get('X-Plinth-CSRF'),
    body: typeof settings.body === 'string' ? JSON.parse(settings.body) : null,
    signal: settings.signal, fetch: deferred(), json: deferred(),
    settled: false, jsonSettled: false, jsonCalls: 0 };
  const index = requests.push(record) - 1;
  settings.signal?.addEventListener('abort', () => aborts.push({ index, account,
    text: host.textContent }));
  // Deliberately continue after AbortSignal: original-owner fences must reject
  // late fetch and JSON completions, independent of transport cooperation.
  return record.fetch.promise;
};
const summary = () => ({ requests: requests.map(({ url, method, account, credentials, cache,
  csrf, body, signal, settled, jsonSettled, jsonCalls }, index) =>
    ({ index, url, method, account, credentials, cache, csrf, body,
      aborted: signal?.aborted || false, settled, jsonSettled, jsonCalls })),
  failures: [...failures], aborts: [...aborts], text: host.textContent,
  active: manager.active?.status || null, count: manager.instances.size });
window.__groups = {
  async prepare(generation = 'groups-A') {
    instance = await manager.prepare({ applicationId: 'admin', generation,
      version: ${JSON.stringify(adminVersion)},
      panel: { id: 'groups', module_url: ${JSON.stringify(adminPrefix + 'panels/groups.js')} },
      tabId: 'groups-tab', paneId: 'groups-pane' }, { context: {} });
    return { ...summary(), hidden: instance.container.hidden, status: instance.status };
  },
  commit() { manager.commit(instance); return summary(); },
  home() { manager.deactivateToHome(); return summary(); },
  destroy() { manager.destroyAll(); return summary(); },
  retire() { retireRealtimeSession('session_ended'); return summary(); },
  rotate() { account = 'B'; document.cookie = 'plinth_csrf=fake-B; path=/; SameSite=Strict'; },
  newRoot() { manager.dispose(); manager = makeManager(); activateRealtimeSession(); instance = null; },
  summary,
  async effects() {
    for (let i = 0; effects.length && i < 30; ++i) { effects.shift()(); await turns(); }
    if (effects.length) throw new Error('actual Preact effects did not quiesce');
    await turns(); return summary();
  },
  async reply(index, status, body, holdJson = false) {
    const r = requests[index]; if (!r || r.settled) throw new Error('invalid fetch settlement');
    r.settled = true;
    r.fetch.resolve({ status, ok: status >= 200 && status < 300, statusText: 'mock HTTP',
      json() { r.jsonCalls++; return r.json.promise; } });
    if (!holdJson) { r.jsonSettled = true; r.json.resolve(body); }
    await turns(); return summary();
  },
  async json(index, body) {
    const r = requests[index]; if (!r || r.jsonSettled) throw new Error('invalid JSON settlement');
    r.jsonSettled = true; r.json.resolve(body); await turns(); return summary();
  },
  async reject(index) {
    const r = requests[index]; if (!r || r.settled) throw new Error('invalid rejection');
    r.settled = true; r.jsonSettled = true;
    r.json.resolve(null);
    r.fetch.reject(new Error('mock forwarded request, lost reply')); await turns(); return summary();
  },
  async cleanup() {
    manager.dispose();
    for (const r of requests) {
      if (!r.settled) { r.settled = true; r.fetch.resolve({ status: 401, ok: false,
        json() { return Promise.resolve({ error: 'not_authenticated' }); } }); }
      if (!r.jsonSettled) { r.jsonSettled = true; r.json.resolve({ error: 'not_authenticated' }); }
    }
    await turns(); effects.length = 0; host.replaceChildren(); retireRealtimeSession('test_cleanup');
    window.fetch = original.fetch; options.requestAnimationFrame = original.raf;
    return { managerEmpty: manager.instances.size === 0, inactive: manager.active === null,
      unloadRemoved: !manager.beforeUnloadInstalled, domEmpty: !host.childNodes.length,
      restored: window.fetch === original.fetch && options.requestAnimationFrame === original.raf,
      settled: requests.every(r => r.settled && r.jsonSettled) };
  },
};
`;

let browser;
async function scenario(name, check) {
    let server, context, page, failure;
    const sockets = new Set(), fixtureErrors = [], pageErrors = [], cleanupErrors = [];
    try {
        server = createServer(async (request, response) => {
            try {
                const path = new URL(request.url, 'http://localhost').pathname;
                if (path === '/favicon.ico') return response.writeHead(204).end();
                if (path === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
                if (path === '/harness.js') return response.writeHead(200,
                    { 'Content-Type': 'application/javascript' }).end(harness);
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
        server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
        await bounded(() => new Promise((done, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', done);
        }), `${name} listen`);
        context = await bounded(() => browser.newContext(), `${name} context`);
        page = await bounded(() => context.newPage(), `${name} page`);
        page.setDefaultTimeout(5000); page.setDefaultNavigationTimeout(5000);
        page.on('pageerror', error => pageErrors.push(error.message));
        await page.goto(`http://127.0.0.1:${server.address().port}/`);
        await page.waitForFunction(() => Boolean(window.__groups));
        await bounded(() => check(page), name, 30000);
        assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []);
    } catch (error) { failure = error; }
    finally {
        try {
            if (page && !page.isClosed()) assert.deepEqual(await bounded(
                () => page.evaluate(() => window.__groups?.cleanup()), `${name} fixture cleanup`),
            { managerEmpty: true, inactive: true, unloadRemoved: true, domEmpty: true, restored: true, settled: true });
        } catch (error) { cleanupErrors.push(error); }
        try { await bounded(() => context?.close(), `${name} context cleanup`); }
        catch (error) { cleanupErrors.push(error); }
        if (page && !page.isClosed()) cleanupErrors.push(new Error(`${name}: page remained open`));
        try {
            await bounded(() => new Promise((done, reject) => {
                if (!server) return done();
                server.close(error => error && error.code !== 'ERR_SERVER_NOT_RUNNING' ? reject(error) : done());
                server.closeAllConnections(); for (const socket of sockets) socket.destroy();
            }), `${name} server cleanup`);
            assert.equal(server?.listening || false, false);
            assert([...sockets].every(socket => socket.destroyed));
        } catch (error) { cleanupErrors.push(error); }
    }
    if (failure || cleanupErrors.length) throw new AggregateError(
        [...(failure ? [failure] : []), ...cleanupErrors], `${name} failed`);
    return name;
}

const actorA = { user: { id: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa', username: 'alice' },
    session: { id: 'cccccccc-cccc-4ccc-8ccc-cccccccccccc' } };
const actorB = { user: { id: 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb', username: 'bob' },
    session: { id: 'dddddddd-dddd-4ddd-8ddd-dddddddddddd' } };
const custom = { id: '11111111-1111-4111-8111-111111111111', name: 'alpha',
    description: 'Original description', built_in: false, created_at: '2026-10-24T00:00:00Z' };
const other = { ...custom, id: '22222222-2222-4222-8222-222222222222', name: 'beta' };
const adminGroup = { ...custom, id: '33333333-3333-4333-8333-333333333333', name: 'admin', built_in: true };
const everyone = { ...custom, id: '44444444-4444-4444-8444-444444444444', name: 'everyone', built_in: true };
const spare = '55555555-5555-4555-8555-555555555555';
const secondSpare = '66666666-6666-4666-8666-666666666666';
const created = { id: '77777777-7777-4777-8777-777777777777', name: 'newgroup',
    built_in: false, created_at: custom.created_at }; // Native blank description is omitted.
const effects = page => page.evaluate(() => window.__groups.effects());
const summary = page => page.evaluate(() => window.__groups.summary());
const requests = async page => (await summary(page)).requests;
const writes = async page => (await requests(page)).filter(r => r.method !== 'GET');
const reply = (page, request, status, body, holdJson = false) => page.evaluate(
    ({ index, status, body, holdJson }) => window.__groups.reply(index, status, body, holdJson),
    { index: request.index, status, body, holdJson });
function standard(rows = [custom]) {
    const byId = new Map([custom, other, adminGroup, everyone, created].map(row => [row.id, row]));
    return request => {
        if (request.url === '/api/auth/session') return { status: 200, body: request.account === 'A' ? actorA : actorB };
        if (request.method !== 'GET') throw new Error(`unexpected mock mutation ${request.method} ${request.url}`);
        if (request.url === '/api/groups') return { status: 200, body: { groups: rows } };
        const row = byId.get(request.url.slice('/api/groups/'.length));
        assert(row, `unexpected mock read ${request.url}`);
        return { status: 200, body: { ...row, member_count: 2 } };
    };
}
async function pump(page, respond = standard()) {
    for (let turn = 0; turn < 60; ++turn) {
        await effects(page);
        const pending = (await requests(page)).filter(r => !r.settled);
        let answered = false;
        for (const request of pending) {
            const response = respond(request);
            if (!response) continue;
            if (response.reject) await page.evaluate(index => window.__groups.reject(index), request.index);
            else await reply(page, request, response.status, response.body, response.holdJson);
            answered = true;
        }
        if (!answered) { await effects(page); return; }
    }
    throw new Error('mock response schedule did not quiesce');
}
async function activated(page, rows = [custom]) {
    await page.evaluate(() => window.__groups.prepare());
    await page.evaluate(() => window.__groups.commit());
    await pump(page, standard(rows));
    assert.equal(await page.locator('#groups-phase').getAttribute('data-phase'), 'active');
}
async function selected(page, row = custom, respond = standard()) {
    await page.locator(`[data-group-id="${row.id}"]`).click();
    await pump(page, respond);
    assert.equal(await page.locator('#groups-selected-title').textContent(), row.name);
}

const checks = [
    ['B01 PREPARED has no I/O; actual activation renders full-list pages and detail', async page => {
        const prepared = await page.evaluate(() => window.__groups.prepare());
        assert.equal(prepared.hidden, true); assert.equal(prepared.status, 'prepared');
        assert.deepEqual(prepared.requests, []);
        assert.equal(await page.locator('#groups-phase').getAttribute('data-phase'), 'prepared');
        await page.evaluate(() => window.__groups.commit());
        const rows = Array.from({ length: 26 }, (_, i) => ({ ...custom,
            id: `${String(i + 10000000).padStart(8, '0')}-1111-4111-8111-111111111111`, name: `group_${i}` }));
        rows[0] = custom;
        await pump(page, standard(rows));
        assert.equal(await page.locator('#groups-actor').textContent(), 'Account checked before request: alice');
        assert.equal(await page.locator('#groups-phase').textContent(), 'Ready');
        assert.equal(await page.locator('#groups-list [data-group-id]').count(), 25);
        await page.locator('#groups-next').click();
        assert.equal(await page.locator('#groups-list [data-group-id]').count(), 1);
        await page.locator('#groups-filter').fill('alpha');
        assert.equal(await page.locator('#groups-list [data-group-id]').count(), 1);
        await selected(page);
        assert.equal(await page.locator('#groups-edit-description').inputValue(), custom.description);
        assert.match(await page.locator('#groups-member-count').textContent(), /2$/);
        assert((await requests(page)).every(r => r.credentials === 'same-origin' && r.cache === 'no-store'));
        assert((await requests(page)).filter(r => r.url === '/api/groups').every(r => !r.url.includes('?')));
    }],
    ['B02 accessible local validation focuses its form and deletion requires the exact name', async page => {
        await activated(page); await selected(page);
        await page.getByLabel('New group name', { exact: true }).fill('INVALID');
        await page.locator('#groups-create').click(); await effects(page);
        assert.equal(await page.locator('#groups-form-error').getAttribute('data-error-code'), 'validation');
        assert.equal(await page.evaluate(() => document.activeElement.id), 'groups-create-name');
        assert.equal((await writes(page)).length, 0);
        await page.getByLabel('New group name', { exact: true }).fill('validname');
        await page.locator('#groups-create-description').fill('🪐'.repeat(65));
        await page.locator('#groups-create').click(); await effects(page);
        assert.match(await page.locator('#groups-form-error').textContent(), /256 UTF-8 bytes/);
        assert.equal(await page.evaluate(() => document.activeElement.id), 'groups-create-description');
        assert.equal(await page.locator('#groups-create-description').inputValue(), '🪐'.repeat(65));
        assert.equal((await writes(page)).length, 0);
        await page.locator('#groups-delete-name').fill('Alpha');
        assert.equal(await page.locator('#groups-delete').isDisabled(), true);
        await page.locator('#groups-delete-name').fill('alpha');
        assert.equal(await page.locator('#groups-delete').isEnabled(), true);
        await page.locator('#groups-list [data-group-id]').click(); await pump(page);
        assert.equal(await page.locator('#groups-delete-name').inputValue(), '');
        assert.equal(await page.locator('#groups-delete').isDisabled(), true);
    }],
    ['B03 built-in metadata is readonly; admin spare membership is confirmed; everyone stays virtual', async page => {
        const fallback = standard([adminGroup, everyone]);
        await activated(page, [adminGroup, everyone]); await selected(page, everyone, fallback);
        assert.equal(await page.locator('#groups-edit-name').isEditable(), false);
        assert.equal(await page.locator('#groups-edit-description').isEditable(), false);
        assert.equal(await page.locator('#groups-update').isDisabled(), true);
        assert.equal(await page.locator('#groups-delete').isDisabled(), true);
        assert.equal(await page.locator('#groups-member-add').isDisabled(), true);
        assert.equal(await page.locator('#groups-member-remove').isDisabled(), true);
        assert.match(await page.locator('#groups-virtual-membership').textContent(), /not a count of all identities/);
        await selected(page, adminGroup, fallback);
        assert.equal(await page.locator('#groups-edit-name').isEditable(), false);
        assert.equal(await page.locator('#groups-edit-description').isEditable(), false);
        await page.locator('#groups-member-id').fill(spare);
        assert.equal(await page.locator('#groups-member-remove').isDisabled(), true);
        await page.getByLabel('Repeat User ID to confirm an admin membership change').fill(spare);
        await page.locator('#groups-member-remove').click();
        await pump(page, r => r.method === 'DELETE' ? { status: 200, body: { status: 'removed' } } : fallback(r));
        assert.deepEqual((await writes(page)).map(r => [r.method, r.url]),
            [['DELETE', `/api/groups/${adminGroup.id}/members/${spare}`]]);
        assert.match(await page.locator('#groups-attempts').textContent(), new RegExp(spare));
        assert.equal(await page.locator('[data-outcome="succeeded"]').count(), 1);
    }],
    ['B04 actual UI actions serialize copied update/member intents and send explicit blank descriptions', async page => {
        await activated(page); await selected(page);
        await page.locator('#groups-edit-name').fill('renamed');
        await page.locator('#groups-edit-description').fill('');
        await page.locator('#groups-member-id').fill(spare);
        // Two same-task UI events exercise the admission boundary before a
        // disabled busy render can mask a queued-intent regression.
        await page.evaluate(() => {
            document.querySelector('#groups-edit-form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
            document.querySelector('#groups-member-add').click();
        });
        const fallback = standard();
        await pump(page, r => r.method === 'PUT' ? null : fallback(r));
        assert.equal((await writes(page)).length, 1);
        const first = (await writes(page))[0];
        assert.deepEqual(first.body, { name: 'renamed', description: '' });
        await page.locator('#groups-edit-name').fill('laterdraft');
        await page.locator('#groups-member-id').fill(secondSpare);
        await reply(page, first, 200, { id: custom.id, name: 'renamed',
            built_in: false, created_at: custom.created_at });
        await pump(page, r => r.method === 'POST' ? { status: 201,
            body: { group_id: custom.id, user_id: spare, added_at: '2026-10-24T00:00:00Z' } } : fallback(r));
        assert.deepEqual((await writes(page)).map(r => [r.method, r.url, r.body]), [
            ['PUT', `/api/groups/${custom.id}`, { name: 'renamed', description: '' }],
            ['POST', `/api/groups/${custom.id}/members`, { user_id: spare }],
        ]);
        assert.equal(await page.locator('#groups-edit-name').inputValue(), 'laterdraft');
        await page.locator('#groups-member-id').fill(spare);
        await page.locator('#groups-member-remove').click();
        await pump(page, r => r.method === 'DELETE' ? { status: 200, body: { status: 'removed' } } : fallback(r));
        await page.locator('#groups-create-name').fill('newgroup');
        await page.locator('#groups-create-description').fill('');
        await page.locator('#groups-create').click();
        await pump(page, r => r.method === 'POST' ? { status: 201, body: created } : fallback(r));
        const mutations = await writes(page);
        assert.deepEqual(mutations.map(r => r.method), ['PUT', 'POST', 'DELETE', 'POST']);
        assert.equal(mutations[2].url, `/api/groups/${custom.id}/members/${spare}`);
        assert.deepEqual(mutations[3].body, { name: 'newgroup', description: '' });
        assert(mutations.every(r => r.csrf === 'fake-A' && r.credentials === 'same-origin'));
        assert.equal(await page.locator('[data-outcome="succeeded"]').count(), 4);
        assert.equal(await page.locator('#groups-edit-description').inputValue(), '');
    }],
    ['B05 a visible authenticated panel receiving 403 clears protected rows without fake logout', async page => {
        await activated(page); await selected(page);
        const fallback = standard();
        await page.locator('#groups-member-id').fill(spare);
        await page.locator('#groups-member-add').click();
        await pump(page, r => r.method === 'POST' ? { reject: true } : fallback(r));
        await page.locator('[id^="groups-observe-"]').click();
        await pump(page, r => r.url === '/api/groups' ?
            { status: 200, body: {}, holdJson: true } : fallback(r));
        const observation = (await requests(page)).findLast(r => r.url === '/api/groups' && !r.jsonSettled);
        await page.locator('#groups-refresh').click();
        await pump(page, r => r.url === '/api/groups' ? { status: 403,
            body: { error: 'permission_denied' } } : fallback(r));
        await page.evaluate(({ index, body }) => window.__groups.json(index, body),
            { index: observation.index, body: { groups: [{ ...custom, name: 'reconcile_private_name' }] } });
        await pump(page, fallback);
        assert.equal(await page.locator('#groups-phase').getAttribute('data-phase'), 'active');
        assert.match(await page.locator('#groups-actor').textContent(), /alice/);
        assert.equal(await page.locator('#groups-list-error').getAttribute('data-error-code'), 'permission_denied');
        assert.equal(await page.locator('#groups-list [data-group-id]').count(), 0);
        assert.equal(await page.locator('#groups-detail').count(), 0);
        assert.equal(await page.locator('[data-observed-state]').count(), 0);
        assert.equal(await page.locator('[data-reconciliation="complete"]').count(), 0);
        assert.doesNotMatch(await page.locator('#admin-groups-panel').textContent(),
            /Original description|reconcile_private_name/);
    }],
    ['B06 Home/unmount/SDK retirement bar queued work and late original-owner acknowledgements', async page => {
        await activated(page); await selected(page);
        await page.locator('#groups-filter').fill('alpha');
        await page.locator('#groups-edit-name').fill('queuedname');
        await page.locator('#groups-member-id').fill(spare);
        await page.evaluate(() => {
            document.querySelector('#groups-edit-form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
            document.querySelector('#groups-member-add').click();
        });
        await pump(page, r => r.method === 'PUT' ? null : standard()(r));
        const pending = (await writes(page))[0];
        assert.equal((await writes(page)).length, 1);
        assert.equal(pending.aborted, false);
        await page.evaluate(() => window.__groups.home()); await effects(page);
        await reply(page, pending, 200, { ...custom, name: 'queuedname' });
        await effects(page);
        assert.equal((await writes(page)).length, 1);
        assert.equal(await page.locator('#groups-phase').getAttribute('data-phase'), 'inactive');
        assert.equal(await page.locator('[data-request-id]').count(), 0);
        await page.evaluate(() => { window.__groups.rotate(); window.__groups.commit(); });
        await pump(page);
        assert.match(await page.locator('#groups-actor').textContent(), /bob/);
        assert.equal(await page.locator('#groups-filter').inputValue(), '');
        assert.equal(await page.locator('#groups-create-name').inputValue(), '');
        await page.locator('#groups-create-name').fill('sdk_barred');
        await page.evaluate(() => {
            const form = document.querySelector('#groups-create-form');
            form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
            form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
        });
        await effects(page);
        const oldAdmissions = (await requests(page)).filter(r => !r.settled);
        assert.equal(oldAdmissions.length, 1);
        await page.evaluate(() => window.__groups.retire()); await effects(page);
        for (const admission of oldAdmissions) await reply(page, admission, 200, actorB);
        await effects(page);
        assert.equal(await page.locator('#groups-phase').getAttribute('data-phase'), 'session-ended');
        assert.doesNotMatch(await page.locator('#groups-actor').textContent(), /bob/);
        await page.evaluate(() => window.__groups.destroy()); await effects(page);
        assert.equal(await page.locator('#admin-groups-panel').count(), 0);
        assert.equal((await writes(page)).length, 1);
    }],
    ['B07 lost and malformed acknowledgements stay UNKNOWN with explicit bounded GET-only observation', async page => {
        await activated(page); await selected(page);
        const dataReads = (await requests(page)).filter(r => r.method === 'GET' && r.url !== '/api/auth/session').length;
        await page.locator('#groups-member-id').fill(spare);
        await page.locator('#groups-member-add').click();
        const fallback = standard();
        await pump(page, r => r.method === 'POST' ? { reject: true } : fallback(r));
        assert.equal(await page.locator('[data-outcome="unknown"]').count(), 1);
        assert.match(await page.locator('#groups-attempts').textContent(), /member count cannot confirm membership/);
        assert.equal((await requests(page)).filter(r => r.method === 'GET' && r.url !== '/api/auth/session').length,
            dataReads, 'UNKNOWN must not automatically reconcile');
        const before = await requests(page);
        await page.locator('[id^="groups-observe-"]').click(); await pump(page);
        const observed = (await requests(page)).slice(before.length);
        assert(observed.every(r => r.method === 'GET'));
        assert.equal(observed.filter(r => r.url === '/api/groups').length, 1);
        assert.equal(observed.filter(r => r.url === `/api/groups/${custom.id}`).length, 1);
        assert.equal(await page.locator('[data-outcome="unknown"]').count(), 1);
        assert.equal(await page.locator('[data-reconciliation="complete"]').count(), 1);
        assert.equal((await writes(page)).length, 1);
        await page.locator('#groups-update').click();
        await pump(page, r => r.method === 'PUT' ? { status: 200, body: {} } : fallback(r));
        assert.equal(await page.locator('[data-outcome="unknown"]').count(), 2);
        assert.equal((await writes(page)).length, 2);
        assert.equal(await page.locator('[data-outcome="succeeded"]').count(), 0);
    }],
    ['B08 conflicts preserve drafts; old selection and old-account completions cannot replace fresh state', async page => {
        const fallback = standard([custom, other]);
        await activated(page, [custom, other]); await selected(page, custom, fallback);
        await page.locator('#groups-edit-name').fill('localdraft');
        await page.locator('#groups-update').click();
        await pump(page, r => r.method === 'PUT' ? { status: 409, body: { error: 'group_exists' } } : fallback(r));
        assert.equal(await page.locator('#groups-edit-name').inputValue(), 'localdraft');
        assert.equal(await page.locator('[data-outcome="failed"]').count(), 1);
        assert.equal(await page.locator('[data-outcome="failed"]').getAttribute('data-error-code'), 'group_exists');
        await page.locator(`[data-group-id="${custom.id}"]`).click();
        await pump(page, r => r.url === `/api/groups/${custom.id}` ?
            { status: 200, body: {}, holdJson: true } : fallback(r));
        const old = (await requests(page)).findLast(r => r.url === `/api/groups/${custom.id}` && !r.jsonSettled);
        await selected(page, other, fallback);
        await page.locator('#groups-edit-name').fill('freshdraft');
        await page.evaluate(({ index, body }) => window.__groups.json(index, body),
            { index: old.index, body: { ...custom, member_count: 2 } });
        await effects(page);
        assert.equal(await page.locator('#groups-selected-title').textContent(), 'beta');
        assert.equal(await page.locator('#groups-edit-name').inputValue(), 'freshdraft');
        await page.locator(`[data-group-id="${other.id}"]`).click();
        await pump(page, r => r.url === `/api/groups/${other.id}` ?
            { status: 200, body: {}, holdJson: true } : fallback(r));
        const oldActor = (await requests(page)).findLast(r => r.url === `/api/groups/${other.id}` && !r.jsonSettled);
        await page.evaluate(() => { window.__groups.retire(); window.__groups.rotate(); window.__groups.newRoot(); });
        await page.evaluate(() => window.__groups.prepare('groups-B'));
        await page.evaluate(() => window.__groups.commit()); await pump(page, fallback);
        await selected(page, other, fallback); await page.locator('#groups-edit-name').fill('bobdraft');
        await page.evaluate(({ index, body }) => window.__groups.json(index, body),
            { index: oldActor.index, body: { ...other, name: 'old_private_name', member_count: 99 } });
        await effects(page);
        assert.match(await page.locator('#groups-actor').textContent(), /bob/);
        assert.equal(await page.locator('#groups-edit-name').inputValue(), 'bobdraft');
        assert.doesNotMatch(await page.locator('#admin-groups-panel').textContent(), /old_private_name/);
        // Delete is already admitted when a same-ID list selection starts a
        // detail read. Its late JSON must not resurrect a deleted selection.
        await page.locator('#groups-delete-name').fill('beta');
        await page.locator('#groups-delete').click();
        await pump(page, r => r.method === 'DELETE' ? null : fallback(r));
        const deletion = (await writes(page)).findLast(r => r.method === 'DELETE');
        assert(deletion && !deletion.settled);
        await page.locator(`[data-group-id="${other.id}"]`).click();
        await pump(page, r => r.method === 'DELETE' ? null : r.url === `/api/groups/${other.id}` ?
            { status: 200, body: {}, holdJson: true } : fallback(r));
        const deletedRead = (await requests(page)).findLast(r => r.url === `/api/groups/${other.id}` &&
            r.method === 'GET' && !r.jsonSettled);
        await reply(page, deletion, 200, { status: 'deleted' }); await pump(page, fallback);
        await page.evaluate(({ index, body }) => window.__groups.json(index, body),
            { index: deletedRead.index, body: { ...other, name: 'deleted_private_name', member_count: 99 } });
        await pump(page, fallback);
        assert.equal(await page.locator('#groups-detail').count(), 0);
        assert.equal(await page.locator('#groups-selected-title').textContent(), 'Select a group');
        assert.doesNotMatch(await page.locator('#admin-groups-panel').textContent(), /deleted_private_name/);
        assert.equal((await writes(page)).length, 2);
    }],
];

const initial = await identity(), completed = [];
try {
    browser = await bounded(() => chromium.launch({ headless: true }), 'Chromium launch', 10000);
    assert.equal(checks.length, 8, 'all eight named mocked groups browser controls must be registered');
    assert.deepEqual(checks.map(([name]) => name.slice(0, 3)), ['B01', 'B02', 'B03', 'B04', 'B05', 'B06', 'B07', 'B08']);
    for (const [name, check] of checks) completed.push(await scenario(name, check));
    assert.equal(completed.length, 8, 'all eight mocked controls must execute without skips');
    assert.deepEqual(await identity(), initial, 'reviewed actual sources changed during mocked browser proof');
    console.log(JSON.stringify({ groupsAdminMockBrowserCases: completed, expectedCases: 8,
        cleanup: 'PASS', actualSources: initial, moduleVersionFromManifest: adminVersion,
        scope: 'actual Preact/PanelManager/shipped SDK/groups modules; explicit mocked HTTP; no native/install/upgrade proof' }));
} finally {
    await bounded(() => browser?.close(), 'Chromium cleanup', 10000);
}
