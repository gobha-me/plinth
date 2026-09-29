import { h, Component } from 'preact';
import { call, reconnectRealtime, subscribe } from '@plinth/frontend/sdk';
import { PanelManager } from '../panels/loader.js';
import {
    catalogTargetKeys, choosePanel, makeTarget, normalizeCatalog,
    normalizeLauncherPreference, orderApplications, updatePreference,
} from './model.js';

const APPLICATIONS_CHANNEL = 'plinth:system:applications.changed';
const DEVELOPMENT_MODE = window.__PLINTH_PRODUCTION__ === false;
const TERMINAL_SESSION = new Set(['not_authenticated', 'session_expired', 'session_revoked']);
const TERMINAL_RECOVERABLE = new Set(['auth_failed', 'auth_timeout']);
const IDENTITY_COLORS = Object.freeze([
    'var(--accent)', 'var(--success)', 'var(--warn)', 'var(--danger)', 'var(--text-2)',
]);
const ICON_PATHS = Object.freeze({
    'edit-3': [
        'M12 20h9',
        'M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4Z',
    ],
});

function monogram(title) {
    return [...title.trim()][0]?.toUpperCase() || '?';
}

function identityColor(applicationId) {
    let hash = 2166136261;
    for (const character of applicationId) {
        hash ^= character.codePointAt(0);
        hash = Math.imul(hash, 16777619);
    }
    return IDENTITY_COLORS[(hash >>> 0) % IDENTITY_COLORS.length];
}

function ApplicationMark({ application, iconToken = application.icon, small = false }) {
    const resolvedIcon = ICON_PATHS[iconToken] ? iconToken
        : (ICON_PATHS[application.icon] ? application.icon : null);
    return h('span', {
        class: `app-monogram${small ? ' small' : ''}`,
        'aria-hidden': 'true',
        'data-icon-token': resolvedIcon,
        style: { '--app-color': identityColor(application.id) },
    }, resolvedIcon ? h('svg', {
        viewBox: '0 0 24 24', fill: 'none', stroke: 'currentColor',
        'stroke-width': '1.75', 'stroke-linecap': 'round', 'stroke-linejoin': 'round',
    }, ICON_PATHS[resolvedIcon].map(path => h('path', { d: path }))) : monogram(application.title));
}

function DirtyDialog({ onCancel, onDiscard, isCurrent }) {
    let dialog;
    const keydown = event => {
        if (!isCurrent() || !dialog?.isConnected) return;
        if (event.key === 'Escape') {
            event.preventDefault();
            onCancel();
            return;
        }
        if (event.key !== 'Tab') return;
        const controls = [...dialog.querySelectorAll('button:not([disabled])')];
        const index = controls.indexOf(document.activeElement);
        const next = event.shiftKey
            ? (index <= 0 ? controls.length - 1 : index - 1)
            : (index < 0 || index === controls.length - 1 ? 0 : index + 1);
        event.preventDefault();
        controls[next]?.focus();
    };
    return h('div', { class: 'dialog-scrim' },
        h('div', {
            class: 'dirty-dialog', role: 'dialog', 'aria-modal': 'true',
            'aria-labelledby': 'dirty-dialog-title', onKeyDown: keydown,
            ref: element => {
                dialog = element;
                queueMicrotask(() => {
                    if (isCurrent() && element?.isConnected) {
                        element.querySelector('.dirty-cancel')?.focus();
                    }
                });
            },
        },
        h('h2', { id: 'dirty-dialog-title' }, 'Discard changes and continue?'),
        h('p', null, 'Unsaved changes in the current panel will be lost.'),
        h('div', { class: 'dialog-actions' },
            h('button', { type: 'button', class: 'dirty-cancel', onClick: onCancel }, 'Cancel'),
            h('button', { type: 'button', class: 'danger-button', onClick: onDiscard }, 'Discard'))));
}

