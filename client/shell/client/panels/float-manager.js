import { h, render, Component } from 'preact';
import { makePanelApi, normaliseCombo } from './panel_api.js';
import {
    normalizeFloatDescriptor, floatIdentityKey, normalizeFloatContext, copyPanelContext,
    classifyFloatWorkArea, defaultFloatGeometry, clampFloatGeometry, saveFloatGeometry,
} from './float-model.js';
import { createFloatDispatch, trackFloatImport, retireFloatRecord } from './float-reservations.js';

const LOAD_DEADLINE = 15000;
const nativeClock = () => ({ now: () => performance.now(),
    setTimeout: (...args) => globalThis.setTimeout(...args),
    clearTimeout: timer => globalThis.clearTimeout(timer) });
function readiness() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return { promise, resolve };
}
function dataField(value, key) {
    if (!value || typeof value !== 'object') return undefined;
    const field = Object.getOwnPropertyDescriptor(value, key);
    return field && Object.hasOwn(field, 'value') ? field.value : undefined;
}
// These callback factories deliberately do not close over a frame or descriptor.
function dirtyNotifier(dispatch, token, attempt) {
    return (_panelId, dirty) => dispatch.deliver(token, attempt, { phase: 'dirty', dirty });
}
function delayedBoundaryFailure(dispatch, token, attempt) {
    queueMicrotask(() => dispatch.deliver(token, attempt, { phase: 'boundary' }));
}
class FloatBoundary extends Component {
    constructor(props) { super(props); this.state = { failed: false }; }
    componentDidCatch() {
        this.setState({ failed: true });
        this.props.onError();
    }
    render(props, state) { return state.failed ? null : props.children; }
}

// Mechanism only. resolve() is an explicitly injected SYNCHRONOUS current-
// authority port returning {descriptor,target}; #46 owns its reviewed adapter.
// Async discovery belongs to that adapter before this serialized admission
// decision. Missing ports refuse before imports or document reservations.
export class FloatManager {
    constructor({ reservations, interaction, adapter, clock = nativeClock(),
        document: doc = globalThis.document, importModule = url => import(url), onFailure } = {}) {
        this.reservations = reservations;
        this.interaction = interaction;
        this.adapter = adapter;
        this.clock = clock;
        this.document = doc;
        this.importModule = importModule;
        this.onFailure = onFailure;
        this.records = new Map();
        this.identities = new Map();
        this.dispatch = createFloatDispatch();
        this.listeners = new Set();
        this.focusListeners = new Set();
        this.chromeCleanup = new Map();
        this.retired = false;
        this.workArea = classifyFloatWorkArea(0, 0);
        this.unsubscribeFrame = interaction.onRetire(() => this.retire());
        this.unsubscribeInteraction = interaction.subscribe(() => this.reap());
    }

    isCurrent() { return !this.retired && this.interaction.isCurrent(); }
    subscribe(callback) { this.listeners.add(callback); return () => this.listeners.delete(callback); }
    onFocus(callback) { this.focusListeners.add(callback); return () => this.focusListeners.delete(callback); }
    focusTrigger(token) {
        const record = this.records.get(token);
        return this.owns(record) ? record.trigger : null;
    }
    registerChromeCleanup(token, callback) {
        if (!this.owns(this.records.get(token)) || typeof callback !== 'function' ||
            this.chromeCleanup.has(token)) throw new Error('invalid float chrome owner');
        this.chromeCleanup.set(token, callback);
        return () => {
            if (this.chromeCleanup.get(token) === callback) this.chromeCleanup.delete(token);
        };
    }
    cleanupChrome(record) {
        const callback = this.chromeCleanup.get(record.token);
        this.chromeCleanup.delete(record.token); // Fence reentrant cleanup first.
        try { callback?.(); return true; }
        catch {
            this.reservations.failCleanup(record.token, { dirty: record.dirty });
            return false;
        }
    }
    changed() {
        if (!this.isCurrent()) return;
        this.interaction.publish();
        for (const callback of [...this.listeners]) callback();
    }
    snapshot() {
        return [...this.records.values()].map(record => ({
            token: record.token, id: record.id, title: record.title,
            applicationTitle: record.applicationTitle, readiness: record.readiness,
            presentation: record.presentation, maximized: record.maximized,
            geometry: record.geometry, container: record.container, dirty: record.dirty,
            retryAvailable: record.readiness === 'failed' && !record.retiring &&
                !this.reservations.status().poisoned &&
                this.reservations.inspect(record.token)?.pending === false &&
                this.reservations.inspect(record.token)?.cleanupComplete === true,
            rank: record.rank, retiring: record.retiring, liveOnly: true,
        }));
    }

