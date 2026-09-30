// Actual installed fixture/SDK/Launcher/HTTP/PG/events; fake identities only.
// The outer production supervisor owns the kernel and disposable database.
import assert from 'node:assert/strict';
import { createHash, createHmac, randomBytes, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const target = new URL(process.env.PLINTH_BASE_URL || 'about:blank');
assert(['http:', 'https:'].includes(target.protocol) && !target.username && !target.password &&
    target.pathname === '/' && !target.search && !target.hash, 'task-owned kernel origin required');
const baseURL = target.origin, buildDir = process.env.PLINTH_TEST_BUILD_DIR;
assert(buildDir, 'matching PLINTH_TEST_BUILD_DIR required');
const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const source = await readFile(join(repo, 'tests/extensions/sdk-demo/client/panels/demo.js'));
const archive = await readFile(join(buildDir, 'fixtures/extensions/sdk-demo.zip'));
const moduleHash = createHash('sha256').update(source).digest('hex');
const channel = 'plinth:data:ext_shell.user_preferences';
const fixtureName = 'sdkdemo'; // Historical test-only sdk-demo identity is retired.
const panelRule = 'plinth.sdkdemo.panel';
const throwerRule = 'sdkdemo.thrower', throwerSignature = 'sdkdemo:1:thrower';
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env['PLINTH_PG_' + suffix], `PLINTH_PG_${suffix} required`);
    pgEnv['PG' + suffix] = process.env['PLINTH_PG_' + suffix];
}
function sql(statement) {
    return execFileSync('psql', ['-q', '-X', '-v', 'ON_ERROR_STOP=1', '-A', '-t', '-c', statement],
        { env: pgEnv, encoding: 'utf8', timeout: 15000, stdio: ['ignore', 'pipe', 'pipe'] }).trim();
}
async function bounded(operation, label, milliseconds = 15000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label}: deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
async function persisted(after) {
    const deadline = Date.now() + 15000;
    do {
        const row = sql(`SELECT json_build_object('seq',seq,'payload',payload)::text FROM plinth.events
            WHERE channel='${channel}' AND seq>${after} ORDER BY seq LIMIT 1`);
        if (row) return JSON.parse(row);
        await new Promise(resolveWait => setTimeout(resolveWait, 25));
    } while (Date.now() < deadline);
    throw new Error('native preference event deadline');
}
const operator = randomUUID(), consumer = randomUUID(), group = randomUUID();
const sessions = [randomUUID(), randomUUID()];
const tokens = [randomBytes(32).toString('base64url'), randomBytes(32).toString('base64url')];
const hashes = tokens.map(token => createHash('sha256').update(token).digest('hex'));
const csrf = tokens.map(token => createHmac('sha256', token).update('plinth.csrf.v1').digest('base64url'));
const headers = { Cookie: `plinth_session=${tokens[0]}; plinth_csrf=${csrf[0]}`,
    Origin: baseURL, 'X-Plinth-CSRF': csrf[0] };
