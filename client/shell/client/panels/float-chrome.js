import { h, Component } from 'preact';
import { classifyFloatWorkArea, clampFloatGeometry, defaultFloatGeometry } from './float-model.js';

const FOCUSABLE = 'button:not([disabled]),a[href],input:not([disabled]),select:not([disabled]),'
    + 'textarea:not([disabled]),[tabindex]:not([tabindex="-1"]),[contenteditable="true"]';
const JUMP_REASON = 'Jump to app is unavailable until the reviewed navigation adapter is implemented.';

function eligible(element) {
    return !!element?.isConnected && !element.disabled && !element.closest('[hidden],[inert]')
        && element.getClientRects().length > 0;
}
function controls(root) {
    return root ? [...root.querySelectorAll(FOCUSABLE)].filter(element =>
        element.tabIndex >= 0 && eligible(element)) : [];
}
function containTab(event, root) {
    const entries = controls(root);
    const index = entries.indexOf(root.ownerDocument.activeElement);
    if (!entries.length) { event.preventDefault(); root.focus(); return; }
    const next = event.shiftKey ? (index <= 0 ? entries.length - 1 : index - 1)
        : (index < 0 || index === entries.length - 1 ? 0 : index + 1);
    event.preventDefault();
    event.stopPropagation();
    entries[next].focus();
}
function size(area) { return { width: area.width, height: area.height }; }
function retiring(record) { return record.retiring === true || record.readiness === 'retiring'; }
function status(record) {
    if (retiring(record)) return 'Closing: waiting for owned preparation and cleanup.';
    if (record.cleanupFailed || record.readiness === 'cleanup-failed') return 'Cleanup failed. New float admission is disabled.';
    if (record.readiness === 'loading') return 'Loading panel…';
    if (record.readiness === 'failed') return record.retryAvailable
        ? 'This panel could not be displayed. You can retry or close it.'
        : 'This panel could not be displayed. Retry is waiting for prior preparation and cleanup.';
    return '';
}

// Measurement begins only when instantiated and connected. Coordinates are
// post-layout CSS pixels: neither browser zoom nor shell scale is applied twice.
export class FloatViewportController {
    constructor({ onArea, topbarElement = () => null, window: win = globalThis.window,
        document: doc = globalThis.document }) {
        this.onArea = onArea;
        this.topbarElement = topbarElement;
        this.window = win;
        this.document = doc;
        this.connected = false;
        this.measure = () => {
            if (!this.connected) return;
            const viewport = this.window.visualViewport;
            const width = viewport?.width ?? this.window.innerWidth;
            const height = viewport?.height ?? this.window.innerHeight;
            const left = viewport?.offsetLeft ?? 0;
            const top = viewport?.offsetTop ?? 0;
            const insets = this.window.getComputedStyle(this.probe);
            const inset = key => Math.max(0, Number.parseFloat(insets[key]) || 0);
            const bar = this.topbarElement();
            const barBottom = bar?.isConnected ? bar.getBoundingClientRect().bottom : top;
            const workTop = Math.max(top + inset('paddingTop'), barBottom);
            const workLeft = Math.ceil(left + inset('paddingLeft'));
            const area = Object.freeze({ left: workLeft,
                top: Math.ceil(workTop),
                width: Math.max(0, Math.floor(left + width - inset('paddingRight') - workLeft)),
                height: Math.max(0, Math.floor(top + height - inset('paddingBottom') - Math.ceil(workTop))) });
            if (this.previous && Object.keys(area).every(key => area[key] === this.previous[key])) return;
            this.previous = area;
            this.onArea(area);
        };
    }

    connect() {
        if (this.connected) return;
        this.connected = true;
        this.probe = this.document.createElement('div');
        this.probe.className = 'float-safe-area';
        this.probe.setAttribute('aria-hidden', 'true');
        this.document.body.append(this.probe);
        this.window.addEventListener('resize', this.measure);
        this.window.addEventListener('orientationchange', this.measure);
        this.window.visualViewport?.addEventListener('resize', this.measure);
        this.window.visualViewport?.addEventListener('scroll', this.measure);
        if (this.window.ResizeObserver) {
            this.observer = new this.window.ResizeObserver(this.measure);
            this.observer.observe(this.document.documentElement);
            const topbar = this.topbarElement();
            if (topbar) this.observer.observe(topbar);
        }
        this.measure();
    }

