import { h, render, Component } from 'preact';
import { makePanelApi, normaliseCombo } from './panel_api.js';

const MAX_INACTIVE = 8;

class PanelImportError extends Error {
    constructor(target, cause) {
        super(`panel ${target.applicationId}#${target.panel.id} module import failed`);
        this.name = 'PanelImportError';
        this.cause = cause;
    }
}

function instanceKey(target) {
    return `${target.generation}\u0000${target.panel.id}`;
}

class PanelBoundary extends Component {
    constructor(props) {
        super(props);
        this.state = { error: null };
    }
    componentDidCatch(error, info) {
        this.setState({ error });
        this.props.onError(error, info);
    }
    render(props, state) {
        return state.error ? props.fallback(state.error) : props.children;
    }
}

function FailureView({ onRetry, onHome }) {
    return h('div', { class: 'panel-failure', role: 'alert' },
        h('p', null, 'This panel could not be displayed.'),
        h('div', { class: 'panel-failure-actions' },
            h('button', { type: 'button', onClick: onRetry }, 'Retry'),
            h('button', { type: 'button', onClick: onHome }, 'Return to Home')));
}

export class PanelManager {
    constructor(host, options = {}) {
        this.host = host;
        this.options = options;
        this.instances = new Map();
        this.active = null;
        this.clock = 0;
        this.beforeUnloadInstalled = false;
        this.beforeUnload = event => {
            if (!this.active?.dirty) return;
            event.preventDefault();
            event.returnValue = '';
        };
        this.keydown = event => this.dispatchShortcut(event);
        document.addEventListener('keydown', this.keydown);
    }

    dispatchShortcut(event) {
        if (!this.active || this.active.status !== 'active') return;
        const mods = [];
        if (event.altKey) mods.push('Alt');
        if (event.ctrlKey) mods.push('Ctrl');
        if (event.metaKey) mods.push('Meta');
        if (event.shiftKey) mods.push('Shift');
        const key = event.key.length === 1 ? event.key.toUpperCase() : event.key;
        let combo;
        try { combo = normaliseCombo([...mods, key].join('+')); } catch { return; }
        const callback = this.active.api.__shell_internal.getShortcuts().get(combo);
        if (typeof callback !== 'function') return;
        event.preventDefault();
        try { callback(event); } catch (error) { this.reportFailure(this.active, error); }
    }

    reportFailure(instance, error, info = null) {
        this.options.onFailure?.(instance.target, error, info);
    }

    failureView(instance) {
        return h(FailureView, {
            onRetry: () => this.options.onRetry?.(instance.target),
            onHome: () => this.options.onHome?.(),
        });
    }

    async prepare(target) {
        const key = instanceKey(target);
        const retained = this.instances.get(key);
        if (retained) {
            if (retained.loadPromise) await retained.loadPromise;
            return retained;
        }

        const container = document.createElement('section');
        container.className = 'panel-container';
        container.hidden = true;
        container.tabIndex = -1;
        container.setAttribute('role', 'tabpanel');
        container.setAttribute('aria-labelledby', target.tabId);
        container.id = target.paneId;
        container.dataset.ipoint = `ext.${target.applicationId}.${target.panel.id}.primaryPane`;
        container.dataset.ipointLayer = 'extension';
        this.host.append(container);

        const instance = {
            key, target, container, api: null, dirty: false, lastUsed: 0,
            status: 'loading', loadPromise: null,
        };
        this.instances.set(key, instance);
        instance.loadPromise = (async () => {
            try {
                const importer = this.options.importModule || (url => import(url));
                let module;
                try {
                    module = await importer(target.panel.module_url);
                } catch (cause) {
                    throw new PanelImportError(target, cause);
                }
                if (!this.instances.has(instance.key)) {
                    throw new DOMException('panel load superseded', 'AbortError');
                }
                if (typeof module.default !== 'function') {
                    throw new Error(`panel ${target.applicationId}#${target.panel.id} has no default export factory`);
                }
                instance.api = makePanelApi({
                    shell: {
                        notifyDirtyChange: (_panelId, dirty) => this.dirtyChanged(instance, dirty),
                    },
                    panel: target.panel,
                    context: {},
                    packageRow: {
                        name: target.applicationId,
                        version: target.version,
                        generation: target.generation,
                    },
                });
                const PanelComponent = module.default(instance.api);
                let renderError = null;
                render(h(PanelBoundary, {
                    onError: (error, info) => {
                        renderError = error;
                        instance.status = 'failed';
                        this.reportFailure(instance, error, info);
                    },
                    fallback: () => this.failureView(instance),
                }, h(PanelComponent, {})), container);
                if (renderError) throw renderError;
                instance.status = 'prepared';
            } catch (error) {
                if (error.name !== 'AbortError' && instance.status !== 'failed') {
                    this.reportFailure(instance, error);
                }
                this.destroy(instance, { deactivate: false });
                throw error;
            } finally {
                instance.loadPromise = null;
            }
        })();
        await instance.loadPromise;
        return instance;
    }