export class Launcher extends Component {
    constructor(props) {
        super(props);
        this.owner = Symbol();
        this.retired = false;
        this.recoveryToken = 0;
        this.state = {
            catalogStatus: 'unrequested', applications: [], navigation: { kind: 'home' },
            menuOpen: false, menuIndex: 0, focusedPanel: null, dirtyIntent: null,
            liveMessage: '', statusMessage: '', panelFailure: null,
            authorityCode: null, overlay: false,
            narrow: typeof window !== 'undefined' && window.matchMedia('(max-width: 42rem)').matches,
        };
        this.preference = normalizeLauncherPreference(null);
        this.catalogToken = 0;
        this.navigationToken = 0;
        this.pendingTarget = null;
        this.catalogAbort = null;
        this.subscriptionReady = false;
        this.automaticRecoverySpent = false;
        this.recovering = false;
        this.preferenceWrite = Promise.resolve();
        this.menuItems = [];
    }

    owns(owner = this.owner) {
        return !this.retired && owner === this.owner;
    }

    setOwnedState(update, callback, current) {
        const owner = this.owner;
        const isCurrent = () => this.owns(owner) && (!current || current());
        if (!isCurrent()) return;
        this.setState((state, props) => {
            if (!isCurrent()) return null;
            return typeof update === 'function' ? update(state, props) : update;
        }, () => {
            if (isCurrent()) callback?.();
        });
    }

    forceOwnedUpdate() {
        if (this.owns()) this.forceUpdate();
    }

    componentDidMount() {
        const owner = this.owner;
        const isCurrent = () => this.owns(owner);
        if (!isCurrent()) return;
        this.panelManager = new PanelManager(this.panelHost, {
            onFailure: (target, error, info) => {
                if (isCurrent()) this.auditPanelFailure(target, error, info);
            },
            onRetry: target => {
                if (isCurrent()) this.navigateTarget(target, { source: 'retry', forceNew: true });
            },
            onHome: () => { if (isCurrent()) this.requestHome(); },
            onDirtyChange: () => { if (isCurrent()) this.forceOwnedUpdate(); },
        });
        this.preferencePromise = this.loadPreference();
        this.unsubscribeApplications = subscribe(APPLICATIONS_CHANNEL,
            () => { if (isCurrent()) this.refreshWhenPreferencesReady(); }, {
                onReady: () => { if (isCurrent()) this.realtimeReady(); },
                onError: error => { if (isCurrent()) this.realtimeError(error); },
            });
        this.visibility = () => {
            if (isCurrent() && document.visibilityState === 'visible' && this.subscriptionReady) {
                this.refreshWhenPreferencesReady();
            }
        };
        document.addEventListener('visibilitychange', this.visibility);
        this.narrowQuery = window.matchMedia('(max-width: 42rem)');
        this.narrowChanged = event => {
            if (isCurrent()) this.setOwnedState({ narrow: event.matches }, null, isCurrent);
        };
        this.narrowQuery.addEventListener('change', this.narrowChanged);
    }

    componentWillUnmount() {
        // Retire admission before cleanup invokes any panel or transport callbacks.
        this.retired = true;
        this.recoveryToken++;
        this.subscriptionReady = false;
        this.recovering = false;
        this.catalogToken++;
        this.navigationToken++;
        this.catalogAbort?.abort();
        this.unsubscribeApplications?.();
        document.removeEventListener('visibilitychange', this.visibility);
        this.narrowQuery?.removeEventListener('change', this.narrowChanged);
        this.panelManager?.dispose();
    }

    async loadPreference() {
        const owner = this.owner;
        const isCurrent = () => this.owns(owner);
        if (!isCurrent()) return;
        try {
            const result = await call('shell.preferences.get', { key: 'shell.launcher' });
            if (!isCurrent()) return;
            this.preference = normalizeLauncherPreference(result?.value);
            this.setOwnedState(({ applications }) => ({
                applications: orderApplications(applications, this.preference),
            }), null, isCurrent);
        } catch {
            if (!isCurrent()) return;
            this.preference = normalizeLauncherPreference(null);
            this.setOwnedState({ statusMessage: 'Launcher preferences could not be loaded.' }, null, isCurrent);
        }
    }

