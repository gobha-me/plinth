// Native admin transport coverage, not a manager UI or a permission grant.
// All mutations use the ordinary authenticated HTTP endpoint; SQL only observes.
import assert from 'node:assert/strict';
import { randomUUID } from 'node:crypto';

export const ADMIN_DEFAULT_CASES = Object.freeze([
    'native administrator logical-default CRUD and acknowledged stale-handler retention',
    'fresh admin-only methods, cookie CSRF and explicit administrator PAT transport',
    'closed native admin query and mutation byte-boundary refusals without storage changes',
    'native admin byte-ordered 64-plus-one paging and credential-isolated opaque cursors',
]);
export const ADMIN_DEFAULT_BOUNDS = Object.freeze({ keys: 65, response: 131072,
    mutation: 2048, requests: 240, operationMs: 10000, journeyMs: 75000, cleanupMs: 30000 });
const endpoint = '/api/frontend/handler-defaults/admin';
const stale = Object.freeze({ application_id: 'not-installed-admin46', panel_id: 'absent-panel' });
const installed = Object.freeze({ application_id: 'floatprovider', panel_id: 'preview' });

export function adminDefaultFixtureKeys(suffix = randomUUID().replaceAll('-', '')) {
    assert.equal(typeof suffix, 'string'); assert.equal(suffix.length, 32);
    assert.match(suffix, /^[0-9a-f]{32}$/);
    return Array.from({ length: ADMIN_DEFAULT_BOUNDS.keys }, (_, index) =>
        `application/x-plinth-admin46-${suffix}-${String(index).padStart(3, '0')}`);
}

function exact(value, keys) {
    assert(value !== null && typeof value === 'object' && !Array.isArray(value));
    assert.deepEqual(Object.keys(value).sort(), [...keys].sort());
}
function handler(value) {
    exact(value, ['application_id', 'panel_id']);
    assert.match(value.application_id, /^[a-z][a-z0-9_-]{0,63}$(?![\s\S])/);
    assert.match(value.panel_id, /^[a-z][a-z0-9_-]{0,63}$(?![\s\S])/);
}
function hardened(result, profile) {
    assert.equal(result.cache, 'no-store');
    assert.equal(result.nosniff, 'nosniff');
    const vary = (result.vary ?? '').toLowerCase().split(',').map(value => value.trim());
    assert(vary.includes('cookie') && vary.includes('authorization'));
    assert.equal(result.profile, profile, 'successful management reply retains its admitted document profile');
}
function single(result, type, expected, profile) {
    assert.equal(result.status, 200); hardened(result, profile);
    exact(result.value, ['schema_version', 'content_type', 'handler']);
    assert.equal(result.value.schema_version, 1); assert.equal(result.value.content_type, type);
    if (result.value.handler !== null) handler(result.value.handler);
    assert.deepEqual(result.value.handler, expected);
}
function page(result, profile) {
    assert.equal(result.status, 200); hardened(result, profile);
    exact(result.value, ['schema_version', 'entries', 'next_cursor']);
    assert.equal(result.value.schema_version, 1);
    assert(Array.isArray(result.value.entries) && result.value.entries.length <= 64);
    let previous = '';
    for (const entry of result.value.entries) {
        exact(entry, ['content_type', 'handler']); handler(entry.handler);
        assert.equal(typeof entry.content_type, 'string');
        assert(Buffer.compare(Buffer.from(previous), Buffer.from(entry.content_type)) < 0);
        previous = entry.content_type;
    }
    assert(result.value.next_cursor === null || typeof result.value.next_cursor === 'string');
    if (result.value.next_cursor !== null) {
        assert(result.value.next_cursor.length > 0 && result.value.next_cursor.length <= 4096);
        assert(!result.value.next_cursor.includes('application/'), 'raw MIME position is never a caller cursor');
    }
    return result.value;
}