    commit(instance, { discardActive = false } = {}) {
        if (!this.instances.has(instance.key)) throw new Error('panel instance is no longer available');
        if (this.active === instance) return instance;
        const previous = this.active;
        if (previous) {
            const previousFailed = previous.status === 'failed';
            try { previous.api.__shell_internal.fireDeactivate(); }
            catch (error) { this.reportFailure(previous, error); }
            if (discardActive || previous.dirty || previousFailed) {
                this.destroy(previous, { deactivate: false });
            } else {
                previous.container.hidden = true;
                previous.status = 'inactive';
                previous.lastUsed = ++this.clock;
            }
        }

        this.active = instance;
        instance.container.hidden = false;
        instance.status = 'active';
        instance.lastUsed = ++this.clock;
        try {
            instance.api.__shell_internal.fireActivate();
        } catch (error) {
            if (previous && this.instances.has(previous.key) && !discardActive && !previous.dirty) {
                this.reportFailure(instance, error);
                // The candidate became active before its activation callbacks
                // ran. Destruction must therefore run its deactivation chain
                // even though activation failed, before restoring the prior
                // clean instance.
                this.destroy(instance, { deactivate: true });
                this.active = previous;
                previous.container.hidden = false;
                previous.status = 'active';
                try { previous.api.__shell_internal.fireActivate(); }
                catch (reactivateError) { this.showFailure(previous, reactivateError); }
                this.syncUnloadGuard();
                throw error;
            }
            this.showFailure(instance, error);
        }
        this.syncUnloadGuard();
        this.evict();
        return instance;
    }

    showFailure(instance, error) {
        instance.status = 'failed';
        render(this.failureView(instance), instance.container);
        instance.container.hidden = false;
        this.reportFailure(instance, error);
    }

    deactivateToHome({ discardActive = false } = {}) {
        const previous = this.active;
        if (!previous) return;
        const previousFailed = previous.status === 'failed';
        try { previous.api.__shell_internal.fireDeactivate(); }
        catch (error) { this.reportFailure(previous, error); }
        this.active = null;
        if (discardActive || previous.dirty || previousFailed) {
            this.destroy(previous, { deactivate: false });
        } else {
            previous.container.hidden = true;
            previous.status = 'inactive';
            previous.lastUsed = ++this.clock;
        }
        this.syncUnloadGuard();
        this.evict();
    }

    dirtyChanged(instance, dirty) {
        if (!this.instances.has(instance.key)) return;
        instance.dirty = dirty;
        if (instance !== this.active && dirty) {
            this.destroy(instance, { deactivate: false });
        }
        this.syncUnloadGuard();
        this.options.onDirtyChange?.(this.active?.dirty === true);
    }

