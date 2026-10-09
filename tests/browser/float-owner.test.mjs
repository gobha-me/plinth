import assert from 'node:assert/strict';
import test from 'node:test';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import {
    createFloatReservations, createFloatDispatch, trackFloatImport, retireFloatRecord,
} from '../../client/shell/client/panels/float-reservations.js';

function deferred() {
    let resolve, reject;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    return { promise, resolve, reject };
}

test('document reservations refuse a sixth opaque live owner', () => {
    const budget = createFloatReservations();
    const tokens = Array.from({ length: 5 }, () => budget.reserve());
    assert(tokens.every(token => typeof token === 'symbol'));
    assert.equal(new Set(tokens).size, 5);
    assert.equal(budget.reserve(), null);
    assert.deepEqual(budget.status(), { count: 5, pending: 0, retiring: 0, poisoned: false, unsaved: false });
    assert.deepEqual(Object.keys(budget.inspect(tokens[0])), ['pending', 'cleanupComplete', 'retiring', 'poisoned']);
    assert.equal(budget.inspect(Symbol()), null);
});

test('presentation and retained failure do not reclaim reservations', () => {
    const budget = createFloatReservations();
    const tokens = Array.from({ length: 5 }, () => budget.reserve());
    for (const token of tokens) {
        const attempt = budget.beginAttempt(token);
        assert(budget.settleAttempt(token, attempt));
        assert(budget.completeCleanup(token, attempt));
    }
    assert.equal(budget.status().count, 5, 'cleaned failed incarnation still owns its live record');
    assert.equal(budget.reserve(), null);
    assert(budget.retire(tokens[0]));
    assert.equal(budget.status().count, 4);
    assert.notEqual(budget.reserve(), null);
});

test('retired frame delivery is severed before late import settlement', async () => {
    const budget = createFloatReservations(), dispatch = createFloatDispatch(), pending = deferred();
    const token = budget.reserve(), attempt = budget.beginAttempt(token);
    let calls = 0;
    assert(dispatch.bind(token, attempt, () => { calls++; }));
    const tracked = trackFloatImport(budget, dispatch, token, attempt, pending.promise);
    budget.completeCleanup(token, attempt);
    let detachedBeforePublish = false;
    const unsubscribe = budget.subscribe(() => {
        detachedBeforePublish = !dispatch.deliver(token, attempt, { ok: true, module: {} });
    });
    retireFloatRecord(budget, dispatch, token, { dirty: true });
    unsubscribe();
    assert(detachedBeforePublish);
    assert.equal(budget.status().count, 1);
    assert.equal(budget.status().unsaved, true);
    pending.resolve({ default() { calls++; } });
    assert.equal(await tracked, undefined);
    assert.equal(calls, 0);
    assert.equal(budget.status().count, 0);
    assert.equal(budget.status().unsaved, false);
});

test('five stalled preparations survive repeated frame replacement', async () => {
    const budget = createFloatReservations(), waits = [], tracked = [];
    for (let frame = 0; frame < 5; frame++) {
        const dispatch = createFloatDispatch(), wait = deferred();
        const token = budget.reserve(), attempt = budget.beginAttempt(token);
        dispatch.bind(token, attempt, () => assert.fail('retired frame received preparation'));
        tracked.push(trackFloatImport(budget, dispatch, token, attempt, wait.promise));
        dispatch.clear();
        budget.completeCleanup(token, attempt);
        budget.retire(token);
        waits.push(wait);
        assert.equal(budget.status().pending, frame + 1);
    }
    for (let replacement = 0; replacement < 10; replacement++) {
        createFloatDispatch();
        assert.equal(budget.reserve(), null);
    }
    assert.equal(budget.status().count, 5);
    waits.forEach(wait => wait.reject(new Error('fake private detail')));
    await Promise.all(tracked);
    assert.equal(budget.status().count, 0);
});

test('retirement releases capacity only after preparation and cleanup both settle', () => {
    for (const cleanupFirst of [false, true]) {
        const budget = createFloatReservations(), token = budget.reserve(), attempt = budget.beginAttempt(token);
        budget.retire(token);
        if (cleanupFirst) budget.completeCleanup(token, attempt);
        else budget.settleAttempt(token, attempt);
        assert.equal(budget.status().count, 1);
        if (cleanupFirst) budget.settleAttempt(token, attempt);
        else budget.completeCleanup(token, attempt);
        assert.equal(budget.status().count, 0);
    }
});

test('pending preparation and incomplete cleanup prevent overlapping Retry', async () => {
    const budget = createFloatReservations(), dispatch = createFloatDispatch(), pending = deferred();
    const token = budget.reserve(), first = budget.beginAttempt(token);
    const deliveries = [];
    dispatch.bind(token, first, outcome => deliveries.push(outcome));
    const tracked = trackFloatImport(budget, dispatch, token, first, pending.promise);
    assert.equal(budget.beginAttempt(token), null);
    assert.throws(() => trackFloatImport(budget, dispatch, token, first, pending.promise),
        /^TypeError: invalid float preparation ownership$/);
    pending.resolve({ value: 'module' }); await tracked;
    assert.deepEqual(deliveries, [{ ok: true, module: { value: 'module' } }]);
    assert.equal(budget.beginAttempt(token), null);
    budget.completeCleanup(token, first);
    const second = budget.beginAttempt(token);
    assert.equal(typeof second, 'symbol');
    assert.notEqual(second, first);
});