export async function verifyAdminDefaultsFoundation({ operator, denied, consumers, request, sql, literal, completed }) {
    const keys = adminDefaultFixtureKeys(), attempted = new Set(), failures = [];
    const deadline = performance.now() + ADMIN_DEFAULT_BOUNDS.journeyMs;
    const profile = await operator.locator('meta[name=plinth-platform-profile]').getAttribute('content');
    assert.match(profile, /^v1:[0-9a-f-]{36}:[0-9a-f]{64}$/);
    assert.equal(sql('SELECT count(*) FROM ext_shell.default_apps'), '0', 'owned fixture starts without administrator defaults');
    const grants = sql('SELECT count(*) FROM plinth.group_rules');
    let requests = 0, pat = null;
    const timeout = end => {
        const value = Math.min(ADMIN_DEFAULT_BOUNDS.operationMs, Math.ceil(end - performance.now()));
        assert(value > 0, 'bounded admin transport deadline expired'); return value;
    };
    async function native(path, { method = 'GET', payload, raw, token, csrf = true,
        anonymous = false, owner = operator, end = deadline } = {}) {
        assert(++requests <= ADMIN_DEFAULT_BOUNDS.requests, 'finite admin fixture request budget');
        if (payload !== undefined) assert(Buffer.byteLength(JSON.stringify(payload)) <= ADMIN_DEFAULT_BOUNDS.mutation);
        return owner.evaluate(async ({ path, method, payload, raw, token, csrf, anonymous, ms, bound }) => {
            let options = { method, credentials: token || anonymous ? 'omit' : 'same-origin',
                headers: {}, signal: AbortSignal.timeout(ms) };
            if (payload !== undefined || raw !== undefined) {
                options.body = raw ?? JSON.stringify(payload); options.headers['Content-Type'] = 'application/json';
            }
            if (token) options.headers.Authorization = `Bearer ${token}`;
            else if (csrf && !anonymous) options = (await import('@plinth/frontend/sdk')).withCsrf(path, options);
            const response = await fetch(path, options), reader = response.body?.getReader(), chunks = [];
            let size = 0;
            try {
                if (reader) for (;;) {
                    const { done, value } = await reader.read(); if (done) break;
                    size += value.byteLength;
                    if (size > bound) { await reader.cancel(); throw new Error('admin response byte bound exceeded'); }
                    chunks.push(value);
                }
            } finally { reader?.releaseLock(); }
            const bytes = new Uint8Array(size); let offset = 0;
            for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.length; }
            const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
            return { status: response.status, value: text ? JSON.parse(text) : null,
                cache: response.headers.get('cache-control'), vary: response.headers.get('vary'),
                nosniff: response.headers.get('x-content-type-options'),
                profile: response.headers.get('x-plinth-platform-profile'), bytes: size };
        }, { path, method, payload, raw, token, csrf, anonymous, ms: timeout(end), bound: ADMIN_DEFAULT_BOUNDS.response });
    }
    async function rawTransport(path, method, raw, { origin, end = deadline } = {}) {
        // APIRequestContext is an actual native HTTP transport, not browser
        // fetch: it can send GET bodies, invalid UTF-8 and a chosen Origin.
        assert(++requests <= ADMIN_DEFAULT_BOUNDS.requests);
        const cookies = await operator.context().cookies();
        const csrf = cookies.find(cookie => cookie.name === 'plinth_csrf')?.value;
        assert.equal(typeof csrf, 'string');
        const response = await operator.context().request.fetch(new URL(path, operator.url()).href,
            { method, data: raw, headers: { 'Content-Type': 'application/json',
                'X-Plinth-CSRF': csrf, Origin: origin ?? new URL(operator.url()).origin },
            timeout: timeout(end), maxRedirects: 0, maxRetries: 0 });
        try {
            const bytes = await response.body(); assert(bytes.length <= ADMIN_DEFAULT_BOUNDS.response);
            return { status: response.status(), value: bytes.length ? JSON.parse(bytes.toString('utf8')) : null };
        } finally { await response.dispose(); }
    }
    const get = (type, options) => native(`${endpoint}?content_type=${encodeURIComponent(type)}`, options);
    async function put(type, choice = stale, options = {}) {
        attempted.add(type); // Include unknown outcomes in explicit normal-API cleanup.
        const result = await native(endpoint, { method: 'PUT',
            payload: { schema_version: 1, content_type: type, handler: choice }, ...options });
        assert.equal(result.status, 200); hardened(result, profile);
        assert.deepEqual(result.value, { schema_version: 1, saved: true }); return result;
    }
    async function remove(type, expected, options = {}) {
        const result = await native(endpoint, { method: 'DELETE',
            payload: { schema_version: 1, content_type: type }, ...options });
        assert.equal(result.status, 200); hardened(result, profile);
        exact(result.value, ['schema_version', 'deleted']); assert.equal(result.value.schema_version, 1);
        assert.equal(typeof result.value.deleted, 'boolean');
        if (expected !== undefined) assert.equal(result.value.deleted, expected);
    }
    const observed = () => sql(`SELECT coalesce(json_agg(t ORDER BY content_type COLLATE "C"),'[]'::json)::text
        FROM (SELECT content_type,application_id,panel_id FROM ext_shell.default_apps) t`);
    try {
        single(await get(keys[0]), keys[0], null, profile);
        await put(keys[0]); single(await get(keys[0]), keys[0], stale, profile);
        assert.equal(sql(`SELECT count(*) FROM ext_shell.default_apps WHERE content_type=${literal(keys[0])}
            AND application_id='not-installed-admin46' AND panel_id='absent-panel'`), '1');
        await put(keys[0], installed); single(await get(keys[0]), keys[0], installed, profile);
        await remove(keys[0], true); await remove(keys[0], false);
        single(await get(keys[0]), keys[0], null, profile);
        completed.push(ADMIN_DEFAULT_CASES[0]);

        await put(keys[0]); const unchanged = observed();
        for (const owner of consumers) for (const method of ['GET', 'PUT', 'DELETE']) {
            const result = await native(endpoint,
                { owner, method, ...(method === 'GET' ? {} : { payload: method === 'PUT'
                    ? { schema_version: 1, content_type: keys[0], handler: installed }
                    : { schema_version: 1, content_type: keys[0] } }) });
            assert.equal(result.status, 403);
            for (const hidden of [stale.application_id, stale.panel_id, keys[0]]) {
                assert(!JSON.stringify(result.value).includes(hidden), 'denial exposes no configured handler');
            }
        }
        for (const method of ['GET', 'PUT', 'DELETE']) {
            assert.equal((await native(endpoint, { method, anonymous: true,
                ...(method === 'GET' ? {} : { payload: method === 'PUT'
                    ? { schema_version: 1, content_type: keys[0], handler: installed }
                    : { schema_version: 1, content_type: keys[0] } }) })).status, 401);
        }
        assert.equal((await native(endpoint, { method: 'PUT', csrf: false,
            payload: { schema_version: 1, content_type: keys[0], handler: installed } })).status, 403);
        assert.equal((await native(endpoint, { method: 'DELETE', csrf: false,
            payload: { schema_version: 1, content_type: keys[0] } })).status, 403);
        assert.equal((await rawTransport(endpoint, 'PUT', JSON.stringify({ schema_version: 1,
            content_type: keys[0], handler: installed }), { origin: 'https://wrong-origin.invalid' })).status, 403);
        assert.equal(observed(), unchanged, 'denied/CSRF requests perform no default DML');
        const created = await native('/api/auth/pats', { method: 'POST',
            payload: { name: 'owned-admin-default-transport' } });
        assert.equal(created.status, 201); pat = created.value;
        assert.equal(typeof pat.token, 'string'); assert.match(pat.id, /^[0-9a-f-]{36}$/);
        await put(keys[0], installed, { token: pat.token });
        single(await get(keys[0], { token: pat.token }), keys[0], installed, profile);
        await remove(keys[0], true, { token: pat.token });
        assert.equal(sql('SELECT count(*) FROM plinth.group_rules'), grants, 'management never grants permission');
        completed.push(ADMIN_DEFAULT_CASES[1]);

        await put(keys[0]); const beforeInvalid = observed();
        const valid = JSON.stringify({ schema_version: 1, content_type: keys[0], handler: stale });
        const exactBody = valid + ' '.repeat(ADMIN_DEFAULT_BOUNDS.mutation - Buffer.byteLength(valid));
        assert.equal(Buffer.byteLength(exactBody), 2048);
        const exactSaved = await native(endpoint, { method: 'PUT', raw: exactBody });
        assert.equal(exactSaved.status, 200); hardened(exactSaved, profile);
        assert.deepEqual(exactSaved.value, { schema_version: 1, saved: true });
        assert.equal((await native(endpoint, { method: 'PUT', raw: exactBody + ' ' })).status, 413);
        const invalidBodies = [
            valid.replace('"schema_version":1', '"schema_version":1,"schema_version":1'),
            valid.replace('"application_id":', '"application_id":"duplicate","application_id":'),
            valid.replace('"panel_id":', '"panel_id":"duplicate","panel\\u005fid":'),
            valid.replace('"schema_version":1', '"schema_version":1.0'),
            valid.replace('"schema_version":1', '"schema_version":true'),
            valid.replace('"schema_version":1', '"schema_version":1,"user_id":"forged"'),
            valid.replace(stale.panel_id, stale.panel_id + '\\u0000'),
            valid.replace(stale.panel_id, stale.panel_id + '\\n'),
            valid + '\u0000', valid + ' trailing', '[]',
        ];
        for (const raw of invalidBodies) assert.equal((await native(endpoint, { method: 'PUT', raw })).status, 400);
        assert.equal((await rawTransport(endpoint, 'PUT', Buffer.concat([Buffer.from(valid), Buffer.from([0xff])]))).status, 400);
        assert.equal((await rawTransport(endpoint, 'GET', '{}')).status, 400);
        for (const query of ['?limit=0', '?limit=65', '?limit=01', '?limit=1.0', '?limit=true',
            '?content_type=text%2Fplain&limit=1', '?content_type=text%2Fplain&content%5ftype=text%2Fplain',
            '?limit=1&%6cimit=1', '?user_id=forged', '?content_type=text%2Fplain%00', '?content_type=text%2Fplain%0A']) {
            assert.equal((await native(endpoint + query)).status, 400);
        }
        assert.equal((await native(endpoint + '?limit=1', { method: 'PUT', raw: valid })).status, 400);
        assert.equal((await native(endpoint, { method: 'DELETE', raw: JSON.stringify({ schema_version: 1,
            content_type: keys[0], handler: stale }) })).status, 400);
        assert.equal(observed(), beforeInvalid, 'raw refusal controls never mutate configured defaults');
        completed.push(ADMIN_DEFAULT_CASES[2]);

        for (const type of keys.slice(1)) await put(type);
        const first = page(await native(endpoint), profile);
        assert.deepEqual(first.entries.map(entry => entry.content_type), keys.slice(0, 64));
        for (const entry of first.entries) assert.deepEqual(entry.handler, stale);
        assert.equal(typeof first.next_cursor, 'string');
        const continuation = `${endpoint}?cursor=${encodeURIComponent(first.next_cursor)}`;
        const second = page(await native(continuation), profile);
        assert.deepEqual(second.entries, [{ content_type: keys[64], handler: stale }]);
        assert.equal(second.next_cursor, null);
        assert.equal((await native(continuation, { token: pat.token })).status, 400);
        assert.equal((await native(continuation, { owner: denied })).status, 403, 'fresh admin gate precedes cursor use');
        assert.equal((await native(continuation + '&limit=1')).status, 400);
        const patFirst = page(await native(endpoint, { token: pat.token }), profile);
        assert.equal((await native(`${endpoint}?cursor=${encodeURIComponent(patFirst.next_cursor)}`)).status, 400);
        const patSecond = page(await native(`${endpoint}?cursor=${encodeURIComponent(patFirst.next_cursor)}`,
            { token: pat.token }), profile);
        assert.deepEqual(patSecond.entries, second.entries); assert.equal(patSecond.next_cursor, null);
        assert.equal(sql('SELECT count(*) FROM ext_shell.default_apps'), '65');
        assert.equal(sql('SELECT count(*) FROM plinth.group_rules'), grants);
        completed.push(ADMIN_DEFAULT_CASES[3]);
    } catch (error) { failures.push(error); }
    finally {
        // Unknown mutation outcomes are not retried or called rolled back.
        // Each distinct attempted owned key receives one deliberate cleanup
        // DELETE; final SQL observation is read-only, never a storage fallback.
        const cleanupDeadline = performance.now() + ADMIN_DEFAULT_BOUNDS.cleanupMs;
        for (const type of attempted) {
            try { await remove(type, undefined, { end: cleanupDeadline }); }
            catch (error) { failures.push(error); }
        }
        if (pat) {
            try {
                assert.equal((await native(`/api/auth/pats/${pat.id}`,
                    { method: 'DELETE', end: cleanupDeadline })).status, 200);
                assert.equal((await native(endpoint, { token: pat.token, end: cleanupDeadline })).status, 401);
            } catch (error) { failures.push(error); }
        }
        try {
            assert.equal(sql('SELECT count(*) FROM ext_shell.default_apps'), '0', 'all attempted admin keys cleaned normally');
            assert.equal(sql('SELECT count(*) FROM plinth.group_rules'), grants);
        } catch (error) { failures.push(error); }
    }
    if (failures.length) throw new AggregateError(failures, 'native admin-default transport or owned cleanup failed');
}