    dispose() {
        if (!this.connected) return;
        this.connected = false;
        let failed = false;
        const clean = operation => { try { operation(); } catch { failed = true; } };
        clean(() => this.window.removeEventListener('resize', this.measure));
        clean(() => this.window.removeEventListener('orientationchange', this.measure));
        clean(() => this.window.visualViewport?.removeEventListener('resize', this.measure));
        clean(() => this.window.visualViewport?.removeEventListener('scroll', this.measure));
        clean(() => this.observer?.disconnect());
        clean(() => { this.probe.remove(); if (this.probe.isConnected) throw new Error('float viewport cleanup failed'); });
        this.probe = this.observer = this.previous = null;
        this.onArea = () => {};
        if (failed) throw new Error('float viewport cleanup failed');
    }
}

function FloatList({ records, onSelect, onAction, embedded = false, blocked = false }) {
    return h(embedded ? 'nav' : 'section', {
        class: embedded ? 'float-switcher' : 'float-list',
        'aria-label': 'Open float panels', hidden: !records.length, inert: blocked,
    }, !embedded && h('h2', null, 'Open panels (including minimized)'),
    h('ul', null, records.map(record => h('li', { key: record.token },
        h('div', { class: 'float-list-actions' },
            h('button', { type: 'button', disabled: retiring(record) || blocked,
                onClick: () => onSelect(record.token),
                'aria-label': `${record.presentation === 'minimized' ? 'Restore' : 'Focus'} ${record.title} — ${record.applicationTitle}` },
            `${record.title}${record.presentation === 'minimized' ? ' (minimized)' : ''}${record.readiness === 'failed' ? ' (failed)' : ''}${record.readiness === 'loading' ? ' (loading)' : ''}${retiring(record) ? ' (closing)' : ''}`),
            h('button', { type: 'button', disabled: retiring(record) || blocked,
                'aria-label': `Close ${record.title}`, onClick: () => onAction('requestClose', record.token) }, 'Close'))))));
}

function BudgetWarning({ onReload, blocked = false }) {
    return h('div', { class: 'float-budget-warning', role: 'alert', inert: blocked },
        h('p', null, 'Float cleanup failed. New panels cannot be opened safely. Reload the shell to recover; unsaved work may be lost.'),
        h('button', { type: 'button', onClick: onReload }, 'Reload shell'));
}
function PendingNotice() {
    return h('p', { class: 'float-budget-waiting', role: 'status' },
        'Panel cleanup is still pending. New panels must wait for owned cleanup to finish.');
}

export class FloatChrome extends Component {
    constructor(props) {
        super(props);
        this.state = { preview: null, editing: null };
        this.mounted = false;
        this.retiredChrome = false;
        this.hostRef = element => {
            this.contentHost = element;
            if (element && this.props.record.container && this.props.record.container.parentNode !== element) {
                element.append(this.props.record.container);
            }
        };
        this.windowRef = element => {
            this.element = element;
            this.props.registerChrome(this.props.record.token, element);
        };
    }

    componentDidMount() {
        this.mounted = true;
        if (!this.current() || retiring(this.props.record)) {
            this.retireChrome(true);
            return;
        }
        this.unsubscribeRetirement = this.props.interaction.onRetire(() => this.retireChrome(true));
        this.unsubscribeChromeCleanup = this.props.manager.registerChromeCleanup(this.props.record.token,
            () => this.retireChrome(true));
    }
    componentDidUpdate() {
        if (this.retiredChrome) return;
        const { record, area, interaction } = this.props;
        if (this.edit && (!this.current() || retiring(record) || record.maximized
                || classifyFloatWorkArea(area.width, area.height).mode !== 'desktop'
                || this.props.blocked || interaction.geometry?.lease !== this.edit.lease)) this.finishEdit(false);
        this.hostRef(this.contentHost);
    }
    componentWillUnmount() {
        this.retireChrome(false);
    }
    retireChrome(remove) {
        if (this.retiredChrome) {
            if (remove && this.cleanupFailed) throw new Error('float chrome cleanup failed');
            return;
        }
        this.retiredChrome = true;
        this.mounted = false;
        let failed = false;
        const clean = operation => { try { operation(); } catch { failed = true; } };
        clean(() => { if (!this.finishEdit(false)) throw new Error('float geometry cleanup failed'); });
        clean(() => this.unsubscribeRetirement?.());
        clean(() => this.unsubscribeChromeCleanup?.());
        clean(() => this.props.registerChrome(this.props.record.token, null));
        if (remove) clean(() => {
            this.element?.remove();
            if (this.element?.isConnected) throw new Error('float chrome removal failed');
        });
        // These are layer-owned UI snapshots, not the manager's record. Clear
        // references before the parent Preact frame's later unmount callback.
        this.props.record.title = this.props.record.applicationTitle = '';
        this.props.record.container = this.props.record.geometry = null;
        this.contentHost = this.element = this.unsubscribeRetirement = this.unsubscribeChromeCleanup = null;
        this.state = { preview: null, editing: null };
        this.cleanupFailed = failed;
        if (failed) {
            this.props.manager.reservations.poison();
            if (remove) throw new Error('float chrome cleanup failed');
        }
    }
    current() {
        return this.props.manager.isCurrent() && this.props.interaction.isCurrent()
            && this.props.manager.snapshot().some(record => record.token === this.props.record.token);
    }

