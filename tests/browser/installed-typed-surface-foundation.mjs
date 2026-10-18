// Real native transport proof only; no endpoint interception or mocked modules.
// This does not prove tray owners, bell/avatar dogfooding or navigation delivery.
import assert from 'node:assert/strict';

export const FOUNDATION_CASES = Object.freeze([
    'ordinary installed primary and tray catalogs, exact typed wire and isolated cursors',
    'indexed mixed-kind admission, cookie CSRF, PAT access and denied no-metadata',
    'captured primary and tray imports, foreign essential refusal and fresh rule fencing',
]);
const endpoint = '/api/frontend/surfaces';
const admission = `${endpoint}/admission`;
const baseKeys = ['application_id', 'panel_id', 'type', 'generation', 'version', 'title', 'icon', 'module_url'];

async function nativeRequest(page, path, { method = 'GET', payload, token, csrf = true, anonymous = false, deadline } = {}) {
    const timeout = deadline === undefined ? 10000 : Math.min(10000, Math.ceil(deadline - performance.now()));
    assert(timeout > 0, 'typed transport deadline expired');
    const result = await page.evaluate(async ({ path, method, payload, token, csrf, anonymous, timeout }) => {
        let options = { method, credentials: token || anonymous ? 'omit' : 'same-origin',
            signal: AbortSignal.timeout(timeout), headers: {},
            ...(payload === undefined ? {} : { body: JSON.stringify(payload) }) };
        if (payload !== undefined) options.headers['Content-Type'] = 'application/json';
        if (token) options.headers.Authorization = `Bearer ${token}`;
        else if (csrf && !anonymous) options = (await import('@plinth/frontend/sdk')).withCsrf(path, options);
        const response = await fetch(path, options);
        const text = await response.text();
        if (new TextEncoder().encode(text).length > 1048576) throw new Error('typed test response exceeded its bound');
        let value = null;
        if (text && response.headers.get('content-type')?.includes('application/json')) value = JSON.parse(text);
        return { status: response.status, value, bytes: new TextEncoder().encode(text).length,
            cache: response.headers.get('cache-control'), vary: response.headers.get('vary') };
    }, { path, method, payload, token, csrf, anonymous, timeout });
    assert(deadline === undefined || performance.now() < deadline, 'typed transport completed after its deadline');
    return result;
}

function pinned(target) {
    return { application_id: target.application_id, panel_id: target.panel_id,
        kind: target.type, expected_generation: target.generation,
        ...(target.type === 'float' ? { capability: target.capability } : {}) };
}
function hardening(response, expectedCache = 'no-store') {
    assert.equal(response.cache, expectedCache);
    const vary = (response.vary ?? '').toLowerCase().split(',').map(value => value.trim());
    assert(vary.includes('cookie') && vary.includes('authorization'));
}
function indexed(response, expected) {
    assert.equal(response.status, 200);
    assert.deepEqual(Object.keys(response.value).sort(), ['results', 'schema_version']);
    assert.equal(response.value.schema_version, 1);
    assert.equal(response.value.results.length, expected.length);
    for (const [index, status] of expected.entries()) {
        const result = response.value.results[index];
        assert.equal(result.index, index);
        assert.equal(result.status, status);
        assert.deepEqual(Object.keys(result).sort(), status === 'available'
            ? ['index', 'status', 'target'] : ['index', 'status']);
    }
    hardening(response);
}

export async function snapshotTypedSurfaceTargets(page, generation, deadline = performance.now() + 15000) {
    const targets = [];
    for (const kind of ['primary', 'tray']) {
        const response = await nativeRequest(page, `${endpoint}?kind=${kind}`, { deadline });
        assert.equal(response.status, 200); hardening(response);
        assert.equal(response.value.kind, kind); assert.equal(response.value.next_cursor, null);
        assert.deepEqual(response.value.targets.map(target => target.panel_id),
            kind === 'primary' ? ['nav-a', 'nav-b'] : ['status-a', 'status-b']);
        for (const target of response.value.targets) {
            assert.equal(target.type, kind); assert.equal(target.generation, generation);
            assert.equal(target.application_id, 'floatprovider');
            assert.equal(typeof target.module_url, 'string');
            targets.push(target);
        }
    }
    return targets;
}

export async function verifyTypedSurfaceUnavailable(page, targets, deadline = performance.now() + 15000) {
    assert.equal(targets.length, 4);
    for (const kind of ['primary', 'tray']) {
        const response = await nativeRequest(page, `${endpoint}?kind=${kind}`, { deadline });
        assert.equal(response.status, 200); assert.deepEqual(response.value.targets, []);
        assert.equal(response.value.next_cursor, null); hardening(response);
    }
    indexed(await nativeRequest(page, admission, { method: 'POST', deadline,
        payload: { schema_version: 1, targets: targets.map(pinned) } }), targets.map(() => 'unavailable'));
    for (const target of targets) assert.equal((await nativeRequest(page, target.module_url, { deadline })).status, 404);
}

