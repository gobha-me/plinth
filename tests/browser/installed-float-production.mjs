// Actual installed-package journey. No endpoint interception or module mocks.
// The native supervisor owns the disposable database and protected policy.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { chromium } from 'playwright';
import { installFailureSummary, emitJourneyFailure, createJourneyFailureSteps, observeSelectedBody } from './installed-float-diagnostics.mjs';
import { boundedStep, createResponseBodyRegistry, observeReadLane, withFrozenReadLane,
    observeAuthorityRecovery } from './installed-float-coordination.mjs';
import { snapshotTypedSurfaceTargets, verifyTypedSurfaceUnavailable,
    verifyTypedSurfaceFoundation } from './installed-typed-surface-foundation.mjs';

const phase = process.argv[2];
assert(['install', 'verify'].includes(phase), 'owned supervisor phase required');
const base = process.env.PLINTH_BASE_URL;
const statePath = process.env.PLINTH_FLOAT_STATE;
const fixtureDir = process.env.PLINTH_FLOAT_FIXTURES;
const fakePassword = process.env.PLINTH_FLOAT_PASSWORD;
assert(base && statePath && fixtureDir && fakePassword);
const origin = new URL(base).origin;
const pg = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env[`PLINTH_PG_${suffix}`]);
    pg[`PG${suffix}`] = process.env[`PLINTH_PG_${suffix}`];
}
const literal = value => `'${String(value).replaceAll("'", "''")}'`;
const sql = statement => execFileSync('psql', ['-XAt', '-v', 'ON_ERROR_STOP=1', '-c', statement],
    { env: pg, encoding: 'utf8', timeout: 15000, stdio: ['ignore', 'pipe', 'pipe'] }).trim();
const jsonSql = statement => JSON.parse(sql(statement));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;
const fixtureRules = ['floatcaller.panel', 'floatprovider.panel', 'floatprovider.preview'];
const contexts = [];
const readLanes = new Map();
const responseBodies = createResponseBodyRegistry();
const bodyObservationTasks = new Set();
const privateSnapshotTasks = new Set();
const privateRealtime = new Map();
const authorityRecoveries = new Map();
const recoveryEvidence = [];
let privateRealtimeClosed = false;
let refusedFailureStage = null;
let browser;
let state;
let failure;
const cleanupFailures = [];
const completed = [];
const failureSteps = createJourneyFailureSteps();

async function publicPost(path, payload) {
    const response = await fetch(base + path, { method: 'POST',
        headers: { Origin: origin, 'Content-Type': 'application/json' },
        body: JSON.stringify(payload), signal: AbortSignal.timeout(15000) });
    return { status: response.status, body: await response.json() };
}
async function request(page, path, method = 'GET', payload, csrf = true) {
    return page.evaluate(async ({ path, method, payload, csrf }) => {
        const sdk = await import('@plinth/frontend/sdk');
        let body;
        if (payload?.zip) {
            body = new FormData();
            body.append('package', new Blob([Uint8Array.from(atob(payload.zip), c => c.charCodeAt(0))],
                { type: 'application/zip' }), payload.name);
        } else if (payload !== undefined) body = JSON.stringify(payload);
        let options = { method, credentials: 'same-origin', ...(body === undefined ? {} : { body }),
            ...(body && !(body instanceof FormData) ? { headers: { 'Content-Type': 'application/json' } } : {}) };
        if (csrf) options = sdk.withCsrf(path, options);
        const response = await fetch(path, options), text = await response.text();
        let value = null;
        if (text) { try { value = JSON.parse(text); } catch { value = { invalidJson: true }; } }
        return { status: response.status, value, profile: response.headers.get('X-Plinth-Platform-Profile') };
    }, { path, method, payload, csrf });
}
function observePrivateRealtime(page) {
    if (privateRealtimeClosed || privateRealtime.size >= 8) return;
    const terminals = { auth_failed: 0, auth_timeout: 0, already_connected: 0,
        not_authenticated: 0, session_expired: 0, session_revoked: 0 };
    const state = { sockets: 0, closed: 0, frames: 0, application_acks: 0, overflow: false };
    const sockets = new Map();
    const increment = key => { state[key] = Math.min(64, state[key] + 1); };
    const onSocket = socket => {
        if (sockets.size === 16) { state.overflow = true; return; }
        increment('sockets');
        const received = event => {
            try {
                const payload = event.payload;
                if (typeof payload !== 'string' || payload.length > 2048 || Buffer.byteLength(payload, 'utf8') > 2048) return;
                increment('frames');
                const value = JSON.parse(payload);
                if (!value || typeof value !== 'object' || Array.isArray(value)) return;
                const keys = Object.keys(value).sort().join(',');
                if (keys === 'error,message,type' && value.type === 'error' &&
                    typeof value.message === 'string' && typeof value.error === 'string' &&
                    Object.hasOwn(terminals, value.error)) {
                    terminals[value.error] = Math.min(64, terminals[value.error] + 1);
                } else if (keys === 'channels,recommended_debounce_ms,recommended_jitter_ms,type' &&
                    value.type === 'subscribed' && Array.isArray(value.channels) && value.channels.length <= 64 &&
                    value.channels.every(channel => typeof channel === 'string' && channel.length <= 256) &&
                    new Set(value.channels).size === value.channels.length &&
                    ['recommended_debounce_ms', 'recommended_jitter_ms'].every(key =>
                        Number.isSafeInteger(value[key]) && value[key] >= 0) &&
                    value.channels.includes('plinth:system:applications.changed')) increment('application_acks');
            } catch { /* Unknown/malformed frames never become retained payloads. */ }
        };
        const closed = () => increment('closed');
        socket.on('framereceived', received); socket.on('close', closed);
        sockets.set(socket, { received, closed });
    };
    page.on('websocket', onSocket);
    privateRealtime.set(page, Object.freeze({ snapshot: () => ({ ...state, terminals: { ...terminals } }), stop() {
        page.off('websocket', onSocket);
        for (const [socket, handlers] of sockets) {
            socket.off('framereceived', handlers.received); socket.off('close', handlers.closed);
        }
        sockets.clear();
    } }));
}
async function login(account, { observeReads = false } = {}) {
    const context = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
    contexts.push(context);
    const page = await context.newPage();
    observePrivateRealtime(page);
    if (!privateRealtimeClosed) {
        assert(authorityRecoveries.size < 8, 'bounded verification recovery observers');
        authorityRecoveries.set(page, observeAuthorityRecovery(page, origin));
    }
    if (observeReads) readLanes.set(page, observeReadLane(page, origin));
    page.setDefaultTimeout(15000);
    page.on('dialog', dialog => dialog.accept());
    await page.goto(base + '/app/');
    await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
    await page.locator('input[name=username]').fill(account.username);
    await page.locator('input[name=password]').fill(account.password);
    const response = page.waitForResponse(item => new URL(item.url()).pathname === '/api/auth/login');
    await page.getByRole('button', { name: 'Sign In', exact: true }).click();
    assert.equal((await response).status(), 200);
    await page.locator('.launcher-navigation').waitFor();
    const session = await request(page, '/api/auth/session');
    assert.equal(session.value.user.id, account.id);
    return page;
}
function packageRow(name) {
    const rows = jsonSql(`SELECT coalesce(json_agg(p),'[]'::json)::text FROM
        (SELECT id,name,version,state,provenance,installed_by_user_id,application_ready FROM plinth.packages
         WHERE name=${literal(name)} AND state IN ('ACTIVE','ACTIVE_FLAGGED','DISABLED')) p`);
    assert(rows.length <= 1, 'one authoritative installed fixture generation');
    return rows[0] ?? null;
}
function saved(account) {
    const value = sql(`SELECT value::text FROM ext_shell.user_preferences
        WHERE user_id=${literal(account.id)} AND key='shell.floats'`);
    return value ? JSON.parse(value) : null;
}
async function surfaces(page) {
    const result = await request(page, '/api/frontend/surfaces?kind=float&limit=64');
    assert.equal(result.status, 200, 'actual typed surface endpoint available');
    const profile = await page.locator('meta[name=plinth-platform-profile]').getAttribute('content');
    assert.match(profile, /^v1:[0-9a-f-]{36}:[0-9a-f]{64}$/);
    assert.equal(result.profile, profile, 'captured document/authority operation agree');
    assert.equal(result.value.schema_version, 1);
    assert.equal(result.value.kind, 'float');
    return result.value.targets.filter(target => target.application_id === 'floatprovider');
}
function remaining(deadline) {
    const value = Math.ceil(deadline - performance.now());
    assert(value > 0, 'owned native UI deadline expired');
    return value;
}
async function withinDeadline(promise, deadline) {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error('owned native UI deadline expired')), remaining(deadline));
        })]);
    } finally { clearTimeout(timer); }
}
async function openCaller(page, deadline = performance.now() + 15000) {
    if (await page.locator('#installed-float-open').isVisible()) return;
    await page.getByRole('button', { name: 'Home', exact: true }).click({ timeout: remaining(deadline) });
    await page.locator('.launcher-grid .launcher-tile[aria-label="Float Caller"]').click({ timeout: remaining(deadline) });
    await page.locator('#installed-float-open').waitFor({ timeout: remaining(deadline) });
}
async function open(page, record, live = false, label = 'Captured relative fixture import') {
    await page.locator('#installed-float-record').fill(record);
    await page.locator(live ? '#installed-float-open-live' : '#installed-float-open').click();
    await page.waitForFunction(record => {
        const result = document.querySelector('#installed-float-open-result')?.textContent;
        if (!result?.startsWith('opened:')) return false;
        const element = document.querySelector(`.float-window[data-float-id="${result.slice(7)}"] .installed-float-provider`);
        return element?.dataset.recordId === record;
    }, record);
    const result = await page.locator('#installed-float-open-result').textContent();
    const id = result.slice('opened:'.length);
    const provider = page.locator(`.float-window[data-float-id="${id}"] .installed-float-provider`);
    await provider.waitFor();
    assert.equal(await provider.getAttribute('data-record-id'), record);
    assert.equal(await provider.getAttribute('data-context-stable'), 'true');
    assert.equal(await provider.locator('.installed-float-relative').textContent(), label);
    await page.waitForFunction(id => document.querySelector(`.float-window[data-float-id="${id}"] .installed-float-dynamic`)?.textContent === 'Captured dynamic fixture import', id);
    return { id, provider };
}
async function save(page, account, expected) {
    const response = page.waitForResponse(item => new URL(item.url()).pathname === '/api/frontend/float-layout' && item.request().method() === 'PUT');
    await page.getByRole('button', { name: 'Save panel layout', exact: true }).click();
    const observed = await response;
    assert.equal(observed.status(), 200);
    assert.deepEqual(await observed.json(), { schema_version: 1, saved: true });
    await page.waitForFunction(() => !document.querySelector('.float-storage-status button:last-child')?.disabled);
    const snapshot = saved(account);
    assert.deepEqual(Object.keys(snapshot).sort(), ['floats', 'version']);
    assert.equal(snapshot.version, 1);
    assert.deepEqual(snapshot.floats.map(item => item.context.record_id).sort(), [...expected].sort());
    for (const item of snapshot.floats) {
        assert.deepEqual(Object.keys(item).sort(), ['application_id', 'capability', 'context', 'geometry', 'maximized', 'panel_id', 'presentation']);
        assert.equal(item.application_id, 'floatprovider');
        assert.equal(item.panel_id, 'preview');
        assert.deepEqual(Object.keys(item.context), ['record_id']);
    }
    return snapshot;
}

