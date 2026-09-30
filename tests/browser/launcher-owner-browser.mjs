// Actual shipped UI/SDK/Preact with mock local authentication, storage and WS.
// This is managed browser regression coverage, not deployed server evidence.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFile, stat } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const client = join(repo, 'client/shell/client');
const manifest = JSON.parse(await readFile(join(repo, 'client/shell/manifest.json')));
const prefix = `/ext/shell/${manifest.version}/`;
const git = (...args) => execFileSync('git', args, { cwd: repo, encoding: 'utf8', timeout: 5000 }).trim();
const sourcePaths = ['client/shell/manifest.json', 'client/shell/client/index.html',
    'client/shell/client/shell.js', 'client/shell/client/sdk.js',
    'client/shell/client/data-query.js', 'client/shell/client/data-controller.js',
    'client/shell/client/launcher/launcher.js', 'client/shell/client/launcher/model.js',
    'client/shell/client/panels/loader.js', 'client/shell/client/panels/panel_api.js',
    'client/shell/client/vendor/preact.module.js', 'client/shell/client/vendor/preact-hooks.module.js'];
async function identity() {
    return { head: git('rev-parse', 'HEAD'), tree: git('rev-parse', 'HEAD^{tree}'),
        sources: Object.fromEntries(await Promise.all(sourcePaths.map(async path =>
            [path, createHash('sha256').update(await readFile(join(repo, path))).digest('hex')]))),
    };
}
const before = await identity();
const frontend = await readFile(join(repo, 'src/kernel/shell/active_frontend.cpp'), 'utf8');
const policy = frontend.slice(frontend.indexOf('constexpr std::string_view STRICT_CSP ='))
    .match(/=\s*((?:"[^"\n]*"\s*)+);/);
assert(policy, 'must serve the actual production CSP');
const csp = [...policy[1].matchAll(/"([^"\n]*)"/g)].map(match => match[1]).join('');
const catalog = { schema_version: 1, applications: [{
    id: 'notes', generation: 'mock-owner-generation', version: '1.0.0', title: 'Notes',
    panels: ['zero', 'one'].map(id => ({ id, title: id === 'zero' ? 'Zero' : 'One',
        module_url: `/ext/notes/1.0.0/panels/${id}.js` })),
}] };
const emptyPreference = () => ({ version: 1, last_application: null,
    last_panels: {}, application_order: [] });
function deferred() {
    let resolvePromise;
    const promise = new Promise(resolveValue => { resolvePromise = resolveValue; });
    return { promise, resolve: resolvePromise };
}
async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline ${milliseconds}ms`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
async function until(predicate, label) {
    const deadline = Date.now() + 5000;
    while (!predicate()) {
        if (Date.now() >= deadline) throw new Error(`${label}: observation deadline`);
        await delay(10);
    }
}
function cookies(header = '') {
    return Object.fromEntries(header.split(';').filter(Boolean).map(entry => {
        const value = entry.trim(); const index = value.indexOf('=');
        return [value.slice(0, index), value.slice(index + 1)];
    }));
}
function sessionCookies(owner) {
    return [`mock_session=${owner}; Path=/; HttpOnly; SameSite=Strict`,
        `plinth_csrf=fake-${owner}; Path=/; SameSite=Strict`];
}
function panelSource(id) {
    return `import { h } from 'preact';
export default function(api) {
  api.onDeactivate(() => { window.__ownerDeactivated = (window.__ownerDeactivated || 0) + 1; });
  return function MockOwnerPanel() { return h('h2', null, ${JSON.stringify(`Mock ${id}`)}); };
}`;
}

let browser;
async function scenario({ name, queue, rotate, failFirst = false, newOwnerWrite = false }) {
    const firstSeen = deferred();
    const writes = [];
    const storage = new Map();
    const sockets = new Set();
    const fixtureErrors = [];
    const cleanupErrors = [];
    let held;
    let server;
    let context;
    let page;
    let released = false;
    let failure;
    let result;
    const release = () => {
        if (!held || released || held.destroyed) return;
        released = true;
        held.writeHead(failFirst ? 503 : 200, { 'Content-Type': 'application/json' })
            .end(JSON.stringify(failFirst
                ? { ok: false, error: { code: 'internal_error', message: 'fake first failure' } }
                : { ok: true, value: { ok: true } }));
    };
    try {
        server = createServer(async (request, response) => {
            try {
                const path = new URL(request.url, 'http://localhost').pathname;
                const owner = cookies(request.headers.cookie).mock_session;
                const json = (status, body, headers = {}) => response.writeHead(status,
                    { 'Content-Type': 'application/json', ...headers }).end(JSON.stringify(body));
                if (path === '/api/auth/session') {
                    return owner === 'A' || owner === 'B'
                        ? json(200, { user: { id: `fake-${owner}`, username: owner === 'A' ? 'alpha' : 'beta' } })
                        : json(401, { error: 'not_authenticated' });
                }
                if (path === '/api/auth/registration') return json(200, { mode: 'disabled' });
                if (path === '/api/auth/logout') {
                    assert.equal(owner, 'A'); assert.equal(request.headers['x-plinth-csrf'], 'fake-A');
                    return json(200, { ok: true }, { 'Set-Cookie': [
                        'mock_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict',
                        'plinth_csrf=; Path=/; Max-Age=0; SameSite=Strict',
                    ] });
                }
                if (path === '/api/auth/login') {
                    let body = ''; for await (const chunk of request) body += chunk;
                    assert.equal(JSON.parse(body).username, 'beta');
                    return json(200, { ok: true }, { 'Set-Cookie': sessionCookies('B') });
                }
                if (path === '/api/frontend/applications') return json(200, catalog);
                if (path === '/api/frontend/sdk.js') {
                    response.writeHead(302, { Location: `${prefix}sdk.js`, 'Cache-Control': 'no-cache' }).end();
                    return;
                }
                if (path.startsWith('/api/cap/')) {
                    assert(owner === 'A' || owner === 'B');
                    assert.equal(request.headers['x-plinth-csrf'], `fake-${owner}`);
                    let body = ''; for await (const chunk of request) body += chunk;
                    const args = JSON.parse(body).args;
                    if (path.endsWith('/shell.preferences.get_all')) {
                        return json(200, { ok: true, value: { entries: [] } });
                    }
                    if (path.endsWith('/shell.preferences.get')) {
                        return json(200, { ok: true, value: { value: storage.get(owner) || emptyPreference() } });
                    }
                    if (path.endsWith('/shell.preferences.set')) {
                        assert.equal(args.key, 'shell.launcher');
                        writes.push({ owner, panel: args.value.last_panels.notes,
                            value: structuredClone(args.value), csrfMatchesOwner: true });
                        if (writes.length === 1) {
                            if (!failFirst) storage.set(owner, structuredClone(args.value));
                            held = response; firstSeen.resolve(); return;
                        }
                        storage.set(owner, structuredClone(args.value));
                        return json(200, { ok: true, value: { ok: true } });
                    }
                    return json(200, { ok: true, value: { ok: true } });
                }
                if (path.startsWith('/ext/notes/1.0.0/panels/')) {
                    const id = path.split('/').at(-1).replace('.js', ''); assert(['zero', 'one'].includes(id));
                    response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(panelSource(id));
                    return;
                }
                let relative;
                if (path.startsWith('/app/')) relative = path.slice(5) || 'index.html';
                if (path.startsWith(prefix)) relative = path.slice(prefix.length);
                if (!relative) { response.writeHead(404).end(); return; }
                const file = resolve(client, relative);
                assert(file.startsWith(client + sep)); assert((await stat(file)).isFile());
                let bytes = await readFile(file);
                if (relative === 'index.html') bytes = Buffer.from(bytes.toString().replace(
                    '<!-- PLINTH_VERSIONED_ASSET_BASE -->', `<base href="${prefix}">`));
                const types = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css',
                    '.woff2': 'font/woff2' };
                response.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream',
                    'Content-Security-Policy': csp, 'Cache-Control': 'no-cache',
                    ...(relative === 'index.html' ? { 'Set-Cookie': sessionCookies('A') } : {}) }).end(bytes);
            } catch (error) {
                fixtureErrors.push(error.message);
                if (!response.headersSent) response.writeHead(500);
                response.end();
            }
        });
        server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
        await bounded(() => new Promise((resolveListen, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', resolveListen);
        }), 'local mock server listen');
        const origin = `http://127.0.0.1:${server.address().port}`;
        context = await bounded(() => browser.newContext(), 'browser context');
        page = await context.newPage(); page.setDefaultTimeout(5000);
        const pageErrors = [];
        page.on('pageerror', error => pageErrors.push(error.message));
        await page.addInitScript(() => {
            window.__ownerDocument = crypto.randomUUID(); window.__ownerCsp = [];
            document.addEventListener('securitypolicyviolation', event =>
                window.__ownerCsp.push(event.violatedDirective));
        });
        await page.routeWebSocket(/\/ws\/events$/, socket => {
            socket.onMessage(message => {
                const frame = JSON.parse(message);
                if (frame.type === 'subscribe' || frame.type === 'unsubscribe') {
                    socket.send(JSON.stringify({ type: frame.type + 'd', channels: frame.channels }));
                }
            });
            socket.send(JSON.stringify({ type: 'connected' }));
        });
        const documentResponse = await page.goto(`${origin}/app/`);
        assert.equal(documentResponse.headers()['content-security-policy'], csp);
        await page.getByRole('button', { name: 'Notes', exact: true }).waitFor();
        const documentIdentity = await page.evaluate(() => window.__ownerDocument);
        await page.getByRole('button', { name: 'Notes', exact: true }).click();
        await page.getByRole('heading', { name: 'Mock zero', exact: true }).waitFor();
        await bounded(() => firstSeen.promise, 'first preference admission');
        if (queue) {
            await page.getByRole('tab', { name: 'One', exact: true }).click();
            await page.getByRole('heading', { name: 'Mock one', exact: true }).waitFor();
        }
        assert.equal(writes.length, 1, 'held first request must serialize dispatch');
        if (rotate) {
            await page.locator('.zone-avatar > button').click();
            await page.keyboard.press('Tab');
            assert.equal(await page.evaluate(() => document.activeElement?.id), 'shell-theme-select');
            await page.keyboard.press('Tab');
            assert.equal(await page.evaluate(() => document.activeElement?.id), 'shell-scale-select');
            await page.keyboard.press('Tab');
            assert.equal(await page.evaluate(() => document.activeElement?.getAttribute('role')), 'menuitem');
            assert.equal(await page.evaluate(() => document.activeElement?.textContent), 'Sign Out');
            await page.keyboard.press('Enter');
            await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
            assert.equal(await page.locator('.panel-container').count(), 0);
            assert((await page.evaluate(() => window.__ownerDeactivated)) >= (queue ? 2 : 1));
            await page.locator('input[name="username"]').fill('beta');
            await page.locator('input[name="password"]').fill('fake-owner-test-password');
            await page.getByRole('button', { name: 'Sign In', exact: true }).click();
            await page.getByRole('button', { name: 'Notes', exact: true }).waitFor();
            assert.equal(await page.evaluate(() => window.__ownerDocument), documentIdentity);
            assert.equal(storage.has('B'), false);
        }
        const firstResponse = page.waitForResponse(r => r.url().endsWith('/api/cap/shell.preferences.set'));
        release(); await (await firstResponse).finished();
        if (!rotate) {
            await until(() => writes.length === 2, 'second same-owner dispatch');
            assert.deepEqual(writes.map(write => [write.owner, write.panel]), [['A', 'zero'], ['A', 'one']]);
        } else {
            // Explicitly finite browser observation, paired with the VM suite's
            // exact captured write-promise drain; no unbounded absence claim.
            await delay(200);
            assert.equal(writes.length, 1, 'retired owner must admit no queued request under B');
            assert.equal(storage.has('B'), false, 'retired preference must not be stored for B');
            if (newOwnerWrite) {
                await page.getByRole('button', { name: 'Notes', exact: true }).click();
                await page.getByRole('heading', { name: 'Mock zero', exact: true }).waitFor();
                await until(() => writes.length === 2, 'new B first action');
                await page.getByRole('tab', { name: 'One', exact: true }).click();
                await page.getByRole('heading', { name: 'Mock one', exact: true }).waitFor();
                await until(() => writes.length === 3, 'new B second action');
                assert.deepEqual(writes.map(write => [write.owner, write.panel]),
                    [['A', 'zero'], ['B', 'zero'], ['B', 'one']]);
                assert.equal(storage.get('B').last_panels.notes, 'one');
            }
        }
        assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []);
        assert.deepEqual(await page.evaluate(() => window.__ownerCsp), []);
        result = { name, requestOwners: writes.map(write => write.owner),
            capturedPanels: writes.map(write => write.panel), sameDocument: true,
            csrfMatchesOwner: writes.every(write => write.csrfMatchesOwner),
            scope: 'actual shipped UI/SDK/Preact; mock local auth/storage/WS; no native or deployed backend' };
    } catch (error) { failure = error; }
    finally {
        try { release(); } catch (error) { cleanupErrors.push(error); }
        // Each close is independently attempted even when a previous close
        // rejects or times out. The outer process supervisor owns descendants.
        try { await bounded(() => context?.close(), 'context cleanup'); }
        catch (error) { cleanupErrors.push(error); }
        try {
            await bounded(() => new Promise((resolveClose, rejectClose) => {
                if (!server) { resolveClose(); return; }
                server.close(error => error && error.code !== 'ERR_SERVER_NOT_RUNNING'
                    ? rejectClose(error) : resolveClose());
                server.closeAllConnections();
                for (const socket of sockets) socket.destroy();
            }), 'server cleanup');
            assert.equal(server?.listening || false, false);
            assert([...sockets].every(socket => socket.destroyed));
        } catch (error) { cleanupErrors.push(error); }
        if (page && !page.isClosed()) cleanupErrors.push(new Error('page remained open after context cleanup'));
    }
    if (failure || cleanupErrors.length) {
        throw new AggregateError([...(failure ? [failure] : []), ...cleanupErrors], `${name} failed`);
    }
    return result;
}