test('stale incarnation settlement cannot deliver to a newer attempt', async () => {
    const budget = createFloatReservations(), dispatch = createFloatDispatch();
    const token = budget.reserve(), first = budget.beginAttempt(token);
    budget.settleAttempt(token, first); budget.completeCleanup(token, first);
    const second = budget.beginAttempt(token), pending = deferred(), deliveries = [];
    dispatch.bind(token, second, value => deliveries.push(value));
    assert.equal(budget.settleAttempt(token, first), false);
    assert.equal(budget.completeCleanup(token, first), false);
    assert.equal(dispatch.deliver(token, first, { ok: true, module: {} }), false);
    const tracked = trackFloatImport(budget, dispatch, token, second, pending.promise);
    const failure = new Error('controlled failure'); pending.reject(failure); await tracked;
    assert.deepEqual(deliveries, [{ ok: false, error: failure }]);
    assert.equal(budget.inspect(token).pending, false);
});

test('duplicate settlement and cleanup completion cannot double-release ownership', () => {
    const budget = createFloatReservations(), token = budget.reserve(), attempt = budget.beginAttempt(token);
    let observations = 0;
    const observer = () => { observations++; };
    const first = budget.subscribe(observer), second = budget.subscribe(observer);
    first(); first();
    budget.settleAttempt(token, attempt);
    assert.equal(observations, 1, 'unsubscribing one owner retains independent subscription');
    second();
    assert.equal(budget.settleAttempt(token, attempt), false);
    assert.equal(budget.completeCleanup(token, attempt), true);
    assert.equal(budget.completeCleanup(token, attempt), false);
    budget.retire(token);
    assert.equal(budget.retire(token), false);
    assert.equal(observations, 1, 'retired observer must not be retained or invoked');
    assert.equal(budget.status().count, 0);
});

test('cleanup failure permanently poisons document admission and retains generic warning', async () => {
    const budget = createFloatReservations(), dispatch = createFloatDispatch(), pending = deferred();
    const token = budget.reserve(), attempt = budget.beginAttempt(token);
    dispatch.bind(token, attempt, () => { throw new Error('uncertain shell cleanup'); });
    const tracked = trackFloatImport(budget, dispatch, token, attempt, pending.promise);
    pending.resolve({}); await tracked;
    assert.deepEqual(budget.inspect(token), { pending: false, cleanupComplete: false, retiring: true, poisoned: true });
    assert.equal(budget.status().unsaved, true);
    assert.equal(budget.status().poisoned, true);
    assert.equal(budget.reserve(), null);
    assert.equal(budget.beginAttempt(token), null);
    assert.equal(budget.completeCleanup(token, attempt), false);
    budget.retire(token); dispatch.clear(); createFloatDispatch();
    assert.equal(budget.status().count, 1);
    assert.equal(budget.reserve(), null);
    assert(!Object.hasOwn(budget, 'reset'));
});

test('chrome cleanup hold prevents premature release after incarnation cleanup', () => {
    for (const fail of [false, true]) {
        const budget = createFloatReservations(), token = budget.reserve(), attempt = budget.beginAttempt(token);
        budget.settleAttempt(token, attempt);
        budget.completeCleanup(token, attempt);
        assert(budget.holdCleanup(token));
        budget.retire(token);
        assert.equal(budget.status().count, 1, 'DOM removal still owns its reservation');
        if (fail) {
            budget.failCleanup(token, { dirty: true });
            assert.equal(budget.completeCleanup(token, attempt), false);
            assert.equal(budget.status().poisoned, true);
            assert.equal(budget.status().count, 1);
        } else {
            assert(budget.completeCleanup(token, attempt));
            assert.equal(budget.status().count, 0);
        }
    }
    const budget = createFloatReservations(), token = budget.reserve();
    assert(budget.holdCleanup(token));
    budget.retire(token);
    assert.equal(budget.status().count, 1);
    assert(budget.completeCleanup(token, undefined), 'reserved chrome can fail before an incarnation exists');
    assert.equal(budget.status().count, 0);
});

// Evaluate the exact shipping graph. Only external Preact/DOM/clock are
// controlled; this fixture is not a deployed authorization or SDK adapter.
const floatClient = resolve(dirname(fileURLToPath(import.meta.url)), '../../client/shell/client');
const floatPaths = ['panels/float-manager.js', 'panels/interaction-owner.js',
    'panels/float-model.js', 'panels/float-reservations.js', 'panels/panel_api.js',
    'sdk.js', 'data-query.js', 'data-controller.js'];
const floatSources = new Map(await Promise.all(floatPaths.map(async path =>
    [resolve(floatClient, path), await readFile(resolve(floatClient, path), 'utf8')])));
const plainFloat = value => JSON.parse(JSON.stringify(value));

