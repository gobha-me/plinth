import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

// Evaluate the complete shipping graph. Only external Preact and browser APIs
// are controlled here; these tests do not emulate a deployed backend.
const client = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const paths = ['launcher/launcher.js', 'launcher/model.js', 'sdk.js',
    'panels/loader.js', 'panels/panel_api.js', 'data-query.js', 'data-controller.js',
    'panels/float-manager.js', 'panels/float-chrome.js', 'panels/float-model.js',
    'panels/float-preferences.js', 'panels/float-reservations.js', 'panels/interaction-owner.js'];
const sources = new Map(await Promise.all(paths.map(async path =>
    [resolve(client, path), await readFile(resolve(client, path), 'utf8')])));
const plain = value => JSON.parse(JSON.stringify(value));
const channel = 'plinth:system:applications.changed';
const catalog = { schema_version: 1, applications: [{
    id: 'notes', generation: 'notes-generation', version: '1.0.0', title: 'Notes',
    panels: ['zero', 'one'].map(id => ({ id, title: id,
        module_url: `/ext/notes/1.0.0/panels/${id}.js` })),
}] };
const preference = panel => ({ version: 1, last_application: 'notes',
    last_panels: { notes: panel }, application_order: ['notes'] });
function deferred() {
    let resolvePromise;
    let rejectPromise;
    const promise = new Promise((resolveValue, rejectValue) => {
        resolvePromise = resolveValue;
        rejectPromise = rejectValue;
    });
    return { promise, resolve: resolvePromise, reject: rejectPromise };
}
function response(status = 200, body = catalog) {
    return { status, ok: status >= 200 && status < 300,
        statusText: 'mock HTTP response', json: async () => body };
}
const capResponse = value => response(200, { ok: true, value });
async function turns() { for (let index = 0; index < 8; index++) await Promise.resolve(); }