    beginEdit(kind, event, pointer = false) {
        const { record, area, interaction } = this.props;
        if (!this.current() || this.edit || retiring(record) || record.maximized
                || classifyFloatWorkArea(area.width, area.height).mode !== 'desktop') return;
        if (pointer && (event.button !== 0 || event.isPrimary === false)) return;
        const before = clampFloatGeometry(record.geometry || defaultFloatGeometry(size(area), record.rank), size(area));
        const lease = interaction.beginGeometry(record.token);
        if (!lease) return;
        this.edit = { kind, before, geometry: before, lease, pointerId: pointer ? event.pointerId : null,
            point: pointer ? { x: Math.round(event.clientX), y: Math.round(event.clientY) } : null,
            capture: pointer ? event.currentTarget : null };
        if (pointer) {
            try { this.edit.capture.setPointerCapture(event.pointerId); }
            catch { this.finishEdit(false); return; }
            event.preventDefault();
        }
        this.setState({ preview: before, editing: kind });
    }

    changeGeometry(dx, dy, base = this.edit?.geometry) {
        if (!this.edit || !this.current()) return;
        const next = this.edit.kind === 'move' ? { ...base, x: base.x + dx, y: base.y + dy }
            : { ...base, width: base.width + dx, height: base.height + dy };
        this.edit.geometry = clampFloatGeometry(next, size(this.props.area));
        this.setState({ preview: this.edit.geometry });
    }
    pointerMove(event) {
        if (!this.edit || event.pointerId !== this.edit.pointerId) return;
        this.changeGeometry(Math.round(event.clientX) - this.edit.point.x,
            Math.round(event.clientY) - this.edit.point.y, this.edit.before);
        event.preventDefault();
    }
    pointerEnd(event, accept) {
        if (!this.edit || event.pointerId !== this.edit.pointerId) return;
        event.preventDefault();
        this.finishEdit(accept);
    }
    finishEdit(accept) {
        const edit = this.edit;
        if (!edit) return true;
        this.edit = null; // Fence lost-capture/re-entrant scope notifications first.
        let clean = true;
        try {
            if (edit.capture?.hasPointerCapture(edit.pointerId)) edit.capture.releasePointerCapture(edit.pointerId);
            if (edit.capture?.hasPointerCapture(edit.pointerId)) throw new Error('float pointer release failed');
        } catch { clean = false; }
        try { this.props.interaction.endGeometry(edit.lease); } catch { clean = false; }
        if (!clean) this.props.manager.reservations.poison();
        if (accept && clean && this.current()) this.props.manager.setGeometry(this.props.record.token, edit.geometry);
        if (this.mounted) this.setState({ preview: null, editing: null });
        return clean;
    }
    geometryKey(event) {
        if (!this.edit) return;
        if (event.key === 'Enter' || event.key === 'Escape') {
            event.preventDefault(); event.stopPropagation(); this.finishEdit(event.key === 'Enter'); return;
        }
        const directions = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
        if (!Object.hasOwn(directions, event.key)) return;
        event.preventDefault(); event.stopPropagation();
        const [x, y] = directions[event.key], step = event.shiftKey ? 1 : 10;
        this.changeGeometry(x * step, y * step);
    }

