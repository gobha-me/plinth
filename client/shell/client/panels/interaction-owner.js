// Internal document/frame ownership. No public SDK or authorization seam.
export class DocumentInteractionOwner {
    constructor({ reservations, document: doc = globalThis.document, window: win = globalThis.window }) {
        this.reservations = reservations;
        this.document = doc;
        this.window = win;
        this.frame = null;
        this.beforeUnloadInstalled = false;
        this.keydown = event => this.frame?.dispatchShortcut(event);
        this.beforeUnload = event => {
            if (!this.hasUnsavedWork()) return;
            event.preventDefault();
            event.returnValue = '';
        };
        doc.addEventListener('keydown', this.keydown);
        // This observer belongs to the document, never to a retired frame.
        this.unsubscribeBudget = reservations.subscribe(() => {
            this.syncUnload();
            this.frame?.publish();
        });
    }

    beginFrame(options = {}) {
        if (this.retiring) throw new DOMException('frame cleanup is still running', 'AbortError');
        this.frame?.retire();
        const frame = new InteractionScope(this, options);
        this.frame = frame;
        this.syncUnload();
        return frame;
    }

    hasUnsavedWork() {
        return this.reservations.status().unsaved === true || this.frame?.hasUnsavedWork() === true;
    }

    syncUnload() {
        const required = this.hasUnsavedWork();
        if (required === this.beforeUnloadInstalled) return;
        if (required) this.window.addEventListener('beforeunload', this.beforeUnload);
        else this.window.removeEventListener('beforeunload', this.beforeUnload);
        this.beforeUnloadInstalled = required;
    }

    dispose() {
        this.frame?.retire();
        this.unsubscribeBudget();
        this.document.removeEventListener('keydown', this.keydown);
        if (this.beforeUnloadInstalled) this.window.removeEventListener('beforeunload', this.beforeUnload);
        this.beforeUnloadInstalled = false;
        // No reservation reset: only destruction of the real document does that.
    }
}

class InteractionScope {
    constructor(documentOwner, { isCurrent = () => true } = {}) {
        this.documentOwner = documentOwner;
        this.current = isCurrent;
        this.retired = false;
        this.primary = null;
        this.floats = new Map();
        this.listeners = new Set();
        this.retirement = new Set();
        this.focused = { kind: 'primary' };
        this.baseModal = null;
        this.confirmation = null;
        this.geometry = null;
    }

    isCurrent() {
        return !this.retired && this.documentOwner.frame === this && this.current();
    }

    subscribe(listener) {
        this.listeners.add(listener);
        return () => this.listeners.delete(listener);
    }

    onRetire(callback) {
        this.retirement.add(callback);
        return () => this.retirement.delete(callback);
    }

    publish() {
        this.documentOwner.syncUnload();
        if (!this.isCurrent()) return;
        for (const listener of [...this.listeners]) listener();
    }

    registerPrimary(provider) {
        if (!this.isCurrent()) return () => {};
        this.primary = provider;
        this.publish();
        return () => {
            if (this.primary !== provider) return;
            this.primary = null;
            this.publish();
        };
    }

    registerFloat(token, provider) {
        if (!this.isCurrent()) return () => {};
        if (!this.floats.has(token) && this.floats.size >= 5) throw new Error('float provider limit');
        this.floats.set(token, provider);
        this.publish();
        return () => {
            if (this.floats.get(token) !== provider) return;
            this.floats.delete(token);
            if (this.focused.token === token) this.focused = { kind: 'none' };
            if (this.baseModal?.token === token) this.baseModal = null;
            if (this.geometry?.token === token) this.geometry = null;
            this.dismissConfirmation(token);
            this.publish();
        };
    }

    hasUnsavedWork() {
        for (const provider of [this.primary, ...this.floats.values()]) {
            if (!provider) continue;
            try { if (provider.isDirty()) return true; }
            catch { return true; } // Unknown dirty state must not suppress the warning.
        }
        return false;
    }

    focusPrimary() { this.setFocus({ kind: 'primary' }); }
    focusFloat(token) { this.setFocus({ kind: 'float', token }); }
    focusShell() { this.setFocus({ kind: 'none' }); }
    setFocus(value) {
        if (!this.isCurrent()) return;
        if (this.focused.kind === value.kind && this.focused.token === value.token) return;
        this.focused = value;
        this.publish();
    }

    dispatchShortcut(event) {
        if (!this.isCurrent() || event.defaultPrevented || this.geometry) return;
        const modal = this.modal;
        if (modal && (modal.kind !== 'float' || this.focused.token !== modal.token)) return;
        const provider = this.focused.kind === 'primary' ? this.primary
            : this.focused.kind === 'float' ? this.floats.get(this.focused.token) : null;
        // A focused loading/failed float deliberately has no primary fallback.
        if (provider?.isEligible()) provider.dispatch(event);
    }

    get modal() {
        return this.confirmation ? { kind: 'confirmation', token: this.confirmation.token } : this.baseModal;
    }

    setModal(kind, token) {
        if (!this.isCurrent()) return false;
        if (kind === 'switcher' && this.baseModal?.kind === 'float') return false;
        if ((!kind && !this.baseModal) ||
            (this.baseModal?.kind === kind && this.baseModal.token === token)) return true;
        this.baseModal = kind ? { kind, token } : null;
        if (kind) this.geometry = null;
        this.publish();
        return true;
    }

    clearModal(token) {
        if (this.baseModal?.token !== token) return;
        this.baseModal = null;
        this.publish();
    }

    beginGeometry(token) {
        if (!this.isCurrent() || this.confirmation || this.geometry) return null;
        const lease = Symbol();
        this.geometry = { token, lease };
        this.publish();
        return lease;
    }

    endGeometry(lease) {
        if (this.geometry?.lease !== lease) return;
        this.geometry = null;
        this.publish();
    }

    confirm({ kind, recordToken, onCancel, onDiscard, trigger }) {
        if (!this.isCurrent() || this.confirmation) return false;
        this.geometry = null;
        this.confirmation = { kind, recordToken, token: Symbol(), onCancel, onDiscard, trigger };
        this.publish();
        return true;
    }

    resolveConfirmation(discard) {
        if (!this.isCurrent() || !this.confirmation) return;
        const confirmation = this.confirmation;
        this.confirmation = null; // Fence re-entrant actions before invoking user code.
        this.publish();
        if (!this.isCurrent()) return;
        if (discard) confirmation.onDiscard();
        else confirmation.onCancel();
    }

    dismissConfirmation(recordToken) {
        if (!this.confirmation || (recordToken !== undefined && this.confirmation.recordToken !== recordToken)) return;
        this.confirmation = null;
        this.publish();
    }

    retire() {
        if (this.retired) return;
        this.retired = true;
        const cleanup = [...this.retirement];
        this.retirement.clear();
        this.listeners.clear();
        this.primary = null;
        this.floats.clear();
        this.confirmation = this.geometry = this.baseModal = null;
        this.focused = { kind: 'none' };
        this.current = () => false;
        if (this.documentOwner.frame === this) this.documentOwner.frame = null;
        this.documentOwner.retiring = true;
        for (const callback of cleanup) {
            try { callback(); } catch { this.documentOwner.reservations.poison({ dirty: true }); }
        }
        this.documentOwner.retiring = false;
        this.documentOwner.syncUnload();
    }
}
