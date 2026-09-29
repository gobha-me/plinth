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
import { h } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';
import { prepareCapabilityRequest, captureView, sameQuery } from './data-query.js';
import { createDataController } from './data-controller.js';

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

// Cookie-authenticated mutations use a session-bound double-submit token. Read
// the cookie for every request so logout/login rotation cannot leave a cached
// token in the shell or in long-lived panel modules. Never forward it to a
// different origin, even if a future caller passes an absolute URL.
export function withCsrf(url, options = {}) {
    const withoutCsrf = () => {
        const headers = new Headers(options.headers || {});
        if (!headers.has('X-Plinth-CSRF')) return { ...options };
        headers.delete('X-Plinth-CSRF');
        return { ...options, headers };
    };
    const method = String(options.method || 'GET').toUpperCase();
    if (['GET', 'HEAD', 'OPTIONS'].includes(method)) return withoutCsrf();

    let target;
    try {
        target = new URL(url, window.location.href);
    } catch (_) {
        return withoutCsrf();
    }
    if (target.origin !== window.location.origin || typeof document === 'undefined') {
        return withoutCsrf();
    }

    const csrfCookie = document.cookie.split(';').map(part => part.trim()).find(part =>
        part.startsWith('plinth_csrf='));
    if (!csrfCookie) return withoutCsrf();

    let token;
    try {
        token = decodeURIComponent(csrfCookie.slice('plinth_csrf='.length));
    } catch (_) {
        return withoutCsrf();
    }
    if (!token) return withoutCsrf();

    const headers = new Headers(options.headers || {});
    headers.set('X-Plinth-CSRF', token);
    return { ...options, headers };
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
    let prepared;
    try {
        prepared = prepareCapabilityRequest(capability, args);
    } catch (error) {
        throw new NetworkError(`fetch failed for ${capability}`, error);
    }
    return requestPrepared(prepared);
}

