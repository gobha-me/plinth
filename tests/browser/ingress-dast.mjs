// Isolated ingress proof: real installed shell/SDK through HTTPS and local ZAP.
// No credentials, response bodies, cookies, URLs or raw errors go to stdout.
import assert from 'node:assert/strict';
import { open, readFile, realpath } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { dirname, isAbsolute, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const started = Date.now();
const totalDeadline = started + 120000;
const runDeadline = totalDeadline - 12000;
const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const labels = ['ROOT', 'APP', 'SESSION', 'LOGIN', 'LOGOUT', 'REGISTRATION', 'CAP', 'WS'];
const receipt = {
    schema: 'plinth.ingress-browser.v1', completed: false, stage: 'CONFIG',
    controls: { BROWSER_AUTH: false, WS_UPGRADE: false },
    routes: Object.fromEntries(labels.map(label => [label, { requests: 0, statuses: [] }])),
    counts: { pageErrors: 0, cspViolations: 0, failedRequests: 0,
        externalRequestsBlocked: 0, externalResponses: 0, invalidWsFrames: 0 },
    proof: {}, cleanup: { contextClosed: false, browserClosed: false },
};
let browser;
let context;
let output;

function check(condition) {
    // Intentionally constant diagnostics, including on malformed config.
    assert(condition, 'isolated ingress browser control failed');
}

function fixtureUrl(value, protocol, hostname) {
    check(typeof value === 'string' && value.length > 0 && value === value.trim());
    const url = new URL(value);
    check(url.protocol === protocol && url.hostname === hostname);
    check(value === url.origin && /^[0-9]+$/.test(url.port));
    check(Number(url.port) > 0 && Number(url.port) <= 65535);
    check(!url.username && !url.password && !url.search && !url.hash && url.pathname === '/');
    return url.origin;
}

async function bounded(promise, deadline, maximum = 15000) {
    let timer;
    try {
        const remaining = Math.min(maximum, deadline - Date.now());
        check(remaining > 0);
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('isolated ingress deadline')), remaining);
        })]);
    } finally {
        clearTimeout(timer);
    }
}

function gate() {
    let fulfill;
    const promise = new Promise(resolvePromise => { fulfill = resolvePromise; });
    return { promise, fulfill };
}

function responseWait(page, predicate) {
    const promise = page.waitForResponse(predicate);
    // Preserve the original rejection for its normal await. If the preceding
    // UI action fails, context teardown cancels this already-owned waiter;
    // observe that rejection so it cannot emit an uncaught raw Playwright error.
    void promise.catch(() => {});
    return promise;
}

function routeLabel(url, origin) {
    if (url.origin !== origin) return null;
    if (url.pathname === '/') return 'ROOT';
    if (url.pathname === '/app/' || url.pathname === '/app') return 'APP';
    if (url.pathname === '/api/auth/session') return 'SESSION';
    if (url.pathname === '/api/auth/login') return 'LOGIN';
    if (url.pathname === '/api/auth/logout') return 'LOGOUT';
    if (url.pathname === '/api/auth/registration') return 'REGISTRATION';
    if (url.pathname.startsWith('/api/cap/')) return 'CAP';
    if (url.pathname === '/ws/events') return 'WS';
    return null;
}

async function prepareOutput(path) {
    check(typeof path === 'string' && isAbsolute(path));
    const absolute = resolve(path);
    const parent = await realpath(dirname(absolute));
    check(parent === dirname(absolute));
    check(!absolute.startsWith(repo + sep));
    const ownedTemporary = /^\/tmp\/plinth-issue40[^/]*\//;
    check(ownedTemporary.test(absolute));
    // Never overwrite an earlier receipt or follow a final-path symlink.
    return open(absolute, 'wx', 0o600);
}