    realtimeReady() {
        if (!this.owns()) return;
        this.recoveryToken++;
        this.subscriptionReady = true;
        this.recovering = false;
        this.setOwnedState({ authorityCode: null });
        this.refreshWhenPreferencesReady();
    }

    refreshWhenPreferencesReady() {
        const owner = this.owner;
        if (!this.owns(owner)) return;
        this.preferencePromise.then(() => {
            if (this.owns(owner) && this.subscriptionReady) this.refreshCatalog();
        });
    }

    realtimeError(error) {
        if (!this.owns()) return;
        const reportedCode = error?.code || 'auth_failed';
        if (reportedCode === 'disconnected') {
            this.setOwnedState(({ applications }) => ({
                catalogStatus: applications.length ? 'stale' : 'failed',
                statusMessage: 'Realtime updates are reconnecting.',
            }));
            return;
        }
        // The server's current terminal vocabulary is closed, but future
        // terminal codes must fail through the same bounded revalidation path
        // as auth_failed instead of leaving the launcher permanently stopped.
        const code = TERMINAL_SESSION.has(reportedCode) ||
            TERMINAL_RECOVERABLE.has(reportedCode) || reportedCode === 'already_connected'
            ? reportedCode : 'auth_failed';
        this.failClosedAuthority(code);
    }

    failClosedAuthority(code) {
        if (!this.owns()) return;
        this.recoveryToken++;
        this.subscriptionReady = false;
        this.recovering = false;
        this.catalogToken++;
        this.navigationToken++;
        this.catalogAbort?.abort();
        this.pendingTarget = null;
        this.panelManager?.destroyAll();
        this.setOwnedState({
            applications: [], navigation: { kind: 'home' }, catalogStatus: 'failed',
            authorityCode: code, menuOpen: false, dirtyIntent: null, panelFailure: null,
            liveMessage: '', statusMessage: 'Application access must be revalidated.',
        });
        if (TERMINAL_SESSION.has(code)) {
            this.props.onSessionEnd(code);
        } else if (TERMINAL_RECOVERABLE.has(code) && !this.automaticRecoverySpent) {
            this.automaticRecoverySpent = true;
            this.recoverAuthority();
        }
    }

    async recoverAuthority() {
        const owner = this.owner;
        if (!this.owns(owner) || this.recovering) return;
        const token = ++this.recoveryToken;
        const isCurrent = () => this.owns(owner) && token === this.recoveryToken;
        this.recovering = true;
        try {
            const response = await fetch('/api/auth/session', { credentials: 'include' });
            if (!isCurrent()) return;
            if (response.status === 401) {
                let code = 'not_authenticated';
                try {
                    const body = await response.json();
                    if (!isCurrent()) return;
                    code = body.error || code;
                } catch { /* generic */ }
                if (!isCurrent()) return;
                this.props.onSessionEnd(code);
                return;
            }
            if (!response.ok) throw new Error('session validation failed');
            reconnectRealtime();
            this.setOwnedState({ statusMessage: 'Reconnecting application updates…' }, null, isCurrent);
        } catch {
            if (!isCurrent()) return;
            this.recovering = false;
            this.setOwnedState({ catalogStatus: 'failed', statusMessage: 'Application access could not be revalidated.' },
                null, isCurrent);
        }
    }

    retry() {
        if (!this.owns()) return;
        if (this.state.authorityCode) {
            this.recoverAuthority();
        } else if (this.subscriptionReady) {
            this.refreshCatalog();
        } else {
            reconnectRealtime();
        }
    }

    useThisTab() {
        if (!this.owns()) return;
        this.automaticRecoverySpent = true;
        this.recoverAuthority();
    }