    authorize(input) {
        const descriptor = normalizeFloatDescriptor(input);
        const resolved = this.adapter.resolve(descriptor);
        if (!resolved) return null;
        const fresh = normalizeFloatDescriptor(dataField(resolved, 'descriptor'));
        const source = dataField(resolved, 'target');
        const panel = dataField(source, 'panel');
        const target = { applicationId: dataField(source, 'applicationId'),
            generation: dataField(source, 'generation'), version: dataField(source, 'version'),
            applicationTitle: dataField(source, 'applicationTitle'),
            panel: { id: dataField(panel, 'id'), title: dataField(panel, 'title'),
                module_url: dataField(panel, 'module_url') } };
        if (!target || target.applicationId !== fresh.application_id ||
            target.generation !== fresh.generation || target.panel?.id !== fresh.panel_id ||
            typeof target.version !== 'string' || !target.version.length || target.version.length > 128 ||
            typeof target.panel.title !== 'string' || !target.panel.title.length || target.panel.title.length > 256 ||
            typeof target.applicationTitle !== 'string' || !target.applicationTitle.length ||
            target.applicationTitle.length > 128 || typeof target.panel.module_url !== 'string' ||
            target.panel.module_url.length > 2048) return null;
        const url = new URL(target.panel.module_url, this.document.location.href);
        if (url.origin !== this.document.location.origin || url.username || url.password || url.hash) return null;
        // Copy shell-owned metadata. Adapter labels/URL are not descriptor identity.
        return { descriptor: fresh, target: {
            applicationId: target.applicationId, generation: target.generation, version: target.version,
            applicationTitle: target.applicationTitle,
            panel: { id: target.panel.id, title: target.panel.title, module_url: url.href },
        } };
    }

    admit(input, { presentation = 'shown', geometry = null, maximized = false,
        background = false, trigger = null } = {}) {
        if (!this.isCurrent()) return { status: 'cancelled' };
        // A reservation observer may synchronously admit at the release
        // boundary before the ordinary scope observer runs.
        this.reap();
        if (!this.isCurrent()) return { status: 'cancelled' };
        if (!this.adapter || typeof this.adapter.resolve !== 'function') return { status: 'target-unavailable' };
        if (presentation !== 'shown' && presentation !== 'minimized') return { status: 'invalid-input' };
        let normalized;
        try {
            normalized = normalizeFloatDescriptor(input);
            if (geometry !== null) geometry = saveFloatGeometry(geometry);
        }
        catch { return { status: 'invalid-input' }; }
        let authorized;
        try { authorized = this.authorize(normalized); }
        catch { return { status: 'target-unavailable' }; }
        if (!this.isCurrent()) return { status: 'cancelled' };
        if (!authorized) return { status: 'target-unavailable' };
        const key = floatIdentityKey(authorized.descriptor);
        const existing = this.identities.get(key);
        if (existing) {
            if (existing.retiring) return { status: 'cancelled' };
            if (!background) this.restore(existing.token);
            return { status: 'deduplicated', token: existing.token, id: existing.id, ready: existing.ready.promise };
        }
        const token = this.reservations.reserve();
        if (!token) return { status: this.reservations.status().poisoned ? 'cleanup-failed' : 'limit-refused' };
        if (!this.isCurrent()) {
            this.reservations.retire(token);
            return { status: 'cancelled' };
        }
        // reserve() publishes synchronously. An observer may already have
        // admitted this exact identity; never replace its dedup owner.
        const concurrent = this.identities.get(key);
        if (concurrent) {
            this.reservations.retire(token); // This unused slot owns no import.
            if (!this.owns(concurrent)) return { status: 'cancelled' };
            if (!background) this.restore(concurrent.token);
            return { status: 'deduplicated', token: concurrent.token,
                id: concurrent.id, ready: concurrent.ready.promise };
        }
        let container, id;
        try {
            container = this.document.createElement('section');
            id = `float-${crypto.randomUUID()}`;
        } catch {
            this.reservations.retire(token);
            return { status: 'load-failed' };
        }
        container.className = 'float-panel-content';
        const ready = readiness();
        const record = {
            token, id, key, descriptor: authorized.descriptor,
            target: authorized.target, title: authorized.target.panel.title,
            applicationTitle: authorized.target.applicationTitle, container, ready,
            readiness: 'loading', presentation, maximized: maximized === true,
            geometry: geometry || defaultFloatGeometry(this.workArea, this.records.size),
            trigger, rank: this.records.size, dirty: false, retiring: false, incarnation: null,
        };
        this.records.set(token, record);
        this.identities.set(key, record);
        record.unregister = this.interaction.registerFloat(token, {
            isDirty: () => record.dirty,
            isEligible: () => this.owns(record) && record.readiness === 'ready' &&
                record.presentation === 'shown' && !record.retiring,
            dispatch: event => this.dispatchShortcut(record, event),
        });
        if (!this.owns(record)) {
            record.unregister?.();
            record.unregister = null;
            return { status: 'cancelled' };
        }
        this.start(record);
        if (!this.owns(record)) return { status: 'cancelled' };
        this.changed();
        if (!background && presentation === 'shown') this.focus(token);
        return { status: 'admitted', token, id: record.id, ready: ready.promise };
    }

