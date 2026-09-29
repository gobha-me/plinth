// Joined production proof: no request routing, mocks, injected production code,
// synthetic consumer login, or second database. Only the install operator is
// seeded with a fake session; the ordinary consumer uses real register/login.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash, createHmac, randomBytes, randomUUID } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
const profile = process.env.PLINTH_BROWSER_PROFILE_DIR;
const conflictZip = process.env.PLINTH_CLIENT_CONFLICT_ZIP;
assert(baseURL && profile && conflictZip, 'run through run-client-contracts.py with owned resources');
const origin = new URL(baseURL).origin;
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env['PLINTH_PG_' + suffix], `PLINTH_PG_${suffix} is required`);
    pgEnv['PG' + suffix] = process.env['PLINTH_PG_' + suffix];
}
const literal = value => "'" + String(value).replaceAll("'", "''") + "'";
function sql(statement) {
    return execFileSync('psql', ['-XAt', '-v', 'ON_ERROR_STOP=1', '-c', statement],
        { env: pgEnv, encoding: 'utf8', timeout: 15000, stdio: ['ignore', 'pipe', 'pipe'] }).trim();
}
async function bounded(operation, label, milliseconds = 10000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label} exceeded ${milliseconds}ms`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
}
const key = 'browser.client_contracts.persist';
const raceKey = 'browser.client_contracts.concurrent';
const value = { owner: 'registered-consumer', nested: ['persisted', 32] };
const username = 'client_contract_' + randomUUID().replaceAll('-', '');
const password = 'fake-client-contract-password-2026';
const operator = randomUUID(), operatorSession = randomUUID();
const operatorToken = randomBytes(32).toString('base64url');
const operatorHash = createHash('sha256').update(operatorToken).digest('hex');
const operatorCsrf = createHmac('sha256', operatorToken).update('plinth.csrf.v1').digest('base64url');
// Open registration requires an existing account and never grants bootstrap
// authority. This separate operator is deliberately not the browser consumer.
sql(`INSERT INTO plinth.users(id,username,password_hash) VALUES
    (${literal(operator)},${literal('operator_' + operator)},'not-a-valid-password-hash');
    INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES
    (${literal(operatorSession)},${literal(operator)},${literal(operatorHash)});
    INSERT INTO plinth.group_members(group_id,user_id)
    SELECT id,${literal(operator)} FROM plinth.groups WHERE name='admin';`);
const registration = await fetch(baseURL + '/api/auth/register', {
    method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password }), signal: AbortSignal.timeout(15000),
});
assert.equal(registration.status, 202);
assert.deepEqual(await registration.json(), { status: 'processed' });
const user = sql(`SELECT id FROM plinth.users WHERE username=${literal(username)}`);
assert.match(user, /^[0-9a-f-]{36}$/);
assert.notEqual(user, operator);
assert.equal(sql(`SELECT count(*) FROM plinth.groups g JOIN plinth.group_rules gr ON gr.group_id=g.id
    JOIN plinth.rbac_rules r ON r.id=gr.rule_id WHERE r.rule='kernel.admin' AND r.orphaned_at IS NULL
    AND (g.name='everyone' OR EXISTS (SELECT 1 FROM plinth.group_members gm
        WHERE gm.group_id=g.id AND gm.user_id=${literal(user)}))`), '0',
'real registered consumer must remain nonadmin, including virtual everyone membership');
const frontend = () => JSON.parse(sql("SELECT row_to_json(p)::text FROM (SELECT id,name,version,state,provenance,frontend_mount,frontend_entry FROM plinth.packages WHERE name='shell' AND state IN ('ACTIVE','ACTIVE_FLAGGED')) p"));
const installed = frontend();
assert.equal(installed.frontend_mount, '/app');
assert.equal(installed.provenance, 'bundled');
const preferences = () => JSON.parse(sql(`SELECT coalesce(json_agg(p ORDER BY key),'[]'::json)::text FROM
    (SELECT key,value,updated_at FROM ext_shell.user_preferences WHERE user_id=${literal(user)}
    AND key IN (${[key, raceKey, 'shell.theme', 'shell.scale_pct'].map(literal).join(',')})) p`));

async function tokens(reader) {
    const redirect = await reader('/api/frontend/tokens.css', false);
    assert.equal(redirect.status, 302);
    assert.equal(redirect.headers['cache-control'], 'no-cache');
    const location = redirect.headers.location;
    assert.equal(location, `/ext/shell/${installed.version}/css/tokens.css`);
    const asset = await reader(location, true);
    assert.equal(asset.status, 200);
    assert.equal(asset.headers['cache-control'], 'public, max-age=31536000, immutable');
    assert.match(asset.body, /:root\s*\{/);
    assert.match(asset.body, /--bg-0:\s*#0b0f14/);
    assert.match(asset.body, /:root\[data-theme="light"\]/);
    assert.match(asset.body, /--bg-0:\s*#f7f7f4/);
    return asset.body;
}
const guest = async (path, body) => {
    const response = await fetch(baseURL + path, { redirect: 'manual', signal: AbortSignal.timeout(10000) });
    return { status: response.status, headers: Object.fromEntries(response.headers),
        body: body ? await response.text() : null };
};
const commands = createInterface({ input: process.stdin });
const commandIterator = commands[Symbol.asyncIterator]();
let context, browser, failure;
const cleanupErrors = [];
try {
    context = await chromium.launchPersistentContext(profile, {
        executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    });
    browser = context.browser();
    const page = await context.newPage();
    page.setDefaultTimeout(15000);
    const pageErrors = [], raceStatuses = [], hydrations = [];
    page.on('pageerror', error => pageErrors.push(error.message));
    page.on('response', response => {
        const path = new URL(response.url()).pathname;
        if (path === '/api/cap/shell.preferences.get_all') hydrations.push(response);
        if (path === '/api/cap/shell.preferences.set' && response.request().postDataJSON()?.args?.key === raceKey) {
            raceStatuses.push(response.status());
        }
    });
    const authenticated = async (path, body) => {
        // Production login issues Secure cookies. Chromium accepts those on
        // trusted loopback; Playwright's separate API client does not. Use
        // the actual logged-in browser and observe the real redirect response.
        const observed = page.waitForResponse(response => new URL(response.url()).pathname === path);
        const fetched = page.evaluate(async path => {
            const response = await fetch(path, { credentials: 'same-origin' });
            return await response.text();
        }, path);
        const response = await observed;
        const bytes = await fetched;
        return { status: response.status(), headers: response.headers(), body: body ? bytes : null };
    };
    const call = (capability, args) => page.evaluate(async ({ capability, args }) =>
        (await import('@plinth/frontend/sdk')).call(capability, args), { capability, args });
    await page.goto(baseURL + '/app/');
    await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
    await page.locator('input[name=username]').fill(username);
    await page.locator('input[name=password]').fill(password);
    const login = page.waitForResponse(response => new URL(response.url()).pathname === '/api/auth/login');
    await page.getByRole('button', { name: 'Sign In', exact: true }).click();
    assert.equal((await login).status(), 200);
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    await page.waitForFunction(() => localStorage.getItem('shellPrefs') !== null);
    const sessionResponse = await authenticated('/api/auth/session', true);
    assert.equal(sessionResponse.status, 200);
    assert.equal(JSON.parse(sessionResponse.body).user.id, user);
    const deniedAdmin = await authenticated('/api/auth/invites', true);
    assert.equal(deniedAdmin.status, 403, 'registered consumer must fail a real kernel.admin gate');
    const originalTokens = await tokens(authenticated);
    assert.equal(await tokens(guest), originalTokens, 'unauthenticated CSS bytes must match');
    const originalShell = (await authenticated('/app/', true)).body;
    assert.equal((await call('shell.preferences.set', { key, value })).ok, true);
    const simultaneous = await page.evaluate(async raceKey => {
        const sdk = await import('@plinth/frontend/sdk');
        return Promise.all(['one', 'two'].map(writer => sdk.call('shell.preferences.set', {
            key: raceKey, value: { writer },
        })));
    }, raceKey);
    assert.deepEqual(simultaneous, [{ ok: true }, { ok: true }]);
    assert.deepEqual(raceStatuses, [200, 200], 'both real simultaneous HTTP writes must succeed');
    const raceRows = JSON.parse(sql(`SELECT coalesce(json_agg(value),'[]'::json)::text
        FROM ext_shell.user_preferences WHERE user_id=${literal(user)} AND key=${literal(raceKey)}`));
    assert.equal(raceRows.length, 1, 'UPSERT must leave exactly one row');
    assert(['one', 'two'].includes(raceRows[0].writer));
    assert.deepEqual(raceRows, [{ writer: raceRows[0].writer }], 'stored JSON must be exactly one submitted value');
    const winner = raceRows[0];
    await page.locator('.zone-avatar > button').click();
    async function choose(selector, preference, selection, expected) {
        const response = page.waitForResponse(response => new URL(response.url()).pathname ===
            '/api/cap/shell.preferences.set' && response.request().postDataJSON()?.args?.key === preference);
        await page.locator(selector).selectOption(selection);
        const saved = await response;
        assert.equal(saved.status(), 200); assert.equal((await saved.json()).ok, true);
        await page.waitForFunction(({ preference, expected }) =>
            JSON.parse(localStorage.getItem('shellPrefs') || '{}')[preference] === expected,
        { preference, expected });
    }
    await choose('#shell-theme-select', 'shell.theme', 'light', 'light');
    await choose('#shell-scale-select', 'shell.scale_pct', '125', 125);
    const hydrated = () => page.waitForFunction(() => document.documentElement.dataset.theme === 'light' &&
        document.documentElement.style.fontSize === '16.875px' &&
        JSON.parse(localStorage.getItem('shellPrefs') || '{}')['shell.theme'] === 'light' &&
        JSON.parse(localStorage.getItem('shellPrefs') || '{}')['shell.scale_pct'] === 125);
    await hydrated();
    assert.equal(await tokens(authenticated), originalTokens, 'preferences must not specialize CSS');
    assert.equal(await tokens(guest), originalTokens, 'unauthenticated CSS stays user-independent after writes');
    const durable = preferences();
    assert.equal(durable.length, 4);
    assert.deepEqual(Object.fromEntries(durable.map(row => [row.key, row.value])), {
        [key]: value, [raceKey]: winner, 'shell.theme': 'light', 'shell.scale_pct': 125,
    });
    const form = new FormData();
    form.append('package', new Blob([await readFile(conflictZip)]), 'conflicting-frontend.zip');
    const rejected = await fetch(baseURL + '/api/packages', { method: 'POST', body: form,
        headers: { Origin: origin, Cookie: `plinth_session=${operatorToken}; plinth_csrf=${operatorCsrf}`,
            'X-Plinth-CSRF': operatorCsrf }, signal: AbortSignal.timeout(30000) });
    // RT2 is the active-mount UNIQUE constraint, reached at ACTIVATING. The
    // real handler maps this non-validation failure to 500, not guessed 409.
    assert.equal(rejected.status, 500);
    const rejection = await rejected.json();
    assert.equal(rejection.state, 'INSTALL_FAILED');
    assert.equal(rejection.failed_at_stage, 'ACTIVATING');
    assert.equal(rejection.kind, 'activation-failed');
    assert.deepEqual(frontend(), installed);
    assert.equal(sql("SELECT count(*) FROM plinth.packages WHERE frontend_mount IS NOT NULL AND state IN ('ACTIVE','ACTIVE_FLAGGED')"), '1');
    assert.equal((await authenticated('/app/', true)).body, originalShell, 'rejected install must not replace bundled shell');
    assert.equal(await tokens(authenticated), originalTokens);
    assert.deepEqual(pageErrors, []);

    // The same persistent browser context remains live while Python signals,
    // stops, and replaces this exact kernel against the SAME root and database.
    console.log('ready_for_client_restart');
    const command = await bounded(() => commandIterator.next(), 'kernel restart command', 120000);
    assert.equal(command.value, 'continue');
    const priorHydrations = hydrations.length;
    await page.evaluate(() => localStorage.setItem('shellPrefs', JSON.stringify({
        'shell.theme': 'dark', 'shell.scale_pct': 80,
    })));
    await page.reload();
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    await hydrated();
    assert(hydrations.length > priorHydrations, 'reload must perform authoritative real get_all hydration');
    const hydration = hydrations.at(-1);
    assert.equal(hydration.status(), 200); assert.equal((await hydration.json()).ok, true);
    assert.deepEqual(await call('shell.preferences.get', { key }), { value });
    assert.deepEqual(await call('shell.preferences.get', { key: raceKey }), { value: winner });
    assert.deepEqual(await call('shell.preferences.get', { key: 'shell.theme' }), { value: 'light' });
    assert.deepEqual(await call('shell.preferences.get', { key: 'shell.scale_pct' }), { value: 125 });
    assert.deepEqual(preferences(), durable, 'restart/read/hydration must preserve the exact persisted rows');
    assert.deepEqual(frontend(), installed);
    assert.equal((await authenticated('/app/', true)).body, originalShell);
    assert.equal(await tokens(authenticated), originalTokens);
    assert.equal(await tokens(guest), originalTokens);
    assert.deepEqual(pageErrors, []);
} catch (error) { failure = error; }
finally {
    commands.close();
    try { await bounded(() => context?.close(), 'persistent browser context cleanup'); }
    catch (error) { cleanupErrors.push(error); }
    // Attempt this independently even if context.close rejected or timed out.
    try { await bounded(() => browser?.close(), 'browser process cleanup'); }
    catch (error) { cleanupErrors.push(error); }
    if (browser?.isConnected()) cleanupErrors.push(new Error('browser remained connected after cleanup'));
}
if (failure || cleanupErrors.length) throw new AggregateError(
    [...(failure ? [failure] : []), ...cleanupErrors], 'joined real client contracts failed');
console.log('PASS joined real client contracts: register/login, concurrent UPSERT, UI mirror, CSS, mount rejection, same-database restart/hydration; browser cleanup PASS');