    async refreshCatalog() {
        const owner = this.owner;
        if (!this.owns(owner)) return;
        const token = ++this.catalogToken;
        const isCurrent = () => this.owns(owner) && token === this.catalogToken;
        this.catalogAbort?.abort();
        const controller = new AbortController();
        this.catalogAbort = controller;
        this.setOwnedState(({ applications }) => ({
            catalogStatus: applications.length ? 'refreshing' : 'loading',
            liveMessage: '', statusMessage: '',
        }), null, isCurrent);
        try {
            const response = await fetch('/api/frontend/applications', {
                credentials: 'include', signal: controller.signal,
                cache: 'no-store',
                headers: { Accept: 'application/json' },
            });
            if (!isCurrent()) return;
            if (response.status === 401) {
                let code = 'not_authenticated';
                try {
                    const body = await response.json();
                    if (!isCurrent()) return;
                    code = body.error || code;
                } catch { /* generic */ }
                if (!isCurrent()) return;
                this.failClosedAuthority(code);
                return;
            }
            if (!response.ok) throw new Error(`application discovery failed: ${response.status}`);
            const body = await response.json();
            if (!isCurrent()) return;
            const applications = orderApplications(normalizeCatalog(body), this.preference);

            const activeRemoved = this.panelManager.reconcile(catalogTargetKeys(applications));
            if (!isCurrent()) return;
            const pendingTargetRemoved = this.pendingTarget &&
                !this.findTarget(applications, this.pendingTarget);
            if (pendingTargetRemoved) {
                this.navigationToken++;
                this.pendingTarget = null;
            }
            const dirtyTargetRemoved = this.state.dirtyIntent?.kind === 'target' &&
                !this.findTarget(applications, this.state.dirtyIntent.target);
            const dismissDirtyIntent = activeRemoved || dirtyTargetRemoved;
            let navigation = this.state.navigation;
            let replacement = null;
            if (navigation.kind === 'application') {
                const current = this.findTarget(applications, navigation);
                if (!current || activeRemoved) {
                    const application = applications.find(item => item.id === navigation.applicationId);
                    if (application) replacement = makeTarget(application, choosePanel(application, this.preference));
                    navigation = { kind: 'home' };
                }
            }
            const nextState = {
                applications, navigation, catalogStatus: 'ready', authorityCode: null,
                liveMessage: activeRemoved || dirtyTargetRemoved || pendingTargetRemoved
                    ? 'Application unavailable' : '',
                statusMessage: '',
                panelFailure: null,
            };
            if (dismissDirtyIntent) nextState.dirtyIntent = null;
            this.setOwnedState(nextState, () => {
                this.automaticRecoverySpent = false;
                if (replacement) this.navigateTarget(replacement, { source: 'forced' });
                else if (activeRemoved) this.homeHeading?.focus();
                else if (dirtyTargetRemoved) this.panelManager.active?.container.focus();
            }, isCurrent);
        } catch (error) {
            if (!isCurrent() || error.name === 'AbortError') return;
            this.setOwnedState(({ applications }) => ({
                catalogStatus: applications.length ? 'stale' : 'failed',
                statusMessage: applications.length
                    ? 'Application list may be out of date.'
                    : 'Applications could not be loaded.',
            }), null, isCurrent);
        }
    }

    findTarget(applications, target) {
        const application = applications.find(item => item.id === target.applicationId);
        if (!application || (target.generation && application.generation !== target.generation)) return null;
        const panel = application.panels.find(item => item.id === (target.panelId || target.panel?.id));
        return panel ? makeTarget(application, panel) : null;
    }

    applicationSelected(application, source = 'switcher') {
        if (!this.owns()) return;
        const panel = choosePanel(application, this.preference);
        this.requestNavigation(makeTarget(application, panel), source);
    }

    panelSelected(application, panel, source = 'tab') {
        if (!this.owns()) return;
        this.requestNavigation(makeTarget(application, panel), source);
    }