async function requestPrepared(prepared, { signal } = {}) {
    const capability = prepared.capability;
    let resp;
    try {
        resp = await fetch(prepared.url, withCsrf(prepared.url, {
            method:      'POST',
            credentials: 'include',
            headers:     { 'Content-Type': 'application/json' },
            body:        prepared.body,
            ...(signal ? { signal } : {}),
        }));
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
    // CSRF rejections happen before capability dispatch and therefore use the
    // kernel's top-level HTTP error shape. Preserve the ordinary capability
    // envelope while exposing either response as one typed SDK error.
    const rawError = body && body.error;
    const err = typeof rawError === 'string'
        ? { code: rawError, message: body.message }
        : (rawError || {});
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

const subscriptions = new Map(); // channel -> Set<{handler, onError, onReady, readyOwner}>
const stateListeners = new Set();
let socket = null;
let reconnectTimer = null;
let backoffMs = 1000;
let terminalError = null;
let realtimeState = Object.freeze({ status: 'idle', error: null });
let sessionOwner = { active: true, error: null, listeners: new Set() };
const hookSessionListeners = new Set();
const DEFAULT_ADVICE = Object.freeze({ debounceMs: 100, jitterMs: 50 });

function grantAdvice(frame) {
    const valid = (value, maximum, fallback) =>
        Number.isInteger(value) && value >= 0 && value <= maximum ? value : fallback;
    return Object.freeze({
        debounceMs: valid(frame.recommended_debounce_ms, 60000, DEFAULT_ADVICE.debounceMs),
        jitterMs: valid(frame.recommended_jitter_ms, 5000, DEFAULT_ADVICE.jitterMs),
    });
}

// These are managed-shell admission seams, not implicit reauthentication on
// scope changes. Old owners can never become live again after retirement.
export function retireRealtimeSession(code = 'session_ended') {
    retireSession(new RealtimeError(code, 'Realtime session has ended'), false);
}

function retireSession(error, report) {
    if (!sessionOwner.active) return;
    const retired = sessionOwner;
    retired.active = false;
    retired.error = error;
    const entries = [...subscriptions.values()].flatMap(set => [...set]);
    for (const set of subscriptions.values()) set.clear();
    subscriptions.clear();
    if (socket) socket.restart = false;
    terminalError = retired.error;
    closeSocket();
    for (const listener of [...retired.listeners]) notify(listener, false);
    retired.listeners.clear();
    setRealtimeState('failed', retired.error);
    for (const entry of report ? entries : []) {
        if (sessionOwner === retired && entry.onError) notify(entry.onError, error);
    }
    for (const listener of [...hookSessionListeners]) notify(listener);
}

export function activateRealtimeSession() {
    if (sessionOwner.active) retireRealtimeSession('session_rotated');
    sessionOwner = { active: true, error: null, listeners: new Set() };
    for (const listener of [...hookSessionListeners]) notify(listener);
    reconnectRealtime();
}

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

function reportReady(owner, channel) {
    const grant = owner.advice.get(channel);
    if (!grant) return;
    const set = subscriptions.get(channel);
    for (const entry of [...(set || [])]) {
        if (!set.has(entry) || entry.readyOwner === grant || !entry.onReady) continue;
        entry.readyOwner = grant;
        queueMicrotask(() => {
            if (sessionOwner === owner.session && sessionOwner.active &&
                socket === owner && !owner.closing && terminalError === null &&
                subscriptions.get(channel) === set && owner.advice.get(channel) === grant &&
                set.has(entry) && owner.granted.has(channel) &&
                entry.readyOwner === grant) notify(entry.onReady, grant.value);
        });
    }
}

function closeSocket(restart = false) {
    if (reconnectTimer !== null) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    const previous = socket;
    if (previous) {
        // Retain ownership until the close event. Starting a replacement before
        // that event would briefly create two live sockets for one session.
        previous.closing = true;
        previous.advice.clear();
        previous.restart ||= restart;
        previous.ws.close();
    }
}

function failRealtime(error) {
    if (['not_authenticated', 'session_expired', 'session_revoked'].includes(error.code)) {
        retireSession(error, true);
        return;
    }
    terminalError = error;
    closeSocket();
    setRealtimeState('failed', error);
    reportError(error);
}

// Call explicitly after successful sign-in or to reclaim a displaced session.
// Auth failures never cause an unbounded background authentication loop.
export function reconnectRealtime() {
    if (!sessionOwner.active) return;
    terminalError = null;
    backoffMs = 1000;
    setRealtimeState('idle');
    closeSocket(true);
    ensureWs();
}

function reconcile(owner) {
    if (socket !== owner || owner.closing || !owner.authenticated || owner.pending ||
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
            owner.advice.delete(channel);
        } else if (acknowledged.has(channel)) {
            owner.granted.add(channel);
            owner.advice.set(channel, { value: grantAdvice(frame) });
            reportReady(owner, channel);
        } else {
            owner.denied.add(channel);
            reportError(new RealtimeError('subscription_denied',
                `Subscription was not granted: ${channel}`), channel);
        }
    }
    reconcile(owner);
}

function receiveFrame(owner, event) {
    if (socket !== owner || owner.closing || sessionOwner !== owner.session || !sessionOwner.active) return;
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
    if (!sessionOwner.active || terminalError || !subscriptions.size) return;
    if (socket) {
        if (socket.closing) socket.restart = true;
        return;
    }
    if (reconnectTimer !== null) return;
    const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
    const ws = new WebSocket(`${proto}://${window.location.host}/ws/events`);
    const owner = { ws, authenticated: false, granted: new Set(),
        denied: new Set(), advice: new Map(), session: sessionOwner,
        pending: null, closing: false, restart: false };
    socket = owner;
    ws.addEventListener('message', event => receiveFrame(owner, event));
    // A browser WebSocket error is always followed by close. Only close owns
    // reconnect scheduling, avoiding two sockets/timers from one disconnect.
    ws.addEventListener('close', event => {
        if (socket !== owner) return;
        socket = null;
        owner.advice.clear();
        if (owner.closing) {
            if (owner.restart) ensureWs();
            return;
        }
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
    if (options.onReady !== undefined && typeof options.onReady !== 'function') {
        throw new TypeError('subscribe onReady must be a function');
    }
    const admitted = sessionOwner;
    if (!admitted.active) {
        queueMicrotask(() => {
            if (sessionOwner === admitted && options.onError) notify(options.onError, admitted.error);
        });
        return () => {};
    }
    let set = subscriptions.get(channel);
    if (!set) {
        set = new Set();
        subscriptions.set(channel, set);
        socket?.denied.delete(channel);
    }
    const entry = { handler, onError: options.onError, onReady: options.onReady,
        readyOwner: null };
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
        if (socket?.granted.has(channel)) reportReady(socket, channel);
        if (socket) reconcile(socket);
    }
    return function unsubscribe() {
        if (!set.delete(entry)) return;
        if (sessionOwner !== admitted || subscriptions.get(channel) !== set) return;
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
// Snapshot-backed hooks own conservative smart requeries. Without a snapshot,
// data remains the original outer event frame. Stored state is tagged with its
// render owner so a changed query/session cannot show the previous owner's data
// even before effect cleanup; body identity stays private and in memory.

export function useData(channel, opts) {
    opts = opts || {};
    const renderOwner = useRef(null);
    const requestCapture = useRef(null);
    const viewCapture = useRef(null);
    const [stored, setStored] = useState(null);
    const [, setSessionRevision] = useState(0);
    const session = sessionOwner;
    const query = { channel, scope: opts.scope, initialData: opts.initialData,
        view: null, snapshot: null, preparationError: null };
    const sameAdmission = (capture, capability) => capture &&
        capture.channel === channel && Object.is(capture.capability, capability) &&
        Object.is(capture.scope, opts.scope) && capture.session === session;
    if (opts.snapshot != null) {
        let capability;
        try {
            capability = opts.snapshot.capability;
            const args = opts.snapshot.args;
            let capture = requestCapture.current;
            // A continuously supplied object is one immutable query input.
            // Automatic renders must not serialize later caller mutations;
            // supply a new object to request a changed query. Explicit owner
            // discriminators recapture even when input references are reused.
            if (!sameAdmission(capture, capability) || !Object.is(capture.args, args)) {
                capture = { channel, capability, scope: opts.scope, session, args,
                    snapshot: null, error: null };
                try { capture.snapshot = prepareCapabilityRequest(capability, args); }
                catch (error) { capture.error = new NetworkError(`fetch failed for ${capability}`, error); }
                requestCapture.current = capture;
            }
            query.snapshot = capture.snapshot;
            query.preparationError = capture.error;
            let descriptor = viewCapture.current;
            if (!sameAdmission(descriptor, capability) || descriptor.source !== opts.view ||
                descriptor.identity !== query.snapshot?.identity) {
                descriptor = { channel, capability, scope: opts.scope, session,
                    source: opts.view, identity: query.snapshot?.identity, value: captureView(opts.view) };
                viewCapture.current = descriptor;
            }
            query.view = descriptor.value;
        } catch (error) {
            query.preparationError = new NetworkError(`fetch failed for ${capability}`, error);
        }
    } else {
        // Re-entering snapshot mode is a fresh logical query admission.
        requestCapture.current = null;
        viewCapture.current = null;
    }
    let owner = renderOwner.current;
    if (!owner || owner.session !== session || owner.allowed !== session.active ||
        !sameQuery(owner.query, query)) {
        owner = { query, session, allowed: session.active };
        renderOwner.current = owner;
    }
    const isCurrent = () => renderOwner.current === owner &&
        sessionOwner === owner.session && owner.session.active;
    useEffect(() => {
        const refresh = () => setSessionRevision(value => value + 1);
        hookSessionListeners.add(refresh);
        // Rotation can occur between render and this effect's admission.
        if (renderOwner.current?.session !== sessionOwner ||
            renderOwner.current?.allowed !== sessionOwner.active) refresh();
        return () => hookSessionListeners.delete(refresh);
    }, []);
    useEffect(() => {
        if (!isCurrent()) return;
        const controller = createDataController({
            query: owner.query, isCurrent,
            publish: state => {
                if (isCurrent()) setStored(previous => isCurrent() ? { owner, state } : previous);
            },
            request: requestPrepared, subscribe,
            admission: {
                isAllowed: () => owner.session === sessionOwner && owner.session.active,
                subscribe: listener => {
                    owner.session.listeners.add(listener);
                    return () => owner.session.listeners.delete(listener);
                },
            },
            clock: () => Date.now(), random: () => Math.random(),
            timer: { set: (fn, milliseconds) => setTimeout(fn, milliseconds), clear: clearTimeout },
            AbortController,
        });
        controller.start();
        return () => controller.dispose();
    }, [owner]);
    if (stored?.owner === owner && isCurrent()) return stored.state;
    return { data: owner.query.initialData,
        error: owner.query.preparationError || (!owner.session.active ? owner.session.error : null),
        loading: owner.session.active && !!owner.query.snapshot && !owner.query.preparationError };
}

// ── Convenience namespace ───────────────────────────────────────────

export const plinth = { call, subscribe, useData, getRealtimeState,
    onRealtimeState, reconnectRealtime };
