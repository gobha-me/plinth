import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { createServer } from 'node:http';
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const temporary = await mkdtemp(join(tmpdir(), 'plinth-browser-'));
let server;
let browser;
try {
    let baseURL = process.env.PLINTH_BASE_URL;
    if (!baseURL) {
        // Use the shipping ZIP layout, not a rewritten test-only module graph.
        let archive = process.env.PLINTH_SHELL_ZIP;
        if (!archive) {
            archive = join(temporary, 'shell.zip');
            execFileSync('cmake', ['-E', 'tar', 'cf', archive, '--format=zip', '--',
                'manifest.json', 'capabilities.json', 'rbac.json', 'panels.json',
                'config.json', 'client', 'server', 'migrations'],
            { cwd: join(repo, 'client/shell') });
        }
        execFileSync('cmake', ['-E', 'tar', 'xf', resolve(archive)], { cwd: temporary });
        const manifest = JSON.parse(await readFile(join(temporary, 'manifest.json')));
        const source = await readFile(join(repo, 'src/kernel/shell/active_frontend.cpp'), 'utf8');
        const policy = source.slice(source.indexOf('constexpr std::string_view STRICT_CSP ='))
            .match(/=\s*((?:"[^"\n]*"\s*)+);/);
        assert(policy, 'cannot read production CSP');
        const csp = [...policy[1].matchAll(/"([^"\n]*)"/g)].map(m => m[1]).join('');
        server = createServer(async (req, res) => {
            try {
                const path = new URL(req.url, 'http://localhost').pathname;
                if (path === '/') {
                    res.writeHead(302, { Location: '/app/' }).end();
                    return;
                }
                if (path === '/api/frontend/sdk.js') {
                    res.writeHead(302, {
                        Location: `/ext/shell/${manifest.version}/sdk.js`,
                        'Cache-Control': 'no-cache',
                    }).end();
                    return;
                }
                if (path === '/api/auth/session') {
                    res.writeHead(401, { 'Content-Type': 'application/json' })
                        .end(JSON.stringify({ error: 'not_authenticated' }));
                    return;
                }
                if (path === '/api/auth/registration') {
                    res.writeHead(200, { 'Content-Type': 'application/json' })
                        .end(JSON.stringify({ mode: 'disabled' }));
                    return;
                }
                const prefix = `/ext/shell/${manifest.version}/`;
                let relative;
                if (path.startsWith('/app/')) relative = path.slice(5) || 'index.html';
                if (path.startsWith(prefix)) relative = path.slice(prefix.length);
                if (!relative) { res.writeHead(404).end(); return; }
                const root = join(temporary, 'client');
                const file = resolve(root, relative);
                if (!file.startsWith(root + sep) || !(await stat(file)).isFile()) {
                    res.writeHead(404).end(); return;
                }
                const types = { '.html': 'text/html', '.js': 'application/javascript',
                    '.css': 'text/css', '.woff2': 'font/woff2' };
                let body = await readFile(file);
                if (relative === 'index.html') {
                    body = Buffer.from(body.toString().replace('<!-- PLINTH_VERSIONED_ASSET_BASE -->',
                        `<base href="${prefix}">`));
                }
                res.writeHead(200, {
                    'Content-Type': types[extname(file)] || 'application/octet-stream',
                    'Content-Security-Policy': csp,
                    'Cache-Control': 'no-cache',
                }).end(body);
            } catch { res.writeHead(404).end(); }
        });
        await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
        baseURL = `http://127.0.0.1:${server.address().port}`;
    }
    browser = await chromium.launch({
        executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    });
    for (const path of ['/', '/app/']) {
        const context = await browser.newContext();
        const page = await context.newPage();
        const failures = [];
        page.on('pageerror', error => failures.push(error.message));
        page.on('requestfailed', req => {
            if (['script', 'stylesheet', 'font'].includes(req.resourceType())) {
                failures.push(`${req.url()}: ${req.failure().errorText}`);
            }
        });
        page.on('response', res => {
            if (['script', 'stylesheet', 'font'].includes(res.request().resourceType()) && res.status() >= 400) {
                failures.push(`${res.status()} ${res.url()}`);
            }
        });
        await page.addInitScript(() => {
            window.__cspViolations = [];
            document.addEventListener('securitypolicyviolation', e => {
                window.__cspViolations.push(`${e.violatedDirective}: ${e.blockedURI}`);
            });
        });
        await page.routeWebSocket(/\/ws(?:\/events)?$/, socket => {
            socket.onMessage(message => {
                const frame = JSON.parse(message);
                if (frame.type === 'subscribe' || frame.type === 'unsubscribe') {
                    socket.send(JSON.stringify({ type: frame.type + 'd', channels: frame.channels }));
                }
            });
            socket.send(JSON.stringify({ type: 'connected' }));
        });
        const response = await page.goto(baseURL + path);
        try {
            await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
        } catch (cause) {
            const violations = await page.evaluate(() => window.__cspViolations);
            throw new Error(`Shell did not reach sign-in: ${JSON.stringify({ failures, violations })}`,
                { cause });
        }
        assert.equal(new URL(page.url()).pathname, '/app/');
        assert.equal(await page.evaluate(() => window.__PLINTH_PRODUCTION__), true);
        const csp = response.headers()['content-security-policy'];
        assert(csp, 'production document must send CSP');
        const scriptPolicy = csp.split(';').find(p => p.trim().startsWith('script-src '));
        assert(scriptPolicy.includes("'self'"));
        assert(!scriptPolicy.includes("'unsafe-inline'"));
        const mapText = await page.locator('script[type="importmap"]').textContent();
        const digest = createHash('sha256').update(mapText).digest('base64');
        assert(scriptPolicy.includes(`'sha256-${digest}'`), 'CSP must authorize exact packaged import map');
        const sdkResponse = await context.request.get(baseURL + '/api/frontend/sdk.js');
        assert(sdkResponse.ok());
        const sdkPath = new URL(sdkResponse.url()).pathname;
        assert.match(sdkPath, /^\/ext\/[^/]+\/[^/]+\/sdk\.js$/);
        assert.equal(await page.evaluate(async sdkPath => {
            const published = await import('@plinth/frontend/sdk');
            const versioned = await import(sdkPath);
            return typeof published.call === 'function' && typeof versioned.useData === 'function';
        }, sdkPath), true, 'document specifier and direct versioned SDK both import successfully');
        // Only demo backend calls are mocked: loader, Preact and SDK hooks run
        // unchanged. The realtime protocol has a separate integration gate.
        await page.route('**/ext/sdk-demo/0.1.0/panels/demo.js', route => route.fulfill({
            path: join(repo, 'tests/extensions/sdk-demo/client/panels/demo.js'),
            contentType: 'application/javascript',
        }));
        await page.route('**/api/cap/shell.preferences.get', route => route.fulfill({
            json: { ok: true, value: { value: 'browser-hook-ok' } },
        }));
        await page.evaluate(async () => {
            const container = document.createElement('div');
            container.id = 'sdk-smoke';
            document.body.append(container);
            const { loadPanel } = await import(new URL('panels/loader.js', document.baseURI).href);
            await loadPanel('sdk-demo', '0.1.0', 'demo', container, {
                panel: { id: 'demo', client_path: 'demo.js' },
            });
        });
        await page.getByRole('heading', { name: 'SDK Demo Panel' }).waitFor();
        try {
            await page.getByText('browser-hook-ok', { exact: true }).waitFor({ timeout: 10000 });
        } catch (cause) {
            throw new Error(`SDK demo failed: ${await page.locator('#sdk-smoke').textContent()}; ` +
                JSON.stringify(failures), { cause });
        }
        assert.deepEqual(await page.evaluate(() => window.__cspViolations), []);
        assert.deepEqual(failures, []);
        await context.close();
        console.log(`PASS ${path}: sign-in, strict CSP, versioned SDK, shared Preact demo + SDK hook`);
    }

    // The shell exposes only the fields supported by the data-minimal
    // registration contract. Both public modes consume the generic processed
    // response without revealing whether an account was created.
    for (const mode of ['invite', 'open']) {
        const context = await browser.newContext();
        const page = await context.newPage();
        await page.route('**/api/auth/session', route => route.fulfill({
            status: 401, json: { error: 'not_authenticated' },
        }));
        await page.route('**/api/auth/registration', route => route.fulfill({
            json: { mode },
        }));
        let submitted;
        await page.route('**/api/auth/register', async route => {
            submitted = route.request().postDataJSON();
            await route.fulfill({ status: 202, json: { status: 'processed' } });
        });
        await page.goto(baseURL + '/app/');
        await page.getByRole('button', { name: 'Create an Account', exact: true }).click();
        await page.getByRole('heading', { name: 'Create a Plinth account', exact: true }).waitFor();
        assert.equal(await page.locator('input[name="email"]').count(), 0);
        assert.equal(await page.locator('input[name="real_name"]').count(), 0);
        await page.locator('input[name="username"]').fill(`canary-${mode}`);
        await page.locator('input[name="password"]').fill('fake-canary-password');
        if (mode === 'invite') {
            await page.locator('input[name="invite_token"]').fill('fake-invite-token');
        } else {
            assert.equal(await page.locator('input[name="invite_token"]').count(), 0);
        }
        await page.getByRole('button', { name: 'Create Account', exact: true }).click();
        await page.getByRole('status').waitFor();
        assert.equal(submitted.username, `canary-${mode}`);
        assert.equal(submitted.password, 'fake-canary-password');
        assert.equal(submitted.invite_token,
            mode === 'invite' ? 'fake-invite-token' : undefined);
        assert.deepEqual(Object.keys(submitted).sort(),
            mode === 'invite'
                ? ['invite_token', 'password', 'username']
                : ['password', 'username']);
        assert.match(await page.getByRole('status').textContent(), /Registration was processed/);
        await context.close();
        console.log(`PASS data-minimal ${mode} registration form and generic completion`);
    }
    // Exercise the actual boundary emission in both configured modes. Only the
    // development config response and audit sink are test-controlled.
    for (const production of [true, false]) {
        const context = await browser.newContext();
        const page = await context.newPage();
        if (!production) {
            await page.route('**/runtime-config.js', route => route.fulfill({
                body: 'window.__PLINTH_PRODUCTION__ = false;',
                contentType: 'application/javascript',
            }));
        }
        let resolveAudit;
        const audit = new Promise(resolve => { resolveAudit = resolve; });
        await page.route('**/api/cap/shell.audit.emit', async route => {
            resolveAudit(route.request().postDataJSON().args);
            await route.fulfill({ json: { ok: true, value: null } });
        });
        await page.goto(baseURL + '/app/?force-throw=1');
        await page.getByText('Something went wrong.', { exact: true }).waitFor();
        const payload = await Promise.race([audit, new Promise((_, reject) => {
            const timeout = setTimeout(() => reject(new Error('boundary audit timed out')), 10000);
            timeout.unref();
        })]);
        assert.equal(payload.error_message, 'shell boundary test throw');
        assert.equal(Object.hasOwn(payload, 'error_stack'), !production);
        await context.close();
        console.log(`PASS boundary stack redaction: production=${production}`);
    }

    // Exercise the shipping shell's cookie-to-header path and logout state
    // transition. A rejected or interrupted mutation must not claim that a
    // still-valid server session was ended.
    for (const outcome of ['success', 'terminal', 'csrf-rejected', 'network-failed']) {
        const context = await browser.newContext();
        await context.addCookies([{
            name: 'plinth_csrf', value: `csrf-${outcome}`, url: baseURL,
        }]);
        const page = await context.newPage();
        const csrfHeaders = [];
        await page.route('**/api/auth/session', route => route.fulfill({
            json: { user: { id: 'test-user', username: 'admin' } },
        }));
        await page.route('**/api/cap/shell.preferences.get', route => {
            csrfHeaders.push(route.request().headers()['x-plinth-csrf']);
            return route.fulfill({ json: { ok: true, value: { value: null } } });
        });
        await page.route('**/api/frontend/applications', route => route.fulfill({
            json: { schema_version: 1, applications: [] },
        }));
        await page.route('**/api/auth/logout', route => {
            csrfHeaders.push(route.request().headers()['x-plinth-csrf']);
            if (outcome === 'success') return route.fulfill({ json: { ok: true } });
            if (outcome === 'terminal') {
                return route.fulfill({ status: 401, json: { error: 'session_revoked' } });
            }
            if (outcome === 'csrf-rejected') {
                return route.fulfill({
                    status: 403,
                    json: { error: 'csrf_failed', message: 'Request validation failed' },
                });
            }
            return route.abort('connectionfailed');
        });
        await page.routeWebSocket(/\/ws(?:\/events)?$/, socket => {
            socket.onMessage(message => {
                const frame = JSON.parse(message);
                if (frame.type === 'subscribe' || frame.type === 'unsubscribe') {
                    socket.send(JSON.stringify({ type: frame.type + 'd', channels: frame.channels }));
                }
            });
            socket.send(JSON.stringify({ type: 'connected' }));
        });

        await page.goto(baseURL + '/app/');
        const avatar = page.locator('.zone-avatar > button');
        await avatar.waitFor();
        await avatar.click();
        const signOut = page.getByRole('menuitem', { name: 'Sign Out', exact: true });
        const logoutRequest = page.waitForRequest('**/api/auth/logout');
        // Launcher state can update underneath the popover as its initial
        // catalog and realtime requests settle. Dispatch on the exact menu
        // element so actionability retries cannot chase a replaced node.
        await signOut.dispatchEvent('click');
        await logoutRequest;

        if (outcome === 'success' || outcome === 'terminal') {
            await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
        } else {
            await avatar.waitFor();
            assert.equal(await page.getByRole('heading', {
                name: 'Sign in to Plinth', exact: true,
            }).count(), 0);
        }
        assert(csrfHeaders.length >= 2, 'capability and logout requests must both be observed');
        assert(csrfHeaders.every(value => value === `csrf-${outcome}`));
        assert.equal((await page.locator('body').textContent()).includes(`csrf-${outcome}`), false);
        await context.close();
        console.log(`PASS CSRF header and logout state: ${outcome}`);
    }
} finally {
    await browser?.close();
    if (server) await new Promise(resolve => server.close(resolve));
    await rm(temporary, { recursive: true, force: true });
}