async function fixture() {
    const requests = [];
    const sockets = [];
    const microtasks = [];
    const timers = new Map();
    const plans = new Map();
    const pending = [];
    const operations = [];
    const launchers = [];
    const effects = [];
    const sessionEnds = [];
    const listeners = new Map();
    const documentEvents = [], windowEvents = [], windowListeners = new Map();
    let nextTimer = 0;
    let account = 'A';
    const document = {
        cookie: 'plinth_csrf=fake-A', visibilityState: 'visible', activeElement: null,
        addEventListener(type, fn) { documentEvents.push(['add', type, fn]); listeners.set(type, fn); },
        removeEventListener(type, fn) { documentEvents.push(['remove', type, fn]); if (listeners.get(type) === fn) listeners.delete(type); },
        getElementById(id) { return new Element(id); },
        createElement(tag) { return new Element(tag); },
    };
    class Element {
        constructor(name) { this.name = name; this.dataset = {}; this.children = [];
            this.attributes = new Map(); this.isConnected = true; this.inert = false; }
        append(child) { this.children.push(child); child.parent = this; }
        setAttribute(key, value) { this.attributes.set(key, value); }
        getAttribute(key) { return this.attributes.get(key) ?? null; }
        removeAttribute(key) { this.attributes.delete(key); }
        contains(element) { return element === this || this.children.some(child => child.contains(element)); }
        closest() { return this.inert || this.hidden ? this : this.parent?.closest() || null; }
        remove() { this.isConnected = false; if (this.parent) this.parent.children = this.parent.children.filter(c => c !== this); }
        focus() { effects.push(['focus', this.name]); document.activeElement = this; }
        scrollIntoView() { effects.push(['scroll', this.name]); }
        querySelector() { return new Element('dirty-cancel'); }
        querySelectorAll() { return []; }
    }
    class Component {
        constructor(props) {
            this.props = props; this.updates = []; this.callbacks = [];
            this.publications = []; this.forceUpdates = 0;
        }
        setState(update, callback) {
            const apply = () => {
                const patch = typeof update === 'function' ? update(this.state, this.props) : update;
                if (patch != null) { this.publications.push(plain(patch)); Object.assign(this.state, patch); }
                if (callback) {
                    if (this.holdCallbacks) this.callbacks.push(callback);
                    else callback();
                }
            };
            if (this.holdUpdates) this.updates.push(apply);
            else apply();
        }
        forceUpdate() { this.forceUpdates++; }
    }
    class Socket {
        static OPEN = 1;
        constructor(url) { this.url = url; this.readyState = 0; this.listeners = new Map();
            this.frames = []; sockets.push(this); }
        addEventListener(type, listener) {
            const list = this.listeners.get(type) || []; list.push(listener); this.listeners.set(type, list);
        }
        emit(type, value = {}) { for (const listener of this.listeners.get(type) || []) listener(value); }
        open() { this.readyState = 1; }
        receive(value) { this.emit('message', { data: JSON.stringify(value) }); }
        send(value) { assert.equal(this.readyState, 1); this.frames.push(JSON.parse(value)); }
        close(code = 1000) { this.readyState = 3; this.emit('close', { code, reason: '' }); }
    }
    const media = { matches: false, addEventListener(type, fn) { this.listener = fn; },
        removeEventListener() { this.listener = null; } };
    function defaultResponse(url) {
        if (url === '/api/frontend/applications') return response();
        if (url === '/api/auth/session') return response(200, { user: { id: account } });
        if (url === '/api/cap/shell.preferences.get') return capResponse({ value: null });
        if (url === '/api/cap/shell.preferences.set' || url === '/api/cap/shell.audit.emit') {
            return capResponse({ ok: true });
        }
        throw new Error(`unexpected platform fetch ${url}`);
    }
    const context = vm.createContext({
        window: { __PLINTH_PRODUCTION__: true, matchMedia: () => media,
            location: { origin: 'https://plinth.test', href: 'https://plinth.test/app/',
                protocol: 'https:', host: 'plinth.test' },
            addEventListener(type, fn) { windowEvents.push(['add', type, fn]); windowListeners.set(type, fn); },
            removeEventListener(type, fn) { windowEvents.push(['remove', type, fn]); if (windowListeners.get(type) === fn) windowListeners.delete(type); } },
        document, URL, Headers, AbortController, DOMException, TextEncoder,
        performance: { now: () => 0 },
        console: { error() {} }, WebSocket: Socket,
        queueMicrotask(fn) { microtasks.push(fn); },
        setTimeout(fn, milliseconds) { const id = ++nextTimer; timers.set(id, { fn, milliseconds }); return id; },
        clearTimeout(id) { timers.delete(id); },
        fetch(url, options = {}) {
            requests.push({ url, account, csrf: options.headers?.get?.('X-Plinth-CSRF'),
                args: options.body ? JSON.parse(options.body).args : undefined, options });
            const queue = plans.get(url);
            return queue?.length ? queue.shift().promise : Promise.resolve(defaultResponse(url));
        },
    });
    const preact = new vm.SyntheticModule(['h', 'render', 'Component'], function () {
        this.setExport('h', (type, props, ...children) => ({ type, props: props || {}, children }));
        this.setExport('render', () => {});
        this.setExport('Component', Component);
    }, { context, identifier: 'platform:preact' });
    const hooks = new vm.SyntheticModule(['useEffect', 'useState', 'useRef'], function () {
        for (const name of ['useEffect', 'useState', 'useRef']) {
            this.setExport(name, () => { throw new Error(`unexpected hook ${name} in Launcher fixture`); });
        }
    }, { context, identifier: 'platform:preact/hooks' });
    const modules = new Map();
    function load(path) {
        assert(sources.has(path), `module outside exact shipping graph: ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(sources.get(path),
            { context, identifier: path }));
        return modules.get(path);
    }
    const launcherModule = load(resolve(client, 'launcher/launcher.js'));
    await launcherModule.link((specifier, importer) => {
        if (specifier === 'preact') return preact;
        if (specifier === 'preact/hooks') return hooks;
        if (specifier === '@plinth/frontend/sdk') return load(resolve(client, 'sdk.js'));
        assert(specifier.startsWith('.'), `unexpected external import: ${specifier}`);
        return load(resolve(dirname(importer.identifier), specifier));
    });
    await launcherModule.evaluate();
    assert.equal(modules.size, paths.length, 'must evaluate the complete actual module graph');
    assert.deepEqual([...modules.keys()].sort(), [...sources.keys()].sort(),
        'the evaluated imports must exactly match the reviewed shipping graph');
    const Launcher = launcherModule.namespace.Launcher;
    const model = modules.get(resolve(client, 'launcher/model.js')).namespace;
    const sdk = modules.get(resolve(client, 'sdk.js')).namespace;
    function createLauncher() {
        const launcher = new Launcher({ onSessionEnd: code => sessionEnds.push(code) });
        launcher.panelHost = new Element('host'); launcher.homeHeading = new Element('home');
        launcher.componentDidMount(); launchers.push(launcher); return launcher;
    }
    const launcher = createLauncher();
    await launcher.preferencePromise;
    const f = { launcher, model, sdk, requests, sockets, effects, sessionEnds, media,
        documentEvents, windowEvents, listeners, windowListeners,
        FloatLayer: modules.get(resolve(client, 'panels/float-chrome.js')).namespace.FloatLayer,
        document, microtasks, createLauncher,
        run(promise) { operations.push(promise); promise.catch(() => {}); return promise; },
        hold(url) {
            const item = deferred(); pending.push({ url, item });
            const queue = plans.get(url) || []; queue.push(item); plans.set(url, queue); return item;
        },
        rotate() { account = 'B'; document.cookie = 'plinth_csrf=fake-B'; },
        writes() { return requests.filter(r => r.url === '/api/cap/shell.preferences.set'); },
        catalogs() { return requests.filter(r => r.url === '/api/frontend/applications'); },
        audits() { return requests.filter(r => r.url === '/api/cap/shell.audit.emit'); },
        async grant() {
            const socket = sockets.at(-1); socket.open(); socket.receive({ type: 'connected' });
            socket.receive({ type: 'subscribed', channels: [channel] });
            while (microtasks.length) microtasks.shift()();
            await turns();
        },
        flushUpdates() { for (const l of launchers) while (l.updates.length) l.updates.shift()(); },
        flushCallbacks() { for (const l of launchers) while (l.callbacks.length) l.callbacks.shift()(); },
        async cleanup() {
            for (const l of launchers) l.componentWillUnmount();
            for (const { url, item } of pending) item.resolve(defaultResponse(url));
            await Promise.allSettled(operations);
            microtasks.length = 0;
            assert.equal(timers.size, 0, 'unmount must release the actual SDK reconnect timer');
        },
    };
    return f;
}
function ownerTest(name, fn) {
    test(name, { timeout: 5000 }, async () => {
        const f = await fixture();
        try { await fn(f); } finally { await f.cleanup(); }
    });
}
function queueWrite(f, panel) {
    f.launcher.preference = preference(panel); f.launcher.writePreference();
    return f.run(f.launcher.preferenceWrite);
}

ownerTest('W01 same owner serializes two captured preference writes', async f => {
    const first = f.hold('/api/cap/shell.preferences.set');
    queueWrite(f, 'zero'); await turns(); const last = queueWrite(f, 'one');
    assert.equal(f.writes().length, 1); first.resolve(capResponse({ ok: true })); await last;
    assert.deepEqual(f.writes().map(r => [r.account, r.args.value.last_panels.notes]), [['A', 'zero'], ['A', 'one']]);
});
ownerTest('W02 failed first write still admits next same-owner write', async f => {
    const first = f.hold('/api/cap/shell.preferences.set');
    queueWrite(f, 'zero'); await turns(); const last = queueWrite(f, 'one');
    first.reject(new Error('fake transport rejection')); await last;
    assert.deepEqual(f.writes().map(r => r.account), ['A', 'A']);
    assert(f.launcher.publications.some(p => /could not be saved/.test(p.statusMessage || '')));
});
ownerTest('W03 queued nested preference is immutable after capture', async f => {
    const first = f.hold('/api/cap/shell.preferences.set');
    queueWrite(f, 'zero'); await turns(); const last = queueWrite(f, 'one');
    f.launcher.preference.last_panels.notes = 'changed';
    f.launcher.preference.application_order.push('files');
    first.resolve(capResponse({ ok: true })); await last;
    assert.equal(f.writes()[1].args.value.last_panels.notes, 'one');
    assert.deepEqual(f.writes()[1].args.value.application_order, ['notes']);
});
ownerTest('W04 transient disconnect and nonready subscription permit valid writes', async f => {
    f.launcher.realtimeError({ code: 'disconnected' });
    assert.equal(f.launcher.subscriptionReady, false); await queueWrite(f, 'one');
    assert.equal(f.writes().length, 1);
});
ownerTest('W05 retired owner drains exact queue without dispatch under new account', async f => {
    const first = f.hold('/api/cap/shell.preferences.set');
    queueWrite(f, 'zero'); await turns(); const last = queueWrite(f, 'one');
    f.launcher.componentWillUnmount(); f.rotate(); first.resolve(capResponse({ ok: true })); await last;
    assert.deepEqual(f.writes().map(r => [r.account, r.args.value.last_panels.notes]), [['A', 'zero']]);
});
ownerTest('W06 newly mounted owner writes with new account CSRF', async f => {
    f.launcher.componentWillUnmount(); f.rotate();
    const next = f.createLauncher(); await next.preferencePromise;
    next.preference = preference('one'); next.writePreference(); await f.run(next.preferenceWrite);
    assert.deepEqual(f.writes().map(r => [r.account, r.csrf]), [['B', 'fake-B']]);
});
ownerTest('W07 invoking write after retirement admits no request', async f => {
    f.launcher.componentWillUnmount(); await queueWrite(f, 'one'); assert.equal(f.writes().length, 0);
});
ownerTest('W08 late admitted write error does not publish retired state', async f => {
    const first = f.hold('/api/cap/shell.preferences.set'); const last = queueWrite(f, 'one');
    await turns(); f.launcher.componentWillUnmount(); const before = f.launcher.publications.length;
    first.reject(new Error('fake late transport failure')); await last;
    assert.equal(f.launcher.publications.length, before);
});
for (const outcome of ['success', 'error']) {
    ownerTest(`L01 late preference load ${outcome} cannot mutate retired state`, async f => {
        const held = f.hold('/api/cap/shell.preferences.get');
        const operation = f.run(f.launcher.loadPreference());
        const beforePreference = plain(f.launcher.preference);
        f.launcher.componentWillUnmount(); const before = f.launcher.publications.length;
        if (outcome === 'success') held.resolve(capResponse({ value: preference('one') }));
        else held.reject(new Error('fake late get failure'));
        await operation; assert.deepEqual(plain(f.launcher.preference), beforePreference);
        assert.equal(f.launcher.publications.length, before);
    });
}
ownerTest('L02 retired preference readiness continuation admits no catalog', async f => {
    const ready = deferred(); f.launcher.preferencePromise = ready.promise;
    f.launcher.subscriptionReady = true; f.launcher.refreshWhenPreferencesReady();
    f.launcher.componentWillUnmount(); ready.resolve(); await ready.promise; await turns();
    assert.equal(f.catalogs().length, 0);
});
ownerTest('L03 current preference load still updates remembered panel', async f => {
    const held = f.hold('/api/cap/shell.preferences.get');
    const operation = f.run(f.launcher.loadPreference()); held.resolve(capResponse({ value: preference('one') }));
    await operation; assert.equal(f.launcher.preference.last_panels.notes, 'one');
});
for (const outcome of ['200', '401', 'error']) {
    ownerTest(`R01 retired recovery ${outcome} cannot reconnect or end session`, async f => {
        const held = f.hold('/api/auth/session'); const operation = f.run(f.launcher.recoverAuthority());
        f.launcher.componentWillUnmount(); const before = f.launcher.publications.length;
        const socketCount = f.sockets.length;
        if (outcome === 'error') held.reject(new Error('fake session fetch failure'));
        else held.resolve(response(Number(outcome), { error: 'session_revoked' }));
        await operation; assert.equal(f.sockets.length, socketCount);
        assert.deepEqual(f.sessionEnds, []); assert.equal(f.launcher.publications.length, before);
    });
}
for (const outcome of ['success', 'error']) {
    ownerTest(`R02 recovery 401 JSON ${outcome} after retirement cannot end session`, async f => {
        const held = f.hold('/api/auth/session'); const json = deferred();
        const operation = f.run(f.launcher.recoverAuthority());
        held.resolve({ status: 401, ok: false, json: () => json.promise }); await turns();
        f.launcher.componentWillUnmount();
        if (outcome === 'success') json.resolve({ error: 'session_revoked' });
        else json.reject(new Error('fake late session JSON error'));
        await operation; assert.deepEqual(f.sessionEnds, []);
    });
}
for (const outcome of ['200', '401', 'error']) {
    ownerTest(`R03 older recovery ${outcome} loses authority to newer granted recovery`, async f => {
        const old = f.hold('/api/auth/session'); const first = f.run(f.launcher.recoverAuthority());
        f.launcher.failClosedAuthority('already_connected');
        const current = f.hold('/api/auth/session'); const second = f.run(f.launcher.recoverAuthority());
        current.resolve(response(200)); await second; await f.grant();
        const state = plain(f.launcher.state); const socketCount = f.sockets.length;
        const publications = f.launcher.publications.length;
        if (outcome === 'error') old.reject(new Error('fake stale recovery failure'));
        else old.resolve(response(Number(outcome), { error: 'session_revoked' }));
        await first; assert.deepEqual(f.sessionEnds, []); assert.equal(f.sockets.length, socketCount);
        assert.deepEqual(plain(f.launcher.state), state); assert.equal(f.launcher.publications.length, publications);
    });
}
for (const outcome of ['success', 'error']) {
    ownerTest(`R04 older recovery JSON ${outcome} loses authority to newer granted recovery`, async f => {
        const old = f.hold('/api/auth/session'); const json = deferred();
        const first = f.run(f.launcher.recoverAuthority());
        old.resolve({ status: 401, ok: false, json: () => json.promise }); await turns();
        f.launcher.failClosedAuthority('already_connected');
        const second = f.run(f.launcher.recoverAuthority()); await second; await f.grant();
        if (outcome === 'success') json.resolve({ error: 'session_revoked' });
        else json.reject(new Error('fake superseded session JSON error'));
        await first; assert.deepEqual(f.sessionEnds, []);
    });
}
ownerTest('R05 older error cannot clear a newer pending recovery', async f => {
    const old = f.hold('/api/auth/session'); const first = f.run(f.launcher.recoverAuthority());
    f.launcher.failClosedAuthority('already_connected'); const current = f.hold('/api/auth/session');
    const second = f.run(f.launcher.recoverAuthority());
    old.reject(new Error('fake stale failure')); await first; assert.equal(f.launcher.recovering, true);
    current.resolve(response()); await second;
});
ownerTest('R06 current recovery 401 still ends its session', async f => {
    const held = f.hold('/api/auth/session'); const operation = f.run(f.launcher.recoverAuthority());
    held.resolve(response(401, { error: 'session_revoked' })); await operation;
    assert.deepEqual(f.sessionEnds, ['session_revoked']);
});
ownerTest('R07 repeated manual same-session recovery remains available', async f => {
    for (let index = 0; index < 2; index++) {
        f.launcher.failClosedAuthority('already_connected');
        const before = f.sockets.length; await f.run(f.launcher.recoverAuthority());
        assert.equal(f.sockets.length, before + 1); await f.grant();
        assert.equal(f.launcher.subscriptionReady, true); assert.equal(f.launcher.recovering, false);
    }
    assert.deepEqual(f.sessionEnds, []);
});
ownerTest('R08 ordinary catalog refresh does not supersede valid recovery', async f => {
    const held = f.hold('/api/auth/session'); const operation = f.run(f.launcher.recoverAuthority());
    await f.run(f.launcher.refreshCatalog()); const before = f.sockets.length;
    held.resolve(response()); await operation; assert.equal(f.sockets.length, before + 1);
});
for (const stage of ['fetch', 'json', 'json-error']) {
    ownerTest(`C01 stale catalog 401 at ${stage} cannot retire newer catalog`, async f => {
        const old = f.hold('/api/frontend/applications'); const json = deferred();
        const first = f.run(f.launcher.refreshCatalog());
        if (stage !== 'fetch') { old.resolve({ status: 401, ok: false, json: () => json.promise }); await turns(); }
        await f.run(f.launcher.refreshCatalog()); const state = plain(f.launcher.state);
        if (stage === 'fetch') old.resolve(response(401, { error: 'session_revoked' }));
        else if (stage === 'json') json.resolve({ error: 'session_revoked' });
        else json.reject(new Error('fake superseded catalog JSON error'));
        await first; assert.deepEqual(f.sessionEnds, []); assert.deepEqual(plain(f.launcher.state), state);
    });
}
for (const outcome of ['200', '503', 'error']) {
    ownerTest(`C02 ignored abort old catalog ${outcome} cannot publish`, async f => {
        const old = f.hold('/api/frontend/applications'); const first = f.run(f.launcher.refreshCatalog());
        const signal = f.catalogs()[0].options.signal; await f.run(f.launcher.refreshCatalog());
        assert.equal(signal.aborted, true); const publications = f.launcher.publications.length;
        if (outcome === 'error') old.reject(new Error('fake ignored-abort fetch failure'));
        else old.resolve(response(Number(outcome)));
        await first; assert.equal(f.launcher.publications.length, publications);
    });
}
for (const outcome of ['success', 'error']) {
    ownerTest(`C03 retired catalog ${outcome} at JSON cannot publish`, async f => {
        const old = f.hold('/api/frontend/applications'); const json = deferred();
        const operation = f.run(f.launcher.refreshCatalog());
        old.resolve({ status: 200, ok: true, json: () => json.promise }); await turns();
        f.launcher.componentWillUnmount(); const publications = f.launcher.publications.length;
        if (outcome === 'success') json.resolve(catalog); else json.reject(new Error('fake JSON failure'));
        await operation; assert.equal(f.launcher.publications.length, publications);
    });
}
ownerTest('C04 current catalog 401 still retires correct authority', async f => {
    const held = f.hold('/api/frontend/applications'); const operation = f.run(f.launcher.refreshCatalog());
    held.resolve(response(401, { error: 'session_revoked' })); await operation;
    assert.deepEqual(f.sessionEnds, ['session_revoked']); assert.deepEqual(plain(f.launcher.state.applications), []);
});
ownerTest('C05 current 503 keeps last-good applications stale', async f => {
    await f.run(f.launcher.refreshCatalog()); const previous = plain(f.launcher.state.applications);
    const held = f.hold('/api/frontend/applications'); const operation = f.run(f.launcher.refreshCatalog());
    held.resolve(response(503)); await operation;
    assert.equal(f.launcher.state.catalogStatus, 'stale');
    assert.deepEqual(plain(f.launcher.state.applications), previous);
});
for (const stage of ['fetch', 'json', 'json-error']) {
    ownerTest(`C06 retired catalog 401 at ${stage} cannot end session`, async f => {
        const held = f.hold('/api/frontend/applications'); const json = deferred();
        const operation = f.run(f.launcher.refreshCatalog());
        if (stage !== 'fetch') {
            held.resolve({ status: 401, ok: false, json: () => json.promise }); await turns();
        }
        f.launcher.componentWillUnmount(); const before = f.launcher.publications.length;
        if (stage === 'fetch') held.resolve(response(401, { error: 'session_revoked' }));
        else if (stage === 'json') json.resolve({ error: 'session_revoked' });
        else json.reject(new Error('fake retired catalog JSON error'));
        await operation; assert.deepEqual(f.sessionEnds, []);
        assert.equal(f.launcher.publications.length, before);
    });
}
for (const mode of ['current', 'retired', 'superseded']) {
    ownerTest(`N01 ${mode} actual loader import failure controls authoritative refetch`, async f => {
        await f.run(f.launcher.refreshCatalog());
        const application = f.launcher.state.applications[0];
        const target = f.model.makeTarget(application, application.panels[0]);
        const imported = deferred(); f.launcher.panelManager.options.importModule = () => imported.promise;
        const operation = f.run(f.launcher.navigateTarget(target));
        if (mode === 'retired') f.launcher.componentWillUnmount();
        if (mode === 'superseded') f.launcher.commitHome(false);
        const count = f.catalogs().length;
        imported.reject(new Error('mock old-generation HTTP 404')); await operation;
        assert.equal(f.catalogs().length, count + (mode === 'current' ? 1 : 0));
    });
}
ownerTest('N02 prepared panel resolving after retirement cannot commit or write', async f => {
    await f.run(f.launcher.refreshCatalog()); const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    const imported = deferred(); f.launcher.panelManager.options.importModule = () => imported.promise;
    const operation = f.run(f.launcher.navigateTarget(target));
    f.launcher.componentWillUnmount(); imported.resolve({ default: () => () => null }); await operation;
    assert.equal(f.launcher.panelManager.active, null); assert.equal(f.writes().length, 0);
    assert.deepEqual(f.effects, []);
});
ownerTest('U01 admitted functional preference updater is inert after retirement', async f => {
    f.launcher.holdUpdates = true;
    const held = f.hold('/api/cap/shell.preferences.get'); const operation = f.run(f.launcher.loadPreference());
    held.resolve(capResponse({ value: preference('one') })); await operation;
    assert(f.launcher.updates.length > 0, 'actual preference path must schedule an updater');
    f.launcher.componentWillUnmount(); const publications = f.launcher.publications.length;
    f.flushUpdates(); assert.equal(f.launcher.publications.length, publications);
});
ownerTest('U02 admitted menu completion cannot focus after retirement', async f => {
    await f.run(f.launcher.refreshCatalog()); f.launcher.holdCallbacks = true;
    f.launcher.menuItems = [{ focus: () => f.effects.push(['focus', 'menu']) }];
    f.launcher.openMenu(); assert(f.launcher.callbacks.length > 0);
    f.launcher.componentWillUnmount(); f.flushCallbacks(); assert.deepEqual(f.effects, []);
});
function vnodeFind(node, predicate) {
    if (!node || typeof node !== 'object') return null;
    if (predicate(node)) return node;
    for (const child of (node.children || []).flat(Infinity)) {
        const match = vnodeFind(child, predicate); if (match) return match;
    }
    return null;
}
for (const mode of ['current', 'retired']) {
    ownerTest(`U03 ${mode} actual connected dirty dialog controls microtask focus`, async f => {
        f.launcher.state.dirtyIntent = { kind: 'home' };
        const root = f.launcher.render();
        const dialog = vnodeFind(root, node => typeof node.type === 'function' && node.type.name === 'DirtyDialog');
        assert(dialog, 'shipping renderer must construct the actual dirty dialog');
        const tree = dialog.type(dialog.props);
        const inner = vnodeFind(tree, node => node.props.class === 'dirty-dialog');
        inner.props.ref({ isConnected: true,
            querySelector: () => ({ focus: () => f.effects.push(['focus', 'dirty']) }) });
        if (mode === 'retired') f.launcher.componentWillUnmount();
        while (f.microtasks.length) f.microtasks.shift()();
        assert.deepEqual(f.effects, mode === 'current' ? [['focus', 'dirty']] : []);
    });
}
ownerTest('U04 retained actual PanelManager callbacks are inert after retirement', async f => {
    await f.run(f.launcher.refreshCatalog()); const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    const manager = f.launcher.panelManager;
    f.launcher.componentWillUnmount(); const publications = f.launcher.publications.length;
    manager.options.onFailure(target, new Error('mock late panel failure'));
    manager.options.onRetry(target); manager.options.onHome(); manager.options.onDirtyChange(); await turns();
    assert.equal(f.audits().length, 0); assert.equal(f.writes().length, 0);
    assert.equal(f.launcher.publications.length, publications); assert.equal(f.launcher.forceUpdates, 0);
});
ownerTest('U05 disposal-time panel callback cannot admit old-owner audit', async f => {
    await f.run(f.launcher.refreshCatalog()); const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    f.launcher.panelManager.options.importModule = async () => ({ default: api => {
        api.onDeactivate(() => { throw new Error('mock deactivation error'); }); return () => null;
    } });
    await f.run(f.launcher.navigateTarget(target)); await f.run(f.launcher.preferenceWrite);
    const before = f.audits().length; f.launcher.componentWillUnmount(); await turns();
    assert.equal(f.audits().length, before);
});
ownerTest('U06 current actual PanelManager failure still admits redacted audit', async f => {
    await f.run(f.launcher.refreshCatalog()); const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    f.launcher.panelManager.options.importModule = async () => ({ default: () => () => null });
    const instance = await f.launcher.panelManager.prepare(target);
    f.launcher.panelManager.reportFailure(instance, new Error('fake detail'));
    await turns(); assert.equal(f.audits().length, 1);
    assert.equal(f.audits()[0].args.error_message, 'Panel lifecycle failure');
    assert.equal(f.audits()[0].args.error_stack, undefined);
});
ownerTest('U07 retired existing callback and recovery entry points admit nothing', async f => {
    const visibility = f.launcher.visibility; const narrow = f.launcher.narrowChanged;
    f.launcher.componentWillUnmount(); const requests = f.requests.length;
    const publications = f.launcher.publications.length;
    visibility(); narrow({ matches: true });
    f.launcher.realtimeReady(); f.launcher.realtimeError({ code: 'disconnected' });
    f.launcher.retry(); f.launcher.useThisTab();
    await f.run(f.launcher.recoverAuthority()); await f.run(f.launcher.loadPreference());
    await f.run(f.launcher.refreshCatalog()); await turns();
    assert.equal(f.requests.length, requests); assert.equal(f.launcher.publications.length, publications);
    assert.deepEqual(f.sessionEnds, []);
});

async function activePrimary(f, configure = () => {}) {
    await f.run(f.launcher.refreshCatalog());
    const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    let api;
    f.launcher.panelManager.options.importModule = async () => ({ default: value => {
        api = value; configure(value); return () => null;
    } });
    await f.run(f.launcher.navigateTarget(target));
    await f.run(f.launcher.preferenceWrite);
    return api;
}

ownerTest('F01 actual shipping float ports remain unavailable without generic preference fallback', async f => {
    const before = f.requests.length;
    assert.equal(f.launcher.floatPreferences.status().available, false);
    assert.equal(f.launcher.floatPreferences.status().restore, 'unavailable');
    assert.equal(f.launcher.floatPreferences.layoutChanged(), false);
    assert.equal(f.launcher.floatManager.admit({}).status, 'target-unavailable');
    await turns();
    assert.equal(f.requests.length, before);
    assert.equal(f.launcher.floatReservations.status().count, 0);
});

ownerTest('F02 actual primary loader uses one shared shortcut dispatcher and document unload guard', async f => {
    let invoked = 0;
    const api = await activePrimary(f, value => value.registerShortcut('Ctrl+K', () => invoked++));
    const keydown = f.listeners.get('keydown');
    assert.equal(f.documentEvents.filter(([action, type]) => action === 'add' && type === 'keydown').length, 1);
    assert.notEqual(keydown, f.launcher.panelManager.keydown);
    keydown({ key: 'k', ctrlKey: true }); assert.equal(invoked, 1);
    f.launcher.interaction.focusShell(); keydown({ key: 'k', ctrlKey: true }); assert.equal(invoked, 1);
    api.setDirty(true);
    assert.equal(f.launcher.panelManager.beforeUnloadInstalled, false);
    assert.equal(f.windowEvents.filter(([action, type]) => action === 'add' && type === 'beforeunload').length, 1);
    let prevented = 0; const event = { preventDefault() { prevented++; } };
    f.windowListeners.get('beforeunload')(event);
    assert.equal(prevented, 1); assert.equal(event.returnValue, '');
    api.setDirty(false); assert.equal(f.windowListeners.has('beforeunload'), false);
});

ownerTest('F03 primary and float confirmations share one lease without queued navigation', async f => {
    const api = await activePrimary(f); api.setDirty(true);
    const scope = f.launcher.interaction;
    f.launcher.requestHome();
    const primary = scope.confirmation;
    assert.equal(primary.kind, 'primary'); assert.equal(f.launcher.state.dirtyIntent.kind, 'home');
    assert.equal(scope.confirm({ kind: 'float', recordToken: Symbol(), onCancel() {}, onDiscard() {} }), false);
    f.launcher.requestHome(); assert.equal(scope.confirmation, primary);
    f.launcher.cancelDirty();
    assert.equal(scope.confirmation, null); assert.equal(f.launcher.state.dirtyIntent, null);
    let cancelled = 0;
    assert.equal(scope.confirm({ kind: 'float', recordToken: Symbol(), onCancel() { cancelled++; }, onDiscard() {} }), true);
    const floating = scope.confirmation;
    f.launcher.requestHome(); f.launcher.confirmDirty(); f.launcher.cancelDirty();
    assert.equal(scope.confirmation, floating); assert.equal(f.launcher.state.dirtyIntent, null);
    scope.resolveConfirmation(false); assert.equal(cancelled, 1);
});

ownerTest('F04 primary dirty Cancel restores an eligible trigger or current primary fallback', async f => {
    const api = await activePrimary(f); api.setDirty(true);
    const trigger = f.document.createElement('trigger'); f.document.activeElement = trigger;
    f.launcher.requestHome(); f.launcher.cancelDirty();
    assert.deepEqual(f.effects.at(-1), ['focus', 'trigger']);
    trigger.inert = true; f.document.activeElement = trigger;
    f.launcher.requestHome(); f.launcher.cancelDirty();
    assert.equal(f.document.activeElement, f.launcher.panelManager.active.container);
    assert.equal(f.launcher.interaction.focused.kind, 'primary');
});

ownerTest('F05 scope retirement fences actual asynchronous primary publication before idempotent disposal', async f => {
    await f.run(f.launcher.refreshCatalog());
    const application = f.launcher.state.applications[0];
    const target = f.model.makeTarget(application, application.panels[0]);
    const imported = deferred(); f.launcher.panelManager.options.importModule = () => imported.promise;
    const operation = f.run(f.launcher.navigateTarget(target));
    const publications = f.launcher.publications.length, requests = f.requests.length;
    f.launcher.interaction.retire();
    assert.equal(f.launcher.owns(), false);
    assert.equal(f.launcher.floatManager.retired, true);
    assert.equal(f.launcher.floatPreferences.status().retired, true);
    assert.equal(f.launcher.floatPreferenceIoOwner.pending().liveFrames, 0);
    imported.resolve({ default: () => () => null }); await operation;
    assert.equal(f.launcher.publications.length, publications); assert.equal(f.requests.length, requests);
    assert.equal(f.launcher.panelManager.active, null);
    f.launcher.componentWillUnmount(); f.launcher.componentWillUnmount();
    assert.equal(f.listeners.has('keydown'), false);
});

ownerTest('F06 narrow switcher owns its lease and never inerts its own topbar ancestor', async f => {
    await f.run(f.launcher.refreshCatalog());
    const names = ['primaryMain', 'topbar', 'homeButton', 'appTrigger', 'tabs', 'userControlsHost', 'overlayButton'];
    for (const name of names) f.launcher[name] = f.document.createElement(name);
    for (const name of names.slice(2)) f.launcher.topbar.append(f.launcher[name]);
    f.launcher.state.narrow = true; f.launcher.openMenu(); f.launcher.componentDidUpdate();
    assert.equal(f.launcher.interaction.modal.kind, 'switcher');
    assert.equal(f.launcher.interaction.modal.token, f.launcher.switcherToken);
    assert.deepEqual(plain(f.launcher.backgroundElements().map(element => element.name)), names.filter(name => name !== 'topbar'));
    f.launcher.closeMenu(); f.launcher.componentDidUpdate();
    assert.equal(f.launcher.interaction.modal, null);
    assert.deepEqual(plain(f.launcher.backgroundElements().map(element => element.name)), ['primaryMain', 'topbar']);
    f.launcher.interaction.setModal('float', Symbol()); f.launcher.openMenu();
    assert.equal(f.launcher.state.menuOpen, false);
    assert.equal(f.launcher.interaction.modal.kind, 'float');
});

ownerTest('F07 actual FloatLayer preserves narrow switcher lease and restores background isolation', async f => {
    await f.run(f.launcher.refreshCatalog());
    const names = ['primaryMain', 'topbar', 'homeButton', 'appTrigger', 'tabs', 'userControlsHost', 'overlayButton'];
    for (const name of names) f.launcher[name] = f.document.createElement(name);
    for (const name of names.slice(2)) f.launcher.topbar.append(f.launcher[name]);
    const layer = new f.FloatLayer({ manager: f.launcher.floatManager, interaction: f.launcher.interaction,
        backgroundElements: () => f.launcher.backgroundElements() });
    layer.mounted = true;
    layer.state.area = { left: 0, top: 0, width: 800, height: 600 };
    const token = Symbol(); layer.state.records = [{ token, presentation: 'shown', rank: 0, retiring: false }];
    f.launcher.state.narrow = true; f.launcher.openMenu();
    layer.componentDidUpdate();
    assert.equal(f.launcher.interaction.modal.kind, 'switcher');
    assert.equal(f.launcher.topbar.inert, false); assert.equal(f.launcher.appTrigger.inert, true);
    f.launcher.closeMenu(); layer.componentDidUpdate();
    assert.equal(f.launcher.interaction.modal.kind, 'float');
    assert.equal(f.launcher.topbar.inert, true); assert.equal(f.launcher.appTrigger.inert, false);
    layer.state.records = []; layer.componentDidUpdate();
    assert.equal(f.launcher.interaction.modal, null);
    assert.equal(f.launcher.topbar.inert, false); assert.equal(f.launcher.primaryMain.inert, false);
    assert.equal(f.launcher.floatReservations.status().poisoned, false);
    layer.retireLayer();
});

ownerTest('F08 authority retirement deactivates actual primary once before repeated unmount and late callbacks', async f => {
    let deactivated = 0;
    const api = await activePrimary(f, value => value.onDeactivate(() => { deactivated++; }));
    api.setDirty(true);
    const before = f.requests.length;
    f.launcher.interaction.retire();
    assert.equal(deactivated, 1); assert.equal(f.launcher.panelManager.active, null);
    assert.equal(f.windowListeners.has('beforeunload'), false);
    f.launcher.componentWillUnmount(); f.launcher.componentWillUnmount();
    api.setDirty(true); await turns();
    assert.equal(deactivated, 1); assert.equal(f.requests.length, before);
    assert.equal(f.launcher.documentInteraction.beforeUnloadInstalled, false);
});