const admissionPath = '/api/frontend/surfaces/admission';
function actualAdmission(response) {
    return new URL(response.url()).pathname === admissionPath && response.request().method() === 'POST';
}
async function pinned(page, target) {
    return request(page, admissionPath, 'POST', { schema_version: 1, targets: [pinnedTuple(target)] });
}
function pinnedTuple(target) {
    return { application_id: target.application_id, panel_id: target.panel_id, kind: 'float',
        expected_generation: target.generation, capability: target.capability };
}
function expectedWatch(nativeRequest, targets) {
    const url = new URL(nativeRequest.url());
    if (url.origin !== origin || url.pathname !== admissionPath || nativeRequest.method() !== 'POST') return null;
    const data = nativeRequest.postData();
    if (!data || data.length > 32768) return null;
    let value;
    try { value = JSON.parse(data); } catch { return null; }
    if (!value || Object.keys(value).sort().join(',') !== 'schema_version,targets' || value.schema_version !== 1 ||
        !Array.isArray(value.targets) || value.targets.length < 1 || value.targets.length > targets.length) return null;
    const expected = new Set(targets.map(target => JSON.stringify(Object.values(pinnedTuple(target))))), seen = new Set();
    for (const item of value.targets) {
        if (!item || Object.keys(item).sort().join(',') !== 'application_id,capability,expected_generation,kind,panel_id') return null;
        const identity = JSON.stringify([item.application_id, item.panel_id, item.kind, item.expected_generation, item.capability]);
        if (!expected.has(identity) || seen.has(identity)) return null;
        seen.add(identity);
    }
    // A genuine hint may already have retired one panel. The remaining watch
    // must still be a nonempty unique SUBSET of the explicitly expected tuples.
    return value.targets;
}
function observeFreshResponse(page, select, barrier, deadline) {
    const requests = new Map(), diagnostics = { requests: 0, responses: [] };
    let resolve, reject, selectedFresh = false, settled = false, closed = false, outcome = null, bodyObservation;
    const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
    // A click can fail before its observer is awaited; retain a handled promise.
    promise.catch(() => {});
    const fail = (error = new Error('native request identity/timing observation failed: ' + JSON.stringify(diagnostics))) => {
        if (settled || closed) return;
        settled = true;
        outcome = { error: true, cause: error };
        reject(error);
    };
    const onRequest = nativeRequest => {
        if (closed || settled || selectedFresh) return;
        const selected = select(nativeRequest);
        if (!selected) return;
        if (diagnostics.requests === 64) { fail(); return; }
        diagnostics.requests++;
        requests.set(nativeRequest, selected);
    };
    const onResponse = response => {
        if (closed || settled || selectedFresh) return;
        const nativeRequest = response.request(), selected = requests.get(nativeRequest);
        if (!selected) return;
        requests.delete(nativeRequest);
        // Playwright populates Chromium startTime at response time, not at the
        // request event. Queued pre-mutation protocol events are not freshness.
        const start = nativeRequest.timing().startTime;
        const relation = !Number.isFinite(start) || start <= 0 ? 'missing' : start > barrier ? 'fresh' : 'earlier';
        if (diagnostics.responses.length < 16) diagnostics.responses.push({
            relation, status: response.status(), panels: selected.map(item => item.panel_id) });
        if (relation === 'missing') { fail(); return; }
        if (relation !== 'fresh') return;
        let body;
        try { body = responseBodies.capture(response); }
        catch (error) { fail(error); return; }
        // Passive diagnostics only. No new Network domain, cache or interception.
        try {
            bodyObservation = observeSelectedBody(page, response);
            bodyObservationTasks.add(bodyObservation.finished);
        } catch { /* A diagnostic failure cannot replace actual body validation. */ }
        selectedFresh = true;
        outcome = { response, selected, body };
        body.settlement.then(result => {
            if (closed || settled) return;
            if (!result.ok) { bodyObservation?.reject(result.error); fail(result.error); return; }
            settled = true;
            resolve(outcome);
        }).catch(fail);
    };
    const timer = setTimeout(fail, remaining(deadline));
    page.on('request', onRequest);
    page.on('response', onResponse);
    return { promise, diagnostics, peek: () => outcome, stop: () => {
        closed = true;
        clearTimeout(timer);
        page.off('request', onRequest);
        page.off('response', onResponse);
        bodyObservation?.stop();
        requests.clear();
    } };
}
const fixtureContentType = 'application/x-plinth-float-fixture';
function expectedLogicalPreview(nativeRequest, target) {
    const url = new URL(nativeRequest.url());
    if (url.origin !== origin || url.pathname !== admissionPath || nativeRequest.method() !== 'POST') return null;
    const data = nativeRequest.postData();
    if (!data || data.length > 32768) return null;
    let value;
    try { value = JSON.parse(data); } catch { return null; }
    const expected = { application_id: target.application_id, panel_id: target.panel_id,
        kind: 'float', expected_generation: null, content_type: fixtureContentType };
    if (!value || Object.keys(value).sort().join(',') !== 'schema_version,targets' || value.schema_version !== 1 ||
        !Array.isArray(value.targets) || value.targets.length !== 1) return null;
    const item = value.targets[0];
    if (!item || Object.keys(item).sort().join(',') !== Object.keys(expected).sort().join(',') ||
        Object.entries(expected).some(([key, field]) => item[key] !== field)) return null;
    return [item];
}
function expectedResolverCatalog(nativeRequest) {
    const url = new URL(nativeRequest.url());
    const expected = { kind: 'float', limit: '1', include_admin_default: '1', content_type: fixtureContentType };
    return url.origin === origin && nativeRequest.method() === 'GET' && url.pathname === '/api/frontend/surfaces' &&
        [...url.searchParams].length === 4 &&
        Object.entries(expected).every(([key, value]) => url.searchParams.get(key) === value) ? [] : null;
}
async function observedDefaultOpen(page, record, target, branch) {
    const deadline = performance.now() + 15000;
    // The caller samples this browser barrier after the preference ACK or
    // exact fixture-row mutation. Pre-mutation responses cannot prove a tier.
    const barrier = await withinDeadline(page.evaluate(() => performance.timeOrigin + performance.now()), deadline);
    let logical = null, catalog = null;
    const catalogRequests = [];
    const onRequest = nativeRequest => {
        const url = new URL(nativeRequest.url());
        if (url.origin === origin && url.pathname === '/api/frontend/surfaces' && catalogRequests.length < 65) {
            catalogRequests.push(nativeRequest);
        }
    };
    page.on('request', onRequest);
    try {
        if (branch !== 'fallback') logical = observeFreshResponse(page,
            nativeRequest => expectedLogicalPreview(nativeRequest, target), barrier, deadline);
        if (branch !== 'personal') catalog = observeFreshResponse(page, expectedResolverCatalog, barrier, deadline);
        const opened = await withinDeadline(open(page, record), deadline);
        if (logical) {
            const observed = await logical.promise;
            assert.equal(observed.response.status(), 200, 'fresh logical default admission HTTP status');
            assert.deepEqual(await withinDeadline(observed.body.json(), deadline), {
                schema_version: 1, results: [{ index: 0, status: 'available', target }],
            }, 'actual default tier resolves the exact authorized installed preview');
        }
        if (catalog) {
            assert.equal(catalogRequests.length, 1, 'ordinary default resolution makes one catalog request');
            const observed = await catalog.promise;
            assert.equal(observed.response.status(), 200, 'fresh automatic resolver catalog HTTP status');
            const value = await withinDeadline(observed.body.json(), deadline);
            assert.deepEqual(value, { schema_version: 1, kind: 'float', targets: [target], next_cursor: null,
                admin_default_candidate: branch === 'admin'
                    ? { application_id: target.application_id, panel_id: target.panel_id, generation: target.generation } : null },
            'actual catalog distinguishes administrator candidate from affirmative earliest fallback');
        } else assert.equal(catalogRequests.length, 0, 'usable personal default sends no catalog or administrator lookup');
        return opened;
    } finally {
        logical?.stop(); catalog?.stop(); page.off('request', onRequest);
        catalogRequests.length = 0;
    }
}
async function closeDefaultFixture(page, opened) {
    const deadline = performance.now() + 15000;
    const instance = await opened.provider.getAttribute('data-instance', { timeout: remaining(deadline) });
    assert.match(instance, uuid);
    const window = page.locator(`.float-window[data-float-id="${opened.id}"]`);
    await window.getByRole('button', { name: 'Close', exact: true }).click({ timeout: remaining(deadline) });
    await window.waitFor({ state: 'detached', timeout: remaining(deadline) });
    await retiredInstances(page, [instance], deadline);
}
function requireUnavailableValue(value, count, diagnostics) {
    const results = Array.isArray(value?.results) ? value.results : [];
    const statuses = results.slice(0, 38).map(item => ({
        index: Number.isSafeInteger(item?.index) ? item.index : -1,
        status: ['available', 'unavailable', 'stale'].includes(item?.status) ? item.status : 'invalid' }));
    assert(value?.schema_version === 1 && Object.keys(value).sort().join(',') === 'results,schema_version' && results.length === count &&
        results.every((item, index) => item && Object.keys(item).sort().join(',') === 'index,status' &&
            item.index === index && item.status === 'unavailable'),
    'fresh native admission must be complete/indexed/all unavailable: ' + JSON.stringify({ ...diagnostics, statuses }));
}
async function requireUnavailable(observed, deadline, diagnostics) {
    assert.equal(observed.response.status(), 200, 'fresh native admission HTTP status');
    requireUnavailableValue(await withinDeadline(observed.body.json(), deadline), observed.selected.length, diagnostics);
}
function targetArtifactDigest(target) {
    // The closed thirteen-key target has no top-level digest field. The
    // canonical typed namespace carries it; do not invent an API field.
    const digest = new URL(target.module_url, base).pathname.split('/')[4];
    assert.match(digest, /^[0-9a-f]{64}$/);
    return digest;
}
function callerTimeoutObservation() {
    const codes = new Set(['none', 'opened', 'missing-handler', 'authority-unavailable', 'cancelled',
        'invalid-input', 'limit-refused', 'request-busy', 'load-failed', 'target-unavailable',
        'reload-required', 'session-ended', 'unknown']);
    const keys = ['mounts', 'unmounts', 'open_starts', 'resolved', 'rejected', 'suppressed'];
    const bounded = value => Number.isInteger(value) && value >= 0 && value <= 64 ? value : 0;
    const caller = document.querySelector('#installed-float-caller');
    const output = document.querySelector('#installed-float-open-result');
    const launcher = document.querySelector('.launcher-shell');
    const home = document.querySelector('.launcher-home');
    const host = document.querySelector('.panel-host');
    const text = output?.textContent;
    const status = !output ? 'absent' : text === 'idle' || text === 'opening' ? text :
        typeof text === 'string' && text.length <= 128 && text.startsWith('opened:') ? 'opened' :
        typeof text === 'string' && text.length <= 128 && text.startsWith('refused:') ? 'refused' : 'other';
    const raw = globalThis.__plinthInstalledFloats?.callers;
    const instances = Array.isArray(raw?.instances) ? raw.instances.slice(0, 64) : [];
    const id = Number(caller?.getAttribute('data-caller-incarnation'));
    const current = Number.isInteger(id) && id >= 1 && id <= 64 ? instances[id - 1] : null;
    return { schema_version: 1, visibility: ['visible', 'hidden', 'prerender'].includes(document.visibilityState)
        ? document.visibilityState : 'unknown', caller_present: !!caller, output_present: !!output, output: status,
        launcher_present: !!launcher, home_present: !!home, home_hidden: home?.hidden === true,
        primary_host_present: !!host, primary_host_connected: host?.isConnected === true,
        primary_host_hidden: host?.hidden === true, primary_child_count: Math.min(64, host?.childElementCount ?? 0),
        provider_count: Math.min(64, document.querySelectorAll('.installed-float-provider').length),
        instance_count: instances.length, event_count: bounded(raw?.events), overflow: raw?.overflow === true,
        current_is_last_open: current ? id === raw?.lastStarted : null,
        totals: Object.fromEntries(keys.map(key => [key, Math.min(64,
            instances.reduce((total, entry) => total + bounded(entry?.[key]), 0))])),
        current: current ? { mounted: current.mounted === true,
            ...Object.fromEntries(keys.map(key => [key, bounded(current[key])])),
            last_code: codes.has(current.last_code) ? current.last_code : 'unknown' } : null };
}
async function captureFailurePages() {
    const deadline = performance.now() + 2000;
    const pages = [], unavailableContexts = [];
    let overflow = contexts.length > 8;
    for (const [contextIndex, context] of contexts.slice(0, 8).entries()) {
        let owned;
        try { owned = context.pages(); }
        catch { unavailableContexts.push(contextIndex + 1); continue; }
        const remainingPages = 8 - pages.length;
        if (owned.length > remainingPages) overflow = true;
        for (const [pageIndex, page] of owned.slice(0, remainingPages).entries()) {
            const entry = { context_ordinal: contextIndex + 1, page_ordinal: pageIndex + 1,
                state: 'pending', snapshot: null, realtime: privateRealtime.get(page)?.snapshot() ?? null };
            pages.push(entry);
            let pending;
            try { pending = Promise.resolve(page.evaluate(callerTimeoutObservation)); }
            catch (error) { pending = Promise.reject(error); }
            // Both handlers attach immediately; root joins after browser closure
            // even when this one diagnostic deadline expires before settlement.
            const handled = pending.then(snapshot => { entry.state = 'observed'; entry.snapshot = snapshot; },
                () => { entry.state = 'unavailable'; });
            privateSnapshotTasks.add(handled);
        }
    }
    try { await boundedStep(() => Promise.all([...privateSnapshotTasks]), deadline); }
    catch { /* Pending entries remain explicitly pending, not success. */ }
    return { schema_version: 1, context_count: Math.min(8, contexts.length), page_count: pages.length,
        overflow, unavailable_context_ordinals: unavailableContexts,
        refusal_stage: ['initial-caller', 'fill', 'barrier', 'click', 'native-body', 'refusal-wait',
            'refusal-value'].includes(refusedFailureStage) ? refusedFailureStage : null, pages };
}
async function refusedOpen(page, record, deadline = performance.now() + 15000) {
    let stage = 'initial-caller', observer;
    try {
        await openCaller(page, deadline);
        stage = 'fill';
        await page.locator('#installed-float-record').fill(record, { timeout: remaining(deadline) });
        stage = 'barrier';
        const barrier = await withinDeadline(page.evaluate(() => performance.timeOrigin + performance.now()), deadline);
        observer = observeFreshResponse(page, nativeRequest => {
            const url = new URL(nativeRequest.url());
            return url.origin === origin && nativeRequest.method() === 'GET' && url.pathname === '/api/frontend/surfaces' &&
                url.searchParams.get('kind') === 'float' &&
                url.searchParams.get('limit') === '1' && url.searchParams.get('include_admin_default') === '1' &&
                url.searchParams.get('content_type') === 'application/x-plinth-float-fixture' ? [] : null;
        }, barrier, deadline);
        stage = 'click';
        await page.locator('#installed-float-open').click({ timeout: remaining(deadline) });
        stage = 'native-body';
        const observed = await observer.promise;
        assert.equal(observed.response.status(), 200, 'fresh SDK resolver catalog HTTP status');
        const catalog = await withinDeadline(observed.body.json(), deadline);
        assert(catalog?.schema_version === 1 && catalog.kind === 'float' &&
            Object.keys(catalog).sort().join(',') === 'admin_default_candidate,kind,next_cursor,schema_version,targets' &&
            Array.isArray(catalog.targets) && catalog.targets.length === 0 &&
            catalog.next_cursor === null && catalog.admin_default_candidate === null,
        'fresh ordinary SDK resolver must observe complete native absence');
        stage = 'refusal-wait';
        await page.waitForFunction(() => document.querySelector('#installed-float-open-result')?.textContent?.startsWith('refused:'),
            null, { timeout: remaining(deadline) });
        stage = 'refusal-value';
        const result = await page.locator('#installed-float-open-result').textContent({ timeout: remaining(deadline) });
        assert(['refused:missing-handler', 'refused:authority-unavailable'].includes(result),
            'fresh ordinary SDK request must refuse unavailable authority');
    } catch (error) {
        refusedFailureStage = stage;
        throw error;
    } finally { observer?.stop(); }
}
async function revalidateFromUI(page, record, refusedRecord, expectedTargets,
    { deadline = performance.now() + 15000, beforeFallback = null } = {}) {
    // This browser epoch timestamp is sampled only AFTER the mutation ACK.
    const barrier = await withinDeadline(page.evaluate(() => performance.timeOrigin + performance.now()), deadline);
    const observer = observeFreshResponse(page,
        nativeRequest => expectedWatch(nativeRequest, expectedTargets), barrier, deadline);
    try {
        const provider = page.locator(`.installed-float-provider[data-record-id="${record}"]`);
        const action = provider.getByRole('button', { name: 'Request fixture focus', exact: true });
        const retired = page.waitForFunction(() => document.querySelectorAll('.installed-float-provider').length === 0,
            null, { timeout: remaining(deadline) }).then(async handle => {
            await handle.dispose();
            return { retired: true };
        }).catch(() => ({ expired: true }));
        let fallback = await provider.count() === 0;
        if (!fallback) {
            // One conservative ordinary actionability attempt, not a promise
            // that native teardown finishes within1s. Never force or retry it.
            try { await action.click({ timeout: Math.min(1000, remaining(deadline)) }); }
            catch (error) {
                if (await provider.count() !== 0 || await action.count() !== 0) throw error;
                fallback = true;
            }
        }
        if (!fallback) {
            const outcome = await Promise.race([observer.promise, retired]);
            assert(!outcome.expired, 'native owner retirement deadline expired');
            const observed = outcome.response ? outcome : observer.peek();
            if (observed?.error) throw observed.cause;
            if (observed?.response) await requireUnavailable(observed, deadline, observer.diagnostics);
            else fallback = true;
        }
        if (fallback) {
            // No forced click or synthetic hint: a genuinely retired owner uses
            // one explicit ordinary SDK intent, with its own fresh Request.
            const observed = observer.peek();
            if (observed?.error) throw observed.cause;
            if (observed?.response) await requireUnavailable(observed, deadline, observer.diagnostics);
            observer.stop();
            if (beforeFallback) await beforeFallback();
            await refusedOpen(page, refusedRecord, deadline);
        }
        assert((await retired).retired, 'all old native provider owners physically retired');
    } finally { observer.stop(); }
}
async function retiredInstances(page, instances, deadline = performance.now() + 15000) {
    assert(instances.length > 0);
    await page.waitForFunction(instances => instances.every(instance => {
        const witness = globalThis.__plinthInstalledFloats;
        const value = witness?.incarnations?.get(instance);
        return value?.unmounts === 1 && value.deactivations === 1 && !witness.active.has(instance);
    }), instances, { timeout: remaining(deadline) });
    const observed = await page.evaluate(instances => instances.map(instance => ({
        ...globalThis.__plinthInstalledFloats.incarnations.get(instance),
        active: globalThis.__plinthInstalledFloats.active.has(instance),
        unmounted: globalThis.__plinthInstalledFloats.unmounted.filter(value => value === instance).length,
    })), instances);
    for (const value of observed) assert.deepEqual(value,
        { activations: 1, deactivations: 1, mounts: 1, unmounts: 1, active: false, unmounted: 1 });
}
async function uploadVariant(operator, version) {
    const name = `floatprovider-${version}`;
    const bytes = await readFile(join(fixtureDir, `${name}.zip`));
    state.archiveHashes[name] = sha(bytes);
    return request(operator, '/api/packages', 'POST', { zip: bytes.toString('base64'), name: `${name}.zip` });
}