export async function verifyTypedSurfaceFoundation({ page, denied, request, sql, literal,
    account, generation, retiredGeneration, retiredTargets, floatTargets, completed }) {
    const typed = [];
    const cookiePages = [];
    for (const kind of ['primary', 'tray']) {
        const first = await nativeRequest(page, `${endpoint}?kind=${kind}&limit=1&include_admin_default=0`);
        assert.equal(first.status, 200); hardening(first);
        assert.deepEqual(Object.keys(first.value).sort(), ['kind', 'next_cursor', 'schema_version', 'targets']);
        assert.equal(first.value.schema_version, 1); assert.equal(first.value.kind, kind);
        assert.equal(first.value.targets.length, 1);
        assert.equal(typeof first.value.next_cursor, 'string');
        const nextPath = `${endpoint}?kind=${kind}&limit=1&cursor=${encodeURIComponent(first.value.next_cursor)}`;
        cookiePages.push(nextPath);
        const second = await nativeRequest(page, nextPath);
        assert.equal(second.status, 200); hardening(second);
        assert.equal(second.value.next_cursor, null); assert.equal(second.value.targets.length, 1);
        const records = [...first.value.targets, ...second.value.targets];
        assert.deepEqual(records.map(target => target.panel_id), kind === 'primary'
            ? ['nav-a', 'nav-b'] : ['status-a', 'status-b']);
        for (const target of records) {
            assert.deepEqual(Object.keys(target).sort(), [...baseKeys, ...(kind === 'primary'
                ? ['accepts_navigation_intent', 'context_schema']
                : ['tray_states', 'tray_initial_state', 'preferred_width', 'chrome_essential'])].sort());
            assert.equal(target.type, kind); assert.equal(target.application_id, 'floatprovider');
            assert.equal(target.generation, generation); assert.equal(target.version, '0.3.0');
            assert.match(target.module_url, new RegExp(`^/_plinth/typed/${generation}/[0-9a-f]{64}/${target.panel_id}/client/panels/typed\\.js$`));
            if (kind === 'primary') assert.equal(target.accepts_navigation_intent, true);
            else {
                assert.equal(target.chrome_essential, false, 'foreign declaration never derives essential authority');
                assert.equal(target.tray_initial_state, 'idle'); assert.equal(target.preferred_width, 320);
                assert(Object.hasOwn(target.tray_states, target.tray_initial_state));
            }
        }
        typed.push(...records);
        const deniedCatalog = await nativeRequest(denied, `${endpoint}?kind=${kind}&limit=64`);
        assert.equal(deniedCatalog.status, 200); assert.deepEqual(deniedCatalog.value.targets, []);
        assert.deepEqual(Object.keys(deniedCatalog.value).sort(), ['kind', 'next_cursor', 'schema_version', 'targets']);
        assert.equal((await nativeRequest(page, `${endpoint}?kind=${kind}`, { anonymous: true })).status, 401);
        const otherKind = kind === 'primary' ? 'tray' : 'primary';
        assert.equal((await nativeRequest(page, nextPath.replace(`kind=${kind}`, `kind=${otherKind}`))).status, 400);
        assert.equal((await nativeRequest(page, nextPath.replace('limit=1', 'limit=2'))).status, 400);
        assert.equal((await nativeRequest(denied, nextPath)).status, 400, 'cursor is credential-bound');
        for (const suffix of ['&content_type=text%2Fplain', '&include_admin_default=1']) {
            assert.equal((await nativeRequest(page, `${endpoint}?kind=${kind}${suffix}`)).status, 400);
        }
    }
    completed.push(FOUNDATION_CASES[0]);

    const targets = [...floatTargets, ...typed];
    assert.equal(targets.length, 6);
    const lookups = targets.map(pinned);
    const absent = Array.from({ length: 32 }, (_, index) => ({ application_id: 'floatprovider',
        panel_id: `absent-${index}`, kind: index % 2 ? 'primary' : 'tray', expected_generation: null }));
    const mixed = await nativeRequest(page, admission, { method: 'POST', payload: { schema_version: 1,
        targets: [...lookups, ...absent] } });
    indexed(mixed, [...targets.map(() => 'available'), ...absent.map(() => 'unavailable')]);
    for (const [index, target] of targets.entries()) assert.deepEqual(mixed.value.results[index].target, target);
    indexed(await nativeRequest(page, admission, { method: 'POST', payload: { schema_version: 1,
        targets: typed.map(target => ({ application_id: target.application_id, panel_id: target.panel_id,
            kind: target.type, expected_generation: null })) } }), typed.map(() => 'available'));
    indexed(await nativeRequest(denied, admission, { method: 'POST',
        payload: { schema_version: 1, targets: typed.map(pinned) } }), typed.map(() => 'unavailable'));
    assert.equal((await nativeRequest(page, admission, { method: 'POST', csrf: false,
        payload: { schema_version: 1, targets: [pinned(typed[0])] } })).status, 403);
    for (const invalid of [[...lookups, lookups[0]], [...lookups, ...absent,
        { ...absent[0], panel_id: 'absent-extra' }], [{ ...pinned(typed[0]), capability: 'floatprovider:1:preview' }]]) {
        assert.equal((await nativeRequest(page, admission, { method: 'POST',
            payload: { schema_version: 1, targets: invalid } })).status, 400);
    }
    const created = await request(page, '/api/auth/pats', 'POST', { name: 'owned-typed-surface-proof' });
    assert.equal(created.status, 201);
    assert(typeof created.value?.token === 'string' && created.value.token.startsWith('plinth_'), 'owned PAT issued');
    assert(typeof created.value?.id === 'string' && /^[0-9a-f-]{36}$/.test(created.value.id), 'owned PAT identity returned');
    const patFailures = [];
    try {
        for (const path of cookiePages) assert.equal((await nativeRequest(page, path,
            { token: created.value.token })).status, 400, 'same-user PAT cannot reuse a cookie-bound cursor');
        for (const kind of ['primary', 'tray']) {
            const response = await nativeRequest(page, `${endpoint}?kind=${kind}`, { token: created.value.token });
            assert.equal(response.status, 200); assert.equal(response.value.targets.length, 2); hardening(response);
        }
        indexed(await nativeRequest(page, admission, { method: 'POST', token: created.value.token,
            payload: { schema_version: 1, targets: typed.map(pinned) } }), typed.map(() => 'available'));
        for (const target of typed) assert.equal((await nativeRequest(page, target.module_url,
            { token: created.value.token })).status, 200);
    } catch (error) { patFailures.push(error); }
    try {
        assert.equal((await request(page, `/api/auth/pats/${created.value.id}`, 'DELETE')).status, 200);
        assert.equal((await nativeRequest(page, `${endpoint}?kind=tray`, { token: created.value.token })).status, 401);
    } catch (error) { patFailures.push(error); }
    if (patFailures.length) throw new AggregateError(patFailures, 'owned PAT proof or cleanup failed');
    completed.push(FOUNDATION_CASES[1]);

    for (const target of typed) {
        assert.equal((await nativeRequest(denied, target.module_url)).status, 404);
        assert.equal((await nativeRequest(page, target.module_url.replace(generation, retiredGeneration))).status, 404);
        assert.equal((await nativeRequest(page, target.module_url.replace(`/${target.panel_id}/client/`, '/absent/client/'))).status, 404);
        const head = await nativeRequest(page, target.module_url, { method: 'HEAD' });
        assert.equal(head.status, 200); assert.equal(head.bytes, 0); hardening(head, 'private, no-store');
        const imported = await page.evaluate(async url => {
            const module = await import(url), result = await module.capturedProbe();
            return { factory: typeof module.default, label: result.label, detail: result.detail,
                sdkShared: result.sdk === await import('@plinth/frontend/sdk'),
                preactShared: result.preact === await import('preact') };
        }, target.module_url);
        assert.deepEqual(imported, { factory: 'function', label: 'Captured replacement fixture import 0.3.0',
            detail: 'Captured dynamic fixture import', sdkShared: true, preactShared: true });
    }
    indexed(await nativeRequest(page, admission, { method: 'POST', payload: { schema_version: 1,
        targets: typed.map(target => ({ ...pinned(target), expected_generation: retiredGeneration })) } }), typed.map(() => 'stale'));
    indexed(await nativeRequest(page, admission, { method: 'POST', payload: { schema_version: 1,
        targets: [{ ...pinned(typed[0]), kind: 'tray' }] } }), ['unavailable']);
    assert.equal(retiredTargets.length, 4);
    for (const target of retiredTargets) assert.equal((await nativeRequest(page, target.module_url)).status, 404);
    indexed(await nativeRequest(page, admission, { method: 'POST', payload: { schema_version: 1,
        targets: retiredTargets.map(pinned) } }), retiredTargets.map(() => 'stale'));
    const legacy = { ...pinned(typed[0]), panel_id: 'legacy-primary' };
    indexed(await nativeRequest(page, admission, { method: 'POST',
        payload: { schema_version: 1, targets: [legacy] } }), ['unavailable']);
    assert.equal((await nativeRequest(page, typed[0].module_url.replace('/nav-a/client/', '/legacy-primary/client/'))).status, 404);
    // Remove only this test account's existing fixture rule. This is fresh
    // authority proof, not a fabricated realtime hint or additional grant.
    assert.equal(sql(`WITH removed AS (DELETE FROM plinth.group_rules gr USING plinth.rbac_rules r
        WHERE gr.rule_id=r.id AND r.rule='floatprovider.panel' AND gr.group_id IN
        (SELECT group_id FROM plinth.group_members WHERE user_id=${literal(account.id)}) RETURNING gr.rule_id)
        SELECT count(*) FROM removed`), '1');
    for (const kind of ['primary', 'tray']) {
        const response = await nativeRequest(page, `${endpoint}?kind=${kind}`);
        assert.equal(response.status, 200); assert.deepEqual(response.value.targets, []);
    }
    indexed(await nativeRequest(page, admission, { method: 'POST',
        payload: { schema_version: 1, targets: typed.map(pinned) } }), typed.map(() => 'unavailable'));
    for (const target of typed) assert.equal((await nativeRequest(page, target.module_url)).status, 404);
    completed.push(FOUNDATION_CASES[2]);
}