    owns(record) { return !!record && this.isCurrent() && this.records.get(record.token) === record && !record.retiring; }
    currentAttempt(record, attempt) {
        return this.owns(record) && record.incarnation?.token === attempt && !record.incarnation.fenced;
    }
    start(record) {
        if (!this.owns(record)) return false;
        const attempt = this.reservations.beginAttempt(record.token);
        if (!attempt) return false;
        if (!this.owns(record)) {
            // No importer was invoked: there is no physical preparation to
            // cancel. Settle exactly this unused attempt and its verified cleanup.
            this.reservations.settleAttempt(record.token, attempt);
            this.reservations.completeCleanup(record.token, attempt);
            return false;
        }
        record.readiness = 'loading';
        record.incarnation = { token: attempt, deadline: Infinity,
            fenced: false, api: null, activated: false, deactivated: false,
            unmounted: false, unbound: false, preparing: false, renderFailed: false };
        try {
            record.incarnation.deadline = this.clock.now() + LOAD_DEADLINE;
            this.bind(record, attempt);
            record.timer = this.clock.setTimeout(() => this.fail(record, attempt), LOAD_DEADLINE);
        } catch {
            // Nothing physical was started; never strand a phantom import.
            this.reservations.settleAttempt(record.token, attempt);
            this.fail(record, attempt);
            return false;
        }
        let promise;
        try { promise = this.importModule(record.target.panel.module_url); }
        catch (error) { promise = Promise.reject(error); }
        // The only native-import continuations live in the document helper.
        try { trackFloatImport(this.reservations, this.dispatch, record.token, attempt, promise); }
        catch { this.fail(record, attempt); }
        return true;
    }
    bind(record, attempt) {
        this.dispatch.bind(record.token, attempt, outcome => this.receive(record, attempt, outcome));
    }
    receive(record, attempt, outcome) {
        if (!this.currentAttempt(record, attempt)) { this.changed(); return; }
        this.bind(record, attempt); // Dispatch consumes each exact binding before invocation.
        if (outcome.phase === 'dirty') {
            record.dirty = outcome.dirty === true;
            this.changed();
            return;
        }
        if (outcome.phase === 'boundary') { this.fail(record, attempt); return; }
        if (!outcome.ok || this.clock.now() >= record.incarnation.deadline) {
            this.fail(record, attempt);
            return;
        }
        this.prepare(record, attempt, outcome.module);
    }
    checkLoading(record, attempt) {
        return this.currentAttempt(record, attempt) && this.clock.now() < record.incarnation.deadline;
    }
    prepare(record, attempt, module) {
        const incarnation = record.incarnation;
        try {
            if (typeof module?.default !== 'function') throw new Error('missing float factory');
            incarnation.api = makePanelApi({
                shell: { notifyDirtyChange: dirtyNotifier(this.dispatch, record.token, attempt) },
                panel: record.target.panel,
                context: copyPanelContext(normalizeFloatContext(record.descriptor.context)),
                packageRow: { name: record.target.applicationId, version: record.target.version,
                    generation: record.target.generation },
            });
            const Panel = module.default(incarnation.api);
            if (!this.checkLoading(record, attempt) || typeof Panel !== 'function') throw new Error('float factory failed');
            incarnation.preparing = true;
            render(h(FloatBoundary, { onError: () => {
                incarnation.renderFailed = true;
                if (!incarnation.preparing) {
                    // Immediately exclude failed content; cleanup follows after Preact unwinds.
                    record.readiness = 'failed';
                    delayedBoundaryFailure(this.dispatch, record.token, attempt);
                }
            } }, h(Panel, {})), record.container);
            incarnation.preparing = false;
            if (incarnation.renderFailed || !this.checkLoading(record, attempt)) throw new Error('float render failed');
            if (record.presentation === 'shown' && !this.activate(record, attempt, true)) return;
            if (!this.checkLoading(record, attempt)) throw new Error('float readiness deadline');
            record.readiness = 'ready';
            this.clock.clearTimeout(record.timer);
            record.timer = null;
            record.ready.resolve({ status: 'ready' });
            this.changed(); // Background readiness never requests focus.
        } catch { this.fail(record, attempt); }
    }
    activate(record, attempt, loading = false) {
        const incarnation = record.incarnation;
        if (!this.currentAttempt(record, attempt)) return false;
        if (incarnation.activated) return true;
        incarnation.activated = true;
        try { incarnation.api.__shell_internal.fireActivate(); }
        catch { this.fail(record, attempt); return false; }
        if (!this.currentAttempt(record, attempt) || (loading && !this.checkLoading(record, attempt))) {
            this.fail(record, attempt);
            return false;
        }
        return true;
    }
    diagnostic() {
        if (!this.isCurrent()) return;
        try { this.onFailure?.('float-component-failed'); }
        catch { this.reservations.poison({ dirty: true }); }
    }
    cleanup(record, { completeReservation = true } = {}) {
        const incarnation = record.incarnation;
        if (!incarnation) return true;
        incarnation.fenced = true;
        this.clock.clearTimeout(record.timer);
        record.timer = null;
        this.dispatch.detach(record.token);
        let complete = true;
        if (incarnation.activated && !incarnation.deactivated) {
            incarnation.deactivated = true;
            try { incarnation.api?.__shell_internal.fireDeactivate(); } catch { this.diagnostic(); }
        }
        if (!incarnation.unmounted) {
            incarnation.unmounted = true;
            try { render(null, record.container); } catch { complete = false; }
        }
        if (!incarnation.unbound) {
            incarnation.unbound = true;
            try { incarnation.api?.__shell_internal.unbind(); } catch { complete = false; }
        }
        incarnation.api = null;
        if (!complete) this.reservations.failCleanup(record.token, { dirty: record.dirty });
        else if (completeReservation) this.reservations.completeCleanup(record.token, incarnation.token);
        return complete;
    }
    fail(record, attempt) {
        if (!this.currentAttempt(record, attempt)) return;
        record.readiness = 'failed';
        this.cleanup(record);
        record.ready.resolve({ status: 'load-failed' });
        this.diagnostic();
        this.changed();
    }
    retry(token) {
        const record = this.records.get(token);
        if (!this.owns(record) || !this.snapshot().find(item => item.token === token)?.retryAvailable) return false;
        let authorized;
        try { authorized = this.authorize(record.descriptor); } catch { return false; }
        if (!authorized || !this.owns(record) || floatIdentityKey(authorized.descriptor) !== record.key) return false;
        record.target = authorized.target;
        record.ready = readiness();
        record.dirty = false;
        this.start(record);
        this.changed();
        return true;
    }
    focus(token, { focusChrome = true } = {}) {
        const record = this.records.get(token);
        if (!this.owns(record) || record.presentation !== 'shown') return;
        const ordered = [...this.records.values()].sort((a, b) => a.rank - b.rank).filter(item => item !== record);
        ordered.push(record);
        ordered.forEach((item, rank) => { item.rank = rank; });
        this.interaction.focusFloat(token);
        this.changed();
        if (focusChrome) for (const callback of [...this.focusListeners]) callback(token);
    }
    minimize(token) {
        const record = this.records.get(token);
        if (!this.owns(record)) return;
        record.presentation = 'minimized';
        record.container.hidden = true;
        record.container.inert = true;
        if (this.interaction.focused.kind === 'float' && this.interaction.focused.token === token) {
            this.interaction.focusShell();
        }
        this.changed();
    }
    restore(token) {
        const record = this.records.get(token);
        if (!this.owns(record)) return;
        record.presentation = 'shown';
        record.container.hidden = false;
        record.container.inert = false;
        if (record.readiness === 'ready') this.activate(record, record.incarnation.token);
        this.focus(token);
        this.changed();
    }
    maximize(token) {
        const record = this.records.get(token);
        if (!this.owns(record) || this.workArea.mode !== 'desktop') return;
        record.maximized = !record.maximized;
        this.changed();
    }
    setGeometry(token, geometry) {
        const record = this.records.get(token);
        if (!this.owns(record) || this.workArea.mode !== 'desktop') return;
        const value = clampFloatGeometry(geometry, this.workArea);
        if (value) { record.geometry = value; this.changed(); }
    }
    setWorkArea(area) {
        if (!this.isCurrent()) return;
        const next = classifyFloatWorkArea(area.width, area.height);
        if (next.width === this.workArea.width && next.height === this.workArea.height) return;
        this.workArea = next;
        for (const record of this.records.values()) {
            if (!record.geometry) record.geometry = defaultFloatGeometry(this.workArea, record.rank);
        }
        this.changed();
    }
    requestClose(token) {
        const record = this.records.get(token);
        if (!this.owns(record) || this.interaction.confirmation) return false;
        if (!record.dirty) { this.close(token); return true; }
        return this.interaction.confirm({ kind: 'float', recordToken: token,
            trigger: this.document.activeElement,
            onCancel: () => this.focus(token), onDiscard: () => this.close(token) });
    }
    close(token) {
        const record = this.records.get(token);
        if (!record || record.retiring) return;
        record.retiring = true;
        this.interaction.dismissConfirmation(token);
        this.reservations.holdCleanup(token);
        retireFloatRecord(this.reservations, this.dispatch, token, { dirty: record.dirty });
        const chromeComplete = this.cleanupChrome(record);
        let complete = this.cleanup(record, { completeReservation: false }) && chromeComplete;
        try {
            record.container.remove();
            if (record.container.isConnected) throw new Error('float content removal failed');
        }
        catch { complete = false; }
        // Remove the shortcut/provider owner before publishing reusable quota.
        const unregister = record.unregister;
        record.unregister = null;
        unregister?.();
        if (complete) this.reservations.completeCleanup(token, record.incarnation?.token);
        else this.reservations.failCleanup(token, { dirty: record.dirty });
        record.ready.resolve({ status: 'cancelled' });
        record.target = record.descriptor = record.incarnation = record.trigger = null;
        this.reap();
        this.changed();
    }
    reap() {
        if (!this.isCurrent()) return;
        let changed = false;
        for (const [token, record] of this.records) {
            if (record.retiring && !this.reservations.inspect(token)) {
                this.records.delete(token);
                this.identities.delete(record.key);
                changed = true;
            }
        }
        if (changed) {
            [...this.records.values()].sort((a, b) => a.rank - b.rank).forEach((record, rank) => { record.rank = rank; });
            for (const callback of [...this.listeners]) callback();
        }
    }
    dispatchShortcut(record, event) {
        const api = record.incarnation?.api;
        if (!this.owns(record) || record.readiness !== 'ready' || record.presentation !== 'shown' || !api) return;
        const modifiers = [];
        if (event.altKey) modifiers.push('Alt');
        if (event.ctrlKey) modifiers.push('Ctrl');
        if (event.metaKey) modifiers.push('Meta');
        if (event.shiftKey) modifiers.push('Shift');
        let combo;
        try { combo = normaliseCombo([...modifiers, event.key.length === 1 ? event.key.toUpperCase() : event.key].join('+')); }
        catch { return; }
        try { api.__shell_internal.getShortcuts().get(combo)?.(event); }
        catch { this.fail(record, record.incarnation.token); }
    }
    retire() {
        if (this.retired) return;
        // Fence all current dispatch before invoking any extension cleanup.
        this.retired = true;
        this.dispatch.clear();
        this.listeners.clear();
        this.focusListeners.clear();
        this.unsubscribeInteraction();
        this.unsubscribeFrame();
        for (const record of this.records.values()) {
            if (record.retiring) {
                const unregister = record.unregister;
                record.unregister = null;
                unregister?.();
                continue; // Its completed cleanup must not be reopened on logout.
            }
            this.reservations.holdCleanup(record.token);
            retireFloatRecord(this.reservations, this.dispatch, record.token, { dirty: record.dirty });
            const chromeComplete = this.cleanupChrome(record);
            let complete = this.cleanup(record, { completeReservation: false }) && chromeComplete;
            try {
                record.container.remove();
                if (record.container.isConnected) throw new Error('float content removal failed');
            }
            catch { complete = false; }
            if (complete) this.reservations.completeCleanup(record.token, record.incarnation?.token);
            else this.reservations.failCleanup(record.token, { dirty: record.dirty });
            record.ready.resolve({ status: 'cancelled' });
            record.unregister?.();
            record.target = record.descriptor = record.incarnation = record.trigger = record.unregister = null;
        }
        this.records.clear();
        this.identities.clear();
        this.chromeCleanup.clear();
        this.adapter = this.importModule = this.onFailure = null;
    }
}
