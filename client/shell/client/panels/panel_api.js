// shell.zip/client/panels/panel_api.js
//
// `makePanelApi` factory — ICD-0.6.3 §4.3.
//
// Per-panel scope object injected as `plinth.panel` into the panel
// module's default-export call. Five live methods + six stub methods
// (architect-pinned 5/6 split per ICD §3.1; stubs throw exact-string
// NotImplementedError messages so 0.6.4-0.6.6 cannot silently no-op
// per OQ1).
//
// `__shell_internal` is a private namespace the loader uses to drive
// activation / deactivation / shortcut dispatch / dirty-bit reads
// without the panel module needing visibility. Per
// `DESIGN-shell-v06x.md §10` constraint #2, panels do not introspect
// the shell — convention-plus-discipline.

import {
    NotImplementedError,
    PanelUnboundError,
    ShortcutConflictError,
} from '../sdk.js';

// Normalise modifier ordering per ICD §3.5 / §A.5: alphabetical-by-name
// (Alt, Ctrl, Meta, Shift) so `"Shift+Ctrl+S"` and `"Ctrl+Shift+S"`
// register as the same combo. Case-fold modifiers; preserve the key
// case (matters for letter keys like `s` vs `S` only in display).
const MOD_ORDER = ['Alt', 'Ctrl', 'Meta', 'Shift'];

export function normaliseCombo(combo) {
    if (typeof combo !== 'string' || combo.length === 0) {
        throw new TypeError(
            `registerShortcut: combo must be a non-empty string, got ${typeof combo}`);
    }
    const parts = combo.split('+').map((p) => p.trim()).filter(Boolean);
    if (parts.length === 0) {
        throw new TypeError(`registerShortcut: combo cannot be empty`);
    }
    const mods = [];
    let key = null;
    for (const p of parts) {
        const lc = p.toLowerCase();
        const matched = MOD_ORDER.find((m) => m.toLowerCase() === lc);
        if (matched) {
            if (!mods.includes(matched)) { mods.push(matched); }
        } else {
            if (key !== null) {
                throw new TypeError(
                    `registerShortcut: combo has multiple non-modifier keys`);
            }
            key = p;
        }
    }
    if (key === null) {
        throw new TypeError(
            `registerShortcut: combo must include exactly one non-modifier key`);
    }
    mods.sort((a, b) => MOD_ORDER.indexOf(a) - MOD_ORDER.indexOf(b));
    return [...mods, key].join('+');
}

export function makePanelApi({ shell, panel, context, packageRow }) {
    const activateCallbacks   = [];
    const deactivateCallbacks = [];
    const navIntentCallbacks  = [];
    const shortcutRegistry    = new Map();
    let   dirty   = false;
    let   unbound = false;

    return {
        onActivate(cb) {
            if (unbound) { return; }
            if (typeof cb !== 'function') {
                throw new TypeError('onActivate: callback must be a function');
            }
            activateCallbacks.push(cb);
        },
        onDeactivate(cb) {
            if (unbound) { return; }
            if (typeof cb !== 'function') {
                throw new TypeError('onDeactivate: callback must be a function');
            }
            deactivateCallbacks.push(cb);
        },
        onNavigationIntent(cb) {
            if (unbound) { return; }
            if (typeof cb !== 'function') {
                throw new TypeError(
                    'onNavigationIntent: callback must be a function');
            }
            navIntentCallbacks.push(cb);
        },
        setDirty(isDirty) {
            if (unbound) { return; }
            if (typeof isDirty !== 'boolean') {
                throw new TypeError(
                    `setDirty: expected boolean, got ${typeof isDirty}`);
            }
            dirty = isDirty;
            if (shell && typeof shell.notifyDirtyChange === 'function') {
                shell.notifyDirtyChange(panel.id, isDirty);
            }
        },
        registerShortcut(combo, callback) {
            if (unbound) {
                throw new PanelUnboundError('registerShortcut', panel.id);
            }
            if (typeof callback !== 'function') {
                throw new TypeError(
                    'registerShortcut: callback must be a function');
            }
            const normalised = normaliseCombo(combo);
            if (shortcutRegistry.has(normalised)) {
                throw new ShortcutConflictError(normalised, panel.id);
            }
            shortcutRegistry.set(normalised, callback);
            return function unregister() {
                shortcutRegistry.delete(normalised);
            };
        },
        getContext() {
            return context;
        },

        // ── Stubs (architect-pinned per OQ1: throw / reject) ──
        navigate(_target, _ctx) {
            throw new NotImplementedError('navigate', '6');
        },
        openFloat(_contentType, _ctx) {
            return Promise.reject(new NotImplementedError('openFloat', '5'));
        },
        requestFocus() {
            throw new NotImplementedError('requestFocus', '5');
        },
        setTrayState(_stateName) {
            throw new NotImplementedError('setTrayState', '6');
        },
        setTrayBadge(_value) {
            throw new NotImplementedError('setTrayBadge', '6');
        },

        // ── Private — shell loader use only (not exposed to panels by
        //    convention; ICD §4.3 / DESIGN §10 constraint #2). ──
        __shell_internal: {
            fireActivate() {
                for (const cb of activateCallbacks) { cb(); }
            },
            fireDeactivate() {
                for (const cb of deactivateCallbacks) { cb(); }
            },
            fireNavigationIntent(target, ctx) {
                for (const cb of navIntentCallbacks) { cb(target, ctx); }
            },
            isDirty() { return dirty; },
            getShortcuts() { return shortcutRegistry; },
            getPackageRow() { return packageRow; },
            unbind() {
                unbound = true;
                activateCallbacks.length   = 0;
                deactivateCallbacks.length = 0;
                navIntentCallbacks.length  = 0;
                shortcutRegistry.clear();
            },
        },
    };
}
