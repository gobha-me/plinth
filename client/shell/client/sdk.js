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

import { h } from 'preact';
import { useEffect, useState } from 'preact/hooks';

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
// The browser sends its HttpOnly session cookie in the same-origin upgrade.
// The server's `connected` frame (not the transport open event) admits channel
// requests. One owner holds the socket; one timer owns reconnect backoff.
// Channel changes serialize through acknowledgements so removal during auth,
// pending subscribe, or reconnect cannot resurrect a removed handler.

export class RealtimeError extends Error {
    constructor(code, message) {
        super(message);
        this.name = 'RealtimeError';
        this.code = code;
    }
}

const subscriptions = new Map(); // channel -> Set<{handler, onError}>
const stateListeners = new Set();
let socket = null;
let reconnectTimer = null;
let backoffMs = 1000;
let terminalError = null;
let realtimeState = Object.freeze({ status: 'idle', error: null });

function notify(handler, value) {
    try { handler(value); }
    catch (error) { console.error('[plinth.realtime handler]', error); }
}

function setRealtimeState(status, error = null) {
    realtimeState = Object.freeze({ status, error });
    for (const listener of [...stateListeners]) {
        if (stateListeners.has(listener)) notify(listener, realtimeState);
    }
}

export function getRealtimeState() { return realtimeState; }

export function onRealtimeState(listener) {
    stateListeners.add(listener);
    notify(listener, realtimeState);
    return () => stateListeners.delete(listener);
}

function reportError(error, channel) {
    const sets = channel === undefined
        ? [...subscriptions.values()] : [subscriptions.get(channel)];
    for (const set of sets) {
        for (const entry of [...(set || [])]) {
            if (set.has(entry) && entry.onError) notify(entry.onError, error);
        }
    }
}

function closeSocket() {
    if (reconnectTimer !== null) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    const previous = socket;
    socket = null; // Late close/error/message callbacks no longer own anything.
    previous?.ws.close();
}

function failRealtime(error) {
    terminalError = error;
    closeSocket();
    setRealtimeState('failed', error);
    reportError(error);
}

// Call explicitly after successful sign-in or to reclaim a displaced session.
// Auth failures never cause an unbounded background authentication loop.
export function reconnectRealtime() {
    terminalError = null;
    backoffMs = 1000;
    closeSocket();
    setRealtimeState('idle');
    ensureWs();
}

function reconcile(owner) {
    if (socket !== owner || !owner.authenticated || owner.pending ||
        owner.ws.readyState !== WebSocket.OPEN) return;
    const removed = [...owner.granted].filter(channel => !subscriptions.has(channel));
    const added = [...subscriptions.keys()].filter(channel =>
        !owner.granted.has(channel) && !owner.denied.has(channel));
    const type = removed.length ? 'unsubscribe' : 'subscribe';
    const channels = removed.length ? removed : added;
    if (!channels.length) return;
    owner.pending = { type, channels };
    owner.ws.send(JSON.stringify({ type, channels }));
}

function acceptAcknowledgement(owner, frame) {
    const pending = owner.pending;
    if (!pending || frame.type !== pending.type + 'd' ||
        !Array.isArray(frame.channels) || !frame.channels.every(c => typeof c === 'string')) {
        failRealtime(new RealtimeError('protocol_error', 'Invalid subscription acknowledgement'));
        return;
    }
    owner.pending = null;
    const acknowledged = new Set(frame.channels);
    for (const channel of pending.channels) {
        if (pending.type === 'unsubscribe') {
            owner.granted.delete(channel);
            owner.denied.delete(channel);
        } else if (acknowledged.has(channel)) {
            owner.granted.add(channel);
        } else {
            owner.denied.add(channel);
            reportError(new RealtimeError('subscription_denied',
                `Subscription was not granted: ${channel}`), channel);
        }
    }
    reconcile(owner);
}

