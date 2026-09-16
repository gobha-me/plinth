import assert from 'node:assert/strict';
import { createHash, randomBytes, randomUUID } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
const buildDir = process.env.PLINTH_TEST_BUILD_DIR;
assert(baseURL, 'PLINTH_BASE_URL must name a task-owned production kernel');
assert(buildDir, 'PLINTH_TEST_BUILD_DIR must name the matching build tree');
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const suffix of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env['PLINTH_PG_' + suffix], `PLINTH_PG_${suffix} is required`);
    pgEnv['PG' + suffix] = process.env['PLINTH_PG_' + suffix];
}
function sql(statement) {
    return execFileSync('psql', ['-X', '-v', 'ON_ERROR_STOP=1', '-A', '-t', '-c', statement],
        { env: pgEnv, encoding: 'utf8', timeout: 15000 }).trim();
}

const user = randomUUID();
const session = randomUUID();
const token = randomBytes(32).toString('base64url');
const hash = createHash('sha256').update(token).digest('hex');
sql(`INSERT INTO plinth.users(id,username,password_hash) VALUES
    ('${user}','launcher-${user}','not-a-password-hash');
    INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES ('${session}','${user}','${hash}');
    INSERT INTO plinth.group_members(group_id,user_id)
    SELECT id,'${user}' FROM plinth.groups WHERE name='admin';`);
const cookie = { Cookie: `plinth_session=${token}` };

async function lifecycle(path, init, expected) {
    const deadline = Date.now() + 15000;
    while (true) {
        const response = await fetch(baseURL + path, {
            ...init,
            headers: { ...cookie, ...(init?.headers || {}) },
        });
        if (expected.includes(response.status)) return response;
        if (response.status !== 409 || Date.now() >= deadline) {
            throw new Error(`${init?.method || 'GET'} ${path} returned ${response.status}: ${await response.text()}`);
        }
        await new Promise(resolve => setTimeout(resolve, 25));
    }
}

const browser = await chromium.launch({
    executablePath: process.env.PLINTH_BROWSER || undefined,
    args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
});
try {
    const context = await browser.newContext();
    await context.addCookies([{ name: 'plinth_session', value: token, url: baseURL,
        httpOnly: true, sameSite: 'Strict', secure: baseURL.startsWith('https:') }]);
    const page = await context.newPage();
    const errors = [];
    const realtimeFrames = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('websocket', socket => socket.on('framereceived', event => {
        if (typeof event.payload === 'string') realtimeFrames.push(event.payload);
    }));
    await page.goto(baseURL + '/app/');
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    await page.getByText('No applications are available.', { exact: true }).waitFor();

    const archive = await readFile(join(buildDir, 'fixtures', 'valid-install.zip'));
    const form = new FormData();
    form.append('package', new Blob([archive]), 'notes.zip');
    const installed = await lifecycle('/api/packages', { method: 'POST', body: form }, [201]);
    const record = await installed.json();
    let activeRecord = record;
    assert.equal(record.name, 'notes');
    await page.getByRole('button', { name: 'Notes', exact: true }).waitFor({ timeout: 15000 });

    let moduleLoads = 0;
    page.on('response', response => {
        if (/\/ext\/notes\/1\.2\.3\/panels\/editor\.js$/.test(new URL(response.url()).pathname)) {
            assert.equal(response.status(), 200);
            moduleLoads++;
        }
    });
    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.locator('.panel-container:not([hidden])').waitFor();
    assert.equal(moduleLoads, 1, 'the real versioned panel module must load once');

    await lifecycle(`/api/packages/${record.id}`, {
        method: 'PATCH', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'disable' }),
    }, [200]);
    await page.getByText('No applications are available.', { exact: true }).waitFor({ timeout: 15000 });
    assert.equal(await page.locator('.panel-container').count(), 0,
        'disable invalidation must remove retained panel DOM');

    await lifecycle(`/api/packages/${record.id}`, {
        method: 'PATCH', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'enable' }),
    }, [200]);
    try {
        await page.getByRole('button', { name: 'Notes', exact: true }).waitFor({ timeout: 15000 });
    } catch (error) {
        const catalog = await page.evaluate(async () => {
            const response = await fetch('/api/frontend/applications', { cache: 'no-store' });
            return { status: response.status, body: await response.text() };
        });
        throw new Error(`enable invalidation was not rendered; catalog=${JSON.stringify(catalog)} frames=${JSON.stringify(realtimeFrames.slice(-20))}`, { cause: error });
    }

    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.locator('.panel-container:not([hidden])').waitFor();
    let upgradedModuleLoads = 0;
    page.on('response', response => {
        if (/\/ext\/notes\/1\.3\.0\/panels\/editor\.js$/.test(new URL(response.url()).pathname)) {
            assert.equal(response.status(), 200);
            upgradedModuleLoads++;
        }
    });
    const upgradeArchive = await readFile(join(buildDir, 'fixtures', 'upgrade-v2.zip'));
    const upgradeForm = new FormData();
    upgradeForm.append('package', new Blob([upgradeArchive]), 'notes-v2.zip');
    const upgraded = await lifecycle('/api/packages', { method: 'POST', body: upgradeForm }, [201]);
    activeRecord = await upgraded.json();
    assert.equal(activeRecord.name, 'notes');
    assert.equal(activeRecord.version, '1.3.0');
    assert.notEqual(activeRecord.id, record.id, 'upgrade must publish a new generation');
    try {
        await page.locator('.panel-container').waitFor({ state: 'detached', timeout: 15000 });
    } catch (error) {
        const catalog = await page.evaluate(async () => {
            const response = await fetch('/api/frontend/applications', { cache: 'no-store' });
            return { status: response.status, body: await response.text() };
        });
        throw new Error(`upgrade invalidation did not remove the old panel; catalog=${JSON.stringify(catalog)} frames=${JSON.stringify(realtimeFrames.slice(-30))}`, { cause: error });
    }
    await page.getByRole('button', { name: 'Notes', exact: true }).click();
    await page.locator('.panel-container:not([hidden])').waitFor();
    assert.equal(upgradedModuleLoads, 1,
        'upgrade must discard old DOM and import the new versioned module once');

    await lifecycle(`/api/packages/${activeRecord.id}?confirm=true`, { method: 'DELETE' }, [204]);
    await page.getByText('No applications are available.', { exact: true }).waitFor({ timeout: 15000 });
    assert.deepEqual(errors, []);
    await context.close();
    console.log('PASS production launcher SessionFilter, RBAC, install/disable/enable/upgrade/uninstall, realtime, assets');
} finally {
    await browser.close();
}