const cases = [
    { name: 'P01 managed same-owner serialized writes', queue: true, rotate: false },
    { name: 'P02 managed rejected first write admits next', queue: true, rotate: false, failFirst: true },
    { name: 'N01 managed unmount without queued write', queue: false, rotate: true },
    { name: 'N02 managed retired queue and fresh B actions', queue: true, rotate: true, newOwnerWrite: true },
];
const results = [];
let failure;
try {
    browser = await bounded(() => chromium.launch({
        executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    }), 'Chromium launch', 15000);
    for (const item of cases) results.push(await scenario(item));
    assert.equal(results.length, 4, 'all named managed cases must complete');
    assert.deepEqual(await identity(), before, 'source identity must remain unchanged during browser run');
} catch (error) { failure = error; }
finally {
    try {
        await bounded(() => browser?.close(), 'browser cleanup');
        assert.equal(browser?.isConnected() || false, false);
    } catch (error) {
        failure = failure ? new AggregateError([failure, error], 'case and browser cleanup failed') : error;
    }
}
if (failure) throw failure;
for (const result of results) console.log('PASS ' + JSON.stringify(result));
console.log(JSON.stringify({ managedOwnerCases: results.length, expectedCases: 4, cleanup: 'PASS',
    sourceIdentity: before, finiteQueueProof: 'launcher-owner.test.mjs',
    scope: 'mock backend only; original diagnostic failure retained separately' }));
