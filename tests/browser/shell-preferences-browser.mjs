// Actual shipped shell/SDK/Preact; hermetic fake auth/capability backend only.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, extname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const client = resolve(repo, 'client/shell/client');
const version = JSON.parse(await readFile(resolve(repo, 'client/shell/manifest.json'))).version;
const prefix = `/ext/shell/${version}/`;
const frontend = await readFile(resolve(repo, 'src/kernel/shell/active_frontend.cpp'), 'utf8');
const policy = frontend.slice(frontend.indexOf('constexpr std::string_view STRICT_CSP ='))
    .match(/=\s*((?:"[^"\n]*"\s*)+);/);
assert(policy, 'production CSP required');
const csp = [...policy[1].matchAll(/"([^"\n]*)"/g)].map(match => match[1]).join('');
const keys = { theme: 'shell.theme', scale: 'shell.scale_pct' };
const files = ['index.html', 'css/tokens.css', 'shell.js', 'prepaint.js', 'sdk.js', 'data-query.js', 'data-controller.js',
    'launcher/launcher.js', 'panels/loader.js', 'panels/panel_api.js', 'vendor/preact.module.js'];
async function identity() {
    return Object.fromEntries(await Promise.all(files.map(async path =>
        [path, createHash('sha256').update(await readFile(resolve(client, path))).digest('hex')])));
}
function deferred() {
    let resolvePromise;
    const promise = new Promise(resolveValue => { resolvePromise = resolveValue; });
    return { promise, resolve: resolvePromise };
}
async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
const cookies = owner => [`mock_session=${owner}; Path=/; HttpOnly; SameSite=Strict`,
    `plinth_csrf=fake-${owner}; Path=/; SameSite=Strict`];
let browser;
const completed = [];
async function scenario(name, options, check) {
    let server, context, page, failure;
    const ownedSockets = new Set(), errors = [], pageErrors = [], gates = [], requests = [];
    const stores = new Map([['A', { ...(options.serverPrefs || {}) }], ['B', {}]]);
    const plans = new Map();
    let loginProbeFails = options.loginProbeFailsOnce;
    let loginCompleted = false;
    function hold(capability, status = 200) {
        const gate = deferred(), seen = deferred();
        const item = { gate, seen, status, release: () => gate.resolve() };
        const queue = plans.get(capability) || []; queue.push(item); plans.set(capability, queue);
        gates.push(item); return item;
    }
    const initialHydration = options.holdHydration ? hold('shell.preferences.get_all', options.hydrationStatus || 200) : null;
    try {
        server = createServer(async (request, response) => {
            try {
                const path = new URL(request.url, 'http://localhost').pathname;
                const owner = /(?:^|;\s*)mock_session=([AB])(?:;|$)/.exec(request.headers.cookie || '')?.[1];
                const json = (status, value, headers = {}) => response.writeHead(status,
                    { 'Content-Type': 'application/json', ...headers }).end(JSON.stringify(value));
                if (path === '/api/auth/session') {
                    if (loginCompleted && loginProbeFails) {
                        loginProbeFails = false; return json(401, { error: 'session_expired' });
                    }
                    return owner ? json(200, { user: { id: owner, username: owner === 'A' ? 'alpha' : 'beta' } })
                        : json(401, { error: 'not_authenticated' });
                }
                if (path === '/api/auth/registration') return json(200, { mode: 'disabled' });
                if (path === '/api/auth/logout') {
                    assert.equal(request.headers['x-plinth-csrf'], `fake-${owner}`);
                    return json(200, { ok: true }, { 'Set-Cookie': [
                        'mock_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict',
                        'plinth_csrf=; Path=/; Max-Age=0; SameSite=Strict'] });
                }
                if (path === '/api/auth/login') {
                    let body = ''; for await (const chunk of request) body += chunk;
                    assert.equal(JSON.parse(body).username, 'beta');
                    loginCompleted = true;
                    return json(200, { ok: true }, { 'Set-Cookie': cookies('B') });
                }
                if (path === '/api/frontend/applications') return json(200, { schema_version: 1, applications: [] });
                if (path === '/api/frontend/sdk.js' || path === '/api/frontend/tokens.css') {
                    response.writeHead(302, { Location: prefix + (path.endsWith('.js') ? 'sdk.js' : 'css/tokens.css'),
                        'Cache-Control': 'no-cache' }).end(); return;
                }
                if (path.startsWith('/api/cap/')) {
                    assert(owner, 'fake session required');
                    assert.equal(request.headers['x-plinth-csrf'], `fake-${owner}`);
                    let body = ''; for await (const chunk of request) body += chunk;
                    const capability = decodeURIComponent(path.slice('/api/cap/'.length));
                    const args = JSON.parse(body).args;
                    const record = { owner, capability, args }; requests.push(record);
                    const store = stores.get(owner);
                    let value;
                    if (capability === 'shell.preferences.get_all') {
                        value = { entries: Object.entries(store).map(([key, value]) => ({ key, value })) };
                    } else if (capability === 'shell.preferences.get') value = { value: store[args.key] };
                    else if (capability === 'shell.preferences.set') value = { ok: true };
                    else throw new Error(`unexpected capability ${capability}`);
                    const plan = plans.get(capability)?.shift();
                    if (plan) { plan.seen.resolve(record); await plan.gate.promise; }
                    const status = plan?.status || 200;
                    if (status === 200 && capability === 'shell.preferences.set') store[args.key] = args.value;
                    if (!response.destroyed) json(status, status === 200 ? { ok: true, value }
                        : { ok: false, error: { code: 'internal_error', message: 'fake failure' } });
                    return;
                }
                const relative = path.startsWith(prefix) ? path.slice(prefix.length)
                    : path === '/app/' ? 'index.html' : null;
                if (!relative) { response.writeHead(404).end(); return; }
                const file = resolve(client, relative); assert(file.startsWith(client + sep));
                let bytes = await readFile(file);
                if (relative === 'index.html') bytes = Buffer.from(bytes.toString().replace(
                    '<!-- PLINTH_VERSIONED_ASSET_BASE -->', `<base href="${prefix}">`));
                response.writeHead(200, { 'Content-Type': ({ '.html': 'text/html', '.js': 'application/javascript',
                    '.css': 'text/css' })[extname(file)] || 'application/octet-stream',
                    'Content-Security-Policy': csp, 'Cache-Control': 'no-cache' }).end(bytes);
            } catch (error) {
                errors.push(error.message); if (!response.headersSent) response.writeHead(500); response.end();
            }
        });
        server.on('connection', socket => { ownedSockets.add(socket); socket.on('close', () => ownedSockets.delete(socket)); });
        await bounded(() => new Promise((resolveListen, reject) => {
            server.once('error', reject); server.listen(0, '127.0.0.1', resolveListen);
        }), 'fixture listen');
        const origin = `http://127.0.0.1:${server.address().port}`;
        context = await bounded(() => browser.newContext({ viewport: { width: 1280, height: 900 }, colorScheme: 'light' }), 'context');
        if (!options.startGuest) await context.addCookies([{ name: 'mock_session', value: 'A', url: origin, httpOnly: true },
            { name: 'plinth_csrf', value: 'fake-A', url: origin }]);
        await context.addInitScript(({ localPrefs, disabled }) => {
            if (localPrefs !== undefined) localStorage.setItem('shellPrefs', JSON.stringify(localPrefs));
            if (disabled) {
                Storage.prototype.getItem = () => { throw new Error('fake disabled storage'); };
                Storage.prototype.setItem = () => { throw new Error('fake disabled storage'); };
            }
            window.__preferenceJsonCompleted = [];
            const original = window.fetch;
            window.fetch = async (...args) => {
                const response = await original(...args);
                const json = response.json.bind(response);
                response.json = async () => {
                    try { return await json(); }
                    finally { window.__preferenceJsonCompleted.push(String(args[0])); }
                };
                return response;
            };
        }, { localPrefs: options.localPrefs, disabled: options.disabledStorage });
        page = await context.newPage(); page.setDefaultTimeout(5000);
        page.on('pageerror', error => pageErrors.push(error.message));
        await page.routeWebSocket(/\/ws\/events$/, socket => {
            socket.onMessage(message => {
                const frame = JSON.parse(message);
                if (['subscribe', 'unsubscribe'].includes(frame.type)) socket.send(JSON.stringify({
                    type: frame.type + 'd', channels: frame.channels, debounce_ms: 0, jitter_ms: 0 }));
            }); socket.send(JSON.stringify({ type: 'connected' }));
        });
        assert.equal((await page.goto(origin + '/app/')).headers()['content-security-policy'], csp);
        if (options.startGuest) await page.getByRole('button', { name: 'Sign In', exact: true }).waitFor();
        else {
            await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
            if (!initialHydration) await page.waitForFunction(() =>
                window.__preferenceJsonCompleted.includes('/api/cap/shell.preferences.get_all'));
            await page.locator('.zone-avatar > button').click();
        }
        const applied = () => page.evaluate(() => ({ theme: document.documentElement.dataset.theme,
            font: getComputedStyle(document.documentElement).fontSize,
            themeOption: document.getElementById('shell-theme-select')?.value,
            scaleOption: document.getElementById('shell-scale-select')?.value }));
        const settle = async () => page.evaluate(() => new Promise(resolvePaint =>
            requestAnimationFrame(() => requestAnimationFrame(resolvePaint))));
        await check({ page, context, requests, stores, hold, initialHydration, applied, settle });
        assert.deepEqual(errors, []); assert.deepEqual(pageErrors, []);
    } catch (error) { failure = error; }
    const cleanup = [];
    for (const gate of gates) gate.release();
    try { if (context) await bounded(() => context.close(), 'context cleanup'); } catch (error) { cleanup.push(error); }
    try {
        if (server) await bounded(() => new Promise((resolveClose, reject) => {
            server.close(error => error ? reject(error) : resolveClose()); server.closeAllConnections();
            for (const socket of ownedSockets) socket.destroy();
        }), 'server cleanup');
        assert(!server?.listening); assert([...ownedSockets].every(socket => socket.destroyed));
    } catch (error) { cleanup.push(error); }
    if (failure || cleanup.length) throw new AggregateError([...(failure ? [failure] : []), ...cleanup], name);
    completed.push(name);
}
const before = await identity();
let failure;
try {
    browser = await bounded(() => chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] }), 'browser launch', 15000);
    await scenario('P01 DB-authoritative hydrate, exact controls, success writes and rem geometry', {
        serverPrefs: { [keys.theme]: 'light', [keys.scale]: 100 },
        localPrefs: { [keys.theme]: 'dark', [keys.scale]: 175 },
    }, async ({ page, requests, applied }) => {
        assert.deepEqual(await applied(), { theme: 'light', font: '13.5px', themeOption: 'light', scaleOption: '100' });
        assert.equal(await page.evaluate(() => getComputedStyle(document.body).backgroundColor),
            'rgb(247, 247, 244)', 'the light palette changes the actual rendered body');
        assert.deepEqual(await page.locator('#shell-theme-select option').evaluateAll(nodes => nodes.map(node => node.value)),
            ['system', 'light', 'dark']);
        assert.deepEqual(await page.locator('#shell-scale-select option').evaluateAll(nodes => nodes.map(node => node.value)),
            ['80', '90', '100', '110', '125', '150', '175']);
        for (const pct of [80, 90, 100, 110, 125, 150, 175]) {
            await page.selectOption('#shell-scale-select', String(pct));
            await page.waitForFunction(expected => Math.abs(parseFloat(getComputedStyle(document.documentElement).fontSize) - expected) < 0.001,
                pct * 0.135);
            const writes = requests.filter(request => request.capability === 'shell.preferences.set');
            assert.deepEqual(writes.at(-1), { owner: 'A', capability: 'shell.preferences.set', args: { key: keys.scale, value: pct } });
            if ([80, 100, 175].includes(pct)) {
                const geometry = await page.evaluate(() => {
                    const avatar = document.querySelector('.zone-avatar > button').getBoundingClientRect();
                    const popover = document.querySelector('.popover').getBoundingClientRect();
                    const zone = document.querySelector('.zone-avatar').getBoundingClientRect();
                    return { offset: popover.right - avatar.right, top: popover.top - zone.top,
                        zoom: getComputedStyle(document.documentElement).zoom };
                });
                assert(Math.abs(geometry.offset - 0.30 * pct * 0.135) <= 1, 'ICD section17 .30rem right-edge anchor');
                assert(Math.abs(geometry.top - 3.26 * pct * 0.135) <= 1, 'stable rem vertical anchor');
                assert.equal(geometry.zoom, '1');
            }
        }
        await page.selectOption('#shell-theme-select', 'dark');
        await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
        assert.deepEqual(requests.filter(request => request.capability === 'shell.preferences.set').at(-1),
            { owner: 'A', capability: 'shell.preferences.set', args: { key: keys.theme, value: 'dark' } });
        assert.equal(await page.evaluate(() => getComputedStyle(document.body).backgroundColor),
            'rgb(11, 15, 20)', 'the dark palette changes the actual rendered body');
        assert.deepEqual(await page.evaluate(() => JSON.parse(localStorage.getItem('shellPrefs'))),
            { [keys.theme]: 'dark', [keys.scale]: 175 });
        assert.equal(requests.filter(request => request.capability === 'shell.preferences.set').length, 8);
    });
    await scenario('N01 held/failed writes do not apply or mirror', {}, async ({ page, hold, applied }) => {
        const first = hold('shell.preferences.set');
        await page.selectOption('#shell-theme-select', 'dark'); await bounded(() => first.seen.promise, 'held write seen');
        assert.equal((await applied()).theme, 'light');
        assert.equal(await page.evaluate(() => JSON.parse(localStorage.getItem('shellPrefs'))['shell.theme']), 'system');
        first.release(); await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
        const failed = hold('shell.preferences.set', 503);
        await page.selectOption('#shell-theme-select', 'light'); await bounded(() => failed.seen.promise, 'failed write seen');
        failed.release(); await page.getByRole('status').filter({ hasText: 'Preferences could not' }).waitFor();
        assert.equal((await applied()).theme, 'dark');
        assert.equal(await page.evaluate(() => JSON.parse(localStorage.getItem('shellPrefs'))['shell.theme']), 'dark');
    });
    await scenario('N02 late hydration cannot overwrite newer successful selection', {
        holdHydration: true, serverPrefs: { [keys.theme]: 'light', [keys.scale]: 150 },
    }, async ({ page, initialHydration }) => {
        await bounded(() => initialHydration.seen.promise, 'held hydration seen');
        await page.selectOption('#shell-theme-select', 'dark');
        await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
        initialHydration.release(); await page.waitForFunction(() =>
            window.__preferenceJsonCompleted.includes('/api/cap/shell.preferences.get_all'));
        assert.equal(await page.evaluate(() => document.documentElement.dataset.theme), 'dark');
        assert.equal(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize), '20.25px');
    });
    await scenario('N03 retired write/hydrate cannot apply to new managed account', {
        holdHydration: true, serverPrefs: { [keys.theme]: 'dark', [keys.scale]: 175 },
    }, async ({ page, hold, initialHydration, requests, stores, settle }) => {
        await bounded(() => initialHydration.seen.promise, 'A hydration seen');
        const write = hold('shell.preferences.set');
        await page.selectOption('#shell-theme-select', 'dark'); await bounded(() => write.seen.promise, 'A write seen');
        await page.selectOption('#shell-theme-select', 'light'); // Queued behind A's held write.
        await page.getByRole('menuitem', { name: 'Sign Out', exact: true }).click();
        await page.locator('input[name=username]').fill('beta'); await page.locator('input[name=password]').fill('fake-password');
        await page.getByRole('button', { name: 'Sign In', exact: true }).click();
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
        await page.waitForFunction(() => window.__preferenceJsonCompleted.filter(
            url => url === '/api/cap/shell.preferences.get_all').length === 1);
        initialHydration.release(); write.release();
        await page.waitForFunction(() => window.__preferenceJsonCompleted.filter(
            url => url === '/api/cap/shell.preferences.get_all').length === 2 &&
            window.__preferenceJsonCompleted.includes('/api/cap/shell.preferences.set'));
        await settle();
        assert.equal(await page.evaluate(() => document.documentElement.dataset.theme), 'light');
        assert.equal(await page.evaluate(() => getComputedStyle(document.documentElement).fontSize), '13.5px');
        assert.deepEqual(stores.get('B'), {});
        assert.deepEqual(requests.filter(request => request.capability === 'shell.preferences.set').map(request => request.owner), ['A']);
    });
    await scenario('P02 disabled storage retains successful UI effects and explicit theme', {
        disabledStorage: true,
    }, async ({ page }) => {
        await page.selectOption('#shell-theme-select', 'dark');
        await page.waitForFunction(() => document.documentElement.dataset.theme === 'dark');
        await page.emulateMedia({ colorScheme: 'dark' }); await page.emulateMedia({ colorScheme: 'light' });
        assert.equal(await page.evaluate(() => document.documentElement.dataset.theme), 'dark');
        await page.selectOption('#shell-scale-select', '125');
        await page.waitForFunction(() => getComputedStyle(document.documentElement).fontSize === '16.875px');
        assert.equal(await page.inputValue('#shell-scale-select'), '125');
    });
    await scenario('N04 login session-check 401 replaces stale form and permits retry', {
        loginProbeFailsOnce: true,
    }, async ({ page }) => {
        await page.getByRole('menuitem', { name: 'Sign Out', exact: true }).click();
        await page.locator('input[name=username]').fill('beta');
        await page.locator('input[name=password]').fill('fake-password');
        await page.getByRole('button', { name: 'Sign In', exact: true }).click();
        await page.getByText('Your session has expired. Please sign in again.', { exact: true }).waitFor();
        assert.equal(await page.getByRole('button', { name: 'Sign In', exact: true }).isEnabled(), true);
        await page.locator('input[name=username]').fill('beta');
        await page.locator('input[name=password]').fill('fake-password');
        await page.getByRole('button', { name: 'Sign In', exact: true }).click();
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    });
    await scenario('N05 malformed local mirror null falls back to documented defaults', {
        localPrefs: null,
    }, async ({ applied }) => {
        assert.deepEqual(await applied(), { theme: 'light', font: '13.5px', themeOption: 'system', scaleOption: '100' });
    });
    await scenario('N06 failed hydration retains current mirror and visible error', {
        holdHydration: true, hydrationStatus: 503, localPrefs: { [keys.theme]: 'dark', [keys.scale]: 175 },
    }, async ({ page, initialHydration, applied }) => {
        await bounded(() => initialHydration.seen.promise, 'held failed hydration'); initialHydration.release();
        await page.getByRole('status').filter({ hasText: 'Preferences could not' }).waitFor();
        assert.deepEqual(await applied(), { theme: 'dark', font: '23.625px', themeOption: 'dark', scaleOption: '175' });
    });
    await scenario('N07 rejected selection does not suppress later DB-authoritative hydration', {
        holdHydration: true, serverPrefs: { [keys.theme]: 'light' }, localPrefs: { [keys.theme]: 'dark' },
    }, async ({ page, initialHydration, hold, applied }) => {
        await bounded(() => initialHydration.seen.promise, 'held successful hydration');
        const write = hold('shell.preferences.set', 503);
        await page.selectOption('#shell-theme-select', 'dark'); await bounded(() => write.seen.promise, 'rejected write seen');
        write.release(); await page.getByRole('status').filter({ hasText: 'Preferences could not' }).waitFor();
        initialHydration.release(); await page.waitForFunction(() => document.documentElement.dataset.theme === 'light');
        assert.equal((await applied()).themeOption, 'light');
        assert.equal(await page.evaluate(() => JSON.parse(localStorage.getItem('shellPrefs'))['shell.theme']), 'light');
    });
    await scenario('P03 initial unauthenticated boot retires admission and login reactivates it', {
        startGuest: true,
    }, async ({ page }) => {
        assert.equal(await page.evaluate(async () =>
            (await import('@plinth/frontend/sdk')).getRealtimeState().status), 'failed');
        assert.equal(await page.locator('.login-error').textContent(), '');
        await page.locator('input[name=username]').fill('beta');
        await page.locator('input[name=password]').fill('fake-password');
        await page.getByRole('button', { name: 'Sign In', exact: true }).click();
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
        await page.waitForFunction(async () =>
            (await import('@plinth/frontend/sdk')).getRealtimeState().status === 'connected');
    });
    assert.equal(completed.length, 10);
    assert.deepEqual(await identity(), before, 'source unchanged throughout browser run');
} catch (error) { failure = error; }
let cleanupError;
try { if (browser) await bounded(() => browser.close(), 'browser cleanup'); assert(!browser?.isConnected()); }
catch (error) { cleanupError = error; }
if (failure || cleanupError) throw new AggregateError([failure, cleanupError].filter(Boolean), 'shell preference browser');
for (const name of completed) console.log('PASS ' + name);
console.log(JSON.stringify({ shellPreferenceCases: completed.length, expectedCases: 10, cleanup: 'PASS',
    scope: 'shipped shell/SDK/Preact; mock local auth/capabilities only; native persistence separately gated', sourceIdentity: before }));