    requestNavigation(target, source) {
        if (!this.owns()) return;
        if (this.state.dirtyIntent) return;
        const active = this.panelManager.activeTarget;
        if (active && active.generation === target.generation && active.panel.id === target.panel.id) {
            this.setOwnedState({ menuOpen: false });
            return;
        }
        if (this.panelManager.activeDirty) {
            this.focusBeforeDialog = document.activeElement;
            this.setOwnedState({ dirtyIntent: { kind: 'target', target, source }, menuOpen: false });
            return;
        }
        this.navigateTarget(target, { source });
    }

    requestHome() {
        if (!this.owns()) return;
        if (this.state.dirtyIntent) return;
        if (this.state.navigation.kind === 'home') return;
        if (this.panelManager.activeDirty) {
            this.focusBeforeDialog = document.activeElement;
            this.setOwnedState({ dirtyIntent: { kind: 'home' }, menuOpen: false });
            return;
        }
        this.commitHome(false);
    }

    cancelDirty() {
        if (!this.owns()) return;
        const token = this.navigationToken;
        this.setOwnedState({ dirtyIntent: null }, () => this.focusBeforeDialog?.focus(),
            () => token === this.navigationToken);
    }

    confirmDirty() {
        if (!this.owns()) return;
        const intent = this.state.dirtyIntent;
        if (!intent) return;
        const token = this.navigationToken;
        this.setOwnedState({ dirtyIntent: null }, () => {
            if (intent.kind === 'home') this.commitHome(true);
            else this.navigateTarget(intent.target, {
                source: intent.source,
                discardActive: true,
                restoreTabFocus: intent.source === 'tab',
            });
        }, () => token === this.navigationToken);
    }

    commitHome(discardActive) {
        if (!this.owns()) return;
        const token = ++this.navigationToken;
        this.pendingTarget = null;
        this.panelManager.deactivateToHome({ discardActive });
        this.setOwnedState({ navigation: { kind: 'home' }, menuOpen: false, panelFailure: null },
            () => this.homeHeading?.focus(), () => token === this.navigationToken);
    }

    async navigateTarget(requested, {
        source, discardActive = false, forceNew = false, restoreTabFocus = false,
    } = {}) {
        const owner = this.owner;
        if (!this.owns(owner)) return;
        const target = this.findTarget(this.state.applications, requested);
        if (!target) {
            this.setOwnedState({ liveMessage: 'Application unavailable' });
            this.commitHome(discardActive);
            return;
        }
        const token = ++this.navigationToken;
        const isCurrent = () => this.owns(owner) && token === this.navigationToken;
        this.pendingTarget = target;
        this.setOwnedState({ panelFailure: null, statusMessage: 'Loading panel…', menuOpen: false }, null, isCurrent);
        if (forceNew) {
            const existing = [...this.panelManager.instances.values()].find(instance =>
                instance.target.generation === target.generation && instance.target.panel.id === target.panel.id);
            if (existing) this.panelManager.destroy(existing, { deactivate: existing === this.panelManager.active });
        }
        let instance;
        try {
            instance = await this.panelManager.prepare(target);
            const current = this.findTarget(this.state.applications, target);
            if (!isCurrent() || !current) {
                if (!this.pendingTarget || this.pendingTarget.generation !== target.generation ||
                    this.pendingTarget.panel.id !== target.panel.id) this.panelManager.destroyPrepared(instance);
                return;
            }
            this.panelManager.commit(instance, { discardActive });
        } catch (error) {
            if (!isCurrent()) return;
            if (error?.name === 'PanelImportError') await this.refreshCatalog();
            if (!isCurrent()) return;
            this.pendingTarget = null;
            if (!this.panelManager.activeTarget) {
                this.setOwnedState({ panelFailure: target, statusMessage: '' }, null, isCurrent);
            } else {
                this.setOwnedState({ statusMessage: 'The requested panel could not be displayed.' }, null, isCurrent);
            }
            return;
        }
        if (!isCurrent()) return;
        this.pendingTarget = null;
        this.preference = updatePreference(this.preference, target.applicationId, target.panel.id);
        this.writePreference();
        this.setOwnedState({
            navigation: { kind: 'application', applicationId: target.applicationId,
                panelId: target.panel.id, generation: target.generation },
            focusedPanel: target.panel.id, statusMessage: '', panelFailure: null,
        }, () => {
            if (source === 'forced' || restoreTabFocus) {
                const application = this.state.applications.find(item => item.id === target.applicationId);
                if (application?.panels.length > 1) document.getElementById(target.tabId)?.focus();
                else instance.container.focus();
            } else if (source !== 'tab') {
                instance.container.focus();
            }
        }, isCurrent);
    }

