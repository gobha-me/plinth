// shell.zip/client/sdk.js
//
// Client SDK module — ICD-0.6.3 §3.2 / §A.2.
//
// Browser-side wrappers for kernel capability dispatch
// (`POST /api/cap/{capability}`) and realtime event subscription
// (single shell-managed multiplexed WebSocket). Imported by panel
// modules via the import-map specifier `@plinth/frontend/sdk`
// declared in shell/client/index.html.
//
// Implementation deviations recorded in ICD-0.6.3 §17:
//   - No CSRF header today (no kernel-side CSRF infrastructure;
//     deferred to a follow-up).
//   - Reconnection backoff is silent (per OQ5); state lives in this
//     module and is not surfaced to handlers.

import { h } from '../vendor/preact.module.js';
import { useEffect, useState } from '../vendor/preact-hooks.module.js';

// ── Error classes ───────────────────────────────────────────────────

export class CapabilityError extends Error {
    constructor(code, message, sqlstate) {
        super(message);
        this.name     = 'CapabilityError';
        this.code     = code;
        this.sqlstate = sqlstate;
    }
}

export class NetworkError extends Error {
    constructor(message, cause) {
        super(message);
        this.name  = 'NetworkError';
        this.cause = cause;
    }
}

export class NotImplementedError extends Error {
    constructor(method, closesIn) {
        super(`plinth.panel.${method} is not implemented in 0.6.3 — closes 0.6.${closesIn}`);
        this.name = 'NotImplementedError';
    }
}

export class ShortcutConflictError extends Error {
    constructor(combo, panelId) {
        super(`combo ${combo} already registered by panel '${panelId}'`);
        this.name = 'ShortcutConflictError';
    }
}

export class PanelUnboundError extends Error {
    constructor(method, panelId) {
        super(`${method}: panel '${panelId}' is unbound`);
        this.name = 'PanelUnboundError';
    }
}

// ── plinth.call: HTTP cap-dispatch ──────────────────────────────────
//
// POSTs `{args: [...]}` to /api/cap/{capability}. Returns a Promise
// resolving to the capability's `value` field on 200 OK, or rejecting
// with CapabilityError on 4xx/5xx with the kernel's typed envelope.
// Network failures (fetch reject) reject with NetworkError.

export async function call(capability, args) {
    // Kernel's `cap.call(signature, args?)` takes a single args value
    // (not rest) per `cap_bindings.cpp:92-159`. The SDK matches this
    // shape: pass an object for handlers that destructure `({key, value})`,
    // a primitive for handlers that take a single positional, or undefined
    // for parameterless caps. ICD-0.6.3 §A.2's rest-spread shape is
    // incompatible with the kernel binding and was redesigned here —
    // see ICD §17 deviation #N.
    let resp;
    try {
        const body = (args === undefined) ? { args: null } : { args };
        resp = await fetch(`/api/cap/${encodeURIComponent(capability)}`, {
            method:      'POST',
            credentials: 'include',
            headers:     { 'Content-Type': 'application/json' },
            body:        JSON.stringify(body),
        });
    } catch (e) {
        throw new NetworkError(`fetch failed for ${capability}`, e);
    }
    let body;
    try {
        body = await resp.json();
    } catch (e) {
        throw new NetworkError(`response is not JSON for ${capability}`, e);
    }
    if (resp.ok && body && body.ok === true) {
        return body.value;
    }
    const err = (body && body.error) || {};
    throw new CapabilityError(
        err.code || 'unknown',
        err.message || resp.statusText,
        err.sqlstate);
}

// ── plinth.subscribe: shell-managed WebSocket multiplex ─────────────
//
// One WebSocket connection per page lifetime, lazily opened on first
// subscribe. All channel subscriptions multiplex over the single
// connection. Each handler is wrapped in try/catch so one panel's
// thrown handler does not break other subscribers (handler-error
// isolation per ICD §5.3).
//
// Reconnection: silent exponential backoff 1s → 2s → 4s → 8s → 16s
// → 30s cap (per OQ5). Pending subscriptions are re-asserted on
// reconnect.

const subscriptions = new Map();   // channel → Set<handler>
let   wsPromise     = null;
let   wsBackoffMs   = 1000;
const wsBackoffMax  = 30000;

function ensureWs() {
    if (wsPromise) { return wsPromise; }
    wsPromise = new Promise((resolve) => {
        const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
        const url   = `${proto}://${window.location.host}/ws`;
        const ws    = new WebSocket(url);
        ws.addEventListener('open', () => {
            wsBackoffMs = 1000;
            for (const channel of subscriptions.keys()) {
                ws.send(JSON.stringify({ type: 'subscribe', channel }));
            }
            resolve(ws);
        });
        ws.addEventListener('message', (ev) => {
            let env;
            try { env = JSON.parse(ev.data); } catch { return; }
            const handlers = subscriptions.get(env.channel);
            if (!handlers) { return; }
            for (const h of handlers) {
                try { h(env); }
                catch (e) {
                    // Handler-error isolation: log but continue.
                    // eslint-disable-next-line no-console
                    console.error('[plinth.subscribe handler]', e);
                }
            }
        });
        const reconnect = () => {
            wsPromise = null;
            const delay = Math.min(wsBackoffMs, wsBackoffMax);
            wsBackoffMs = Math.min(wsBackoffMs * 2, wsBackoffMax);
            setTimeout(() => { if (subscriptions.size > 0) { ensureWs(); } }, delay);
        };
        ws.addEventListener('close', reconnect);
        ws.addEventListener('error', reconnect);
    });
    return wsPromise;
}

export function subscribe(channel, handler) {
    let set = subscriptions.get(channel);
    if (!set) {
        set = new Set();
        subscriptions.set(channel, set);
    }
    set.add(handler);
    ensureWs().then((ws) => {
        if (ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ type: 'subscribe', channel }));
        }
    });
    return function unsubscribe() {
        const s = subscriptions.get(channel);
        if (!s) { return; }
        s.delete(handler);
        if (s.size === 0) {
            subscriptions.delete(channel);
            // Tell server to unsubscribe; if WS is closed, the
            // server already dropped the subscription on disconnect.
            if (wsPromise) {
                wsPromise.then((ws) => {
                    if (ws.readyState === WebSocket.OPEN) {
                        ws.send(JSON.stringify({ type: 'unsubscribe', channel }));
                    }
                });
            }
        }
    };
}

// ── plinth.useData: Preact hook ─────────────────────────────────────
//
// Composes `call` (snapshot fetch) + `subscribe` (live updates) into
// `{ data, error, loading }`. Stale-on-error semantics per OQ5: the
// `initialData` (or last-good `data`) persists when an update fails.

export function useData(channel, opts) {
    opts = opts || {};
    const [data, setData]       = useState(opts.initialData);
    const [error, setError]     = useState(null);
    const [loading, setLoading] = useState(opts.snapshot != null);

    useEffect(() => {
        let cancelled = false;
        if (opts.snapshot) {
            const { capability, args } = opts.snapshot;
            call(capability, args).then(
                (v) => { if (!cancelled) { setData(v); setLoading(false); } },
                (e) => { if (!cancelled) { setError(e); setLoading(false); } });
        }
        const unsub = subscribe(channel, (env) => {
            if (cancelled) { return; }
            setData(env);
        });
        return () => { cancelled = true; unsub(); };
    // Channel + opts identity drive resubscription; consumers pass
    // stable opts or accept the conservative re-fetch.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [channel]);

    return { data, error, loading };
}

// ── Convenience namespace ───────────────────────────────────────────

export const plinth = { call, subscribe, useData };