async function shippingFloatFixture() {
    const timers = new Map(), imports = [], managers = [], APIs = [], effects = [];
    const docListeners = new Map(), windowListeners = new Map();
    const controls = { onRender: null, onUnmount: null, throwUnmount: false, throwRemove: false };
    let now = 0, timerId = 0, uuid = 0;
    const counts = { factory: 0, mount: 0, unmount: 0, activate: 0, deactivate: 0, shortcut: 0, remove: 0, resolve: 0 };
    const listen = (registry, type, callback) => {
        if (!registry.has(type)) registry.set(type, new Set());
        registry.get(type).add(callback);
    };
    const unlisten = (registry, type, callback) => registry.get(type)?.delete(callback);
    const location = { href: 'https://plinth.test/app/', origin: 'https://plinth.test',
        host: 'plinth.test', protocol: 'https:' };
    const document = {
        location, cookie: '', activeElement: null,
        addEventListener(type, callback) { listen(docListeners, type, callback); },
        removeEventListener(type, callback) { unlisten(docListeners, type, callback); },
        createElement(tag) { return new Element(tag); },
    };
    class Element {
        constructor(tag) { this.tag = tag; this.hidden = false; this.inert = false; this.isConnected = true; }
        focus() { effects.push(['focus', this]); document.activeElement = this; }
        remove() {
            counts.remove++;
            if (controls.throwRemove) throw new Error('controlled remove failure');
            this.isConnected = false;
        }
    }
    const window = {
        location,
        addEventListener(type, callback) { listen(windowListeners, type, callback); },
        removeEventListener(type, callback) { unlisten(windowListeners, type, callback); },
    };
    const clock = {
        now: () => now,
        setTimeout(callback, delay) { const id = ++timerId; timers.set(id, { callback, at: now + delay }); return id; },
        clearTimeout(id) { timers.delete(id); },
    };
    class Component {
        constructor(props) { this.props = props; }
        setState(update) { Object.assign(this.state, typeof update === 'function' ? update(this.state, this.props) : update); }
    }
    const context = vm.createContext({
        document, window, URL, Headers, AbortController, DOMException, TextEncoder,
        performance: { now: () => now }, crypto: { randomUUID: () => `owned-${++uuid}` },
        setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout, queueMicrotask,
        console: { error() {} },
        fetch() { throw new Error('unexpected platform fetch in mechanism fixture'); },
        WebSocket: class { constructor() { throw new Error('unexpected realtime connection'); } },
    });
    const preact = new vm.SyntheticModule(['h', 'render', 'Component'], function () {
        this.setExport('h', (type, props, ...children) => ({ type, props: props || {}, children }));
        this.setExport('Component', Component);
        this.setExport('render', (vnode, container) => {
            if (vnode === null) {
                counts.unmount++;
                controls.onUnmount?.(container);
                if (controls.throwUnmount) throw new Error('controlled unmount failure');
                container.vnode = null;
                return;
            }
            counts.mount++;
            container.vnode = vnode;
            controls.onRender?.(container);
            try { vnode.children[0].type({}); }
            catch (error) { new vnode.type(vnode.props).componentDidCatch(error, {}); }
        });
    }, { context, identifier: 'external:preact' });
    const hooks = new vm.SyntheticModule(['useEffect', 'useRef', 'useState'], function () {
        for (const name of ['useEffect', 'useRef', 'useState']) {
            this.setExport(name, () => { throw new Error(`unexpected Preact hook ${name}`); });
        }
    }, { context, identifier: 'external:preact/hooks' });
    const modules = new Map();
    function load(path) {
        assert(floatSources.has(path), `module outside exact float shipping graph: ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(floatSources.get(path),
            { context, identifier: path }));
        return modules.get(path);
    }
    const linker = (specifier, importer) => {
        if (specifier === 'preact') return preact;
        if (specifier === 'preact/hooks') return hooks;
        assert(specifier.startsWith('.'), `unexpected external float import: ${specifier}`);
        return load(resolve(dirname(importer.identifier), specifier));
    };
    const managerModule = load(resolve(floatClient, 'panels/float-manager.js'));
    const interactionModule = load(resolve(floatClient, 'panels/interaction-owner.js'));
    await managerModule.link(linker); await interactionModule.link(linker);
    await managerModule.evaluate(); await interactionModule.evaluate();
    assert.equal(modules.size, 8, 'must evaluate every actual shipping module and no substitute controller');
    const { FloatManager } = managerModule.namespace;
    const { DocumentInteractionOwner } = interactionModule.namespace;
    const budget = modules.get(resolve(floatClient, 'panels/float-reservations.js')).namespace.createFloatReservations();
    const owner = new DocumentInteractionOwner({ reservations: budget, document, window });
    function realm(value) {
        context.fixtureJSON = JSON.stringify(value);
        try { return vm.runInContext('JSON.parse(fixtureJSON)', context); }
        finally { delete context.fixtureJSON; }
    }
    const input = (key = 'one', changes = {}) => realm({ application_id: 'notes', generation: 'g1',
        panel_id: 'preview', capability: 'notes:1:preview', context_key: key, context: { record_id: key }, ...changes });
    const adapter = {
        resolve(descriptor) {
            counts.resolve++;
            return { descriptor, target: { applicationId: descriptor.application_id,
                generation: descriptor.generation, version: '1.0.0', applicationTitle: 'Notes',
                panel: { id: descriptor.panel_id, title: 'Preview', module_url: '/ext/notes/1.0.0/panels/preview.js' } } };
        },
    };
    const f = { owner, budget, document, window, clock, controls, counts, imports, APIs, timers,
        effects, realm, input, adapter, managers, context,
        newFrame({ nativeClock = false, clock: injectedClock = clock } = {}) {
            this.frame = owner.beginFrame();
            this.manager = new FloatManager({ reservations: budget, interaction: this.frame,
                adapter, ...(nativeClock ? {} : { clock: injectedClock }), document, importModule(url) {
                    const wait = deferred(); imports.push({ ...wait, url }); return wait.promise;
                } });
            this.manager.setWorkArea({ width: 1200, height: 800 });
            managers.push(this.manager);
            return this.manager;
        },
        module(options = {}) {
            return { default(api) {
                counts.factory++; APIs.push(api); effects.push(['factory', api]);
                api.onActivate(() => { counts.activate++; effects.push(['activate', api]); options.onActivate?.(api); });
                api.onDeactivate(() => { counts.deactivate++; effects.push(['deactivate', api]); options.onDeactivate?.(api); });
                api.registerShortcut('Ctrl+S', () => { counts.shortcut++; options.onShortcut?.(api); });
                options.onFactory?.(api);
                return () => { options.onComponent?.(api); };
            } };
        },
        async turns() { for (let index = 0; index < 8; index++) await Promise.resolve(); },
        async settle(index = 0, module = this.module()) { imports[index].resolve(module); await this.turns(); },
        async ready(key = 'one', options = {}, moduleOptions = {}) {
            const result = this.manager.admit(input(key), options);
            assert.equal(result.status, 'admitted');
            await this.settle(imports.length - 1, this.module(moduleOptions));
            return result;
        },
        advance(milliseconds, runTimers = false) {
            now += milliseconds;
            if (!runTimers) return;
            for (const [id, timer] of [...timers]) {
                if (timer.at > now) continue;
                timers.delete(id); timer.callback();
            }
        },
        record(token) { return this.manager.records.get(token); },
        listenerCount(type, target = windowListeners) { return target.get(type)?.size || 0; },
        key() {
            const event = { key: 's', ctrlKey: true, altKey: false, metaKey: false, shiftKey: false,
                defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
            for (const callback of docListeners.get('keydown') || []) callback(event);
            return event;
        },
        unload() {
            const event = { defaultPrevented: false, preventDefault() { this.defaultPrevented = true; } };
            for (const callback of windowListeners.get('beforeunload') || []) callback(event);
            return event;
        },
        boundary(token) {
            const vnode = this.record(token).container.vnode;
            new vnode.type(vnode.props).componentDidCatch(new Error('controlled later render failure'), {});
        },
        async cleanup() {
            controls.onRender = controls.onUnmount = null;
            controls.throwRemove = controls.throwUnmount = false;
            for (const manager of managers) manager.retire();
            owner.dispose();
            for (const wait of imports) wait.resolve({ default: () => () => null });
            await this.turns();
            assert.equal(timers.size, 0, 'all readiness timers remain owned');
            assert.equal(this.listenerCount('keydown', docListeners), 0);
            assert.equal(this.listenerCount('beforeunload'), 0);
            assert.equal(budget.status().pending, 0, 'all controlled native imports physically settle');
        },
    };
    f.newFrame();
    assert.equal(f.listenerCount('keydown', docListeners), 1);
    return f;
}

function shippingFloatTest(name, operation) {
    test(name, { timeout: 5000 }, async () => {
        const fixture = await shippingFloatFixture();
        try { await operation(fixture); } finally { await fixture.cleanup(); }
    });
}

shippingFloatTest('shipping float admission without an adapter owns no import or reservation', async f => {
    f.manager.adapter = null;
    assert.equal(f.manager.admit(f.input()).status, 'target-unavailable');
    assert.equal(f.imports.length, 0); assert.equal(f.budget.status().count, 0);
});

shippingFloatTest('shipping float admission distinguishes invalid input from unavailable authority', async f => {
    assert.equal(f.manager.admit(f.input('one', { context: { nested: {} } })).status, 'invalid-input');
    assert.equal(f.counts.resolve, 0);
    f.adapter.resolve = () => { throw new Error('controlled authority outage'); };
    assert.equal(f.manager.admit(f.input()).status, 'target-unavailable');
    f.adapter.resolve = () => null;
    assert.equal(f.manager.admit(f.input()).status, 'target-unavailable');
    assert.equal(f.imports.length, 0); assert.equal(f.budget.status().count, 0);
});

shippingFloatTest('shipping concurrent dedup shares readiness while context-key collisions stay distinct', async f => {
    const first = f.manager.admit(f.input());
    const duplicate = f.manager.admit(f.input('one', { capability: 'notes:01:preview' }));
    assert.equal(duplicate.status, 'deduplicated');
    assert.equal(duplicate.token, first.token); assert.equal(duplicate.ready, first.ready);
    const collision = f.manager.admit(f.input('one', { context: { record_id: 'different' } }));
    assert.equal(collision.status, 'admitted'); assert.notEqual(collision.token, first.token);
    assert.equal(f.imports.length, 2); assert.equal(f.budget.status().count, 2);
    await f.settle(0); await f.settle(1);
    assert.equal(f.counts.factory, 2); assert.equal(f.counts.activate, 2);
});

shippingFloatTest('shipping five minimized ready floats retain all slots without activation', async f => {
    for (let index = 0; index < 5; index++) await f.ready('key-' + index, { presentation: 'minimized' });
    assert.equal(f.counts.factory, 5); assert.equal(f.counts.activate, 0);
    assert.equal(f.manager.admit(f.input('six')).status, 'limit-refused');
    assert.equal(f.imports.length, 5); assert.equal(f.budget.status().count, 5);
});

shippingFloatTest('shipping five loading floats cannot reclaim capacity by closing a pending owner', async f => {
    const admitted = Array.from({ length: 5 }, (_, index) => f.manager.admit(f.input('key-' + index)));
    assert.equal(f.manager.admit(f.input('six')).status, 'limit-refused');
    f.manager.close(admitted[0].token);
    assert.equal(f.budget.status().count, 5);
    assert.equal(f.manager.admit(f.input('six')).status, 'limit-refused');
    await f.settle(0);
    assert.equal(f.counts.factory, 0); assert.equal(f.budget.status().count, 4);
    assert.equal(f.manager.admit(f.input('six')).status, 'admitted');
});

shippingFloatTest('shipping five retained failures count and dedup never retries them automatically', async f => {
    const results = Array.from({ length: 5 }, (_, index) => f.manager.admit(f.input('key-' + index)));
    for (let index = 0; index < 5; index++) await f.settle(index, { default() { throw new Error('controlled factory failure'); } });
    assert(f.manager.snapshot().every(record => record.readiness === 'failed'));
    assert.equal(f.manager.admit(f.input('six')).status, 'limit-refused');
    assert.equal(f.manager.admit(f.input('key-0')).status, 'deduplicated');
    assert.equal(f.imports.length, 5); assert.equal(f.budget.status().count, 5);
    assert.equal(f.record(results[0].token).readiness, 'failed');
});

shippingFloatTest('shipping readiness timeout fences a pending import and disables overlapping Retry', async f => {
    const result = f.manager.admit(f.input());
    f.advance(15000, true);
    assert.equal(f.record(result.token).readiness, 'failed');
    assert.equal(f.manager.retry(result.token), false);
    assert.equal(f.budget.inspect(result.token).pending, true);
    await f.settle(0);
    assert.equal(f.counts.factory, 0); assert.equal(f.imports.length, 1);
    assert.equal(f.manager.retry(result.token), true);
    assert.equal(f.imports.length, 2); await f.settle(1);
    assert.equal(f.record(result.token).readiness, 'ready');
});

shippingFloatTest('shipping pending Close then logout neither repeats cleanup nor unregisters twice', async f => {
    const result = f.manager.admit(f.input()), record = f.record(result.token);
    let unregistered = 0;
    const unregister = record.unregister;
    record.unregister = () => { unregistered++; unregister(); };
    f.manager.close(result.token);
    const removed = f.counts.remove, unmounted = f.counts.unmount;
    f.frame.retire();
    assert.equal(unregistered, 1); assert.equal(f.counts.remove, removed); assert.equal(f.counts.unmount, unmounted);
    await f.settle(0);
    assert.equal(f.counts.factory, 0); assert.equal(f.budget.status().count, 0);
});

shippingFloatTest('shipping five old-frame imports prevent fresh login from importing another five', async f => {
    for (let index = 0; index < 5; index++) f.manager.admit(f.input('old-' + index));
    for (let replacement = 0; replacement < 5; replacement++) {
        f.newFrame();
        assert.equal(f.manager.admit(f.input('new-' + replacement)).status, 'limit-refused');
        assert.equal(f.imports.length, 5);
    }
    for (let index = 0; index < 5; index++) await f.settle(index);
    assert.equal(f.counts.factory, 0); assert.equal(f.budget.status().count, 0);
    assert.equal(f.manager.admit(f.input('fresh')).status, 'admitted');
});

shippingFloatTest('shipping synchronous factory overrun fails before render despite a delayed timer', async f => {
    const result = f.manager.admit(f.input());
    await f.settle(0, f.module({ onFactory() { f.advance(15000); } }));
    assert.equal(f.record(result.token).readiness, 'failed');
    assert.equal(f.counts.mount, 0); assert.equal(f.counts.activate, 0); assert.equal(f.timers.size, 0);
});

shippingFloatTest('shipping synchronous render overrun unmounts once before any activation', async f => {
    f.controls.onRender = () => f.advance(15000);
    const result = f.manager.admit(f.input()); await f.settle(0);
    assert.equal(f.record(result.token).readiness, 'failed');
    assert.equal(f.counts.mount, 1); assert.equal(f.counts.unmount, 1); assert.equal(f.counts.activate, 0);
});

shippingFloatTest('shipping synchronous activation overrun deactivates its partial incarnation once', async f => {
    const result = f.manager.admit(f.input());
    await f.settle(0, f.module({ onActivate() { f.advance(15000); } }));
    assert.equal(f.record(result.token).readiness, 'failed');
    assert.equal(f.counts.activate, 1); assert.equal(f.counts.deactivate, 1); assert.equal(f.counts.unmount, 1);
    f.manager.close(result.token);
    assert.equal(f.counts.deactivate, 1); assert.equal(f.counts.unmount, 1);
});

shippingFloatTest('shipping thrown activation unbinds and deactivates exactly once', async f => {
    const result = f.manager.admit(f.input());
    await f.settle(0, f.module({ onActivate() { throw new Error('controlled partial activation'); } }));
    assert.equal(f.record(result.token).readiness, 'failed');
    assert.equal(f.counts.deactivate, 1);
    assert.throws(() => f.APIs[0].registerShortcut('Ctrl+X', () => {}), error => error.name === 'PanelUnboundError');
    f.manager.close(result.token); f.frame.retire();
    assert.equal(f.counts.activate, 1); assert.equal(f.counts.deactivate, 1); assert.equal(f.counts.unmount, 1);
});

shippingFloatTest('shipping never-shown minimized readiness activates only on its first restore', async f => {
    const result = await f.ready('one', { presentation: 'minimized' });
    const container = f.record(result.token).container, api = f.APIs[0];
    assert.equal(f.counts.activate, 0);
    f.manager.restore(result.token);
    for (let index = 0; index < 4; index++) { f.manager.minimize(result.token); f.manager.restore(result.token); }
    assert.equal(f.counts.activate, 1); assert.equal(f.counts.deactivate, 0);
    assert.equal(f.record(result.token).container, container); assert.equal(f.APIs[0], api);
    assert.equal(f.counts.factory, 1); assert.equal(f.counts.mount, 1);
});

shippingFloatTest('shipping dirty Cancel preserves state and Discard shares one document unload listener', async f => {
    const first = await f.ready('one'), second = await f.ready('two');
    const record = f.record(first.token), container = record.container;
    f.APIs[0].setDirty(true); f.APIs[1].setDirty(true);
    assert.equal(f.listenerCount('beforeunload'), 1);
    assert.equal(f.unload().defaultPrevented, true);
    assert.equal(f.manager.requestClose(first.token), true);
    assert.equal(f.manager.requestClose(second.token), false);
    f.frame.resolveConfirmation(false);
    assert.equal(f.record(first.token).container, container); assert.equal(record.dirty, true);
    assert.equal(f.budget.status().count, 2);
    f.manager.requestClose(first.token); f.frame.resolveConfirmation(true);
    assert.equal(f.budget.status().count, 1); assert.equal(f.listenerCount('beforeunload'), 1);
    f.APIs[1].setDirty(false);
    assert.equal(f.listenerCount('beforeunload'), 0);
});

shippingFloatTest('shipping shortcut dispatch reaches only the focused eligible ready owner', async f => {
    let primary = 0;
    f.frame.registerPrimary({ isDirty: () => false, isEligible: () => true, dispatch: () => primary++ });
    const ready = await f.ready(); f.manager.focus(ready.token); f.key();
    assert.equal(f.counts.shortcut, 1); assert.equal(primary, 0);
    const loading = f.manager.admit(f.input('loading')); f.manager.focus(loading.token); f.key();
    assert.equal(f.counts.shortcut, 1); assert.equal(primary, 0);
    await f.settle(1, {}); f.key();
    assert.equal(f.counts.shortcut, 1); assert.equal(primary, 0);
    f.manager.minimize(ready.token); f.manager.focus(ready.token); f.key();
    assert.equal(f.counts.shortcut, 1);
    f.frame.focusPrimary(); f.key(); assert.equal(primary, 1);
});

shippingFloatTest('shipping confirmation and geometry suspend shortcuts with one modal owner', async f => {
    const result = await f.ready(); f.manager.focus(result.token);
    f.frame.setModal('float', result.token); f.key(); assert.equal(f.counts.shortcut, 1);
    const lease = f.frame.beginGeometry(result.token); assert.equal(typeof lease, 'symbol');
    assert.equal(f.frame.beginGeometry(result.token), null);
    f.key(); assert.equal(f.counts.shortcut, 1);
    f.frame.endGeometry(lease);
    f.APIs[0].setDirty(true); f.manager.requestClose(result.token);
    assert.equal(f.frame.modal.kind, 'confirmation');
    assert.equal(f.frame.confirm({ onCancel() {}, onDiscard() {} }), false);
    f.key(); assert.equal(f.counts.shortcut, 1);
    f.frame.resolveConfirmation(false);
    assert.equal(f.frame.modal.kind, 'float'); f.key(); assert.equal(f.counts.shortcut, 2);
    assert.equal(f.frame.setModal('switcher', Symbol()), false, 'switcher cannot create a second float modal');
});

shippingFloatTest('shipping unmount failure poisons document admission across replacement frames', async f => {
    const result = await f.ready(); f.APIs[0].setDirty(true);
    f.controls.throwUnmount = true; f.manager.close(result.token);
    assert.equal(f.budget.status().poisoned, true); assert.equal(f.budget.status().count, 1);
    assert.equal(f.listenerCount('beforeunload'), 1);
    f.controls.throwUnmount = false; f.newFrame();
    assert.equal(f.manager.admit(f.input('new')).status, 'cleanup-failed');
    assert.equal(f.imports.length, 1); assert.equal(f.budget.status().unsaved, true);
});

shippingFloatTest('shipping failed-incarnation chrome removal failure cannot release a poisoned slot', async f => {
    const result = f.manager.admit(f.input()); await f.settle(0, {});
    assert.equal(f.budget.inspect(result.token).cleanupComplete, true);
    f.controls.throwRemove = true; f.manager.close(result.token);
    assert.equal(f.budget.status().poisoned, true); assert.equal(f.budget.status().count, 1);
    assert.equal(f.manager.admit(f.input('new')).status, 'cleanup-failed');
    assert.equal(f.counts.unmount, 1, 'already-complete incarnation cleanup is not repeated');
});

shippingFloatTest('shipping logout reentrant from deactivation does not unregister the closing record twice', async f => {
    const result = await f.ready('one', {}, { onDeactivate() { f.frame.retire(); } });
    const record = f.record(result.token), unregister = record.unregister;
    let count = 0;
    record.unregister = () => { count++; unregister(); };
    f.manager.close(result.token);
    assert.equal(count, 1);
    assert.equal(f.counts.deactivate, 1); assert.equal(f.counts.unmount, 1); assert.equal(f.counts.remove, 1);
    assert.equal(f.budget.status().count, 0);
});

shippingFloatTest('shipping Close reentrant from factory cannot mount or resurrect a retired owner', async f => {
    const result = f.manager.admit(f.input());
    await f.settle(0, f.module({ onFactory() { f.manager.close(result.token); } }));
    assert.equal(f.counts.mount, 0); assert.equal(f.counts.activate, 0);
    assert.equal(f.budget.status().count, 0); assert.equal(f.manager.records.size, 0);
    assert.deepEqual(plainFloat(await result.ready), { status: 'cancelled' });
});

shippingFloatTest('shipping responsive geometry transitions preserve exact desktop state and component identity', async f => {
    const result = await f.ready(), record = f.record(result.token), container = record.container;
    f.manager.setGeometry(result.token, f.realm({ x: 17, y: 29, width: 500, height: 300 }));
    const geometry = plainFloat(record.geometry); f.manager.maximize(result.token);
    for (const width of [1024, 768, 767, 320, 0, 1025, 1200]) f.manager.setWorkArea({ width, height: 800 });
    assert.deepEqual(plainFloat(record.geometry), geometry); assert.equal(record.maximized, true);
    assert.equal(record.container, container); assert.equal(f.counts.factory, 1);
    assert.equal(f.counts.mount, 1); assert.equal(f.counts.activate, 1); assert.equal(f.counts.deactivate, 0);
    f.manager.setWorkArea({ width: 768, height: 800 }); f.manager.maximize(result.token);
    assert.equal(record.maximized, true, 'modal maximize is unsupported, not a state reset');
});

shippingFloatTest('shipping unavailable refresh or Retry never invents empty authority or automatic reload', async f => {
    const result = f.manager.admit(f.input()); await f.settle(0, {});
    f.adapter.resolve = () => null;
    assert.equal(f.manager.retry(result.token), false);
    assert.equal(f.manager.admit(f.input('new')).status, 'target-unavailable');
    f.advance(60000, true); await f.turns();
    assert.equal(f.imports.length, 1); assert.equal(f.budget.status().count, 1);
    assert.equal(f.record(result.token).readiness, 'failed');
});

shippingFloatTest('shipping panel context is independently owned while public float and navigation SDK remain stubs', async f => {
    const input = f.input(), result = f.manager.admit(input); await f.settle(0);
    const api = f.APIs[0], context = api.getContext();
    input.context.record_id = 'source-mutated'; context.record_id = 'panel-mutated';
    assert.equal(f.record(result.token).descriptor.context.record_id, 'one');
    assert.equal(api.getContext(), context);
    await assert.rejects(api.openFloat('text/plain', {}), error => error.name === 'NotImplementedError');
    assert.throws(() => api.navigate('notes:preview', {}), error => error.name === 'NotImplementedError');
    assert.throws(() => api.requestFocus(), error => error.name === 'NotImplementedError');
    assert.equal(f.imports.length, 1);
});

shippingFloatTest('shipping minimizing a nonfocused float preserves the existing eligible shortcut destination', async f => {
    const first = await f.ready('one'), second = await f.ready('two');
    f.manager.focus(first.token);
    f.manager.minimize(second.token);
    f.key();
    assert.equal(f.counts.shortcut, 1, 'only minimizing the focused owner changes the focus destination');
});

shippingFloatTest('shipping synchronous reserve observer retirement prevents DOM allocation and import', async f => {
    let retired = false;
    const unsubscribe = f.budget.subscribe(status => {
        if (!retired && status.count === 1) { retired = true; f.frame.retire(); }
    });
    const result = f.manager.admit(f.input()); unsubscribe();
    assert.equal(result.status, 'cancelled');
    assert.equal(f.imports.length, 0); assert.equal(f.counts.remove, 0);
    assert.equal(f.budget.status().count, 0); assert.equal(f.timers.size, 0);
});

shippingFloatTest('shipping synchronous provider publication retirement prevents component preparation', async f => {
    let retired = false;
    const unsubscribe = f.frame.subscribe(() => {
        if (!retired && f.frame.floats.size === 1) { retired = true; f.frame.retire(); }
    });
    const result = f.manager.admit(f.input()); unsubscribe();
    assert.equal(result.status, 'cancelled');
    assert.equal(f.imports.length, 0); assert.equal(f.counts.factory, 0);
    assert.equal(f.budget.status().count, 0); assert.equal(f.timers.size, 0);
    assert.equal(f.counts.remove, 1);
});

shippingFloatTest('shipping synchronous begin-attempt retirement settles only its unused preparation', async f => {
    let retired = false;
    const unsubscribe = f.budget.subscribe(status => {
        if (!retired && status.pending === 1) { retired = true; f.frame.retire(); }
    });
    const result = f.manager.admit(f.input()); unsubscribe();
    assert.equal(result.status, 'cancelled');
    assert.equal(f.imports.length, 0); assert.equal(f.counts.factory, 0);
    assert.equal(f.budget.status().pending, 0); assert.equal(f.budget.status().count, 0);
    assert.equal(f.timers.size, 0); assert.equal(f.counts.remove, 1);
});

shippingFloatTest('empty document cleanup poison retains warning and denies admission without a live token', async f => {
    const budget = f.budget;
    let notifications = 0;
    const unsubscribe = budget.subscribe(() => notifications++);
    budget.poison({ dirty: false });
    assert.deepEqual(plainFloat(budget.status()), { count: 0, pending: 0, retiring: 0, poisoned: true, unsaved: false });
    budget.poison();
    assert.equal(budget.status().unsaved, true);
    budget.poison({ dirty: false }); unsubscribe();
    assert.equal(budget.status().unsaved, true, 'later clean status cannot erase uncertain unsaved work');
    assert.equal(budget.reserve(), null);
    assert.equal(notifications, 3);
    assert.throws(() => budget.poison({ dirty: 'yes' }), error => error.name === 'TypeError');
    assert(!Object.hasOwn(budget, 'reset'));
    assert.equal(f.listenerCount('beforeunload'), 1);
    assert.equal(f.unload().defaultPrevented, true);
    f.frame.onRetire(() => { throw new Error('unverifiable frame cleanup'); });
    f.newFrame();
    assert.equal(f.manager.admit(f.input()).status, 'cleanup-failed');
    assert.equal(f.imports.length, 0); assert.equal(f.budget.status().count, 0);
    assert.equal(f.listenerCount('beforeunload'), 1, 'document warning survives an empty replacement frame');
});

shippingFloatTest('shipping native clock binds browser timer calls to their global receiver', async f => {
    f.context.fixtureClock = f.clock;
    vm.runInContext(`(() => {
        const clock = fixtureClock;
        globalThis.setTimeout = function (callback, delay) {
            if (this !== globalThis) throw new TypeError('illegal timer receiver');
            return clock.setTimeout(callback, delay);
        };
        globalThis.clearTimeout = function (timer) {
            if (this !== globalThis) throw new TypeError('illegal timer receiver');
            return clock.clearTimeout(timer);
        };
    })()`, f.context);
    delete f.context.fixtureClock;
    f.newFrame({ nativeClock: true });
    const ready = await f.ready();
    assert.equal(f.record(ready.token).readiness, 'ready');
    assert.equal(f.counts.activate, 1);
    f.manager.close(ready.token);
    assert.equal(f.budget.status().count, 0);
    assert.equal(f.timers.size, 0);
});

shippingFloatTest('shipping synchronous clock setup failure rolls back unused preparation and permits slot reuse', async f => {
    for (const method of ['now', 'setTimeout']) {
        let fail = true;
        const throwingClock = { ...f.clock, [method](...args) {
            if (fail) { fail = false; throw new Error('controlled clock setup failure'); }
            return f.clock[method](...args);
        } };
        f.newFrame({ clock: throwingClock });
        const previousImports = f.imports.length;
        const result = f.manager.admit(f.input(method));
        assert.equal(f.imports.length, previousImports, 'no importer starts before clock setup succeeds');
        assert.equal(f.record(result.token).readiness, 'failed');
        assert.equal(f.budget.status().pending, 0);
        assert.equal(f.budget.status().count, 1);
        f.manager.close(result.token);
        assert.equal(f.budget.status().count, 0);
        const reused = await f.ready(`${method}-reuse`);
        assert.equal(f.record(reused.token).readiness, 'ready');
        f.manager.close(reused.token);
        assert.equal(f.budget.status().count, 0);
    }
});

shippingFloatTest('shipping authority metadata accessors never execute or admit an import', async f => {
    const resolve = f.adapter.resolve;
    let accessed = 0;
    for (const path of [['descriptor'], ['target'], ['target', 'version'], ['target', 'panel'],
        ['target', 'panel', 'title'], ['target', 'panel', 'module_url']]) {
        f.adapter.resolve = descriptor => {
            const result = resolve(descriptor);
            let object = result;
            for (const key of path.slice(0, -1)) object = object[key];
            const key = path.at(-1), value = object[key];
            Object.defineProperty(object, key, { get() { accessed++; return value; } });
            return result;
        };
        assert.equal(f.manager.admit(f.input(path.join('-'))).status, 'target-unavailable');
        assert.equal(accessed, 0);
        assert.equal(f.imports.length, 0); assert.equal(f.budget.status().count, 0);
    }
});

shippingFloatTest('shipping invalid geometry is rejected before authority or document reservation', async f => {
    let accessed = 0;
    const malformed = [f.realm({ x: 0, y: 0, width: 1.5, height: 240 }),
        f.realm({ x: 0, y: 0, width: 320 }), f.realm({ x: 0, y: 0, width: 320, height: 240, extra: true })];
    const infinite = f.realm({ x: 0, y: 0, width: 320, height: 240 });
    infinite.width = Infinity; malformed.push(infinite);
    const accessor = f.realm({ x: 0, y: 0, width: 320, height: 240 });
    Object.defineProperty(accessor, 'width', { get() { accessed++; return 320; } }); malformed.push(accessor);
    for (const geometry of malformed) {
        assert.equal(f.manager.admit(f.input(), { geometry }).status, 'invalid-input');
        assert.equal(f.counts.resolve, 0); assert.equal(accessed, 0);
        assert.equal(f.imports.length, 0); assert.equal(f.budget.status().count, 0);
        assert.equal(f.counts.remove, 0);
    }
});

shippingFloatTest('shipping silent failed content removal retains a poisoned reservation after Close', async f => {
    const result = await f.ready(); const container = f.record(result.token).container;
    let attempts = 0;
    container.remove = () => { attempts++; };
    f.manager.close(result.token);
    assert.equal(attempts, 1); assert.equal(container.isConnected, true);
    assert.equal(f.budget.status().poisoned, true); assert.equal(f.budget.status().count, 1);
    assert.equal(f.budget.inspect(result.token).cleanupComplete, false);
    f.newFrame();
    assert.equal(attempts, 1, 'frame retirement does not retry uncertain physical cleanup');
    assert.equal(f.manager.admit(f.input('replacement')).status, 'cleanup-failed');
    assert.equal(f.imports.length, 1);
});

shippingFloatTest('shipping reserve-publication reentrant duplicate shares one canonical owner and import', async f => {
    let entered = false, nested;
    const unsubscribe = f.budget.subscribe(status => {
        if (entered || status.count !== 1) return;
        entered = true;
        nested = f.manager.admit(f.input('same'));
    });
    let outer;
    try { outer = f.manager.admit(f.input('same')); } finally { unsubscribe(); }
    assert.equal(nested.status, 'admitted');
    assert.equal(outer.status, 'deduplicated');
    assert.equal(outer.token, nested.token);
    assert.equal(outer.id, nested.id);
    assert.equal(outer.ready, nested.ready);
    assert.equal(f.budget.status().count, 1);
    assert.equal(f.manager.records.size, 1);
    assert.equal(f.manager.identities.size, 1);
    assert.equal(f.frame.floats.size, 1);
    assert.equal(f.imports.length, 1, 'the unused outer reservation starts no physical import');
    await f.settle(0);
    assert.equal((await outer.ready).status, 'ready');
    assert.equal(f.counts.factory, 1);
    assert.equal(f.counts.mount, 1);
    assert.equal(f.counts.activate, 1);
    f.manager.close(outer.token);
    assert.equal(f.budget.status().count, 0);
    assert.equal(f.frame.floats.size, 0);
    assert.equal(f.counts.deactivate, 1);
});

shippingFloatTest('shipping confirmation publication retirement fences both Cancel and Discard callbacks', async f => {
    for (const discard of [false, true]) {
        f.newFrame();
        const frame = f.frame; let callbacks = 0;
        assert.equal(frame.confirm({ kind: 'float', recordToken: Symbol(),
            onCancel() { callbacks++; }, onDiscard() { callbacks++; } }), true);
        frame.subscribe(() => { if (!frame.confirmation) frame.retire(); });
        frame.resolveConfirmation(discard);
        assert.equal(frame.retired, true);
        assert.equal(frame.confirmation, null);
        assert.equal(f.owner.frame, null);
        assert.equal(callbacks, 0, 'an obsolete prompt grants neither focus restoration nor discard authorization');
        assert.equal(f.budget.status().count, 0);
        assert.equal(f.imports.length, 0);
    }
});