async function packageDiagnostic(response) {
    // Emit only bounded protocol identifiers and fixture validation locations,
    // never an arbitrary server message, report body, URL, or request headers.
    const identifier = value => typeof value === 'string' &&
        /^[A-Za-z][A-Za-z0-9_.\[\]-]{0,159}$/.test(value) ? value : 'omitted';
    const classify = value => {
        if (typeof value !== 'string') return 'omitted';
        if (/^schema or role setup for ext_sdk-demo failed: ERROR:\s+extension identity cannot be represented as an isolated database role(?:\r?\n|$)/.test(value)) {
            return 'EXTENSION_DATABASE_IDENTITY_REJECTED';
        }
        if (/^(?:drop_schema_and_migrations failed: )?teardown of ext_sdk-demo failed: ERROR:\s+syntax error at or near "-"(?:\r?\n|$)/.test(value)) {
            return 'HYPHENATED_SCHEMA_TEARDOWN_SYNTAX';
        }
        return 'omitted';
    };
    let body;
    try { body = await bounded(() => response.json(), 'package error body', 5000); }
    catch { return { body: 'unavailable' }; }
    const result = { kind: identifier(body?.kind), stage: identifier(body?.failed_at_stage),
        code: identifier(body?.error?.code),
        message: body?.message === 'validation failed' ? body.message : 'omitted',
        messageClass: classify(body?.message || body?.error?.message) };
    let report = body?.report;
    if (response.status === 422 && !report) {
        // Regular install responses omit the report, but persist it on the
        // exactly owned failed package. A failed diagnostic cannot hide status.
        try {
            const stored = sql(`SELECT last_install_report::text FROM plinth.packages
                WHERE name='${fixtureName}' AND installed_by_user_id='${operator}'`);
            if (stored) report = JSON.parse(stored);
        } catch { result.validationReport = 'unavailable'; }
    }
    if (Array.isArray(report?.messages)) {
        result.validationCount = report.messages.length;
        result.validation = report.messages.slice(0, 32).map(item => ({
            severity: ['error', 'warning'].includes(item?.severity) ? item.severity : 'omitted',
            rule: identifier(item?.rule),
            path: typeof item?.path === 'string' && item.path.length <= 256 &&
                /^(manifest|capabilities|panels|rbac|config)\.json(?:\/[A-Za-z0-9_.-]+)*$/.test(item.path)
                ? item.path : 'omitted',
        }));
    }
    if (typeof report?.pg_sqlstate === 'string' && /^[0-9A-Z]{5}$/.test(report.pg_sqlstate)) {
        result.sqlstate = report.pg_sqlstate;
    }
    if (result.messageClass === 'omitted' && report?.message) result.messageClass = classify(report.message);
    return result;
}
async function lifecycle(path, options, expected) {
    const deadline = Date.now() + 15000;
    do {
        const response = await fetch(baseURL + path, { ...options,
            headers: { ...headers, ...options?.headers }, signal: AbortSignal.timeout(5000) });
        if (response.status === expected) return response;
        if (response.status !== 409) throw new Error(`owned package operation returned ${response.status}: ` +
            JSON.stringify(await packageDiagnostic(response)));
        await response.arrayBuffer();
        await new Promise(resolveWait => setTimeout(resolveWait, 25));
    } while (Date.now() < deadline);
    throw new Error('owned package operation deadline');
}
const effective = `SELECT DISTINCT r.rule FROM plinth.rbac_rules r
    JOIN plinth.group_rules gr ON gr.rule_id=r.id JOIN plinth.groups g ON g.id=gr.group_id
    LEFT JOIN plinth.group_members gm ON gm.group_id=g.id
    WHERE r.orphaned_at IS NULL AND (gm.user_id='${consumer}' OR g.name='everyone')`;