    render({ record, area, manager, interaction, activeModal, blocked, registerChrome,
        onAction, onSelect, onReload, poisoned, pendingCleanup, records }, state) {
        if (this.retiredChrome) return null;
        const mode = classifyFloatWorkArea(area.width, area.height).mode;
        const desktop = mode === 'desktop';
        const hidden = mode === 'deferred' || record.presentation === 'minimized' || (!desktop && !activeModal);
        const geometry = state.preview || record.geometry || defaultFloatGeometry(size(area), record.rank);
        const placed = desktop ? (record.maximized ? { x: 0, y: 0, width: area.width, height: area.height }
            : geometry && clampFloatGeometry(geometry, size(area)))
            : { x: mode === 'slide-over' ? Math.max(0, area.width - 640) : 0, y: 0,
                width: mode === 'slide-over' ? Math.min(area.width, 640) : area.width, height: area.height };
        const titleId = `${record.id}-float-title`, jumpId = `${record.id}-float-jump-reason`;
        const helpId = `${record.id}-float-geometry-help`, inactive = blocked || hidden;
        const action = name => { this.finishEdit(false); onAction(name, record.token); };
        const geometryEnabled = desktop && !record.maximized && !retiring(record);
        const message = status(record);
        return h('section', {
            ref: this.windowRef, class: 'float-window', 'data-float-id': record.id,
            'data-mode': mode, 'data-editing': String(!!state.editing), hidden,
            inert: inactive, role: !desktop && activeModal ? 'dialog' : 'region',
            'aria-modal': !desktop && activeModal && !blocked ? 'true' : undefined,
            'aria-labelledby': titleId, tabIndex: -1,
            style: placed ? { left: `${placed.x}px`, top: `${placed.y}px`, width: `${placed.width}px`,
                height: `${placed.height}px`, zIndex: record.rank } : {},
            onFocusIn: () => { if (this.current() && !inactive) manager.focus(record.token, { focusChrome: false }); },
            onPointerDown: () => { if (this.current() && !inactive) manager.focus(record.token, { focusChrome: false }); },
            onKeyDown: event => this.geometryKey(event),
        }, h('header', { class: 'float-header' },
            h('div', {
                class: 'float-titlebar', 'data-float-drag': 'true',
                onPointerDown: event => {
                    if (!event.target.closest('button,a,input,select,textarea')) this.beginEdit('move', event, true);
                },
                onPointerMove: event => this.pointerMove(event),
                onPointerUp: event => this.pointerEnd(event, true),
                onPointerCancel: event => this.pointerEnd(event, false),
                onLostPointerCapture: () => this.finishEdit(false),
            }, h('span', { class: 'float-mark', 'aria-hidden': 'true' }, [...record.applicationTitle.trim()][0]?.toUpperCase() || '?'),
            h('h2', { id: titleId, class: 'float-title', title: record.title }, record.title)),
            h('p', { class: 'float-badge' }, record.applicationTitle),
            h('div', { class: 'float-actions' },
                !desktop && h('button', { type: 'button', disabled: retiring(record), onClick: () => action('requestClose') }, 'Back'),
                h('button', { type: 'button', disabled: retiring(record), onClick: () => action('minimize') }, 'Minimize'),
                h('button', { type: 'button', disabled: !desktop || retiring(record),
                    title: !desktop ? 'Maximize is unavailable in modal presentation.' : undefined,
                    'aria-describedby': !desktop ? `${record.id}-float-maximize-reason` : undefined,
                    onClick: () => action('maximize') }, record.maximized ? 'Restore size' : 'Maximize'),
                h('button', { type: 'button', disabled: !geometryEnabled,
                    'aria-pressed': String(state.editing === 'move'), 'aria-describedby': helpId,
                    onClick: event => this.beginEdit('move', event) }, 'Move'),
                h('button', { type: 'button', disabled: retiring(record), onClick: () => action('requestClose') }, 'Close'),
                h('button', { type: 'button', disabled: true, 'aria-describedby': jumpId }, 'Jump to app')),
            !desktop && h('p', { id: `${record.id}-float-maximize-reason`, class: 'float-live-only' }, 'Maximize is unavailable in modal presentation.'),
            h('p', { id: jumpId, class: 'float-live-only' }, JUMP_REASON),
            record.liveOnly && h('p', { class: 'float-live-only' }, 'Not restored after reload. Unsaved edits are never restored.'),
            h('p', { id: helpId, class: 'float-geometry-help', role: state.editing ? 'status' : undefined },
                state.editing ? `${state.editing === 'move' ? 'Move' : 'Resize'}: arrows change 10 pixels; Shift+arrow changes 1 pixel. Enter accepts; Escape reverts.`
                    : 'Use Move or Resize with arrow keys; Enter accepts and Escape reverts.'),
            !desktop && activeModal && h(FloatList, { records, onSelect, onAction, embedded: true }),
            !desktop && activeModal && poisoned && h(BudgetWarning, { onReload }),
            !desktop && activeModal && pendingCleanup && h(PendingNotice)),
        message && h('div', { class: 'float-status', role: 'status' },
            h('p', null, message), record.readiness === 'failed' && h('button', {
                type: 'button', disabled: !record.retryAvailable, onClick: () => action('retry'),
            }, 'Retry')),
        h('div', { class: 'float-content', ref: this.hostRef }),
        h('button', { type: 'button', class: 'float-resize', disabled: !geometryEnabled,
            'aria-pressed': String(state.editing === 'resize'), 'aria-describedby': helpId,
            onClick: event => { if (!event.detail) this.beginEdit('resize', event); },
            onPointerDown: event => this.beginEdit('resize', event, true),
            onPointerMove: event => this.pointerMove(event),
            onPointerUp: event => this.pointerEnd(event, true),
            onPointerCancel: event => this.pointerEnd(event, false),
            onLostPointerCapture: () => this.finishEdit(false),
        }, 'Resize'));
    }
}