try {
    browser = await chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] });
    if (phase === 'install') {
        const label = randomUUID().replaceAll('-', '');
        state = { accounts: {}, packages: {}, groups: [], archiveHashes: {} };
        for (const role of ['operator', 'a', 'b', 'denied']) {
            const account = { username: `float46_${role}_${label}`, password: fakePassword };
            const result = await publicPost(role === 'operator' ? '/api/auth/bootstrap' : '/api/auth/register',
                { username: account.username, password: account.password,
                    ...(role === 'operator' ? { bootstrap_token: process.env.PLINTH_TEST_BOOTSTRAP_TOKEN } : {}) });
            assert.equal(result.status, role === 'operator' ? 201 : 202);
            account.id = role === 'operator' ? result.body.id : sql(`SELECT id FROM plinth.users WHERE username=${literal(account.username)}`);
            assert.match(account.id, uuid);
            state.accounts[role] = account;
        }
        const operator = await login(state.accounts.operator);
        for (const name of ['floatcaller', 'floatprovider']) {
            assert.equal(packageRow(name), null, 'never replace a pre-existing package');
            const bytes = await readFile(join(fixtureDir, `${name}.zip`));
            state.archiveHashes[name] = sha(bytes);
            const result = await request(operator, '/api/packages', 'POST', { zip: bytes.toString('base64'), name: `${name}.zip` });
            if (result.status !== 201) {
                let report = null;
                if (typeof result.value?.id === 'string' && uuid.test(result.value.id)) {
                    try {
                        // The ordinary POST response omits its persisted
                        // validation report. Read only this failed fixture row,
                        // with a finite text budget before materialization.
                        report = jsonSql(`SELECT CASE WHEN octet_length(last_install_report::text)<=32768
                            THEN last_install_report ELSE NULL END::text FROM plinth.packages
                            WHERE id=${literal(result.value.id)} AND name=${literal(name)}
                            AND installed_by_user_id=${literal(state.accounts.operator.id)}`);
                    } catch { /* Diagnostic lookup must not replace the actual HTTP failure. */ }
                }
                assert.equal(result.status, 201, 'ordinary authenticated ZIP install: ' +
                    installFailureSummary(name, result, report));
            }
            const row = packageRow(name);
            assert.equal(row.id, result.value.id);
            assert.equal(row.provenance, 'user');
            assert.equal(row.installed_by_user_id, state.accounts.operator.id);
            assert.equal(row.state, 'ACTIVE');
            state.packages[name] = row;
        }
        for (const rule of fixtureRules) {
            assert.equal(sql(`SELECT count(*) FROM plinth.group_rules g JOIN plinth.rbac_rules r ON r.id=g.rule_id WHERE r.rule=${literal(rule)}`), '0', 'no package default permission grants');
        }
        for (const role of ['a', 'b']) {
            const group = randomUUID(); state.groups.push(group);
            sql(`INSERT INTO plinth.groups(id,name,description) VALUES (${literal(group)},${literal(`float46_${role}_${label}`)},'owned installed float fixture');
                INSERT INTO plinth.group_members(group_id,user_id) VALUES (${literal(group)},${literal(state.accounts[role].id)});`);
            for (const rule of fixtureRules) {
                assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule=${literal(rule)} AND orphaned_at IS NULL`), '1');
                sql(`INSERT INTO plinth.group_rules(group_id,rule_id) SELECT ${literal(group)},id FROM plinth.rbac_rules WHERE rule=${literal(rule)}`);
            }
        }
        for (const role of ['a', 'b', 'denied']) assert.equal(sql(`SELECT count(*) FROM plinth.group_members m JOIN plinth.group_rules g ON g.group_id=m.group_id JOIN plinth.rbac_rules r ON r.id=g.rule_id WHERE m.user_id=${literal(state.accounts[role].id)} AND r.rule='kernel.admin'`), '0', 'consumer is not administrator');
        // The documented ordinary surface lookup may capture on demand. Never
        // insert/backfill a receipt just to make the reviewed policy match.
        const beforePolicy = await surfaces(await login(state.accounts.a));
        assert.equal(beforePolicy.length, 2);
        assert(beforePolicy.every(target => target.persistence_contract === null));
        const receipt = jsonSql(`SELECT row_to_json(r)::text FROM plinth.package_artifact_receipts r WHERE package_id=${literal(state.packages.floatprovider.id)} AND profile=1`);
        assert.match(receipt.artifact_digest, /^[0-9a-f]{64}$/);
        state.receipt = receipt;
        await writeFile(statePath, JSON.stringify(state), { mode: 0o600, flag: 'wx' });
        console.log(JSON.stringify({ phase, installed: 2, actualAuth: true, defaultFixtureGrants: 0 }));
    } else {
        state = JSON.parse(await readFile(statePath, 'utf8'));
        const a = await login(state.accounts.a), b = await login(state.accounts.b, { observeReads: true }), denied = await login(state.accounts.denied);
        const targets = await surfaces(a);
        assert.equal(targets.length, 2);
        const preview = targets.find(item => item.panel_id === 'preview');
        const live = targets.find(item => item.panel_id === 'live');
        assert(preview.persistence_contract, 'real reviewed protected policy is eligible');
        assert.deepEqual(preview.persistence_contract.approved_keys, ['record_id']);
        assert.equal(preview.persistence_contract.artifact_digest, state.receipt.artifact_digest);
        assert.equal(live.persistence_contract, null, 'required unapproved reopen field is live-only');
        assert.deepEqual(await surfaces(denied), [], 'authorization filters before exposure');
        completed.push('real protected policy, captured profile, authorized/denied surface filtering');
        const originalTypedTargets = await snapshotTypedSurfaceTargets(b, preview.generation);
        await failureSteps.run('initial-caller', () => openCaller(a));
        const personalWhere = `user_id=${literal(state.accounts.a.id)} AND key='shell.handler_defaults'`;
        const adminWhere = `content_type=${literal(fixtureContentType)}`;
        failureSteps.run('default-state', () => {
            assert.equal(sql(`SELECT count(*) FROM ext_shell.user_preferences WHERE ${personalWhere}`), '0');
            assert.equal(sql(`SELECT count(*) FROM ext_shell.default_apps WHERE ${adminWhere}`), '0');
        });
        const personalDefault = { version: 1, choices: [{ content_type: fixtureContentType,
            application_id: 'floatprovider', panel_id: 'preview' }] };
        await failureSteps.run('personal-write', async () => {
            const personalSaved = await request(a, '/api/cap/shell.preferences.set', 'POST', {
                args: { key: 'shell.handler_defaults', value: personalDefault },
            });
            assert.equal(personalSaved.status, 200);
            assert.equal(personalSaved.value.ok, true);
            assert.deepEqual(personalSaved.value.value, { ok: true });
        });
        try {
            failureSteps.run('personal-write', () => {
                assert.deepEqual(jsonSql(`SELECT value::text FROM ext_shell.user_preferences WHERE ${personalWhere}`), personalDefault);
                assert.equal(sql(`SELECT count(*) FROM ext_shell.user_preferences WHERE user_id=${literal(state.accounts.b.id)}
                    AND key='shell.handler_defaults'`), '0', 'personal preference is owned by A only');
            });
            const opened = await failureSteps.run('personal-open', () => observedDefaultOpen(a, 'fixture-personal-default', preview, 'personal'));
            await failureSteps.run('personal-close', () => closeDefaultFixture(a, opened));
        } finally {
            await failureSteps.run('personal-delete', async () => {
                const deleted = await request(a, '/api/cap/shell.preferences.set', 'POST', { args: { key: 'shell.handler_defaults' } });
                assert.equal(deleted.status, 200);
                assert.equal(deleted.value.ok, true);
                assert.deepEqual(deleted.value.value, { ok: true, deleted: true });
                assert.equal(sql(`SELECT count(*) FROM ext_shell.user_preferences WHERE ${personalWhere}`), '0');
            });
        }
        // The administrator manager UI/API is not implemented. Seed exactly
        // this owned preference row, not a grant or a claimed HTTP admin write.
        failureSteps.run('admin-seed', () => {
            assert.equal(sql(`WITH inserted AS (INSERT INTO ext_shell.default_apps(content_type,application_id,panel_id)
                VALUES (${literal(fixtureContentType)},'floatprovider','preview') RETURNING 1)
                SELECT count(*) FROM inserted`), '1');
        });
        try {
            const opened = await failureSteps.run('admin-open', () => observedDefaultOpen(a, 'fixture-admin-default', preview, 'admin'));
            await failureSteps.run('admin-close', () => closeDefaultFixture(a, opened));
        } finally {
            failureSteps.run('admin-delete', () => {
                assert.equal(sql(`WITH removed AS (DELETE FROM ext_shell.default_apps WHERE ${adminWhere}
                    AND application_id='floatprovider' AND panel_id='preview' RETURNING 1)
                    SELECT count(*) FROM removed`), '1');
                assert.equal(sql(`SELECT count(*) FROM ext_shell.default_apps WHERE ${adminWhere}`), '0');
            });
        }
        const first = await failureSteps.run('fallback-open', () => observedDefaultOpen(a, 'fixture-record-A', preview, 'fallback'));
        const instance = await failureSteps.run('fallback-focus', async () => {
            const value = await first.provider.getAttribute('data-instance');
            assert.match(value, uuid);
            await first.provider.locator('.installed-float-unsaved').fill('fake unsaved text must never persist');
            await first.provider.getByRole('button', { name: 'Request fixture focus', exact: true }).click();
            return value;
        });
        await failureSteps.run('fallback-modules', async () => {
            const singleton = await a.evaluate(() => ({ sdk: __plinthInstalledFloats.sdk.size,
                preact: __plinthInstalledFloats.preact.size, hooks: __plinthInstalledFloats.hooks.size }));
            assert.deepEqual(singleton, { sdk: 1, preact: 1, hooks: 1 });
            const modules = await a.evaluate(() => performance.getEntriesByType('resource').map(item => item.name).filter(name => name.includes('/_plinth/typed/')));
            const prefix = new URL(preview.module_url, base).pathname.replace(/[^/]+$/, '');
            for (const filename of ['preview.js', 'label.js', 'detail.js']) assert(modules.some(name => new URL(name).pathname === prefix + filename), 'actual native captured module graph');
        });
        completed.push('ordinary caller SDK openFloat, ready ID, native relative/dynamic imports, shared modules');
        await save(a, state.accounts.a, ['fixture-record-A']);
        assert.equal(saved(state.accounts.b), null, 'no cross-user descriptor');
        await a.reload();
        const restored = a.locator('.installed-float-provider[data-record-id="fixture-record-A"]');
        await restored.waitFor();
        assert.notEqual(await restored.getAttribute('data-instance'), instance);
        assert.equal(await restored.locator('.installed-float-unsaved').inputValue(), '');
        completed.push('guarded SQL minimal descriptor and reload new instance without unsaved state');
        await openCaller(b);
        await open(b, 'fixture-record-B');
        await save(b, state.accounts.b, ['fixture-record-B']);
        assert.equal(saved(state.accounts.a).floats[0].context.record_id, 'fixture-record-A');
        await a.context().close();
        const newA = await login(state.accounts.a);
        await newA.locator('.installed-float-provider[data-record-id="fixture-record-A"]').waitFor();
        assert.equal(await newA.locator('.installed-float-provider[data-record-id="fixture-record-B"]').count(), 0);
        completed.push('separate user save and fresh authenticated-session restore isolation');
        await openCaller(newA);
        await open(newA, 'fixture-live-only', true);
        await save(newA, state.accounts.a, ['fixture-record-A']);
        completed.push('required transient field stays live-only and is excluded from storage');
        const before = saved(state.accounts.a);
        for (const args of [{ key: 'shell.floats', value: { version: 1, floats: [] } }, { key: 'shell.floats' }]) {
            const result = await request(newA, '/api/cap/shell.preferences.set', 'POST', { args });
            assert(result.status !== 200 || result.value?.ok === false, 'generic set/delete must refuse guarded key');
            assert.deepEqual(saved(state.accounts.a), before);
        }
        assert.equal((await request(newA, '/api/frontend/float-layout', 'PUT', { schema_version: 1, layout: { version: 1, floats: [] }, bindings: [] }, false)).status, 403);
        assert.deepEqual(saved(state.accounts.a), before);
        const admission = await request(denied, '/api/frontend/surfaces/admission', 'POST', { schema_version: 1,
            targets: [{ application_id: 'floatprovider', panel_id: 'preview', kind: 'float',
                expected_generation: preview.generation, capability: preview.capability }] });
        assert.equal(admission.status, 200);
        assert.deepEqual(admission.value.results, [{ index: 0, status: 'unavailable' }]);
        const direct = await request(denied, new URL(preview.module_url, base).pathname);
        assert([403, 404].includes(direct.status), 'native direct module access rechecks authority');
        const modulePath = new URL(preview.module_url, base).pathname;
        assert.equal((await request(newA, modulePath + '?x=1')).status, 404,
            'real HTTP parser separates query; authorized typed assets still refuse it');
        const rangeStatus = await newA.evaluate(async path => {
            const response = await fetch(path, { credentials: 'same-origin', headers: { Range: 'bytes=0-1' } });
            await response.text();
            return response.status;
        }, modulePath);
        assert.equal(rangeStatus, 416, 'real typed asset HTTP request refuses Range');
        completed.push('actual CSRF, generic write/delete guard, denied pinned admission and module access');

        // Additive installed-owner matrix. The original six groups above and
        // final normal-uninstall group remain required, not replaced by these.
        const bOriginal = b.locator('.installed-float-provider[data-record-id="fixture-record-B"]');
        const bOriginalInstance = await bOriginal.getAttribute('data-instance');
        const crashed = await open(b, 'fixture-render-recovery');
        const crashedInstance = await crashed.provider.getAttribute('data-instance');
        const crashWindow = b.locator(`.float-window[data-float-id="${crashed.id}"]`);
        await crashed.provider.locator('.installed-float-unsaved').fill('fake lost component state');
        await crashed.provider.getByRole('button', { name: 'Crash fixture render', exact: true }).click();
        const retry = crashWindow.getByRole('button', { name: 'Retry', exact: true });
        await retry.waitFor();
        await b.waitForFunction(id => {
            const button = document.querySelector(`.float-window[data-float-id="${id}"] .float-status button`);
            return button && !button.disabled;
        }, crashed.id);
        await retiredInstances(b, [crashedInstance]);
        assert.equal(await crashWindow.locator('.installed-float-provider').count(), 0);
        assert.equal(await bOriginal.getAttribute('data-instance'), bOriginalInstance);
        await bOriginal.locator('.installed-float-unsaved').fill('unaffected other component');
        await bOriginal.locator('.installed-float-unsaved').fill('');
        assert.equal(await b.locator('#installed-float-open').isVisible(), true, 'caller primary remains usable');
        // The unaffected-owner input legitimately raised its window over the
        // failed one. Use ordinary chrome to focus the failed owner first;
        // settle that distinct admission before observing the Retry request.
        const failedFocus = b.locator('.float-list button').filter({ hasText: '(failed)' });
        assert.equal(await failedFocus.count(), 1, 'one failed owner in the ordinary panel list');
        const focusedAdmission = b.waitForResponse(actualAdmission);
        await failedFocus.click();
        const focusedResponse = await focusedAdmission;
        assert.equal(focusedResponse.status(), 200);
        const focusedValue = await focusedResponse.json();
        assert(focusedValue.results.length > 0);
        assert(focusedValue.results.every(result => result.status === 'available'));
        await retry.click({ trial: true });
        const factoriesBeforeRetry = await b.evaluate(() => __plinthInstalledFloats.factories);
        const retriedAdmission = b.waitForResponse(actualAdmission);
        await retry.click();
        assert.equal((await retriedAdmission).status(), 200);
        await crashed.provider.waitFor();
        const recoveredInstance = await crashed.provider.getAttribute('data-instance');
        assert.notEqual(recoveredInstance, crashedInstance);
        assert.equal(await crashed.provider.locator('.installed-float-unsaved').inputValue(), '');
        await b.waitForFunction(instance => __plinthInstalledFloats.incarnations.get(instance)?.activations === 1 &&
            __plinthInstalledFloats.incarnations.get(instance)?.mounts === 1, recoveredInstance);
        assert.equal(await b.evaluate(() => __plinthInstalledFloats.factories), factoriesBeforeRetry + 1);
        await crashWindow.getByRole('button', { name: 'Close', exact: true }).click();
        await crashWindow.waitFor({ state: 'detached' });
        await retiredInstances(b, [recoveredInstance]);
        completed.push('installed render failure and explicit native-admitted Retry isolate component ownership');

        await newA.locator('.installed-float-provider[data-record-id="fixture-record-A"] .installed-float-unsaved')
            .fill('fake dirty owner cannot veto authority retirement');
        const aInstances = await newA.locator('.installed-float-provider').evaluateAll(elements => elements.map(element => element.dataset.instance));
        assert.equal(aInstances.length, 2);
        const revokeDeadline = performance.now() + 15000;
        const revokedRecovery = authorityRecoveries.get(newA).arm(revokeDeadline);
        try {
            const removed = sql(`WITH removed AS (DELETE FROM plinth.group_rules gr USING plinth.rbac_rules r
                WHERE gr.group_id=${literal(state.groups[0])} AND gr.rule_id=r.id
                AND r.rule='floatprovider.preview' AND r.extension_name='floatprovider'
                AND EXISTS(SELECT 1 FROM plinth.group_members gm WHERE gm.group_id=gr.group_id
                    AND gm.user_id=${literal(state.accounts.a.id)}) RETURNING gr.rule_id)
                SELECT count(*) FROM removed`);
            assert.equal(removed, '1', 'revoke only A existing exact owned grant; never re-grant');
            const revoked = await withinDeadline(request(newA, admissionPath, 'POST', {
                schema_version: 1, targets: [pinnedTuple(preview), pinnedTuple(live)] }), revokeDeadline);
            assert.equal(revoked.status, 200, 'independent request issued after committed revocation');
            requireUnavailableValue(revoked.value, 2, { phase: 'independent-after-delete', panels: ['preview', 'live'] });
            await revalidateFromUI(newA, 'fixture-record-A', 'fixture-revoked-A', [preview, live],
                { deadline: revokeDeadline, beforeFallback: () => revokedRecovery.wait() });
            recoveryEvidence.push(await revokedRecovery.wait());
            await retiredInstances(newA, aInstances, revokeDeadline);
            assert.equal(await newA.getByRole('button', { name: 'Discard', exact: true }).count(), 0);
            assert.equal((await request(newA, modulePath)).status, 404);
            const revokedFactories = await newA.evaluate(() => __plinthInstalledFloats.factories);
            await refusedOpen(newA, 'fixture-still-revoked-A', revokeDeadline);
            assert.equal(await newA.evaluate(() => __plinthInstalledFloats.factories), revokedFactories);
        } finally { revokedRecovery.stop(); }
        const permitted = await pinned(b, preview);
        assert.equal(permitted.status, 200);
        assert.equal(permitted.value.results[0].status, 'available');
        const unaffected = await open(b, 'fixture-authorized-B');
        const unaffectedInstance = await unaffected.provider.getAttribute('data-instance');
        const unaffectedWindow = b.locator(`.float-window[data-float-id="${unaffected.id}"]`);
        await unaffectedWindow.getByRole('button', { name: 'Close', exact: true }).click();
        await unaffectedWindow.waitFor({ state: 'detached' });
        await retiredInstances(b, [unaffectedInstance]);
        completed.push('fresh capability revocation retires only the affected authenticated user');

        const disableDeadline = performance.now() + 15000;
        const operator = await boundedStep(() => login(state.accounts.operator, { observeReads: true }), disableDeadline);
        const originalPackage = state.packages.floatprovider;
        await operator.locator('.launcher-main[aria-busy="false"]').waitFor({ timeout: remaining(disableDeadline) });
        await readLanes.get(operator).drain(disableDeadline);
        assert(readLanes.get(operator).catalogCompleted() > 0, 'actual operator catalog body and rendering settled');
        await bOriginal.locator('.installed-float-unsaved').fill('fake dirty before disable', { timeout: remaining(disableDeadline) });
        await readLanes.get(b).drain(disableDeadline);
        // A legitimately suspended background tab keeps its dirty/live owner,
        // but cannot start another periodic/focus read during this one PATCH.
        // This is fixture coordination, not an unfrozen polling stress claim.
        const disabledRecovery = authorityRecoveries.get(b).arm(disableDeadline);
        try {
            const disabled = await withFrozenReadLane(b, readLanes.get(b), disableDeadline, async () => {
                const value = await request(operator, `/api/packages/${originalPackage.id}`, 'PATCH', { action: 'disable' });
                assert.equal(value.status, 200, installFailureSummary('floatprovider', value, null));
                return value;
            });
            assert.equal(disabled.value.state, 'DISABLED');
            assert.equal(packageRow('floatprovider').state, 'DISABLED');
            await revalidateFromUI(b, 'fixture-record-B', 'fixture-disabled-B', [preview],
                { deadline: disableDeadline, beforeFallback: () => disabledRecovery.wait() });
            recoveryEvidence.push(await disabledRecovery.wait());
            await retiredInstances(b, [bOriginalInstance], disableDeadline);
            const disabledAdmission = await pinned(b, preview);
            assert.equal(disabledAdmission.status, 200);
            assert.deepEqual(disabledAdmission.value.results, [{ index: 0, status: 'unavailable' }]);
            assert.equal((await request(b, modulePath)).status, 404);
            await verifyTypedSurfaceUnavailable(b, originalTypedTargets, disableDeadline);
        } finally { disabledRecovery.stop(); }
        const enableDeadline = performance.now() + 15000;
        const enabledRecovery = authorityRecoveries.get(b).arm(enableDeadline);
        let enabledFloat;
        try {
            const enabled = await withinDeadline(request(operator, `/api/packages/${originalPackage.id}`, 'PATCH',
                { action: 'enable' }), enableDeadline);
            assert.equal(enabled.status, 200);
            assert.equal(enabled.value.state, 'ACTIVE');
            assert.equal(packageRow('floatprovider').id, originalPackage.id);
            assert.equal(packageRow('floatprovider').application_ready, true);
            recoveryEvidence.push(await enabledRecovery.wait());
            assert.equal(await b.locator('.installed-float-provider').count(), 0, 'enable alone never resurrects a component');
            await openCaller(b, enableDeadline);
            enabledFloat = await withinDeadline(open(b, 'fixture-enabled-B'), enableDeadline);
        } finally { enabledRecovery.stop(); }
        const enabledInstance = await enabledFloat.provider.getAttribute('data-instance');
        assert.notEqual(enabledInstance, bOriginalInstance);
        assert.equal((await surfaces(b)).find(target => target.panel_id === 'preview').generation, preview.generation);
        assert.deepEqual(await snapshotTypedSurfaceTargets(b, preview.generation), originalTypedTargets);
        completed.push('ordinary disable and enable retire old owners without automatic resurrection');

        const failedUpgrade = await uploadVariant(operator, '0.2.0');
        assert.equal(failedUpgrade.status, 400, installFailureSummary('floatprovider', failedUpgrade, null));
        assert.equal(failedUpgrade.value.kind, 'upgrade-migration-failed');
        assert.match(failedUpgrade.value.id, uuid);
        const attempted = jsonSql(`SELECT row_to_json(p)::text FROM (SELECT id,name,version,state,installed_by_user_id
            FROM plinth.packages WHERE id=${literal(failedUpgrade.value.id)}) p`);
        assert.equal(attempted.name, 'floatprovider');
        assert.equal(attempted.version, '0.2.0');
        assert.equal(attempted.state, 'INSTALL_FAILED');
        assert.equal(attempted.installed_by_user_id, state.accounts.operator.id);
        const fenced = packageRow('floatprovider');
        assert.equal(fenced.id, originalPackage.id);
        assert.equal(fenced.state, 'ACTIVE');
        assert.equal(fenced.application_ready, false, 'migration failure leaves old authority fenced, not rolled back');
        await revalidateFromUI(b, 'fixture-enabled-B', 'fixture-fenced-B', [preview]);
        await retiredInstances(b, [enabledInstance]);
        const fencedAdmission = await pinned(b, preview);
        assert.equal(fencedAdmission.status, 200);
        assert.deepEqual(fencedAdmission.value.results, [{ index: 0, status: 'unavailable' }]);
        assert.equal((await request(b, modulePath)).status, 404);
        state.retiredPackages = [{ id: attempted.id, version: attempted.version, state: 'INSTALL_FAILED' }];
        await verifyTypedSurfaceUnavailable(b, originalTypedTargets);
        completed.push('failed migration upgrade preserves the old generation fence without rollback claims');

        const upgraded = await uploadVariant(operator, '0.3.0');
        assert.equal(upgraded.status, 201, installFailureSummary('floatprovider', upgraded, null));
        const replacement = packageRow('floatprovider');
        assert.equal(replacement.id, upgraded.value.id);
        assert.notEqual(replacement.id, originalPackage.id);
        assert.equal(replacement.version, '0.3.0');
        assert.equal(replacement.application_ready, true);
        assert.equal(replacement.installed_by_user_id, state.accounts.operator.id);
        assert.equal(sql(`SELECT state FROM plinth.packages WHERE id=${literal(originalPackage.id)}`), 'SUPERSEDED');
        state.retiredPackages.push({ id: originalPackage.id, version: originalPackage.version, state: 'SUPERSEDED' });
        state.packages.floatprovider = replacement;
        const replacementTargets = await surfaces(b);
        const replacementTarget = replacementTargets.find(target => target.panel_id === 'preview');
        assert.equal(replacementTarget.generation, replacement.id);
        assert.equal(replacementTarget.version, '0.3.0');
        const replacementDigest = targetArtifactDigest(replacementTarget);
        assert.notEqual(replacementDigest, targetArtifactDigest(preview));
        assert.equal(replacementTarget.persistence_contract, null, 'immutable old policy never approves replacement bytes');
        assert(replacementTargets.every(target => target.persistence_contract === null));
        assert.equal(await b.locator('.installed-float-provider').count(), 0, 'successful upgrade does not resurrect old instances');
        const current = await open(b, 'fixture-replacement-B', false, 'Captured replacement fixture import 0.3.0');
        assert.notEqual(await current.provider.getAttribute('data-instance'), enabledInstance);
        const currentWindow = b.locator(`.float-window[data-float-id="${current.id}"]`);
        assert.match(await currentWindow.textContent(), /Not restored after reload/);
        const replacementPath = new URL(replacementTarget.module_url, base).pathname;
        assert(replacementPath.startsWith(`/_plinth/typed/${replacement.id}/${replacementDigest}/preview/client/`));
        const currentModule = await request(b, replacementPath);
        assert.equal(currentModule.status, 200);
        assert.equal((await request(b, modulePath)).status, 404);
        const replacementResources = await b.evaluate(() => performance.getEntriesByType('resource').map(entry => new URL(entry.name).pathname));
        assert(replacementResources.includes(replacementPath), 'actual import used the replacement captured namespace');
        const currentInstance = await current.provider.getAttribute('data-instance');
        await current.provider.locator('.installed-float-unsaved').fill('fake replacement edit never approved for persistence');
        await current.provider.locator('.installed-float-unsaved').fill('');
        // Authority changes invalidate the prior persistence epoch. Deliberate
        // user Restore refreshes it; no invented ready flag or silent retry.
        const freshLayout = b.waitForResponse(response =>
            new URL(response.url()).pathname === '/api/frontend/float-layout' && response.request().method() === 'GET');
        await b.getByRole('button', { name: 'Restore saved panels', exact: true }).click();
        const layoutResponse = await freshLayout;
        assert.equal(layoutResponse.status(), 200);
        const layout = await layoutResponse.json();
        assert.equal(layout.policy_revision, '1', 'startup policy is unchanged, not rewritten for new bytes');
        assert(layout.restore_targets.every(result => result.status === 'unavailable'));
        await b.waitForFunction(() => {
            const button = [...document.querySelectorAll('.float-storage-status button')]
                .find(button => button.textContent === 'Restore saved panels');
            return button && !button.disabled;
        });
        await save(b, state.accounts.b, []);
        assert.equal(saved(state.accounts.b).floats.length, 0, 'unapproved replacement remains excluded from explicit snapshot');
        assert.equal(await current.provider.getAttribute('data-instance'), currentInstance, 'save does not replace live-only UI');
        completed.push('successful replacement recovers through new native bytes without inherited persistence approval');
        await verifyTypedSurfaceFoundation({ page: b, denied, request, sql, literal,
            account: state.accounts.b, generation: replacement.id, retiredGeneration: originalPackage.id,
            retiredTargets: originalTypedTargets,
            floatTargets: replacementTargets, completed });
    }
} catch (error) {
    failure = error;
    // One global, private-only diagnostic covers every failing journey stage
    // before contexts close. Never replace or reinterpret the primary error.
    try { console.error('PLINTH_FLOAT_PRIVATE_UI ' + JSON.stringify(await captureFailurePages())); }
    catch { /* Preserve the exact original failure even if diagnostics fail. */ }
}
finally {
    privateRealtimeClosed = true;
    for (const observer of authorityRecoveries.values()) observer.stop();
    for (const observer of privateRealtime.values()) observer.stop();
    privateRealtime.clear();
    for (const lane of readLanes.values()) lane.stop();
    readLanes.clear();
    for (const context of contexts) { try { await context.close(); } catch (error) { cleanupFailures.push(error); } }
    if (browser) { try { await browser.close(); } catch (error) { cleanupFailures.push(error); } }
    try {
        const deadline = performance.now() + 2000;
        const [bodyErrors] = await Promise.all([responseBodies.drain(deadline),
            boundedStep(() => Promise.all([...bodyObservationTasks, ...privateSnapshotTasks]), deadline),
            ...[...authorityRecoveries.values()].map(observer => observer.drain(deadline))]);
        for (const error of bodyErrors) {
            if (error !== failure && !cleanupFailures.includes(error)) cleanupFailures.push(error);
        }
    } catch (error) { cleanupFailures.push(error); }
    finally { bodyObservationTasks.clear(); privateSnapshotTasks.clear(); authorityRecoveries.clear(); }
}
// The supervisor drops the exact disposable database and owned filesystem even
// when install or verify fails. Normal package uninstall is separately required
// on successful verification, never replaced with SQL deletion.
if (phase === 'verify' && state && !failure && !cleanupFailures.length) {
    try {
        browser = await chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
            args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] });
        const operator = await login(state.accounts.operator);
        for (const name of ['floatcaller', 'floatprovider']) {
            const row = packageRow(name);
            assert.equal(row.id, state.packages[name].id);
            assert.equal(row.installed_by_user_id, state.accounts.operator.id);
            assert.equal((await request(operator, `/api/packages/${row.id}?confirm=true`, 'DELETE')).status, 204);
            assert.equal(packageRow(name), null);
        }
        // Consumer contexts are already closed and no active provider remains.
        // Only exact owned failed/superseded attempts may be removed here,
        // through normal authenticated lifecycle DELETE, never SQL cleanup.
        for (const expected of state.retiredPackages ?? []) {
            const rows = jsonSql(`SELECT coalesce(json_agg(p),'[]'::json)::text FROM
                (SELECT id,name,version,state,installed_by_user_id FROM plinth.packages
                 WHERE id=${literal(expected.id)}) p`);
            if (!rows.length) continue; // Normal prior uninstall may cascade.
            assert.equal(rows.length, 1);
            const row = rows[0];
            assert.equal(row.name, 'floatprovider');
            assert.equal(row.version, expected.version);
            assert.equal(row.state, expected.state);
            assert.equal(row.installed_by_user_id, state.accounts.operator.id);
            assert.equal((await request(operator, `/api/packages/${row.id}?confirm=true`, 'DELETE')).status, 204);
            assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE id=${literal(row.id)}`), '0');
        }
        assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE name IN ('floatcaller','floatprovider')
            AND installed_by_user_id=${literal(state.accounts.operator.id)}`), '0');
        completed.push('normal authenticated package uninstall after consumer contexts close');
    } catch (error) { cleanupFailures.push(error); }
    finally { if (browser) { try { await browser.close(); } catch (error) { cleanupFailures.push(error); } } }
}
if (failure || cleanupFailures.length) {
    emitJourneyFailure(phase, completed, failure ?? cleanupFailures[0], undefined, failureSteps.failed());
    throw new AggregateError([...(failure ? [failure] : []), ...cleanupFailures], 'installed float journey/cleanup failed');
}
if (phase === 'verify') {
    assert.equal(completed.length, 15);
    assert.deepEqual(recoveryEvidence, Array.from({ length: 3 }, () =>
        ({ terminal: 1, new_socket_ack: 1, fresh_catalog: 1, home_empty: 1 })));
    console.log(JSON.stringify({ phase, expectedCases: 15, completed, actualAuth: true,
        packageProvenance: 'user', archiveHashes: state.archiveHashes,
        protectedPolicy: true, normalUninstall: true,
        authorityRecovery: { barriers: 3, terminals: 3, newSocketAcks: 3, freshCatalogs: 3, emptyPrimary: 3 },
        scope: 'native ordinary ZIP/API/auth/RBAC/SDK/import/protected policy/guarded SQL/reload/component recovery/revocation/package lifecycle plus primary/tray typed transport; no tray-UI/navigation/process-crash or lost-ACK proof' }));
}