const maxSequence = () => Number(sql(`SELECT coalesce(max(seq),0) FROM plinth.events WHERE channel='${channel}'`));
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
// Preserve rejection when awaited, but avoid orphaned witness promises after a
// different assertion fails and cleanup closes the page before their deadline.
function observed(promise) { promise.catch(() => {}); return promise; }
let browser, context, page, packageId, seeded = false, createdRule = false, ruleId, failure;
const errors = [], snapshots = [], completed = [];
try {
    assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE name IN ('sdk-demo','${fixtureName}')`), '0',
        'refuse an unowned current or historical fixture');
    assert.equal(sql("SELECT count(*) FROM plinth.groups WHERE name='admin'"), '1');
    assert.equal(sql("SELECT count(*) FROM plinth.capabilities WHERE signature='kernel:1:config.get'"), '1');
    assert.equal(sql("SELECT count(*) FROM plinth.capabilities WHERE signature='shell:1:issue32absent'"), '0');
    assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule='${panelRule}'`), '0',
        'refuse an unowned existing fixture panel rule');
    assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule='${throwerRule}'`), '0',
        'refuse an unowned existing fixture thrower rule');
    assert.equal(sql(`SELECT count(*) FROM plinth.capabilities WHERE signature='${throwerSignature}'`), '0',
        'refuse an unowned existing fixture thrower capability');
    // A timed-out psql may have committed: cleanup must still inspect our IDs.
    seeded = true;
    const insertedRule = sql(`BEGIN; INSERT INTO plinth.users(id,username,password_hash) VALUES
        ('${operator}','sdk-operator-${operator}','not-a-password-hash'),
        ('${consumer}','sdk-consumer-${consumer}','not-a-password-hash');
        INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES
        ('${sessions[0]}','${operator}','${hashes[0]}'), ('${sessions[1]}','${consumer}','${hashes[1]}');
        INSERT INTO plinth.group_members(group_id,user_id) SELECT id,'${operator}' FROM plinth.groups WHERE name='admin';
        INSERT INTO plinth.groups(id,name) VALUES ('${group}','sdk-fixture-${group}');
        INSERT INTO plinth.group_members(group_id,user_id) VALUES ('${group}','${consumer}');
        WITH inserted AS (INSERT INTO plinth.rbac_rules(rule,namespace,description,extension_name)
        VALUES ('shell.realtime.subscribe','shell','Installed SDK fixture test-only grant','shell')
        ON CONFLICT DO NOTHING RETURNING id) SELECT id FROM inserted;
        INSERT INTO plinth.group_rules(group_id,rule_id)
        SELECT '${group}',id FROM plinth.rbac_rules WHERE rule='shell.realtime.subscribe'; COMMIT;`);
    createdRule = insertedRule !== '';
    ruleId = sql("SELECT id FROM plinth.rbac_rules WHERE rule='shell.realtime.subscribe'");
    assert.match(ruleId, uuid);
    if (createdRule) assert.equal(insertedRule, ruleId);
    assert.equal(sql(`SELECT count(*) FROM (${effective}) rules WHERE rule IN ('kernel.admin','kernel.config.get')`), '0');
    assert.equal(sql(`SELECT count(*) FROM (${effective}) rules WHERE rule='shell.realtime.subscribe'`), '1');
    assert.equal(sql(`SELECT count(*) FROM ext_shell.user_preferences WHERE user_id='${consumer}' AND key='shell.theme'`), '0');
    const form = new FormData(); form.append('package', new Blob([archive]), 'sdk-demo.zip');
    const installed = await lifecycle('/api/packages', { method: 'POST', body: form }, 201);
    const record = await bounded(() => installed.json(), 'install body');
    assert.equal(record.name, fixtureName); assert.equal(record.version, '0.1.0');
    assert.match(record.id, uuid); packageId = record.id;
    assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE id='${packageId}'
        AND name='${fixtureName}' AND installed_by_user_id='${operator}'`), '1');
    const ownedPanelRule = sql(`SELECT id FROM plinth.rbac_rules WHERE rule='${panelRule}'
        AND namespace='plinth' AND extension_name='${fixtureName}' AND orphaned_at IS NULL`);
    assert.match(ownedPanelRule, uuid, 'installed fixture owns exactly its declared panel rule');
    const ownedThrowerRule = sql(`SELECT id FROM plinth.rbac_rules WHERE rule='${throwerRule}'
        AND namespace='${fixtureName}' AND extension_name='${fixtureName}' AND orphaned_at IS NULL`);
    assert.match(ownedThrowerRule, uuid, 'installed fixture owns exactly its declared thrower rule');
    assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE extension_name='${fixtureName}'`), '2');
    assert.equal(sql(`SELECT count(*) FROM plinth.capabilities WHERE signature='${throwerSignature}'
        AND namespace='${fixtureName}' AND version=1 AND function='thrower' AND scope='instance'
        AND provider_type='extension' AND extension_name='${fixtureName}' AND rbac_rule='${throwerRule}'
        AND enabled`), '1');
    assert.equal(sql(`SELECT count(*) FROM plinth.capabilities WHERE extension_name='${fixtureName}'`), '1');
    assert.equal(sql(`SELECT count(*) FROM plinth.group_rules gr JOIN plinth.rbac_rules r ON r.id=gr.rule_id
        WHERE r.rule IN ('${panelRule}','${throwerRule}')`), '0', 'fixture declares no default/everyone grants');
    sql(`INSERT INTO plinth.group_rules(group_id,rule_id) VALUES
        ('${group}','${ownedPanelRule}'), ('${group}','${ownedThrowerRule}')`);
    assert.equal(sql(`SELECT count(*) FROM (${effective}) rules WHERE rule='${panelRule}'`), '1');
    assert.equal(sql(`SELECT count(*) FROM (${effective}) rules WHERE rule='${throwerRule}'`), '1');
    assert.equal(sql(`SELECT count(*) FROM plinth.group_rules WHERE rule_id IN
        ('${ownedPanelRule}','${ownedThrowerRule}') AND group_id<>'${group}'`), '0');
    assert.equal(sql(`SELECT count(*) FROM (${effective}) rules WHERE rule IN ('kernel.admin','kernel.config.get')`), '0');
    completed.push('I01 real package install');
    browser = await bounded(() => chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] }), 'browser launch');
    context = await bounded(() => browser.newContext(), 'browser context');
    await context.addCookies([
        { name: 'plinth_session', value: tokens[1], url: baseURL, httpOnly: true,
            sameSite: 'Strict', secure: target.protocol === 'https:' },
        { name: 'plinth_csrf', value: csrf[1], url: baseURL, httpOnly: false,
            sameSite: 'Strict', secure: target.protocol === 'https:' },
    ]);
    page = await context.newPage(); page.setDefaultTimeout(15000); page.setDefaultNavigationTimeout(15000);
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => {
        if (new URL(request.url()).pathname === '/api/cap/shell.preferences.get' &&
            request.postDataJSON()?.args?.key === 'shell.theme') snapshots.push(request);
    });
    await page.goto(baseURL + '/app/');
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    const application = await bounded(() => page.evaluate(async name => {
        const response = await fetch('/api/frontend/applications', { cache: 'no-store' });
        if (response.status !== 200) throw new Error('fixture catalog unavailable');
        return (await response.json()).applications.find(item => item.id === name);
    }, fixtureName), 'fixture discovery');
    assert(application); assert.equal(application.version, '0.1.0');
    assert.equal(application.generation, packageId); assert.equal(application.panels.length, 1);
    assert.equal(application.panels[0].id, 'demo');
    assert.equal(application.panels[0].module_url, `/ext/${fixtureName}/0.1.0/panels/demo.js`);
    // This observation-only wrapper records real JSON completion. The extra
    // subscriber witnesses the Launcher write even before panel effects start.
    await page.evaluate(async channel => {
        const sdk = await import('@plinth/frontend/sdk'); window.__installedSdk = sdk;
        window.__installedProbe = { ready: false, errors: [], events: [], completed: 0 };
        window.__installedOriginalFetch = window.fetch;
        window.fetch = async (...args) => {
            const response = await window.__installedOriginalFetch(...args);
            if (new URL(args[0], location.href).pathname === '/api/cap/shell.preferences.get' &&
                JSON.parse(args[1]?.body || '{}').args?.key === 'shell.theme') {
                const json = response.json.bind(response);
                response.json = async () => { const value = await json(); window.__installedProbe.completed++; return value; };
            }
            return response;
        };
        window.__installedRemoveProbe = sdk.subscribe(channel, event => {
            if (window.__installedProbe.events.length === 32) {
                window.__installedProbe.errors.push('event overflow'); return;
            }
            window.__installedProbe.events.push(event);
        }, { onReady: () => { window.__installedProbe.ready = true; },
            onError: error => window.__installedProbe.errors.push(error.code) });
    }, channel);
    await page.waitForFunction(() => window.__installedProbe.ready || window.__installedProbe.errors.length);
    assert.deepEqual(await page.evaluate(() => window.__installedProbe.errors), []);
    const prior = maxSequence();
    const loadedPromise = observed(page.waitForResponse(response => new URL(response.url()).pathname === application.panels[0].module_url));
    const initialPromise = observed(page.waitForResponse(response =>
        new URL(response.url()).pathname === '/api/cap/shell.preferences.get' &&
        response.request().postDataJSON()?.args?.key === 'shell.theme'));
    const launcherPromise = observed(page.waitForResponse(response =>
        new URL(response.url()).pathname === '/api/cap/shell.preferences.set' &&
        response.request().postDataJSON()?.args?.key === 'shell.launcher'));
    await page.getByRole('button', { name: application.title, exact: true }).click();
    const loaded = await loadedPromise; assert.equal(loaded.status(), 200);
    assert.deepEqual(await bounded(() => loaded.body(), 'installed module body'), source, 'installed module equals current fixture bytes');
    const initial = await initialPromise; assert.equal(initial.status(), 200);
    assert.deepEqual(initial.request().postDataJSON(), { args: { key: 'shell.theme' } });
    const initialBody = await bounded(() => initial.json(), 'initial snapshot body');
    assert.equal(initialBody.ok, true); assert.deepEqual(initialBody.value, {});
    const launcher = await launcherPromise; assert.equal(launcher.status(), 200);
    assert.equal((await bounded(() => launcher.json(), 'launcher write body')).ok, true);
    await page.locator('#sdk-demo-panel').waitFor({ state: 'visible' });
    await page.waitForFunction(() => document.getElementById('sdk-demo-raw-ready')?.textContent === 'true' &&
        document.getElementById('sdk-demo-loading')?.textContent === 'false');
    assert.equal(await page.locator('#sdk-demo-theme').textContent(), '(unset)');
    assert.equal(await page.locator('#sdk-demo-error').textContent(), 'none');
    assert.equal(await page.locator('#sdk-demo-raw-error').textContent(), 'none');
    assert.equal(await page.locator('#sdk-demo-activations').textContent(), '1');
    assert.equal(await page.locator('#sdk-demo-deactivations').textContent(), '0');
    completed.push('I02 ordinary Launcher panel/C01 actual initial snapshot');
    const launcherEvent = await persisted(prior);
    assert(Number.isSafeInteger(launcherEvent.seq) && launcherEvent.seq > prior);
    assert.equal(launcherEvent.payload.channel, channel);
    assert.equal(launcherEvent.payload.schema, 'ext_shell'); assert.equal(launcherEvent.payload.table, 'user_preferences');
    assert.deepEqual(launcherEvent.payload.ops,
        [{ op: 'insert', count: 1 }, { op: 'update', count: 0 }, { op: 'delete', count: 0 }]);
    assert.deepEqual(JSON.parse(sql(`SELECT value::text FROM ext_shell.user_preferences
        WHERE user_id='${consumer}' AND key='shell.launcher'`)), launcher.request().postDataJSON().args.value);
    await page.waitForFunction(seq => window.__installedProbe.events.some(event => event.payload?.seq === seq), launcherEvent.seq);
    const paint = () => page.evaluate(() => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done))));
    await paint();
    const rawBaseline = Number(await page.locator('#sdk-demo-raw-count').textContent());
    assert([0, 1].includes(rawBaseline), 'only the known launcher event may precede the baseline');
    const initialReads = 1 + rawBaseline;
    await page.waitForFunction(expected => window.__installedProbe.completed === expected, initialReads);
    await paint();
    assert.equal(snapshots.length, initialReads, 'one initial read plus one fallback for a delivered launcher event');
    assert.equal(await page.locator('#sdk-demo-theme').textContent(), '(unset)');
    assert.deepEqual(await page.evaluate(() => window.__installedProbe.errors), []);
    const beforeSnapshots = snapshots.length, beforeSequence = maxSequence();
    const outcome = await bounded(() => page.evaluate(() => window.__installedSdk.call(
        'shell.preferences.set', { key: 'shell.theme', value: 'dark' })), 'native SDK theme write');
    assert.equal(outcome.ok, true);
    const native = await persisted(beforeSequence);
    assert(Number.isSafeInteger(native.seq) && native.seq > beforeSequence);
    assert.equal(native.payload.channel, channel); assert.equal(native.payload.schema, 'ext_shell');
    assert.equal(native.payload.table, 'user_preferences');
    assert.deepEqual(native.payload.ops, [{ op: 'insert', count: 1 }, { op: 'update', count: 0 }, { op: 'delete', count: 0 }]);
    assert(native.payload.ops.every(op => !Object.hasOwn(op, 'ids')), 'native proof must remain counts-only');
    await page.waitForFunction(seq => window.__installedProbe.events.some(event => event.payload?.seq === seq) &&
        document.getElementById('sdk-demo-raw-seq')?.textContent === String(seq), native.seq);
    const raw = await page.evaluate(seq => window.__installedProbe.events.find(event => event.payload?.seq === seq), native.seq);
    assert.equal(raw.channel, channel); assert.deepEqual(raw.payload.ops, native.payload.ops);
    await page.waitForFunction(() => document.getElementById('sdk-demo-theme')?.textContent === 'dark');
    await page.waitForFunction(expected => window.__installedProbe.completed === expected, beforeSnapshots + 1);
    assert.equal(snapshots.length, beforeSnapshots + 1, 'one isolated native event causes one real requery');
    assert.equal(Number(await page.locator('#sdk-demo-raw-count').textContent()), rawBaseline + 1);
    assert.equal(await page.locator('#sdk-demo-error').textContent(), 'none');
    assert.equal(sql(`SELECT value::text FROM ext_shell.user_preferences
        WHERE user_id='${consumer}' AND key='shell.theme'`), '"dark"');
    completed.push('I03 SDK write/native counts/raw subscription/same-panel smart UI');
    async function rejected(capability, code, status) {
        const responsePromise = observed(page.waitForResponse(response => new URL(response.url()).pathname === '/api/cap/' + capability));
        const result = await bounded(() => page.evaluate(async capability => {
            const sdk = window.__installedSdk;
            try { await sdk.call(capability); return { resolved: true }; }
            catch (error) { return { name: error.name, code: error.code,
                typed: error instanceof sdk.CapabilityError, message: error.message }; }
        }, capability), 'native typed SDK rejection');
        assert.deepEqual({ name: result.name, code: result.code, typed: result.typed },
            { name: 'CapabilityError', code, typed: true });
        assert.equal(typeof result.message, 'string'); assert(result.message.length > 0);
        const response = await responsePromise; assert.equal(response.status(), status);
        assert.deepEqual(response.request().postDataJSON(), { args: null });
        const actual = (await bounded(() => response.json(), 'typed error body')).error;
        assert.equal(actual.code, code); assert.equal(result.message, actual.message);
        if (code === 'cap.handler_threw') {
            assert.match(result.message, /TypeError/);
            assert.match(result.message, /sdkdemo controlled handler failure/);
        }
    }
    await rejected('kernel.config.get', 'rbac_denied', 403); completed.push('C02 native non-admin RBAC');
    await rejected('shell.issue32absent', 'not_found', 404); completed.push('C03 native missing capability');
    await rejected('sdkdemo.thrower', 'cap.handler_threw', 500); completed.push('C05 native controlled handler throw taxonomy');
    const malformedPromise = observed(page.waitForResponse(response =>
        new URL(response.url()).pathname === '/api/cap/shell.preferences.get_all' && response.request().postData() === '{}'));
    const controls = await bounded(() => page.evaluate(async () => {
        const sdk = window.__installedSdk, original = window.fetch;
        let failedAttempts = 0, malformedAttempts = 0, ordinaryBody, network, malformed;
        try {
            window.fetch = (...args) => {
                if (new URL(args[0], location.href).pathname === '/api/cap/shell.preferences.get_all') {
                    failedAttempts++; return Promise.reject(new TypeError('fake controlled transport failure'));
                }
                return original(...args);
            };
            try { await sdk.call('shell.preferences.get_all'); network = { resolved: true }; }
            catch (error) { network = { name: error.name, typed: error instanceof sdk.NetworkError, cause: error.cause?.name }; }
        } finally { window.fetch = original; }
        try {
            window.fetch = (url, options) => {
                if (new URL(url, location.href).pathname === '/api/cap/shell.preferences.get_all') {
                    malformedAttempts++; ordinaryBody = options.body; return original(url, { ...options, body: '{}' });
                }
                return original(url, options);
            };
            try { await sdk.call('shell.preferences.get_all'); malformed = { resolved: true }; }
            catch (error) { malformed = { name: error.name, code: error.code, typed: error instanceof sdk.CapabilityError }; }
        } finally { window.fetch = original; }
        const value = await sdk.call('shell.preferences.get_all');
        return { network, malformed, failedAttempts, malformedAttempts, ordinaryBody, restored: window.fetch === original,
            successfulControl: value.entries.some(entry => entry.key === 'shell.theme' && entry.value === 'dark') };
    }), 'restored platform fetch controls');
    assert.deepEqual(controls, { network: { name: 'NetworkError', typed: true, cause: 'TypeError' },
        malformed: { name: 'CapabilityError', code: 'bad_request', typed: true }, failedAttempts: 1,
        malformedAttempts: 1, ordinaryBody: '{"args":null}', restored: true, successfulControl: true });
    const malformedResponse = await malformedPromise; assert.equal(malformedResponse.status(), 400);
    assert.equal((await bounded(() => malformedResponse.json(), 'missing args body')).error.code, 'bad_request');
    completed.push('C04 deterministic NetworkError/restored fetch', 'C06 native missing args/normal null success');
    await page.evaluate(() => document.dispatchEvent(new KeyboardEvent('keydown', {
        key: 'D', ctrlKey: true, shiftKey: true, bubbles: true, cancelable: true,
    })));
    await page.waitForFunction(() => document.getElementById('sdk-demo-shortcuts')?.textContent === '1');
    await page.getByRole('button', { name: 'Home', exact: true }).click();
    await page.waitForFunction(() => document.getElementById('sdk-demo-deactivations')?.textContent === '1');
    assert.equal(await page.locator('#sdk-demo-activations').textContent(), '1');
    completed.push('L01/L02 exact activation/deactivation and DOM shortcut dispatch');
    assert.deepEqual(errors, []); assert.equal(completed.length, 9);
} catch (error) { failure = error; }

// Independent attempts for every exactly owned resource, including failures.
const cleanup = [];
try {
    if (page && !page.isClosed()) await bounded(() => page.evaluate(() => {
        window.__installedRemoveProbe?.();
        if (window.__installedOriginalFetch) window.fetch = window.__installedOriginalFetch;
    }), 'observer cleanup', 5000);
} catch (error) { cleanup.push(error); }
try { if (context) await bounded(() => context.close(), 'context cleanup', 5000); }
catch (error) { cleanup.push(error); }
try { if (browser) await bounded(() => browser.close(), 'browser cleanup', 5000); assert(!browser?.isConnected()); }
catch (error) { cleanup.push(error); }
try {
    if (seeded) {
        const owned = sql(`SELECT id FROM plinth.packages WHERE name='${fixtureName}' AND installed_by_user_id='${operator}'`);
        if (owned) {
            assert.match(owned, uuid, 'one owned package generation');
            if (packageId) assert.equal(owned, packageId);
            await lifecycle(`/api/packages/${owned}?confirm=true`, { method: 'DELETE' }, 204);
        }
        assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE name='${fixtureName}' AND installed_by_user_id='${operator}'`), '0');
        assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule='${panelRule}'
            AND extension_name='${fixtureName}'`), '0', 'normal uninstall removes its declared fixture rule');
        assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule='${throwerRule}'`), '0',
            'normal uninstall removes its declared thrower rule');
        assert.equal(sql(`SELECT count(*) FROM plinth.capabilities WHERE signature='${throwerSignature}'`), '0',
            'normal uninstall removes its declared thrower capability');
    }
} catch (error) { cleanup.push(error); }
try {
    if (seeded) {
        sql(`BEGIN; DELETE FROM ext_shell.user_preferences WHERE user_id IN ('${operator}','${consumer}');
            DELETE FROM plinth.sessions WHERE id IN ('${sessions[0]}','${sessions[1]}');
            DELETE FROM plinth.group_rules WHERE group_id='${group}';
            DELETE FROM plinth.group_members WHERE user_id IN ('${operator}','${consumer}');
            DELETE FROM plinth.groups WHERE id='${group}';
            DELETE FROM plinth.users WHERE id IN ('${operator}','${consumer}'); COMMIT;`);
        assert.equal(sql(`SELECT (SELECT count(*) FROM plinth.users WHERE id IN ('${operator}','${consumer}'))
            +(SELECT count(*) FROM plinth.sessions WHERE id IN ('${sessions[0]}','${sessions[1]}'))
            +(SELECT count(*) FROM plinth.groups WHERE id='${group}')`), '0');
        if (createdRule && ruleId) {
            sql(`DELETE FROM plinth.rbac_rules WHERE id='${ruleId}' AND rule='shell.realtime.subscribe'
                AND NOT EXISTS(SELECT 1 FROM plinth.group_rules WHERE rule_id='${ruleId}')`);
            assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE id='${ruleId}'`), '0');
        }
    }
} catch (error) { cleanup.push(error); }
if (failure || cleanup.length) throw new AggregateError([...(failure ? [failure] : []), ...cleanup], 'installed SDK fixture');
console.log(JSON.stringify({ installedSdkCases: completed.length, expectedCases: 9, nonAdminConsumer: true,
    nativeCountsOnly: true, subsequentSnapshotDelta: 1, fixtureModuleSha256: moduleHash, cleanup: 'PASS',
    scope: 'actual installed fixture/SDK/Launcher/HTTP/PG/events; C04/C06 use restored platform fetch seams',
    handlerThrowTaxonomy: 'cap.handler_threw' }));