export class FloatLayer extends Component {
    constructor(props) {
        super(props);
        this.state = { records: [], area: { left: 0, top: 0, width: 0, height: 0 }, revision: 0 };
        this.chrome = new Map();
        this.background = new Map();
        this.mounted = false;
        this.retiredLayer = false;
        this.registerChrome = (token, element) => {
            if (element) this.chrome.set(token, element);
            else this.chrome.delete(token);
        };
        this.sync = () => {
            if (!this.current()) return;
            this.setState(state => ({ records: this.props.manager.snapshot().map(record => ({ ...record })), revision: state.revision + 1 }));
        };
        this.focusRequest = token => {
            const request = this.pendingFocus = Symbol();
            queueMicrotask(() => {
                if (this.pendingFocus !== request || !this.current() || this.props.interaction.confirmation) return;
                const element = this.chrome.get(token);
                if (element && eligible(element)) (controls(element)[0] || element).focus();
            });
        };
        this.keydown = event => {
            if (!this.current() || event.defaultPrevented) return;
            const scope = this.props.interaction, modal = scope.modal;
            if (!modal) return;
            const root = modal.kind === 'confirmation'
                ? (scope.confirmation?.kind === 'float' ? this.confirmationElement : null)
                : modal.kind === 'float' ? this.chrome.get(modal.token) : null;
            if (!root || !eligible(root)) return;
            if (event.key === 'Tab') containTab(event, root);
            else if (event.key === 'Escape' && !scope.geometry) {
                event.preventDefault(); event.stopPropagation();
                if (modal.kind === 'confirmation') this.resolveConfirmation(false);
                else this.action('requestClose', modal.token);
            }
        };
    }
    current() { return this.mounted && this.props.manager.isCurrent() && this.props.interaction.isCurrent(); }
    componentDidMount() {
        this.mounted = true;
        this.unsubscribeManager = this.props.manager.subscribe(this.sync);
        this.unsubscribeInteraction = this.props.interaction.subscribe(this.sync);
        this.unsubscribeFocus = this.props.manager.onFocus(this.focusRequest);
        this.unsubscribeRetirement = this.props.interaction.onRetire(() => this.retireLayer());
        this.viewport = new FloatViewportController({ topbarElement: this.props.topbarElement,
            onArea: area => {
                if (!this.current()) return;
                this.props.manager.setWorkArea(area);
                this.setState({ area });
            } });
        document.addEventListener('keydown', this.keydown, true);
        this.viewport.connect();
        this.sync();
    }
    componentDidUpdate() {
        if (!this.current()) return;
        const { records, area } = this.state, scope = this.props.interaction;
        const mode = classifyFloatWorkArea(area.width, area.height).mode;
        const top = [...records].filter(record => record.presentation === 'shown' && !retiring(record))
            .sort((left, right) => right.rank - left.rank)[0];
        const token = mode !== 'desktop' && mode !== 'deferred' ? top?.token : null;
        const precedingModal = this.previousModalKind;
        if (token && scope.modal?.kind !== 'switcher') { this.modalToken = token; scope.setModal('float', token); }
        else if (this.modalToken) { scope.clearModal(this.modalToken); this.modalToken = null; }
        this.syncBackground(!!scope.modal);
        const previousMode = this.previousMode;
        this.previousMode = mode;
        if (previousMode && previousMode !== mode && token && !scope.confirmation
                && !this.chrome.get(token)?.contains(document.activeElement)) this.focusRequest(token);
        if ((precedingModal === 'switcher' || precedingModal === 'confirmation')
                && scope.modal?.kind === 'float' && token) {
            // Confirmation Cancel may already have queued an exact trigger
            // return. Repair only missing/ineligible focus after that return,
            // never replace an eligible control within the resumed float.
            queueMicrotask(() => {
                if (!this.current() || scope.modal?.kind !== 'float' || scope.modal.token !== token) return;
                if (!eligible(document.activeElement) || !this.chrome.get(token)?.contains(document.activeElement)) {
                    this.focusRequest(token);
                }
            });
        }
        this.previousModalKind = scope.modal?.kind || null;
    }
    syncBackground(blocked) {
        let elements, failed = false;
        const clean = operation => { try { operation(); } catch { failed = true; } };
        clean(() => { elements = new Set((this.props.backgroundElements?.() || []).filter(element =>
            element?.isConnected && element !== this.layer && !element.contains(this.layer))); });
        elements ||= new Set();
        for (const [element, previous] of this.background) {
            if (blocked && elements.has(element)) continue;
            clean(() => {
                if (element.inert) {
                    element.inert = previous.inert;
                    if (element.inert !== previous.inert) throw new Error('float background restoration failed');
                }
            });
            clean(() => {
                if (element.getAttribute('aria-hidden') === 'true') {
                    if (previous.aria === null) element.removeAttribute('aria-hidden');
                    else element.setAttribute('aria-hidden', previous.aria);
                    if (element.getAttribute('aria-hidden') !== previous.aria) throw new Error('float background restoration failed');
                }
            });
            this.background.delete(element);
        }
        if (blocked) for (const element of elements) {
            clean(() => {
                if (this.background.get(element)?.failed) return;
                if (!this.background.has(element)) this.background.set(element,
                    { inert: element.inert, aria: element.getAttribute('aria-hidden') });
                try {
                    element.inert = true;
                    element.setAttribute('aria-hidden', 'true');
                    if (!element.inert || element.getAttribute('aria-hidden') !== 'true') throw new Error('float background isolation failed');
                } catch (error) { this.background.get(element).failed = true; throw error; }
            });
        }
        if (failed && !this.props.manager.reservations.status().poisoned) this.props.manager.reservations.poison();
        return !failed;
    }
    action(name, token) {
        if (!this.current() || this.props.interaction.confirmation
                || (this.props.interaction.modal && this.props.interaction.modal.kind !== 'float')) return;
        const origin = this.chrome.get(token);
        const trigger = this.props.manager.focusTrigger(token);
        this.props.manager[name](token);
        if (name !== 'minimize' && name !== 'requestClose') return;
        queueMicrotask(() => {
            if (!this.current() || this.props.interaction.confirmation || eligible(origin)) return;
            this.restoreFocus(trigger);
        });
    }
    select(token) {
        if (!this.current() || this.props.interaction.confirmation
                || (this.props.interaction.modal && this.props.interaction.modal.kind !== 'float')) return;
        const record = this.props.manager.snapshot().find(item => item.token === token);
        if (!record || retiring(record)) return;
        if (record.presentation === 'minimized') this.props.manager.restore(token);
        else this.props.manager.focus(token);
    }
    restoreFocus(trigger) {
        if (!this.current()) return;
        if (eligible(trigger)) { trigger.focus(); return; }
        const record = [...this.props.manager.snapshot()].filter(item => item.presentation === 'shown' && !retiring(item))
            .sort((left, right) => right.rank - left.rank)[0];
        const chrome = record && this.chrome.get(record.token);
        if (eligible(chrome)) { (controls(chrome)[0] || chrome).focus(); return; }
        const fallback = this.props.focusFallback?.();
        if (eligible(fallback)) fallback.focus();
    }
    resolveConfirmation(discard) {
        const confirmation = this.props.interaction.confirmation;
        if (!this.current() || confirmation?.kind !== 'float') return;
        const trigger = discard ? this.props.manager.focusTrigger(confirmation.recordToken) : confirmation.trigger;
        this.props.interaction.resolveConfirmation(discard);
        queueMicrotask(() => { if (this.current()) this.restoreFocus(trigger); });
    }
    componentWillUnmount() { this.retireLayer(); }
    retireLayer() {
        if (this.retiredLayer) return;
        this.retiredLayer = true;
        this.mounted = false;
        this.pendingFocus = null;
        let failed = false;
        const clean = operation => { try { operation(); } catch { failed = true; } };
        clean(() => this.viewport?.dispose());
        clean(() => this.unsubscribeManager?.()); clean(() => this.unsubscribeInteraction?.());
        clean(() => this.unsubscribeFocus?.()); clean(() => this.unsubscribeRetirement?.());
        clean(() => document.removeEventListener('keydown', this.keydown, true));
        clean(() => { if (this.modalToken) this.props.interaction.clearModal(this.modalToken); });
        clean(() => { if (!this.syncBackground(false)) throw new Error('float background restoration failed'); });
        clean(() => {
            if (this.layer) { this.layer.hidden = true; this.layer.inert = true; this.layer.setAttribute('aria-hidden', 'true'); }
        });
        clean(() => { this.layer?.remove(); if (this.layer?.isConnected) throw new Error('float layer removal failed'); });
        for (const record of this.state.records) {
            record.title = record.applicationTitle = '';
            record.container = record.geometry = null;
        }
        this.chrome.clear();
        this.background.clear();
        this.viewport = this.layer = this.confirmationElement = this.confirmationToken = this.modalToken = null;
        this.state = { records: [], area: { left: 0, top: 0, width: 0, height: 0 }, revision: 0 };
        if (failed) this.props.manager.reservations.poison();
    }
    render({ manager, interaction }, { records, area }) {
        if (this.retiredLayer) return null;
        const mode = classifyFloatWorkArea(area.width, area.height).mode;
        const top = [...records].filter(record => record.presentation === 'shown' && !retiring(record))
            .sort((left, right) => right.rank - left.rank)[0];
        const modal = mode !== 'desktop' && mode !== 'deferred';
        const confirmation = interaction.confirmation;
        const poisoned = manager.reservations.status().poisoned;
        const pendingCleanup = manager.reservations.status().count > records.length;
        const onReload = () => {
            if (this.current() && !interaction.confirmation && (!interaction.modal || interaction.modal.kind === 'float')) window.location.reload();
        };
        return h('div', { class: 'float-layer', ref: element => { this.layer = element; },
            style: { left: `${area.left}px`, top: `${area.top}px`, width: `${area.width}px`, height: `${area.height}px` } },
        records.map(record => h(FloatChrome, { key: record.token, record, area, records, manager, interaction,
            activeModal: modal && top?.token === record.token,
            blocked: !!confirmation || (interaction.modal && interaction.modal.kind !== 'float'),
            registerChrome: this.registerChrome, onAction: (name, token) => this.action(name, token),
            onSelect: token => this.select(token), onReload, poisoned, pendingCleanup })),
        !modal && h(FloatList, { records, onSelect: token => this.select(token),
            blocked: !!interaction.modal, onAction: (name, token) => this.action(name, token) }),
        (!modal || !top) && poisoned && h(BudgetWarning, { onReload, blocked: !!interaction.modal }),
        (!modal || !top) && pendingCleanup && h(PendingNotice),
        confirmation?.kind === 'float' && h('div', { class: 'float-confirmation-backdrop' },
            h('div', { class: 'float-confirmation', role: 'dialog', 'aria-modal': 'true',
                'aria-labelledby': 'float-confirmation-title', tabIndex: -1,
                ref: element => {
                    this.confirmationElement = element;
                    if (!element || this.confirmationToken === confirmation.token) return;
                    this.confirmationToken = confirmation.token;
                    queueMicrotask(() => {
                        if (this.current() && interaction.confirmation?.token === confirmation.token && element.isConnected) {
                            element.querySelector('button')?.focus();
                        }
                    });
                } }, h('h2', { id: 'float-confirmation-title' }, 'Discard changes and close?'),
            h('p', null, 'Unsaved changes in this panel will be lost.'),
            h('div', { class: 'float-actions' },
                h('button', { type: 'button', onClick: () => this.resolveConfirmation(false) }, 'Cancel'),
                h('button', { type: 'button', onClick: () => this.resolveConfirmation(true) }, 'Discard')))));
    }
}