async function run() {
    output = await prepareOutput(process.env.PLINTH_DAST_RESULT_FILE);
    const origin = fixtureUrl(process.env.PLINTH_DAST_ORIGIN, 'https:', 'plinth.test');
    const proxy = fixtureUrl(process.env.PLINTH_DAST_PROXY, 'http:', '127.0.0.1');
    check(process.env.PLINTH_DAST_USERNAME === 'issue36-admin');
    const password = process.env.PLINTH_DAST_PASSWORD;
    check(typeof password === 'string' && password.length > 0 && password.length <= 4096);
    const moduleDirectory = process.env.PLINTH_PLAYWRIGHT_MODULE_PATH ||
        resolve(repo, 'tests/browser/node_modules/playwright');
    check(isAbsolute(moduleDirectory));
    const modulePath = await realpath(moduleDirectory);
    const packageInfo = JSON.parse(await readFile(resolve(modulePath, 'package.json'), 'utf8'));
    check(packageInfo.name === 'playwright' && packageInfo.version === '1.63.0');
    // Do not let inherited debug settings log transport headers or fill values.
    delete process.env.DEBUG;
    delete process.env.PWDEBUG;
    const require = createRequire(import.meta.url);
    const { chromium } = require(modulePath);
    receipt.stage = 'BROWSER_START';
    browser = await bounded(chromium.launch({
        timeout: 15000,
        // Preserve sandboxing unless the owned local root fixture opts out.
        chromiumSandbox: process.env.PLINTH_BROWSER_NO_SANDBOX !== '1',
        args: ['--disable-background-networking', '--disable-component-update',
            '--disable-domain-reliability', '--disable-sync', '--no-first-run',
            '--metrics-recording-only', '--safebrowsing-disable-auto-update',
            '--host-resolver-rules=MAP plinth.test 127.0.0.1, MAP * ~NOTFOUND, EXCLUDE 127.0.0.1',
            '--disable-features=MediaRouter,OptimizationHints'],
    }), runDeadline);
    context = await bounded(browser.newContext({
        proxy: { server: proxy },
        // Only the explicitly validated owned fixture HTTPS origin is admitted.
        ignoreHTTPSErrors: true,
        serviceWorkers: 'block',
    }), runDeadline);
    await context.route('**/*', async route => {
        let allowed = false;
        try { allowed = new URL(route.request().url()).origin === origin; } catch { /* fail closed */ }
        if (allowed) await route.continue(); // No same-origin response substitution.
        else {
            receipt.counts.externalRequestsBlocked++;
            await route.abort('blockedbyclient');
        }
    });
    const page = await bounded(context.newPage(), runDeadline);
    page.setDefaultTimeout(10000);
    page.setDefaultNavigationTimeout(15000);
    page.on('pageerror', () => { receipt.counts.pageErrors++; });
    page.on('requestfailed', () => { receipt.counts.failedRequests++; });
    await page.exposeFunction('__plinthIngressCspCount', () => { receipt.counts.cspViolations++; });
    await page.addInitScript(() => {
        document.addEventListener('securitypolicyviolation', () => {
            void window.__plinthIngressCspCount();
        });
    });
    page.on('request', request => {
        try {
            const label = routeLabel(new URL(request.url()), origin);
            // Socket creation below owns the WS count; do not double-count a
            // browser backend that also exposes its upgrade as an HTTP request.
            if (label && label !== 'WS') receipt.routes[label].requests++;
        } catch { receipt.counts.externalRequestsBlocked++; }
    });
    page.on('response', response => {
        try {
            const url = new URL(response.url());
            if (url.origin !== origin) { receipt.counts.externalResponses++; return; }
            const label = routeLabel(url, origin);
            if (label && receipt.routes[label].statuses.length < 32) {
                receipt.routes[label].statuses.push(response.status());
            }
        } catch { receipt.counts.externalResponses++; }
    });
    const sockets = [];
    const socketReady = gate();
    const socketClosed = gate();
    const applicationsChannel = 'plinth:system:applications.changed';
    page.on('websocket', socket => {
        let url;
        try { url = new URL(socket.url()); } catch { receipt.counts.invalidWsFrames++; return; }
        if (url.protocol !== 'wss:' || url.hostname !== 'plinth.test' ||
            url.port !== new URL(origin).port || url.pathname !== '/ws/events') {
            receipt.counts.externalRequestsBlocked++;
            return;
        }
        receipt.routes.WS.requests++;
        const record = { connected: false, applicationGrant: false, closed: false,
            receivedFrames: 0, sentFrames: 0 };
        sockets.push(record);
        socket.on('framesent', () => { record.sentFrames++; });
        socket.on('framereceived', event => {
            record.receivedFrames++;
            let frame;
            try { frame = JSON.parse(String(event.payload)); }
            catch { receipt.counts.invalidWsFrames++; return; }
            if (!frame || typeof frame !== 'object' || Array.isArray(frame)) {
                receipt.counts.invalidWsFrames++;
                return;
            }
            if (frame.type === 'connected') record.connected = true;
            if (frame.type === 'subscribed' && Array.isArray(frame.channels) &&
                frame.channels.includes(applicationsChannel)) record.applicationGrant = true;
            if (record.connected && record.applicationGrant && !record.closed) socketReady.fulfill();
        });
        socket.on('close', () => {
            record.closed = true;
            if (sockets.every(candidate => candidate.closed)) socketClosed.fulfill();
        });
        socket.on('socketerror', () => { receipt.counts.invalidWsFrames++; });
    });

    receipt.stage = 'ROOT';
    const rootResponse = await bounded(page.goto(origin + '/'), runDeadline);
    check(rootResponse !== null);
    // Observe actual root behavior, including 404 or a genuine redirect.
    receipt.proof.rootNavigationFinalStatus = rootResponse.status();
    check(receipt.routes.ROOT.requests > 0 && receipt.routes.ROOT.statuses.length > 0);
    receipt.stage = 'LOGIN';
    // If root genuinely redirects to the app, keep that actual document rather
    // than canceling its owned initial session/registration requests by reload.
    const appResponse = page.url() === origin + '/app/' ? rootResponse :
        await bounded(page.goto(origin + '/app/'), runDeadline);
    check(appResponse?.status() === 200);
    await bounded(page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor(), runDeadline);
    await page.locator('input[name="username"]').fill(process.env.PLINTH_DAST_USERNAME);
    await page.locator('input[name="password"]').fill(password);
    const loginResponse = responseWait(page, response =>
        response.url() === origin + '/api/auth/login' && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Sign In', exact: true }).click();
    check((await bounded(loginResponse, runDeadline)).status() === 200);
    await bounded(page.getByRole('heading', { name: 'Home', exact: true }).waitFor(), runDeadline);
    await bounded(socketReady.promise, runDeadline);
    check(sockets.length === 1 && sockets[0].connected && sockets[0].applicationGrant && !sockets[0].closed);
    receipt.controls.WS_UPGRADE = true;
    receipt.proof.websocket = { connections: sockets.length, connected: true, applicationGrant: true };
    console.log('CASE WS_UPGRADE completed');

    receipt.stage = 'COOKIE_POLICY';
    const cookies = await context.cookies(origin);
    const sessions = cookies.filter(cookie => cookie.name === 'plinth_session');
    const csrfCookies = cookies.filter(cookie => cookie.name === 'plinth_csrf');
    check(sessions.length === 1 && csrfCookies.length === 1);
    receipt.proof.cookiePolicy = {
        sessionSecure: sessions[0].secure, sessionHttpOnly: sessions[0].httpOnly,
        sessionStrict: sessions[0].sameSite === 'Strict', csrfSecure: csrfCookies[0].secure,
        csrfReadable: !csrfCookies[0].httpOnly, csrfStrict: csrfCookies[0].sameSite === 'Strict',
        browserVisibility: await page.evaluate(() => {
            const names = document.cookie.split(';').map(part => part.trim().split('=')[0]);
            return { sessionHidden: !names.includes('plinth_session'), csrfPresent: names.includes('plinth_csrf') };
        }),
    };
    const policy = receipt.proof.cookiePolicy;
    check(policy.sessionSecure && policy.sessionHttpOnly && policy.sessionStrict &&
        policy.csrfSecure && policy.csrfReadable && policy.csrfStrict &&
        policy.browserVisibility.sessionHidden && policy.browserVisibility.csrfPresent);

    receipt.stage = 'NATIVE_HTTP';
    receipt.proof.nativeHttp = await bounded(page.evaluate(async () => {
        const result = {};
        for (const [label, path] of [['session', '/api/auth/session'],
            ['registration', '/api/auth/registration'], ['catalog', '/api/frontend/applications']]) {
            const response = await fetch(path, { credentials: 'include' });
            const body = await response.json();
            result[label] = { status: response.status, json: body !== null && typeof body === 'object' };
        }
        return result;
    }), runDeadline);
    check(Object.values(receipt.proof.nativeHttp).every(result => result.status === 200 && result.json));
    receipt.proof.missingCsrf = await bounded(page.evaluate(async () => {
        const response = await fetch('/api/cap/shell.preferences.get', {
            method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ args: { key: 'issue40.browser_probe' } }),
        });
        const body = await response.json();
        return { status: response.status, rejected: body.error === 'csrf_failed' };
    }), runDeadline);
    check(receipt.proof.missingCsrf.status === 403 && receipt.proof.missingCsrf.rejected);
    const capResponse = responseWait(page, response =>
        response.url() === origin + '/api/cap/shell.preferences.get' && response.request().method() === 'POST');
    receipt.proof.sdkCapability = await bounded(page.evaluate(async () => {
        const sdk = await import('/api/frontend/sdk.js');
        const value = await sdk.call('shell.preferences.get', { key: 'issue40.browser_probe' });
        return { canonicalSdkImported: typeof sdk.call === 'function',
            resolvedObject: value !== null && typeof value === 'object' && !Array.isArray(value) };
    }), runDeadline);
    receipt.proof.sdkCapability.status = (await bounded(capResponse, runDeadline)).status();
    check(receipt.proof.sdkCapability.status === 200 && receipt.proof.sdkCapability.canonicalSdkImported &&
        receipt.proof.sdkCapability.resolvedObject);
    console.log('CASE BROWSER_AUTH native-controls completed');

    receipt.stage = 'STYLE_POLICY';
    await page.locator('.zone-avatar > button').click();
    for (const [selector, preference, selection, expected] of [
        ['#shell-theme-select', 'shell.theme', 'light', 'light'],
        ['#shell-scale-select', 'shell.scale_pct', '125', 125],
    ]) {
        const saved = responseWait(page, response =>
            new URL(response.url()).pathname === '/api/cap/shell.preferences.set' &&
            response.request().method() === 'POST' &&
            response.request().postDataJSON()?.args?.key === preference);
        await page.locator(selector).selectOption(selection);
        const response = await bounded(saved, runDeadline);
        check(response.status() === 200 && (await response.json()).ok === true);
        await bounded(page.waitForFunction(({ preference, expected }) =>
            JSON.parse(localStorage.getItem('shellPrefs') || '{}')[preference] === expected,
        { preference, expected }), runDeadline);
    }
    await bounded(page.waitForFunction(() =>
        document.documentElement.dataset.theme === 'light' &&
        document.documentElement.style.fontSize === '16.875px'), runDeadline);
    receipt.proof.styles = { themeApplied: true, scaleApplied: true };
    check(receipt.counts.cspViolations === 0);

    receipt.stage = 'LOGOUT';
    // Restore keyboard navigation to the menu's first item after selectOption.
    await page.locator('.zone-avatar > button').click();
    await page.locator('.zone-avatar > button').click();
    await page.keyboard.press('Tab');
    check(await page.evaluate(() => document.activeElement?.id === 'shell-theme-select'));
    await page.keyboard.press('Tab');
    check(await page.evaluate(() => document.activeElement?.id === 'shell-scale-select'));
    await page.keyboard.press('Tab');
    check(await page.evaluate(() => document.activeElement?.getAttribute('role') === 'menuitem' &&
        document.activeElement?.textContent === 'Sign Out'));
    const logoutResponse = responseWait(page, response =>
        response.url() === origin + '/api/auth/logout' && response.request().method() === 'POST');
    const logoutRegistration = responseWait(page, response =>
        response.url() === origin + '/api/auth/registration' && response.request().method() === 'GET');
    await page.keyboard.press('Enter');
    check((await bounded(logoutResponse, runDeadline)).status() === 200);
    await bounded(page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor(), runDeadline);
    const registrationResponse = await bounded(logoutRegistration, runDeadline);
    check(registrationResponse.status() === 200);
    check(await bounded(registrationResponse.finished(), runDeadline) === null);
    await bounded(socketClosed.promise, runDeadline);
    check(sockets.length === 1 && sockets.every(socket => socket.closed));
    receipt.proof.logout = await bounded(page.evaluate(async () => {
        const response = await fetch('/api/auth/session', { credentials: 'include' });
        await response.text();
        return { sessionStatus: response.status };
    }), runDeadline);
    receipt.proof.logout.originalSocketClosed = true;
    check(receipt.proof.logout.sessionStatus === 401);
    check(Object.values(receipt.counts).every(count => count === 0));
    check(labels.every(label => receipt.routes[label].requests > 0));
    receipt.controls.BROWSER_AUTH = true;
    receipt.completed = true;
    receipt.stage = 'COMPLETE';
    console.log('CASE BROWSER_AUTH completed');
}