    syncUnloadGuard() {
        const required = this.active?.dirty === true;
        if (required && !this.beforeUnloadInstalled) {
            window.addEventListener('beforeunload', this.beforeUnload);
            this.beforeUnloadInstalled = true;
        } else if (!required && this.beforeUnloadInstalled) {
            window.removeEventListener('beforeunload', this.beforeUnload);
            this.beforeUnloadInstalled = false;
        }
    }

    evict() {
        const inactive = [...this.instances.values()]
            .filter(instance => instance !== this.active && instance.status === 'inactive')
            .sort((a, b) => a.lastUsed - b.lastUsed);
        while (inactive.length > MAX_INACTIVE) this.destroy(inactive.shift(), { deactivate: false });
    }

    reconcile(targetKeys) {
        let activeRemoved = false;
        for (const instance of [...this.instances.values()]) {
            if (!targetKeys.has(instance.key)) {
                activeRemoved ||= instance === this.active;
                this.destroy(instance, { deactivate: instance === this.active });
            }
        }
        return activeRemoved;
    }

    destroy(instance, { deactivate = instance === this.active } = {}) {
        if (!instance || !this.instances.has(instance.key)) return;
        if (deactivate && instance.api) {
            try { instance.api.__shell_internal.fireDeactivate(); }
            catch (error) { this.reportFailure(instance, error); }
        }
        if (this.active === instance) this.active = null;
        try { render(null, instance.container); } catch { /* cleanup continues */ }
        instance.api?.__shell_internal.unbind();
        instance.container.remove();
        instance.status = 'destroyed';
        this.instances.delete(instance.key);
        this.syncUnloadGuard();
    }

    destroyPrepared(instance) {
        if (instance !== this.active && instance?.status === 'prepared') {
            this.destroy(instance, { deactivate: false });
        }
    }

    destroyAll() {
        for (const instance of [...this.instances.values()]) {
            this.destroy(instance, { deactivate: instance === this.active });
        }
    }

    dispose() {
        this.destroyAll();
        document.removeEventListener('keydown', this.keydown);
        if (this.beforeUnloadInstalled) window.removeEventListener('beforeunload', this.beforeUnload);
        this.beforeUnloadInstalled = false;
    }

    get activeDirty() { return this.active?.dirty === true; }
    get activeTarget() { return this.active?.target || null; }
}

let compatibilityManager = null;

// Backward-compatible internal smoke seam. Launcher code consumes PanelManager
// and passes the canonical discovery module URL directly.
export async function loadPanel(extName, extVersion, panelId, container, opts = {}) {
    if (!compatibilityManager || compatibilityManager.host !== container) {
        compatibilityManager?.dispose();
        compatibilityManager = new PanelManager(container);
    }
    let panel = opts.panel;
    if (!panel) {
        const response = await fetch(`/ext/${encodeURIComponent(extName)}/${encodeURIComponent(extVersion)}/panels.json`,
            { credentials: 'include' });
        if (!response.ok) throw new Error(`panels.json fetch failed: ${response.status}`);
        panel = (await response.json()).panels?.find(entry => entry.id === panelId);
    }
    if (!panel?.client_path && !panel?.module_url) {
        throw new Error(`panel ${extName}#${panelId} missing client_path`);
    }
    const target = {
        applicationId: extName,
        generation: opts.generation || `${extName}@${extVersion}`,
        version: extVersion,
        panel: {
            ...panel,
            module_url: panel.module_url ||
                `/ext/${encodeURIComponent(extName)}/${encodeURIComponent(extVersion)}/panels/${panel.client_path}`,
        },
        tabId: `compat-tab-${panelId}`,
        paneId: `compat-pane-${panelId}`,
    };
    const instance = await compatibilityManager.prepare(target);
    compatibilityManager.commit(instance);
    return { panelApi: instance.api, panel, instance };
}

export function __activePanelForTest() {
    return compatibilityManager?.active || null;
}