    writePreference() {
        const owner = this.owner;
        if (!this.owns(owner)) return;
        const value = JSON.parse(JSON.stringify(this.preference));
        this.preferenceWrite = this.preferenceWrite.catch(() => {}).then(() => {
            if (this.owns(owner)) return call('shell.preferences.set', { key: 'shell.launcher', value });
        }).catch(() => this.setOwnedState({ statusMessage: 'Launcher preference could not be saved.' },
            null, () => this.owns(owner)));
    }

    auditPanelFailure(target, error, info) {
        if (!this.owns()) return;
        const detail = {
            application_id: target.applicationId,
            panel_id: target.panel.id,
            error_message: DEVELOPMENT_MODE
                ? String(error?.message || error).slice(0, 1024)
                : 'Panel lifecycle failure',
        };
        if (info?.componentStack) detail.component_path = info.componentStack.slice(0, 8192);
        if (DEVELOPMENT_MODE && error?.stack) {
            detail.error_stack = error.stack.slice(0, 8192);
        }
        call('shell.audit.emit', detail).catch(() => {});
    }

    openMenu() {
        if (!this.owns()) return;
        if (!this.state.applications.length) return;
        const token = this.navigationToken;
        const current = this.state.navigation.applicationId;
        const index = Math.max(0, this.state.applications.findIndex(item => item.id === current));
        this.setOwnedState({ menuOpen: true, menuIndex: index }, () => {
            if (this.state.menuOpen) this.menuItems[index]?.focus();
        }, () => token === this.navigationToken);
    }

    closeMenu() {
        if (!this.owns()) return;
        const token = this.navigationToken;
        this.setOwnedState({ menuOpen: false }, () => {
            if (!this.state.menuOpen) this.appTrigger?.focus();
        }, () => token === this.navigationToken);
    }