let failed = false;
try {
    // The outer owned supervisor is still required to reap Chromium descendants
    // if the process itself is killed. Internal execution reserves cleanup time.
    await bounded(run(), runDeadline, runDeadline - Date.now());
} catch {
    failed = true;
} finally {
    for (const [resource, label] of [[context, 'contextClosed'], [browser, 'browserClosed']]) {
        if (!resource) continue;
        try {
            await bounded(resource.close(), totalDeadline, 5000);
            receipt.cleanup[label] = true;
        } catch { failed = true; }
    }
    if (context && !receipt.cleanup.contextClosed) receipt.completed = false;
    if (browser && !receipt.cleanup.browserClosed) receipt.completed = false;
    if (Object.values(receipt.counts).some(count => count !== 0)) failed = true;
    if (failed) receipt.completed = false;
    receipt.elapsedMs = Date.now() - started;
    if (output) {
        try {
            await bounded(output.writeFile(JSON.stringify(receipt, null, 2) + '\n', 'utf8'), totalDeadline, 1000);
        } catch { failed = true; }
        try { await bounded(output.close(), totalDeadline, 1000); }
        catch { failed = true; }
    }
}
if (failed || !receipt.completed) {
    console.error('CASE INGRESS_BROWSER failed; private triage required');
    process.exitCode = 1;
} else {
    console.log('CASE INGRESS_BROWSER completed');
}
