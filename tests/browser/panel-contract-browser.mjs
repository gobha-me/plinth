// Real vendored Preact, PanelManager/PanelApi and SDK graph; panel modules are
// explicitly local fixtures. No native backend or supported navigation claim.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { dirname, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const client = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const sources = ['panels/loader.js', 'panels/panel_api.js', 'sdk.js', 'data-query.js',
    'data-controller.js', 'vendor/preact.module.js', 'vendor/preact-hooks.module.js'];
const identity = async () => Object.fromEntries(await Promise.all(sources.map(async path =>
    [path, createHash('sha256').update(await readFile(resolve(client, path))).digest('hex')])));
async function bounded(operation, label, milliseconds = 5000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline ${milliseconds}ms`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
const html = `<!doctype html><meta charset="utf-8"><div id="host"></div><div id="compat"></div>
<script type="importmap">{"imports":{"preact":"/vendor/preact.module.js",
"preact/hooks":"/vendor/preact-hooks.module.js"}}</script>
<script type="module" src="/harness.js"></script>`;
const panel = name => `import { h } from 'preact';
export default function(api) {
  window.__panel.apis[${JSON.stringify(name)}] = api;
  api.onActivate(() => window.__panel.trace.push(${JSON.stringify(`activate:${name}`)}));
  api.onDeactivate(() => window.__panel.trace.push(${JSON.stringify(`deactivate:${name}`)}));
  api.onNavigationIntent(() => window.__panel.navigationIntents++);
  return function LocalContractPanel() { return h('div', { 'data-local-panel': ${JSON.stringify(name)} },
    h('h2', null, ${JSON.stringify(`Local ${name}`)}),
    h('button', { onClick: () => api.setDirty(true) }, 'Dirty true'),
    h('button', { onClick: () => api.setDirty(false) }, 'Dirty false')); };
}`;
const harness = `
import { render } from 'preact';
import { PanelManager, loadPanel, __activePanelForTest } from '/panels/loader.js';
import { retireRealtimeSession } from '/sdk.js';
const host = document.getElementById('host'), compat = document.getElementById('compat');
const target = name => ({ applicationId: 'fixture', generation: 'local-' + name, version: '1.0.0',
    panel: { id: name, module_url: '/fixture/' + name + '.js' },
    tabId: 'tab-' + name, paneId: 'pane-' + name });
const manager = new PanelManager(host, {
    onFailure: (_target, error) => window.__panel.failures.push(error.name),
    onDirtyChange: dirty => window.__panel.dirty.push(dirty),
});
const instances = new Map();
let abaManager, abaHost, abaCurrent, resolveRetired, retiredOutcome, retiredFailureActions;
const errors = error => ({ name: error.name, message: error.message });
window.__panel = {
    apis: {}, trace: [], dirty: [], shortcuts: [], failures: [], failureActions: [], navigationIntents: 0,
    async prepare(name, context) {
        const instance = await manager.prepare(target(name), context === undefined ? {} : { context });
        instances.set(name, instance); return instance.api.getContext();
    },
    commit(name) { manager.commit(instances.get(name)); },
    state() { return { trace: [...this.trace], dirty: [...this.dirty], shortcuts: [...this.shortcuts],
        failures: [...this.failures], navigationIntents: this.navigationIntents,
        active: manager.activeTarget?.panel.id || null, activeDirty: manager.activeDirty,
        unloadInstalled: manager.beforeUnloadInstalled,
        contexts: Object.fromEntries([...instances].map(([name, instance]) => [name, instance.api.getContext()])) }; },
    beforeUnload() { const event = new Event('beforeunload', { cancelable: true });
        window.dispatchEvent(event); return event.defaultPrevented; },
    invalidDirty() { try { this.apis.a.setDirty('invalid'); } catch (error) { return errors(error); } },
    home() { manager.deactivateToHome(); },
    register() {
        this.apis.a.registerShortcut('Shift+Ctrl+Alt+Meta+K', event => this.shortcuts.push(['a', event.defaultPrevented]));
        this.apis.b.registerShortcut('Meta+Alt+Shift+Ctrl+K', event => this.shortcuts.push(['b', event.defaultPrevented]));
        this.apis.b.registerShortcut('Ctrl+S', event => { event.preventDefault(); this.shortcuts.push(['suppress', event.defaultPrevented]); });
        try { this.apis.a.registerShortcut('Meta+Shift+Alt+Ctrl+K', () => {}); }
        catch (error) { return errors(error); }
    },
    key(key, settings = {}) {
        const event = new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true, ...settings });
        document.dispatchEvent(event); return event.defaultPrevented;
    },
    staleUnregister() {
        const callback = event => this.shortcuts.push(['replacement', event.defaultPrevented]);
        const stale = this.apis.a.registerShortcut('Ctrl+R', callback);
        stale(); this.apis.a.registerShortcut('Ctrl+R', callback); stale(); stale();
    },
    async stubs() {
        const result = {};
        for (const method of ['navigate', 'requestFocus', 'setTrayState', 'setTrayBadge']) {
            try { this.apis.a[method]('mock-target'); }
            catch (error) { result[method] = errors(error); }
        }
        try { await this.apis.a.openFloat('mock-content', {}); }
        catch (error) { result.openFloat = errors(error); }
        return result;
    },
    async compatibility() {
        const context = { caller: 'local-compat', value: 7 };
        const options = { panel: { id: 'compat', module_url: '/fixture/compat.js' }, context };
        const first = await loadPanel('fixture', '1.0.0', 'compat', compat, options);
        const second = await loadPanel('fixture', '1.0.0', 'compat', compat,
            { ...options, context: { caller: 'replacement' } });
        return { context: first.panelApi.getContext(), sameInstance: first.instance === second.instance,
            sameContext: first.panelApi.getContext() === context && second.panelApi.getContext() === context,
            active: __activePanelForTest() === first.instance };
    },
    async prepareReplacement() {
        abaHost = document.createElement('div'); abaHost.id = 'aba-host'; document.body.append(abaHost);
        const held = new Promise(resolveValue => { resolveRetired = resolveValue; });
        abaManager = new PanelManager(abaHost, { importModule: url =>
            url.endsWith('/aba-retired.js') ? held : import(url),
            onFailure: (_target, error) => this.failures.push(error.name),
            onRetry: target => this.failureActions.push('retry:' + target.panel.module_url),
            onHome: () => this.failureActions.push('home') });
        const sameKey = name => ({ applicationId: 'fixture', generation: 'same-aba-generation', version: '1.0.0',
            panel: { id: 'aba', module_url: '/fixture/' + name + '.js' }, tabId: 'tab-aba', paneId: 'pane-aba' });
        retiredOutcome = abaManager.prepare(sameKey('aba-retired')).then(
            () => ({ status: 'fulfilled' }), error => ({ status: 'rejected', name: error.name }));
        const retired = [...abaManager.instances.values()][0];
        // Retain the actual fallback's existing callback closures before A is
        // retired; they must not gain permission from B's identical map key.
        retiredFailureActions = abaManager.failureView(retired).props;
        abaManager.destroyAll();
        abaCurrent = await abaManager.prepare(sameKey('aba-current'), { context: { owner: 'current-B' } });
        abaManager.commit(abaCurrent);
        return this.replacementState();
    },
    async releaseRetired() {
        resolveRetired(await import('/fixture/aba-retired.js'));
        const outcome = await retiredOutcome;
        return { ...this.replacementState(), outcome };
    },
    invokeFailureActions(retired) {
        const actions = retired ? retiredFailureActions : abaManager.failureView(abaCurrent).props;
        actions.onRetry(); actions.onHome(); return [...this.failureActions];
    },
    replacementState() {
        return { retiredFactoryAdmitted: Object.hasOwn(this.apis, 'aba-retired'),
            registeredCurrent: abaManager.instances.get(abaCurrent.key) === abaCurrent,
            activeCurrent: abaManager.active === abaCurrent, connected: abaCurrent.container.isConnected,
            count: abaManager.instances.size, dirty: abaManager.activeDirty,
            unloadInstalled: abaManager.beforeUnloadInstalled,
            context: abaCurrent.api.getContext(),
            trace: this.trace.filter(entry => entry.endsWith(':aba-current')) };
    },
    async reactivateReplacement() {
        abaManager.deactivateToHome();
        const retained = await abaManager.prepare(abaCurrent.target, { context: { owner: 'ignored' } });
        if (retained !== abaCurrent) throw new Error('current replacement was not retained');
        abaManager.commit(retained); retained.container.focus();
        return { ...this.replacementState(), focused: document.activeElement === retained.container };
    },
    cleanup() {
        manager.dispose();
        abaManager?.dispose(); resolveRetired?.(null); abaHost?.remove();
        const compatibility = __activePanelForTest();
        // loadPanel's private compatibility singleton is context-owned. Unbind
        // and remove its fixture view here; verified context closure releases
        // the singleton listener itself without adding a production test seam.
        compatibility?.api.__shell_internal.unbind();
        if (compatibility) render(null, compatibility.container);
        host.replaceChildren(); compat.replaceChildren();
        retireRealtimeSession('test_cleanup');
        return { managerEmpty: manager.instances.size === 0 && (!abaManager || abaManager.instances.size === 0),
            inactive: manager.active === null && (!abaManager || abaManager.active === null),
            unloadRemoved: !manager.beforeUnloadInstalled && !abaManager?.beforeUnloadInstalled,
            domEmpty: !host.childNodes.length && !compat.childNodes.length && !abaHost?.childNodes.length };
    },
};
`;

const before = await identity();
let browser, context, page, server, failure;
const connections = new Set(), cleanupErrors = [], fixtureErrors = [], pageErrors = [], dialogs = [], completed = [];
try {
    server = createServer(async (request, response) => {
        try {
            const path = new URL(request.url, 'http://localhost').pathname;
            if (path === '/favicon.ico') return response.writeHead(204).end();
            if (path === '/') return response.writeHead(200, { 'Content-Type': 'text/html' }).end(html);
            if (path === '/harness.js') return response.writeHead(200,
                { 'Content-Type': 'application/javascript' }).end(harness);
            if (path.startsWith('/fixture/')) {
                const name = path.slice('/fixture/'.length, -3);
                assert(['a', 'b', 'compat', 'aba-retired', 'aba-current'].includes(name));
                return response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(panel(name));
            }
            const file = resolve(client, `.${path}`); assert(file.startsWith(client + sep));
            response.writeHead(200, { 'Content-Type': 'application/javascript' }).end(await readFile(file));
        } catch (error) {
            fixtureErrors.push(error.message); if (!response.headersSent) response.writeHead(500); response.end();
        }
    });
    server.on('connection', socket => { connections.add(socket); socket.on('close', () => connections.delete(socket)); });
    await bounded(() => new Promise((done, reject) => {
        server.once('error', reject); server.listen(0, '127.0.0.1', done);
    }), 'panel server listen');
    browser = await bounded(() => chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] }), 'Chromium launch', 15000);
    context = await bounded(() => browser.newContext(), 'panel browser context');
    page = await context.newPage(); page.setDefaultTimeout(5000);
    page.on('pageerror', error => pageErrors.push(error.message));
    page.on('dialog', dialog => { dialogs.push(dialog.type()); void dialog.dismiss(); });
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => Boolean(window.__panel));
    assert.deepEqual(await page.evaluate(() => window.__panel.prepare('a')), {});
    await page.evaluate(() => window.__panel.commit('a'));
    assert.deepEqual((await page.evaluate(() => window.__panel.state())).trace, ['activate:a']);
    assert.deepEqual(await page.evaluate(() => window.__panel.prepare('b', { caller: 'B', value: 3 })), { caller: 'B', value: 3 });
    await page.evaluate(() => window.__panel.commit('b'));
    await page.evaluate(() => window.__panel.commit('a'));
    assert.deepEqual((await page.evaluate(() => window.__panel.state())).trace,
        ['activate:a', 'deactivate:a', 'activate:b', 'deactivate:b', 'activate:a']);
    assert.deepEqual(await page.evaluate(() => window.__panel.prepare('b', { caller: 'ignored' })), { caller: 'B', value: 3 });
    completed.push('P32.01 exact activation/reactivation order and cached context');

    await page.locator('[data-local-panel="a"]').getByRole('button', { name: 'Dirty true', exact: true }).click();
    let state = await page.evaluate(() => window.__panel.state());
    assert.equal(state.activeDirty, true); assert.equal(state.unloadInstalled, true);
    assert.equal(await page.evaluate(() => window.__panel.beforeUnload()), true);
    await page.locator('[data-local-panel="a"]').getByRole('button', { name: 'Dirty false', exact: true }).click();
    state = await page.evaluate(() => window.__panel.state());
    assert.deepEqual(state.dirty, [true, false]); assert.equal(state.activeDirty, false); assert.equal(state.unloadInstalled, false);
    assert.equal(await page.evaluate(() => window.__panel.beforeUnload()), false);
    assert.deepEqual(await page.evaluate(() => window.__panel.invalidDirty()),
        { name: 'TypeError', message: 'setDirty: expected boolean, got string' });
    await page.evaluate(() => window.__panel.home());
    assert.equal((await page.evaluate(() => window.__panel.state())).active, null);
    assert.equal(await page.locator('.panel-container:not([hidden])').count(), 0);
    assert.deepEqual(dialogs, []); completed.push('P32.02 public dirty true/false removes guard and permits Home');

    await page.evaluate(() => window.__panel.commit('a'));
    assert.deepEqual(await page.evaluate(() => window.__panel.register()),
        { name: 'ShortcutConflictError', message: "combo Alt+Ctrl+Meta+Shift+K already registered by panel 'a'" });
    const modifiers = { ctrlKey: true, altKey: true, shiftKey: true, metaKey: true };
    assert.equal(await page.evaluate(settings => window.__panel.key('k', settings), modifiers), false);
    await page.evaluate(() => window.__panel.commit('b'));
    assert.equal(await page.evaluate(settings => window.__panel.key('k', settings), modifiers), false);
    assert.equal(await page.evaluate(() => window.__panel.key('s', { ctrlKey: true })), true);
    await page.evaluate(() => { window.__panel.commit('a'); window.__panel.staleUnregister(); });
    assert.equal(await page.evaluate(() => window.__panel.key('r', { ctrlKey: true })), false);
    assert.deepEqual((await page.evaluate(() => window.__panel.state())).shortcuts,
        [['a', false], ['b', false], ['suppress', true], ['replacement', false]]);
    completed.push('P32.03 active-only shortcuts, caller default policy and idempotent stale unregister');

    const unsupported = await page.evaluate(() => window.__panel.stubs());
    for (const [method, closesIn] of [['navigate', '6'], ['requestFocus', '5'], ['setTrayState', '6'], ['setTrayBadge', '6'], ['openFloat', '5']]) {
        assert.deepEqual(unsupported[method], { name: 'NotImplementedError',
            message: `plinth.panel.${method} is not implemented in 0.6.3 — closes 0.6.${closesIn}` });
    }
    assert.deepEqual(await page.evaluate(() => window.__panel.compatibility()), {
        context: { caller: 'local-compat', value: 7 }, sameInstance: true, sameContext: true, active: true,
    });
    state = await page.evaluate(() => window.__panel.state());
    assert.equal(state.navigationIntents, 0, 'ordinary activation/Home/stub calls must not manufacture navigation intents');
    assert.deepEqual(state.failures, []); assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []);
    assert.deepEqual(dialogs, []); completed.push('P32.04 literal unsupported methods and actual compatibility context');
    assert.deepEqual(await page.evaluate(() => window.__panel.prepareReplacement()), {
        retiredFactoryAdmitted: false, registeredCurrent: true, activeCurrent: true, connected: true,
        count: 1, dirty: false, unloadInstalled: false, context: { owner: 'current-B' },
        trace: ['activate:aba-current'],
    });
    const released = await page.evaluate(() => window.__panel.releaseRetired());
    assert.deepEqual(released.outcome, { status: 'rejected', name: 'AbortError' },
        'retired original instance must reject before factory admission despite a same-key current replacement');
    assert.equal(released.retiredFactoryAdmitted, false);
    assert.equal(released.registeredCurrent, true); assert.equal(released.activeCurrent, true);
    assert.equal(released.connected, true); assert.equal(released.count, 1);
    assert.deepEqual(await page.evaluate(() => window.__panel.invokeFailureActions(true)), [],
        'retired fallback callbacks must not admit retry or Home through the same-key replacement');
    assert.deepEqual(await page.evaluate(() => window.__panel.invokeFailureActions(false)),
        ['retry:/fixture/aba-current.js', 'home'], 'current fallback callbacks must remain admitted');
    await page.locator('[data-local-panel="aba-current"]').getByRole('button', { name: 'Dirty true', exact: true }).click();
    let replacement = await page.evaluate(() => window.__panel.replacementState());
    assert.equal(replacement.dirty, true); assert.equal(replacement.unloadInstalled, true);
    await page.locator('[data-local-panel="aba-current"]').getByRole('button', { name: 'Dirty false', exact: true }).click();
    replacement = await page.evaluate(() => window.__panel.reactivateReplacement());
    assert.deepEqual(replacement, { retiredFactoryAdmitted: false, registeredCurrent: true,
        activeCurrent: true, connected: true, count: 1, dirty: false, unloadInstalled: false,
        context: { owner: 'current-B' }, focused: true,
        trace: ['activate:aba-current', 'deactivate:aba-current', 'activate:aba-current'] });
    assert.equal(await page.locator('[data-local-panel="aba-current"] h2').textContent(), 'Local aba-current');
    completed.push('P32.05 retired deferred import cannot adopt or evict same-key current replacement');
    assert.deepEqual((await page.evaluate(() => window.__panel.state())).failures, []);
    assert.deepEqual(fixtureErrors, []); assert.deepEqual(pageErrors, []); assert.deepEqual(dialogs, []);
    assert.equal(completed.length, 5); assert.deepEqual(await identity(), before);
} catch (error) { failure = error; }
finally {
    try {
        if (page && !page.isClosed()) assert.deepEqual(await bounded(
            () => page.evaluate(() => window.__panel?.cleanup()), 'panel fixture cleanup'),
        { managerEmpty: true, inactive: true, unloadRemoved: true, domEmpty: true });
    } catch (error) { cleanupErrors.push(error); }
    try { await bounded(() => context?.close(), 'panel context cleanup'); }
    catch (error) { cleanupErrors.push(error); }
    if (page && !page.isClosed()) cleanupErrors.push(new Error('panel page remained open'));
    try {
        await bounded(() => new Promise((done, reject) => {
            if (!server) return done();
            server.close(error => error && error.code !== 'ERR_SERVER_NOT_RUNNING' ? reject(error) : done());
            server.closeAllConnections(); for (const connection of connections) connection.destroy();
        }), 'panel server cleanup');
        assert.equal(server?.listening || false, false);
        assert([...connections].every(connection => connection.destroyed));
    } catch (error) { cleanupErrors.push(error); }
    try { await bounded(() => browser?.close(), 'panel browser cleanup'); assert.equal(browser?.isConnected() || false, false); }
    catch (error) { cleanupErrors.push(error); }
}
if (failure || cleanupErrors.length) throw new AggregateError(
    [...(failure ? [failure] : []), ...cleanupErrors], 'panel contract failed');
for (const name of completed) console.log(`PASS ${name}`);
console.log(JSON.stringify({ panelContractCases: completed.length, expectedCases: 5, cleanup: 'PASS',
    sourceIdentity: before, scope: 'actual shipped seven-module/Preact graph; local panel fixtures; no native backend' }));