    menuKey(event) {
        if (!this.owns()) return;
        const count = this.state.applications.length;
        if (!count) return;
        let index = this.state.menuIndex;
        if (event.key === 'Tab' && this.state.narrow) {
            index = event.shiftKey ? (index + count - 1) % count : (index + 1) % count;
        } else if (event.key === 'ArrowDown') index = (index + 1) % count;
        else if (event.key === 'ArrowUp') index = (index + count - 1) % count;
        else if (event.key === 'Home') index = 0;
        else if (event.key === 'End') index = count - 1;
        else if (event.key === 'Escape') { event.preventDefault(); this.closeMenu(); return; }
        else if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            this.applicationSelected(this.state.applications[index]);
            return;
        } else return;
        event.preventDefault();
        const token = this.navigationToken;
        this.setOwnedState({ menuIndex: index }, () => this.menuItems[index]?.focus(),
            () => token === this.navigationToken && this.state.menuOpen);
    }

    tabKey(event, application, index) {
        if (!this.owns()) return;
        const panels = application.panels;
        let next = index;
        if (event.key === 'ArrowRight' || event.key === 'ArrowDown') next = (index + 1) % panels.length;
        else if (event.key === 'ArrowLeft' || event.key === 'ArrowUp') next = (index + panels.length - 1) % panels.length;
        else if (event.key === 'Home') next = 0;
        else if (event.key === 'End') next = panels.length - 1;
        else if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            this.panelSelected(application, panels[index], 'tab');
            return;
        } else return;
        event.preventDefault();
        const token = this.navigationToken;
        this.setOwnedState({ focusedPanel: panels[next].id }, () => {
            const tab = document.getElementById(makeTarget(application, panels[next]).tabId);
            tab?.focus();
            tab?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
        }, () => token === this.navigationToken);
    }

    renderHome(applications, status) {
        if (status === 'loading' || status === 'unrequested') return h('p', null, 'Loading applications…');
        if (status === 'failed' && !applications.length) return h('div', { class: 'launcher-state' },
            h('p', null, this.state.authorityCode === 'already_connected'
                ? 'Realtime application updates are active in another tab.'
                : 'Applications could not be loaded.'),
            this.state.authorityCode === 'already_connected'
                ? h('button', { type: 'button', onClick: () => this.useThisTab() }, 'Use this tab')
                : h('button', { type: 'button', onClick: () => this.retry() }, 'Retry'));
        if (!applications.length) return h('p', null, 'No applications are available.');
        return h('ul', { class: 'launcher-grid', 'data-ipoint': 'shell.home.launcher',
            'data-ipoint-layer': 'shell' }, applications.map(application =>
            h('li', { key: application.id }, h('button', {
                type: 'button', class: `launcher-tile${this.preference.last_application === application.id ? ' last-used' : ''}`,
                'aria-label': application.title,
                onClick: () => this.applicationSelected(application, 'home'),
            },
            h(ApplicationMark, { application }),
            h('span', { class: 'launcher-tile-copy' },
                h('strong', null, application.title),
                application.description ? h('span', null, application.description) : null)))));
    }

    renderTabs(application) {
        if (!application || application.panels.length < 2) return null;
        const selected = this.state.navigation.panelId;
        const focused = this.state.focusedPanel || selected || application.panels[0].id;
        return h('div', { class: 'primary-tabs', role: 'tablist', 'aria-label': `${application.title} panels`,
            'data-ipoint': `ext.${application.id}.primaryTabs`, 'data-ipoint-layer': 'extension' },
        application.panels.map((panel, index) => {
            const target = makeTarget(application, panel);
            return h('button', {
                key: panel.id, id: target.tabId, type: 'button', role: 'tab',
                'aria-selected': selected === panel.id ? 'true' : 'false',
                'aria-controls': target.paneId, tabIndex: focused === panel.id ? 0 : -1,
                onFocus: () => this.setOwnedState({ focusedPanel: panel.id }),
                onKeyDown: event => this.tabKey(event, application, index),
                onClick: () => this.panelSelected(application, panel, 'tab'),
                disabled: Boolean(this.state.dirtyIntent),
            }, h(ApplicationMark, { application, iconToken: panel.icon, small: true }), panel.title);
        }));
    }

    render() {
        const owner = this.owner;
        const applications = this.state.applications;
        const currentApplication = this.state.navigation.kind === 'application'
            ? applications.find(item => item.id === this.state.navigation.applicationId) : null;
        const onHome = this.state.navigation.kind === 'home';
        const busy = ['loading', 'refreshing'].includes(this.state.catalogStatus);
        const showHome = onHome && !this.state.panelFailure;
        return h('div', { class: 'launcher-shell' },
            h('header', { class: 'topbar', 'data-ipoint': 'shell.topbar', 'data-ipoint-layer': 'shell' },
                h('nav', { class: 'launcher-navigation', 'aria-label': 'Application navigation' },
                    h('button', {
                        type: 'button', class: 'home-button', 'aria-label': 'Home',
                        'aria-current': onHome ? 'page' : null,
                        'aria-pressed': onHome ? 'true' : 'false',
                        'data-ipoint': 'shell.home', 'data-ipoint-layer': 'shell',
                        onClick: () => this.requestHome(), disabled: Boolean(this.state.dirtyIntent),
                    }, '⌂'),
                    h('div', { class: 'app-switcher-wrap' },
                        h('button', {
                            id: 'launcher-app-identity', type: 'button', class: 'app-identity', 'aria-haspopup': 'menu',
                            'aria-expanded': this.state.menuOpen ? 'true' : 'false',
                            'aria-controls': 'launcher-app-switcher',
                            'data-ipoint': 'shell.appIdentity', 'data-ipoint-layer': 'shell',
                            ref: element => { this.appTrigger = element; },
                            onClick: () => this.state.menuOpen ? this.closeMenu() : this.openMenu(),
                            onKeyDown: event => {
                                if (event.key === 'ArrowDown') { event.preventDefault(); this.openMenu(); }
                                if (event.key === 'Escape' && this.state.menuOpen) this.closeMenu();
                            },
                            disabled: !applications.length || Boolean(this.state.dirtyIntent),
                        },
                        currentApplication
                            ? h(ApplicationMark, { application: currentApplication, small: true })
                            : h('span', { class: 'app-monogram small', 'aria-hidden': 'true' }, 'P'),
                        h('span', null, currentApplication?.title || 'Applications'),
                        h('span', { 'aria-hidden': 'true' }, '▾')),
                        this.state.menuOpen ? h('div', {
                            id: 'launcher-app-switcher', class: 'app-switcher',
                            'aria-label': 'Applications',
                            'aria-modal': this.state.narrow ? 'true' : null,
                            role: this.state.narrow ? 'dialog' : null,
                            onKeyDown: event => this.menuKey(event),
                            'data-ipoint': 'shell.appSwitcher', 'data-ipoint-layer': 'shell',
                        }, h('div', { role: 'menu', 'aria-label': 'Applications' },
                        applications.map((application, index) => h('button', {
                            key: application.id, type: 'button', role: 'menuitem',
                            ref: element => { this.menuItems[index] = element; },
                            tabIndex: this.state.menuIndex === index ? 0 : -1,
                            onMouseEnter: () => this.setOwnedState({ menuIndex: index }),
                            onClick: () => this.applicationSelected(application),
                        }, h(ApplicationMark, { application, small: true }), application.title)))) : null),
                    this.renderTabs(currentApplication)),
                h('div', { class: 'topbar-spacer' }),
                this.props.userControls,
                DEVELOPMENT_MODE ? h('button', {
                    type: 'button', class: 'ipoint-toggle',
                    onClick: () => this.setOwnedState({ overlay: !this.state.overlay }),
                }, this.state.overlay ? 'Hide ownership' : 'Show ownership') : null),
            h('main', {
                class: `launcher-main${this.state.overlay ? ' show-ipoints' : ''}`,
                'aria-busy': busy ? 'true' : 'false',
                'data-ipoint': 'shell.content', 'data-ipoint-layer': 'shell',
            },
                h('div', { class: 'launcher-live', 'aria-live': 'polite', 'aria-atomic': 'true' },
                    this.state.liveMessage || this.state.statusMessage),
                h('section', { class: 'launcher-home', hidden: !showHome },
                    h('h1', { tabIndex: -1, ref: element => { this.homeHeading = element; } }, 'Home'),
                    this.renderHome(applications, this.state.catalogStatus)),
                this.state.panelFailure ? h('section', { class: 'panel-load-failure', role: 'alert' },
                    h('p', null, 'This panel could not be displayed.'),
                    h('button', { type: 'button', onClick: () =>
                        this.navigateTarget(this.state.panelFailure, { source: 'retry', forceNew: true }) }, 'Retry'),
                    h('button', { type: 'button', onClick: () => this.commitHome(false) }, 'Return to Home')) : null,
                h('div', { class: 'panel-host', hidden: onHome || Boolean(this.state.panelFailure),
                    ref: element => { this.panelHost = element; } })),
            this.state.overlay ? h('div', { class: 'ipoint-legend', role: 'status' },
                'Ownership: blue = shell, amber = extension') : null,
            this.state.dirtyIntent ? h(DirtyDialog, {
                onCancel: () => this.cancelDirty(), onDiscard: () => this.confirmDirty(),
                isCurrent: () => this.owns(owner),
            }) : null);
    }
}