function receiveFrame(owner, event) {
    if (socket !== owner) return;
    let frame;
    try { frame = JSON.parse(event.data); } catch { return; }
    if (!frame || typeof frame !== 'object') return;
    if (frame.type === 'connected') {
        if (owner.authenticated) return;
        owner.authenticated = true;
        backoffMs = 1000;
        setRealtimeState('connected');
        reconcile(owner);
    } else if (frame.type === 'ping' && owner.authenticated &&
               Number.isSafeInteger(frame.timestamp)) {
        owner.ws.send(JSON.stringify({ type: 'pong', timestamp: frame.timestamp }));
    } else if (frame.type === 'subscribed' || frame.type === 'unsubscribed') {
        acceptAcknowledgement(owner, frame);
    } else if (frame.type === 'error') {
        const code = typeof frame.error === 'string' ? frame.error : 'server_error';
        const error = new RealtimeError(code, frame.message || code);
        if (['auth_failed', 'auth_timeout', 'already_connected', 'session_expired',
             'session_revoked', 'not_authenticated'].includes(code)) {
            failRealtime(error);
        } else {
            reportError(error);
        }
    } else if (frame.type === 'event' && owner.granted.has(frame.channel)) {
        const set = subscriptions.get(frame.channel);
        for (const entry of [...(set || [])]) {
            if (set.has(entry)) notify(entry.handler, frame);
        }
    }
}

function ensureWs() {
    if (socket || reconnectTimer !== null || terminalError || !subscriptions.size) return;
    const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${window.location.host}/ws/events`);
    const owner = { ws, authenticated: false, granted: new Set(),
        denied: new Set(), pending: null };
    socket = owner;
    ws.addEventListener('message', event => receiveFrame(owner, event));
    // A browser WebSocket error is always followed by close. Only close owns
    // reconnect scheduling, avoiding two sockets/timers from one disconnect.
    ws.addEventListener('close', event => {
        if (socket !== owner) return;
        socket = null;
        if ([4001, 4002, 4003].includes(event.code)) {
            const codes = { 4001: 'auth_timeout', 4002: 'auth_failed', 4003: 'already_connected' };
            failRealtime(new RealtimeError(codes[event.code], event.reason || codes[event.code]));
            return;
        }
        if (!subscriptions.size) { setRealtimeState('idle'); return; }
        const error = new RealtimeError('disconnected', 'Realtime connection interrupted');
        const delay = backoffMs;
        backoffMs = Math.min(backoffMs * 2, 30000);
        reconnectTimer = setTimeout(() => {
            reconnectTimer = null;
            ensureWs();
        }, delay);
        setRealtimeState('reconnecting', error);
        reportError(error);
    });
    setRealtimeState('connecting');
}

export function subscribe(channel, handler, options = {}) {
    if (typeof channel !== 'string' || !channel || typeof handler !== 'function') {
        throw new TypeError('subscribe requires a nonempty channel and handler');
    }
    let set = subscriptions.get(channel);
    if (!set) {
        set = new Set();
        subscriptions.set(channel, set);
        socket?.denied.delete(channel);
    }
    const entry = { handler, onError: options.onError };
    set.add(entry);
    if (terminalError) {
        const error = terminalError;
        queueMicrotask(() => {
            if (set.has(entry) && terminalError === error && entry.onError) notify(entry.onError, error);
        });
    } else {
        ensureWs();
        if (socket?.denied.has(channel)) {
            const owner = socket;
            queueMicrotask(() => {
                if (socket === owner && set.has(entry) && owner.denied.has(channel) && entry.onError) {
                    notify(entry.onError, new RealtimeError('subscription_denied',
                        `Subscription was not granted: ${channel}`));
                }
            });
        }
        if (socket) reconcile(socket);
    }
    return function unsubscribe() {
        if (!set.delete(entry)) return;
        if (!set.size) {
            subscriptions.delete(channel);
            socket?.denied.delete(channel);
        }
        if (!subscriptions.size) {
            closeSocket();
            if (!terminalError) setRealtimeState('idle');
        } else if (socket) {
            reconcile(socket);
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
            setError(null);
            setLoading(false);
        }, { onError: (error) => {
            if (!cancelled) { setError(error); setLoading(false); }
        } });
        return () => { cancelled = true; unsub(); };
    // Channel + opts identity drive resubscription; consumers pass
    // stable opts or accept the conservative re-fetch.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    }, [channel]);

    return { data, error, loading };
}

// ── Convenience namespace ───────────────────────────────────────────

export const plinth = { call, subscribe, useData, getRealtimeState,
    onRealtimeState, reconnectRealtime };
