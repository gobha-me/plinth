import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { mkdtemp, readFile, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, extname, join, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const shell = join(repo, 'client/shell');
const manifest = JSON.parse(await readFile(join(shell, 'manifest.json')));
const temporary = await mkdtemp(join(tmpdir(), 'plinth-launcher-browser-'));
let server;
let browser;

const panels = Array.from({ length: 10 }, (_, index) => ({
    id: `panel-${index}`, title: `Panel ${index}`,
    module_url: `/ext/notes/1.0.0/panels/panel-${index}.js`,
}));
const upgradeV1 = {
    id: 'upgrade', generation: 'upgrade-generation-1', version: '1.0.0', title: 'Upgrade',
    description: 'Generation replacement fixture', panels: [
        { id: 'editor', title: 'Editor', module_url: '/ext/upgrade/1.0.0/panels/editor.js' },
        { id: 'removed', title: 'Removed', module_url: '/ext/upgrade/1.0.0/panels/removed.js' },
    ],
};
const upgradeV2 = {
    id: 'upgrade', generation: 'upgrade-generation-2', version: '2.0.0', title: 'Upgrade',
    description: 'Generation replacement fixture', panels: [
        { id: 'editor', title: 'Editor', module_url: '/ext/upgrade/2.0.0/panels/editor.js' },
        { id: 'slow', title: 'Slow', module_url: '/ext/upgrade/2.0.0/panels/slow.js' },
    ],
};
const upgradeV3 = {
    id: 'upgrade', generation: 'upgrade-generation-3', version: '3.0.0', title: 'Upgrade',
    description: 'Failing replacement fixture', panels: [
        { id: 'editor', title: 'Editor', module_url: '/ext/upgrade/3.0.0/panels/editor.js' },
    ],
};
let catalog = { schema_version: 1, applications: [
    { id: 'notes', generation: 'notes-generation', version: '1.0.0', title: 'Notes',
        description: 'Launcher lifecycle fixture', icon: 'edit-3', panels },
    { id: 'broken', generation: 'broken-generation', version: '1.0.0', title: 'Broken',
        description: 'Failure boundary fixture', panels: [
            { id: 'main', title: 'Main', module_url: '/ext/broken/1.0.0/panels/main.js' },
        ] },
    { id: 'activation-fail', generation: 'activation-fail-generation', version: '1.0.0',
        title: 'Activation Fail', description: 'Activation cleanup fixture', panels: [
            { id: 'main', title: 'Main', module_url: '/ext/activation-fail/1.0.0/panels/main.js' },
        ] },
    { id: 'files', generation: 'files-generation', version: '1.0.0', title: 'Files',
        description: 'Single-panel fixture', panels: [
            { id: 'list', title: 'List', module_url: '/ext/files/1.0.0/panels/list.js' },
        ] },
    { id: 'vanishing', generation: 'vanishing-generation', version: '1.0.0', title: 'Vanishing',
        description: 'Import-race fixture', panels: [
            { id: 'main', title: 'Main', module_url: '/ext/vanishing/1.0.0/panels/main.js' },
        ] },
    upgradeV1,
] };
let failDiscovery = true;
let discoveryRequests = 0;
let sessionRequests = 0;
const preferenceWrites = [];
const auditWrites = [];
const upgradeModuleRequests = [];

function panelSource(id) {
    return `import { h } from 'preact';
${id === 'panel-0' ? `try { window.__PLINTH_PRODUCTION__ = false; } catch {}
try { Object.defineProperty(window, '__PLINTH_PRODUCTION__', { value: false }); } catch {}` : ''}
export default function(api) {
  const metrics = globalThis.__panelMetrics ??= { mounts: {}, activates: {}, deactivates: {} };
  metrics.mounts[${JSON.stringify(id)}] = (metrics.mounts[${JSON.stringify(id)}] || 0) + 1;
  api.onActivate(() => metrics.activates[${JSON.stringify(id)}] = (metrics.activates[${JSON.stringify(id)}] || 0) + 1);
  api.onDeactivate(() => metrics.deactivates[${JSON.stringify(id)}] = (metrics.deactivates[${JSON.stringify(id)}] || 0) + 1);
  return function FixturePanel() {
    return h('article', null,
      h('h2', null, ${JSON.stringify(id)}),
      h('button', { type: 'button', onClick: () => api.setDirty(true) }, 'Mark dirty'),
      h('button', { type: 'button', onClick: () => api.setDirty(false) }, 'Mark clean'));
  };
}`;
}

try {
    server = createServer(async (request, response) => {
        try {
            const url = new URL(request.url, 'http://localhost');
            if (url.pathname === '/') {
                response.writeHead(302, { Location: '/app/' }).end();
                return;
            }
            if (url.pathname === '/api/auth/session') {
                sessionRequests++;
                response.writeHead(200, { 'Content-Type': 'application/json' })
                    .end(JSON.stringify({ user: { id: 'fixture-user', username: 'launcher' } }));
                return;
            }
            if (url.pathname === '/api/frontend/applications') {
                discoveryRequests++;
                response.writeHead(failDiscovery ? 503 : 200, { 'Content-Type': 'application/json' })
                    .end(JSON.stringify(failDiscovery
                        ? { error: 'service_unavailable', message: 'Application discovery is temporarily unavailable' }
                        : catalog));
                return;
            }
            if (url.pathname === '/api/frontend/sdk.js') {
                response.writeHead(302, { Location: `/ext/shell/${manifest.version}/sdk.js` }).end();
                return;
            }
            if (url.pathname.startsWith('/api/cap/')) {
                let body = '';
                for await (const chunk of request) body += chunk;
                const args = JSON.parse(body || '{}').args;
                if (url.pathname.endsWith('/shell.preferences.set')) preferenceWrites.push(args.value);
                if (url.pathname.endsWith('/shell.audit.emit')) auditWrites.push(args);
                const value = url.pathname.endsWith('/shell.preferences.get')
                    ? { value: { version: 1, last_application: null, last_panels: {}, application_order: [] } }
                    : { ok: true };
                response.writeHead(200, { 'Content-Type': 'application/json' })
                    .end(JSON.stringify({ ok: true, value }));
                return;
            }
            const appPrefix = '/app/';
            const versionPrefix = `/ext/shell/${manifest.version}/`;
            let relative = null;
            if (url.pathname.startsWith(appPrefix)) relative = url.pathname.slice(appPrefix.length) || 'index.html';
            if (url.pathname.startsWith(versionPrefix)) relative = url.pathname.slice(versionPrefix.length);
            if (relative !== null) {
                const client = join(shell, 'client');
                const file = resolve(client, relative);
                if (!file.startsWith(client + sep) || !(await stat(file)).isFile()) {
                    response.writeHead(404).end(); return;
                }
                const types = { '.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css' };
                let bytes = await readFile(file);
                if (relative === 'index.html') {
                    bytes = Buffer.from(bytes.toString().replace('<!-- PLINTH_VERSIONED_ASSET_BASE -->',
                        `<base href="${versionPrefix}">`));
                }
                response.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream' })
                    .end(bytes);
                return;
            }
            response.writeHead(404).end();
        } catch (error) {
            response.writeHead(500).end(String(error));
        }
    });
    await new Promise(resolveListen => server.listen(0, '127.0.0.1', resolveListen));
    const baseURL = `http://127.0.0.1:${server.address().port}`;
    browser = await chromium.launch({
        executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    });
    const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const page = await context.newPage();
    const cdp = await context.newCDPSession(page);
    await cdp.send('Runtime.enable');
    const sockets = [];
    const errors = [];
    let releaseSlowPanel;
    let slowPanelPending = true;
    const slowPanel = new Promise(resolve => { releaseSlowPanel = resolve; });
    page.on('pageerror', error => errors.push(error.stack || error.message));
    cdp.on('Runtime.exceptionThrown', event => {
        const detail = event.exceptionDetails;
        errors.push(`exception: ${detail.url}:${detail.lineNumber + 1}:${detail.columnNumber + 1} ${detail.text}`);
    });
    await page.route('**/ext/notes/1.0.0/panels/*.js', route => {
        const id = new URL(route.request().url()).pathname.split('/').at(-1).replace('.js', '');
        if (id === 'panel-8' && slowPanelPending) {
            slowPanelPending = false;
            return slowPanel.then(() => route.fulfill({ body: panelSource(id), contentType: 'application/javascript' }));
        }
        return route.fulfill({ body: panelSource(id), contentType: 'application/javascript' });
    });
    await page.route('**/ext/files/1.0.0/panels/list.js', route =>
        route.fulfill({ body: panelSource('files-list'), contentType: 'application/javascript' }));
    await page.route('**/ext/activation-fail/1.0.0/panels/main.js', route => route.fulfill({
        body: `import { h } from 'preact';
export default function(api) {
  const metrics = globalThis.__panelMetrics ??= { mounts: {}, activates: {}, deactivates: {} };
  api.onDeactivate(() => metrics.deactivates['activation-fail'] =
    (metrics.deactivates['activation-fail'] || 0) + 1);
  api.onActivate(() => { throw new Error('activation secret must not be logged'); });
  return function ActivationFailure() { return h('h2', null, 'activation-fail'); };
}`,
        contentType: 'application/javascript',
    }));
    await page.route('**/ext/vanishing/1.0.0/panels/main.js', route => {
        catalog = { ...catalog,
            applications: catalog.applications.filter(application => application.id !== 'vanishing') };
        return route.fulfill({ status: 404, body: 'gone', contentType: 'text/plain' });
    });
    await page.route('**/ext/upgrade/*/panels/*.js', route => {
        const path = new URL(route.request().url()).pathname;
        upgradeModuleRequests.push(path);
        if (path === '/ext/upgrade/2.0.0/panels/slow.js') {
            catalog = { ...catalog,
                applications: catalog.applications.filter(application => application.id !== 'upgrade') };
            return route.fulfill({ status: 404, body: 'generation quiesced', contentType: 'text/plain' });
        }
        if (path === '/ext/upgrade/3.0.0/panels/editor.js') {
            return route.fulfill({
                body: "export default function(){ throw new Error('new generation failed'); }",
                contentType: 'application/javascript',
            });
        }
        const segments = path.split('/');
        const version = segments[3];
        const panelId = segments.at(-1).replace('.js', '');
        return route.fulfill({
            body: panelSource(`upgrade-${version}-${panelId}`),
            contentType: 'application/javascript',
        });
    });
    await page.route('**/ext/broken/1.0.0/panels/main.js', route => route.fulfill({
        body: "export default function(){ return function Broken(){ throw new Error('SECRET fixture render failure'); }; }",
        contentType: 'application/javascript',
    }));
    await page.routeWebSocket(/\/ws\/events$/, socket => {
        sockets.push(socket);
        socket.onMessage(message => {
            const frame = JSON.parse(message);
            if (frame.type === 'subscribe' || frame.type === 'unsubscribe') {
                socket.send(JSON.stringify({ type: frame.type + 'd', channels: frame.channels }));
            }
        });
        socket.send(JSON.stringify({ type: 'connected' }));
    });

    await page.goto(baseURL + '/app/');
    try {
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor({ timeout: 5000 });
    } catch (cause) {
        throw new Error(`launcher did not start: ${JSON.stringify({ errors, body: await page.locator('body').innerText() })}`,
            { cause });
    }
    await page.getByRole('button', { name: 'Retry', exact: true }).waitFor();
    failDiscovery = false;
    await page.getByRole('button', { name: 'Retry', exact: true }).click();
    try {
        await page.getByRole('button', { name: 'Notes', exact: true }).waitFor({ timeout: 5000 });
    } catch (cause) {
        throw new Error(`catalog did not load: ${JSON.stringify({ errors, sockets: sockets.length,
            body: await page.locator('body').innerText() })}`, { cause });
    }
    assert.equal(sockets.length, 1,
        'the shell and launcher must share the canonical SDK realtime connection');
    assert.equal(await page.locator('[data-ipoint="shell.topbar"]').count(), 1);
    assert.equal(await page.locator('[data-ipoint="shell.home.launcher"]').count(), 1);
    assert.equal(await page.getByRole('button', { name: 'Show ownership', exact: true }).count(), 0,
        'production configuration must not expose the ownership overlay');

    const requestsBeforeImportFailure = discoveryRequests;
    await page.getByRole('button', { name: 'Vanishing', exact: true }).click();
    await page.getByText('Application unavailable', { exact: true }).waitFor();
    assert.equal(discoveryRequests, requestsBeforeImportFailure + 1,
        'a versioned module import failure must perform exactly one authoritative catalog refresh');

    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.getByRole('heading', { name: 'panel-0', exact: true }).waitFor();
    assert.equal(await page.evaluate(() => window.__PLINTH_PRODUCTION__), true,
        'panel code must not be able to mutate the production-mode gate');
    assert.equal(await page.getByRole('button', { name: 'Show ownership', exact: true }).count(), 0,
        'a malicious panel must not enable the development ownership overlay');
    assert.equal(await page.locator('.launcher-tile[aria-label="Notes"] [data-icon-token="edit-3"]').count(), 1,
        'known application icon tokens must render through the shell icon vocabulary');
    assert.equal(await page.locator('[data-ipoint="shell.appIdentity"] [data-icon-token="edit-3"]').count(), 1,
        'the active application identity must use its returned icon token');
    assert.equal(await page.getByRole('tab', { name: 'Panel 0', exact: true })
        .locator('[data-icon-token="edit-3"]').count(), 1,
    'a panel without its own known icon must inherit the application icon');
    const identityStyles = await page.locator('.launcher-tile .app-monogram').evaluateAll(elements =>
        elements.slice(0, 3).map(element => element.style.getPropertyValue('--app-color')));
    assert(new Set(identityStyles).size > 1,
        'application identity color must be stable per package rather than one shared accent');
    assert.equal(await page.getByRole('tab').count(), 10);
    assert.equal(await page.locator('[data-ipoint="ext.notes.primaryTabs"][data-ipoint-layer="extension"]').count(), 1);
    assert.equal(await page.locator('[data-ipoint="ext.notes.panel-0.primaryPane"]').count(), 1);
    await page.getByRole('tab', { name: 'Panel 1', exact: true }).focus();
    await page.keyboard.press('Enter');
    await page.getByRole('heading', { name: 'panel-1', exact: true }).waitFor();
    await page.getByRole('tab', { name: 'Panel 0', exact: true }).click();
    assert.equal(await page.evaluate(() => window.__panelMetrics.mounts['panel-0']), 1,
        'retained panel must not remount');

    await page.locator('[data-ipoint="shell.appIdentity"]').click();
    await page.getByRole('menuitem', { name: 'Activation Fail', exact: true }).click();
    await page.waitForFunction(() =>
        window.__panelMetrics?.deactivates?.['activation-fail'] === 1);
    assert.equal(await page.getByRole('heading', { name: 'panel-0', exact: true }).count(), 1,
        'activation failure must restore the prior clean panel');
    assert.equal(await page.evaluate(() => window.__panelMetrics.deactivates['activation-fail']), 1,
        'an active candidate destroyed after activation failure must deactivate exactly once');
    assert.equal(await page.locator('[data-ipoint="ext.activation-fail.main.primaryPane"]').count(), 0);

    await page.getByRole('tab', { name: 'Panel 8', exact: true }).click();
    await page.getByRole('tab', { name: 'Panel 9', exact: true }).click();
    await page.getByRole('heading', { name: 'panel-9', exact: true }).waitFor();
    releaseSlowPanel();
    await page.waitForTimeout(50);
    assert.equal(await page.getByRole('heading', { name: 'panel-9', exact: true }).count(), 1,
        'stale import completion must not replace the newest target');

    await page.getByRole('button', { name: 'Mark dirty', exact: true }).click();
    await page.getByRole('button', { name: 'Home', exact: true }).click();
    await page.getByRole('dialog').waitFor();
    await page.getByRole('button', { name: 'Cancel', exact: true }).click();
    assert.equal(await page.getByRole('heading', { name: 'panel-9', exact: true }).count(), 1);
    await page.getByRole('button', { name: 'Home', exact: true }).click();
    await page.getByRole('button', { name: 'Discard', exact: true }).click();
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();

    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    for (let index = 1; index < 10; ++index) {
        await page.getByRole('tab', { name: `Panel ${index}`, exact: true }).click();
        await page.getByRole('heading', { name: `panel-${index}`, exact: true }).waitFor();
    }
    assert((await page.locator('.panel-container').count()) <= 9,
        'panel cache must retain at most active plus eight inactive instances');

    await page.setViewportSize({ width: 390, height: 780 });
    const tabs = await page.locator('[role="tablist"]').evaluate(element => ({
        clientWidth: element.clientWidth, scrollWidth: element.scrollWidth,
    }));
    assert(tabs.scrollWidth > tabs.clientWidth, 'ten tabs must use the bounded horizontal scroller');
    await page.locator('[data-ipoint="shell.appIdentity"]').click();
    const switcherBox = await page.locator('[data-ipoint="shell.appSwitcher"]').boundingBox();
    assert(Math.abs(switcherBox.y + switcherBox.height - 780) < 2, 'narrow switcher must anchor as a bottom sheet');
    await page.keyboard.press('Escape');
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= document.documentElement.clientWidth), true);
    await page.setViewportSize({ width: 1280, height: 800 });

    await page.getByRole('button', { name: 'Home', exact: true }).click();
    await page.getByRole('button', { name: 'Broken', exact: true }).click();
    await page.getByText('This panel could not be displayed.', { exact: true }).waitFor();
    for (let attempts = 0;
        attempts < 100 && !auditWrites.some(detail => detail.application_id === 'broken');
        ++attempts) {
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    const brokenAudit = auditWrites.find(detail => detail.application_id === 'broken');
    assert.equal(brokenAudit?.panel_id, 'main');
    assert.equal(brokenAudit?.error_message, 'Panel lifecycle failure');
    assert.equal(JSON.stringify(auditWrites).includes('SECRET'), false,
        'production audit payload must redact panel-controlled exception detail');
    await page.getByRole('button', { name: 'Return to Home', exact: true }).click();

    failDiscovery = true;
    const requestsBeforeStaleRefresh = discoveryRequests;
    sockets.at(-1).send(JSON.stringify({ type: 'event', channel: 'plinth:system:applications.changed', payload: {} }));
    try {
        await page.getByText('Application list may be out of date.', { exact: true }).waitFor();
    } catch (cause) {
        throw new Error(`stale refresh did not settle: ${JSON.stringify({
            requestsBeforeStaleRefresh, discoveryRequests, body: await page.locator('body').innerText(), errors,
        })}`, { cause });
    }
    assert.equal(await page.getByRole('button', { name: 'Notes', exact: true }).count(), 1);
    failDiscovery = false;
    sockets.at(-1).send(JSON.stringify({ type: 'event', channel: 'plinth:system:applications.changed', payload: {} }));
    await page.getByRole('button', { name: 'Files', exact: true }).click();
    await page.getByRole('heading', { name: 'files-list', exact: true }).waitFor();

    catalog = { ...catalog, applications: catalog.applications.filter(application => application.id !== 'files') };
    sockets.at(-1).send(JSON.stringify({ type: 'event', channel: 'plinth:system:applications.changed', payload: {} }));
    await page.getByText('Application unavailable', { exact: true }).waitFor();
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    assert.equal(await page.getByRole('heading', { name: 'files-list', exact: true }).count(), 0);
    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.getByRole('heading', { name: 'panel-9', exact: true }).waitFor();

    await page.getByRole('button', { name: 'Mark dirty', exact: true }).click();
    await page.getByRole('tab', { name: 'Panel 8', exact: true }).click();
    await page.getByRole('dialog').waitFor();
    await page.getByRole('button', { name: 'Discard', exact: true }).click();
    await page.getByRole('heading', { name: 'panel-8', exact: true }).waitFor();
    assert.equal(await page.getByRole('tab', { name: 'Panel 8', exact: true })
        .evaluate(element => element === document.activeElement), true,
        'a dirty-confirmed tab activation must restore focus to the selected tab');

    await page.getByRole('button', { name: 'Mark dirty', exact: true }).click();
    await page.getByRole('tab', { name: 'Panel 9', exact: true }).click();
    await page.getByRole('dialog').waitFor();
    const notesApplication = catalog.applications.find(application => application.id === 'notes');
    catalog = { ...catalog, applications: catalog.applications.filter(application => application.id !== 'notes') };
    sockets.at(-1).send(JSON.stringify({ type: 'event', channel: 'plinth:system:applications.changed', payload: {} }));
    await page.getByRole('dialog').waitFor({ state: 'detached' });
    const homeHeading = page.getByRole('heading', { name: 'Home', exact: true });
    await homeHeading.waitFor();
    assert.equal(await homeHeading.evaluate(element => element === document.activeElement), true,
        'forced removal must dismiss dirty confirmation and focus Home');
    assert.equal(await page.locator('[data-ipoint^="ext."][data-ipoint$=".primaryPane"]').count(), 0,
        'forced removal must not retain dirty extension DOM');

    catalog = { ...catalog, applications: [notesApplication, ...catalog.applications] };
    sockets.at(-1).send(JSON.stringify({ type: 'event', channel: 'plinth:system:applications.changed', payload: {} }));
    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.getByRole('heading', { name: 'panel-8', exact: true }).waitFor();

    await page.getByRole('button', { name: 'Home', exact: true }).click();
    await page.getByRole('button', { name: 'Upgrade', exact: true }).click();
    await page.getByRole('heading', { name: 'upgrade-1.0.0-editor', exact: true }).waitFor();
    await page.getByRole('tab', { name: 'Removed', exact: true }).click();
    await page.getByRole('heading', { name: 'upgrade-1.0.0-removed', exact: true }).waitFor();
    await page.getByRole('button', { name: 'Mark dirty', exact: true }).click();

    catalog = { ...catalog,
        applications: catalog.applications.filter(application => application.id !== 'upgrade') };
    sockets.at(-1).send(JSON.stringify({
        type: 'event', channel: 'plinth:system:applications.changed', payload: {},
    }));
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    assert.equal(await page.getByRole('heading', { name: 'upgrade-1.0.0-removed', exact: true }).count(), 0,
        'upgrade quiesce must force-remove the dirty old generation without a prompt');
    assert.equal(await page.getByRole('dialog').count(), 0,
        'topology removal must not leave a dirty-discard dialog behind');

    catalog = { ...catalog, applications: [...catalog.applications, upgradeV2] };
    sockets.at(-1).send(JSON.stringify({
        type: 'event', channel: 'plinth:system:applications.changed', payload: {},
    }));
    await page.getByRole('button', { name: 'Upgrade', exact: true }).click();
    await page.getByRole('heading', { name: 'upgrade-2.0.0-editor', exact: true }).waitFor();
    assert.equal(upgradeModuleRequests.at(-1), '/ext/upgrade/2.0.0/panels/editor.js',
        'the stable application id must load the replacement generation versioned URL');
    assert.equal(await page.getByRole('tab', { name: 'Editor', exact: true }).getAttribute('aria-selected'), 'true',
        'a remembered panel removed by the new generation must fall back to its first panel');

    await page.getByRole('button', { name: 'Mark dirty', exact: true }).click();
    await page.getByRole('tab', { name: 'Slow', exact: true }).click();
    await page.getByRole('dialog').waitFor();
    const requestsBeforeOldImportRace = discoveryRequests;
    await page.getByRole('button', { name: 'Discard', exact: true }).click();
    await page.getByText('Application unavailable', { exact: true }).waitFor();
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    assert.equal(discoveryRequests, requestsBeforeOldImportRace + 1,
        'an old-generation import failure must perform one authoritative refetch');
    assert.equal(await page.locator('[data-ipoint^="ext.upgrade."][data-ipoint$=".primaryPane"]').count(), 0,
        'a racing old-generation import must not resurrect stale or dirty DOM');

    catalog = { ...catalog, applications: [...catalog.applications, upgradeV3] };
    sockets.at(-1).send(JSON.stringify({
        type: 'event', channel: 'plinth:system:applications.changed', payload: {},
    }));
    await page.getByRole('button', { name: 'Upgrade', exact: true }).click();
    await page.locator('.panel-load-failure').waitFor();
    assert.equal(upgradeModuleRequests.at(-1), '/ext/upgrade/3.0.0/panels/editor.js');
    assert.equal(await page.getByRole('heading', { name: /upgrade-(1\.0\.0|2\.0\.0)-/ }).count(), 0,
        'a failed new generation must never resurrect either old generation');
    await page.getByRole('button', { name: 'Return to Home', exact: true }).click();

    const sessionsBeforeUnknownTerminal = sessionRequests;
    const socketsBeforeUnknownTerminal = sockets.length;
    sockets.at(-1).send(JSON.stringify({ type: 'error', error: 'future_terminal_code' }));
    for (let attempts = 0;
        attempts < 200 && (sessionRequests !== sessionsBeforeUnknownTerminal + 1 ||
            sockets.length <= socketsBeforeUnknownTerminal); ++attempts) {
        await new Promise(resolve => setTimeout(resolve, 10));
    }
    assert.equal(sessionRequests, sessionsBeforeUnknownTerminal + 1,
        'an unknown terminal code must use one bounded HTTP authority revalidation');
    assert(sockets.length > socketsBeforeUnknownTerminal,
        'an unknown terminal code must reconnect realtime after valid-session revalidation');
    await page.getByRole('button', { name: 'Notes', exact: true }).waitFor();
    assert.equal(await page.locator('[data-ipoint^="ext."][data-ipoint$=".primaryPane"]').count(), 0,
        'unknown terminal recovery must not restore stale panel DOM');

    sockets.at(-1).send(JSON.stringify({ type: 'error', error: 'already_connected' }));
    await page.getByRole('button', { name: 'Use this tab', exact: true }).waitFor();
    assert.equal(await page.locator('[data-ipoint^="ext."][data-ipoint$=".primaryPane"]').count(), 0,
        'terminal realtime state must remove extension DOM');

    assert(preferenceWrites.length >= 3, 'committed navigation must persist launcher preferences');
    assert.deepEqual(errors, []);
    await context.close();
    console.log('PASS launcher catalog, tabs, retention/LRU, dirty discard, boundary, stale refresh, terminal cleanup');
} finally {
    await browser?.close();
    if (server) await new Promise(resolveClose => server.close(resolveClose));
    await rm(temporary, { recursive: true, force: true });
}
