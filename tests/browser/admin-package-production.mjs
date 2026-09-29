// Actual installed admin package against one task-owned kernel/database.
// The wrapper owns native/image runtime, database, socket and process cleanup;
// this script owns real accounts, grants, browser contexts and package removal.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { chromium } from 'playwright';

const baseURL = process.env.PLINTH_BASE_URL;
const buildDir = process.env.PLINTH_TEST_BUILD_DIR;
const bootstrapToken = process.env.PLINTH_TEST_BOOTSTRAP_TOKEN;
assert(baseURL && buildDir && bootstrapToken,
    'run through run-admin-package.py with an owned kernel and fake bootstrap token');
const origin = new URL(baseURL).origin;
const archive = join(buildDir, 'packages/admin-0.1.0.zip');
const fixture = name => join(buildDir, `fixtures/${name}.zip`);
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const literal = value => "'" + String(value).replaceAll("'", "''") + "'";
const pgEnv = { ...process.env, PGCONNECT_TIMEOUT: '5' };
for (const key of ['HOST', 'PORT', 'USER', 'PASSWORD', 'DATABASE']) {
    assert(process.env[`PLINTH_PG_${key}`], `PLINTH_PG_${key} is required`);
    pgEnv[`PG${key}`] = process.env[`PLINTH_PG_${key}`];
}
function sql(statement) {
    return execFileSync('psql', ['-XAt', '-v', 'ON_ERROR_STOP=1', '-c', statement],
        { env: pgEnv, encoding: 'utf8', timeout: 15000,
            stdio: ['ignore', 'pipe', 'pipe'] }).trim();
}
function jsonSql(statement) { return JSON.parse(sql(statement)); }
function sha256(bytes) { return createHash('sha256').update(bytes).digest('hex'); }
async function bounded(operation, label, timeout = 10000) {
    let timer;
    try {
        return await Promise.race([Promise.resolve().then(operation), new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label} exceeded ${timeout}ms`)), timeout);
        })]);
    } finally { clearTimeout(timer); }
}
async function publicPost(path, payload) {
    const response = await fetch(baseURL + path, { method: 'POST',
        headers: { Origin: origin, 'Content-Type': 'application/json' },
        body: JSON.stringify(payload), signal: AbortSignal.timeout(15000) });
    return { status: response.status, body: await response.json() };
}
async function browserRequest(page, path, method = 'GET', payload = null) {
    return page.evaluate(async ({ path, method, payload }) => {
        const { withCsrf } = await import('@plinth/frontend/sdk');
        let body;
        if (payload?.file) {
            const bytes = Uint8Array.from(atob(payload.file), c => c.charCodeAt(0));
            body = new FormData();
            body.append('package', new Blob([bytes], { type: 'application/zip' }), payload.name);
        } else if (payload !== null) body = JSON.stringify(payload);
        const prepared = withCsrf(path, { method, credentials: 'same-origin',
            ...(body === undefined ? {} : { body }),
            ...(body && !(body instanceof FormData) ?
                { headers: { 'Content-Type': 'application/json' } } : {}) });
        const response = await fetch(path, prepared);
        const text = await response.text();
        let parsed = null;
        if (text) { try { parsed = JSON.parse(text); } catch { parsed = { invalidJson: true }; } }
        return { status: response.status, body: parsed };
    }, { path, method, payload });
}
async function browserUpload(page, zipPath, dryRun = false) {
    const bytes = await readFile(zipPath);
    return browserRequest(page, `/api/packages${dryRun ? '?dry_run=1' : ''}`, 'POST',
        { file: bytes.toString('base64'), name: zipPath.split('/').at(-1) });
}
const label = randomUUID().replaceAll('-', '').slice(0, 18);
const password = `fake-admin-package-${label}-password`;
const principals = Object.fromEntries(['maintainer', 'reader', 'writer', 'visible', 'denied']
    .map(role => [role, { role, username: `admin50_${role}_${label}`, password, id: null,
        context: null, page: null }]));
const ownedGroups = [];
const ownedGroupByRole = new Map();
const completed = [];
const cleanupErrors = [];
let browser;
let failure;
let adminId;
let notesId;
let slowId;
let archiveHash;
let moduleHash;
const moduleHashes = {};
let kernelAuthority;

function packageRow(name, states = ['ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED']) {
    const stateList = states.map(literal).join(',');
    const rows = jsonSql(`SELECT coalesce(json_agg(p),'[]'::json)::text FROM
        (SELECT id,name,version,state,provenance,installed_by_user_id,manifest_checksum,
            frontend_mount,frontend_entry,application_ready FROM plinth.packages
          WHERE name=${literal(name)} AND state IN (${stateList}) ORDER BY installed_at DESC) p`);
    assert(rows.length <= 1, `one authoritative ${name} generation`);
    return rows[0] || null;
}
function audit(action, name) {
    return jsonSql(`SELECT coalesce(json_agg(a ORDER BY timestamp),'[]'::json)::text FROM
        (SELECT action,detail,timestamp FROM plinth.audit_log
          WHERE action=${literal(action)} AND detail->>'name'=${literal(name)}) a`);
}
function grant(role, rules) {
    const user = principals[role];
    const group = randomUUID();
    const groupName = `admin50_${role}_${label}`;
    assert.match(user.id, uuid);
    ownedGroups.push(group);
    ownedGroupByRole.set(role, group);
    sql(`INSERT INTO plinth.groups(id,name,description) VALUES
        (${literal(group)},${literal(groupName)},'task-owned admin package browser fixture');
        INSERT INTO plinth.group_members(group_id,user_id) VALUES
        (${literal(group)},${literal(user.id)});`);
    for (const rule of rules) {
        assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule=${literal(rule)}
            AND orphaned_at IS NULL`), '1', `grant exact registered rule ${rule}`);
        sql(`INSERT INTO plinth.group_rules(group_id,rule_id)
            SELECT ${literal(group)},id FROM plinth.rbac_rules WHERE rule=${literal(rule)}`);
    }
}
async function login(principal) {
    principal.context = await browser.newContext();
    principal.page = await principal.context.newPage();
    const page = principal.page;
    page.setDefaultTimeout(15000);
    await page.goto(baseURL + '/app/');
    await page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
    await page.locator('input[name=username]').fill(principal.username);
    await page.locator('input[name=password]').fill(principal.password);
    const loginResponse = page.waitForResponse(response =>
        new URL(response.url()).pathname === '/api/auth/login');
    await page.getByRole('button', { name: 'Sign In', exact: true }).click();
    assert.equal((await loginResponse).status(), 200, `real ${principal.role} login`);
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    const session = await browserRequest(page, '/api/auth/session');
    assert.equal(session.status, 200);
    assert.equal(session.body.user.id, principal.id);
    return page;
}
async function openAdmin(page, actor) {
    await page.reload();
    await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    await page.getByRole('button', { name: 'Administration', exact: true }).click();
    await page.locator('#admin-packages-panel').waitFor();
    await page.waitForFunction(username =>
        document.getElementById('admin-phase')?.textContent === 'active' &&
        document.getElementById('admin-actor')?.textContent?.includes(username), actor.username);
}
async function selectPackage(page, id) {
    await page.locator(`#admin-package-list button[data-package-id="${id}"]`).click();
    await page.locator('#admin-detail').getByText(`ID ${id}`, { exact: false }).waitFor();
}
async function chooseZip(page, zipPath) {
    await page.locator('#admin-package-file').setInputFiles(zipPath);
}
async function clickAndResponse(page, button, predicate, expected) {
    const response = page.waitForResponse(predicate);
    await page.locator(button).click();
    const observed = await response;
    assert.equal(observed.status(), expected, `${button} authoritative HTTP status`);
    return observed;
}
async function installWriterFetchProxy(page) {
    // The package API captures globalThis.fetch when its panel factory runs.
    // Install once before that import, then change only these per-window
    // delivery controls. All responses originate from the real kernel.
    await page.addInitScript(() => {
        const actualFetch = window.fetch.bind(window);
        const controls = {
            targetId: null, holdFetch: false, holdJson: false,
            losePatch: false, holdReconcile: false,
            oldFetchState: null, oldJsonState: null, committedStatus: null,
            observedRow: null, releaseFetch: null, releaseJson: null,
            releaseReconcile: null,
        };
        window.__adminFetchControls = controls;
        window.fetch = async (url, options) => {
            const target = new URL(url instanceof Request ? url.url : url, location.href);
            const method = options?.method || (url instanceof Request ? url.method : 'GET');
            if (controls.holdFetch && target.pathname === '/api/packages' && method === 'GET') {
                controls.holdFetch = false;
                const response = await actualFetch(url, options);
                const snapshot = await response.clone().json();
                controls.oldFetchState = snapshot.items.find(row => row.id === controls.targetId)?.state;
                return new Promise(resolve => { controls.releaseFetch = () => resolve(response); });
            }
            if (controls.holdJson && target.pathname === '/api/packages' && method === 'GET') {
                controls.holdJson = false;
                const response = await actualFetch(url, options);
                const snapshot = await response.clone().json();
                controls.oldJsonState = snapshot.items.find(row => row.id === controls.targetId)?.state;
                const parse = response.json.bind(response);
                response.json = () => new Promise((resolve, reject) => {
                    controls.releaseJson = async () => {
                        try {
                            resolve(await parse());
                            return 'parsed';
                        } catch (error) {
                            reject(error); // Settle the old controller's pending JSON read.
                            if (error?.name === 'AbortError') return 'AbortError';
                            throw error;
                        }
                    };
                });
                return response;
            }
            if (controls.losePatch && target.pathname === `/api/packages/${controls.targetId}` &&
                method === 'PATCH') {
                controls.losePatch = false;
                const response = await actualFetch(url, options);
                if (response.status !== 200) return response;
                controls.committedStatus = response.status;
                throw new TypeError('controlled lost reply after actual server commit');
            }
            if (controls.holdReconcile && target.pathname === '/api/packages' &&
                target.searchParams.has('include_failed') && method === 'GET') {
                controls.holdReconcile = false;
                return new Promise((resolve, reject) => {
                    controls.releaseReconcile = async () => {
                        try {
                            const response = await actualFetch(url, options);
                            const body = await response.clone().json();
                            controls.observedRow = body.items.find(row => row.id === controls.targetId);
                            resolve(response);
                        } catch (error) { reject(error); }
                    };
                });
            }
            return actualFetch(url, options);
        };
        window.__adminStableFetch = window.fetch;
    });
}

try {
    const zip = await readFile(archive);
    archiveHash = sha256(zip);
    assert(zip.length > 0 && /^[0-9a-f]{64}$/.test(archiveHash));
    for (const [role, path] of Object.entries({ panel: 'client/panels/packages.js',
        api: 'client/packages/api.js', controller: 'client/packages/controller.js' })) {
        const archived = execFileSync('unzip', ['-p', archive, path],
            { timeout: 10000, stdio: ['ignore', 'pipe', 'pipe'] });
        const source = await readFile(new URL(`../../client/admin/${path}`, import.meta.url));
        moduleHashes[role] = sha256(archived);
        assert.equal(moduleHashes[role], sha256(source),
            `${role} archive bytes must equal current reviewed source`);
    }
    moduleHash = moduleHashes.panel;
    assert.equal(sql("SELECT count(*) FROM plinth.packages WHERE name='admin'"), '0',
        'admin must not be bundled at first boot');
    assert.equal(sql("SELECT count(*) FROM plinth.packages WHERE name='shell' AND state IN ('ACTIVE','ACTIVE_FLAGGED')"), '1');
    assert.equal(sql("SELECT count(*) FROM plinth.packages WHERE provenance='bundled' AND name<>'shell'"), '0');
    assert.equal(sql("SELECT count(*) FROM plinth.rbac_rules WHERE rule='plinth.admin.packages'"), '0');
    kernelAuthority = jsonSql(`SELECT coalesce(json_agg(k ORDER BY rule),'[]'::json)::text FROM
        (SELECT r.rule,r.id,r.extension_name,
            (SELECT coalesce(array_agg(g.name ORDER BY g.name),ARRAY[]::text[])
             FROM plinth.group_rules gr JOIN plinth.groups g ON g.id=gr.group_id
             WHERE gr.rule_id=r.id) AS granted_groups
         FROM plinth.rbac_rules r
         WHERE r.rule IN ('kernel.admin','packages.read','packages.install')) k`);
    assert.equal(kernelAuthority.length, 3);
    assert(kernelAuthority.every(row => row.extension_name === 'kernel'));
    completed.push('N01 first boot shell-only and exact local ZIP/source bytes');

    const maintainer = principals.maintainer;
    const bootstrap = await publicPost('/api/auth/bootstrap', {
        username: maintainer.username, password, bootstrap_token: bootstrapToken });
    assert.equal(bootstrap.status, 201, 'real secret-authorized bootstrap');
    assert.equal(bootstrap.body.username, maintainer.username);
    maintainer.id = bootstrap.body.id;
    assert.match(maintainer.id, uuid);
    for (const role of ['reader', 'writer', 'visible', 'denied']) {
        const account = principals[role];
        const registration = await publicPost('/api/auth/register',
            { username: account.username, password });
        assert.equal(registration.status, 202, `real ${role} registration`);
        assert.deepEqual(registration.body, { status: 'processed' });
        account.id = sql(`SELECT id FROM plinth.users WHERE username=${literal(account.username)}`);
        assert.match(account.id, uuid);
    }
    browser = await chromium.launch({ executablePath: process.env.PLINTH_BROWSER || undefined,
        args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [] });
    const maintainerPage = await login(maintainer);
    assert.equal(await maintainerPage.getByRole('button', { name: 'Administration', exact: true }).count(), 0);
    const shellOnly = await browserRequest(maintainerPage, '/api/frontend/applications');
    assert.equal(shellOnly.status, 200);
    assert.deepEqual(shellOnly.body.applications, [], 'no first-boot application except shell');
    completed.push('N02 real bootstrap/register/login and no first-boot admin discovery');

    const dryAdmin = await browserUpload(maintainerPage, archive, true);
    assert.equal(dryAdmin.status, 200);
    assert.equal(dryAdmin.body.state, 'VALIDATING');
    assert.equal(dryAdmin.body.name, 'admin');
    assert.equal(dryAdmin.body.version, '0.1.0');
    assert.equal(sql("SELECT count(*) FROM plinth.packages WHERE name='admin'"), '0');
    assert.equal(audit('packages.installed', 'admin').length, 0);
    const installedAdmin = await browserUpload(maintainerPage, archive);
    assert.equal(installedAdmin.status, 201);
    assert.equal(installedAdmin.body.state, 'ACTIVE');
    assert.equal(installedAdmin.body.name, 'admin');
    adminId = installedAdmin.body.id;
    assert.match(adminId, uuid);
    const admin = packageRow('admin');
    assert.equal(admin.id, adminId);
    assert.equal(admin.version, '0.1.0');
    assert.equal(admin.provenance, 'user');
    assert.equal(admin.installed_by_user_id, maintainer.id);
    assert.equal(admin.frontend_mount, null);
    assert.equal(admin.frontend_entry, null);
    assert.equal(admin.application_ready, true);
    assert.equal(sql("SELECT count(*) FROM plinth.capabilities WHERE extension_name='admin'"), '0');
    assert.equal(sql(`SELECT count(*) FROM pg_class c JOIN pg_namespace n ON n.oid=c.relnamespace
        WHERE n.nspname='ext_admin' AND c.relkind='r'`), '0', 'admin declares no tables');
    const ownerRows = jsonSql(`SELECT coalesce(json_agg(r),'[]'::json)::text FROM
        (SELECT rule,namespace,extension_name FROM plinth.rbac_rules
         WHERE extension_name='admin' AND orphaned_at IS NULL) r`);
    assert.deepEqual(ownerRows, [{ rule: 'plinth.admin.packages', namespace: 'plinth', extension_name: 'admin' }]);
    assert.equal(sql(`SELECT count(*) FROM plinth.group_rules gr JOIN plinth.rbac_rules r
        ON r.id=gr.rule_id WHERE r.rule='plinth.admin.packages'`), '0');
    assert.equal(sql(`SELECT count(*) FROM plinth.panels WHERE package_id=${literal(adminId)}
        AND panel_id='packages' AND panel_type='primary'
        AND declaration->>'rbac_rule'='plinth.admin.packages'`), '1');
    assert.deepEqual(jsonSql(`SELECT coalesce(json_agg(k ORDER BY rule),'[]'::json)::text FROM
        (SELECT r.rule,r.id,r.extension_name,
            (SELECT coalesce(array_agg(g.name ORDER BY g.name),ARRAY[]::text[])
             FROM plinth.group_rules gr JOIN plinth.groups g ON g.id=gr.group_id
             WHERE gr.rule_id=r.id) AS granted_groups
         FROM plinth.rbac_rules r
         WHERE r.rule IN ('kernel.admin','packages.read','packages.install')) k`),
    kernelAuthority, 'admin install must not reassign or grant kernel-owned rules');
    assert.deepEqual(audit('packages.installed', 'admin').map(row => row.detail.installed_by_user_id),
        [maintainer.id]);
    const discovered = await browserRequest(maintainerPage, '/api/frontend/applications');
    assert.equal(discovered.status, 200);
    const application = discovered.body.applications.find(item => item.id === 'admin');
    assert(application && application.panels.length === 1);
    assert.equal(application.version, '0.1.0');
    assert.equal(application.title, 'Administration',
        'launcher title comes from manifest display_name, not panel title');
    assert.equal(application.panels[0].id, 'packages');
    const servedPaths = {
        panel: application.panels[0].module_url,
        api: new URL('../packages/api.js', baseURL + application.panels[0].module_url).pathname,
        controller: new URL('../packages/controller.js', baseURL + application.panels[0].module_url).pathname,
    };
    for (const [role, path] of Object.entries(servedPaths)) {
        const module = await maintainerPage.evaluate(async path => {
            const response = await fetch(path, { credentials: 'same-origin' });
            return { status: response.status, text: await response.text() };
        }, path);
        assert.equal(module.status, 200, `${role} served from installed package`);
        assert.equal(sha256(Buffer.from(module.text)), moduleHashes[role],
            `${role} served bytes must equal exact reviewed archive bytes`);
    }
    completed.push('N03 real authenticated dry run/install, provenance, ownership and audit');

    // The ordinary package grants its visibility rule to nobody by default.
    // Even this bootstrapped maintainer needs an explicit, task-owned panel
    // grant; kernel.admin already supplies the separate package API authority.
    grant('maintainer', ['plinth.admin.packages']);
    grant('reader', ['plinth.admin.packages', 'packages.read']);
    grant('writer', ['plinth.admin.packages', 'packages.read', 'packages.install']);
    grant('visible', ['plinth.admin.packages']);
    for (const role of ['reader', 'writer', 'visible', 'denied']) await login(principals[role]);
    const writerPage = principals.writer.page;
    await installWriterFetchProxy(writerPage);
    await openAdmin(maintainerPage, maintainer);
    await openAdmin(principals.reader.page, principals.reader);
    await openAdmin(writerPage, principals.writer);
    await openAdmin(principals.visible.page, principals.visible);
    await principals.denied.page.reload();
    await principals.denied.page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
    assert.equal(await principals.denied.page.getByRole('button', { name: 'Administration' }).count(), 0);
    for (const role of ['reader', 'writer']) {
        assert.equal((await browserRequest(principals[role].page, '/api/packages')).status, 200);
    }
    for (const role of ['visible', 'denied']) {
        assert.equal((await browserRequest(principals[role].page, '/api/packages')).status, 403);
        assert.equal((await browserRequest(principals[role].page, `/api/packages/${adminId}`)).status, 403);
    }
    assert.match(await principals.visible.page.locator('#admin-list-error').textContent(),
        /^permission_denied: .*packages\.read/,
        'visible-only actor sees the kernel package-read permission denial');
    for (const role of ['reader', 'visible', 'denied']) {
        const page = principals[role].page;
        const deniedInstall = await browserUpload(page, archive, true);
        assert.equal(deniedInstall.status, 403, `${role} install authorization`);
        assert.equal((await browserRequest(page, `/api/packages/${adminId}`, 'PATCH',
            { action: 'disable' })).status, 403);
        assert.equal((await browserRequest(page, `/api/packages/${adminId}?confirm=true`, 'DELETE')).status, 403);
    }
    const csrfDenied = await principals.writer.page.evaluate(async () => {
        const response = await fetch('/api/packages/' + encodeURIComponent('00000000-0000-4000-8000-000000000000'), {
            method: 'PATCH', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ action: 'disable' }) });
        return { status: response.status, code: (await response.json()).error };
    });
    assert.equal(csrfDenied.status, 403);
    assert.equal(csrfDenied.code, 'csrf_failed');
    completed.push('N04 five real actors, independent visibility/API grants, RBAC and CSRF');

    await chooseZip(writerPage, fixture('valid-install'));
    const dryNotes = await clickAndResponse(writerPage, '#admin-dry-run',
        response => new URL(response.url()).pathname === '/api/packages' &&
            new URL(response.url()).searchParams.get('dry_run') === '1', 200);
    assert.equal((await dryNotes.json()).name, 'notes');
    await writerPage.locator('#admin-validation').getByText('VALIDATING', { exact: false }).waitFor();
    const notesInstalled = await clickAndResponse(writerPage, '#admin-install',
        response => new URL(response.url()).pathname === '/api/packages' &&
            !new URL(response.url()).search, 201);
    notesId = (await notesInstalled.json()).id;
    assert.match(notesId, uuid);
    assert.equal(packageRow('notes').installed_by_user_id, principals.writer.id);
    const noteId = `admin50_${label}`;
    const noteBody = `retained across package transitions ${label}`;
    sql(`INSERT INTO ext_notes.notes(id,body) VALUES (${literal(noteId)},${literal(noteBody)})`);
    await writerPage.locator(`#admin-package-list button[data-package-id="${notesId}"]`).waitFor();
    await selectPackage(writerPage, notesId);
    assert.match(await writerPage.locator('#admin-detail').textContent(), /provenance user/);
    await writerPage.locator('#admin-history-load').click();
    await writerPage.locator('#admin-history').getByText('notes 1.2.3', { exact: false }).waitFor();
    assert.deepEqual(audit('packages.installed', 'notes').map(row => row.detail.installed_by_user_id),
        [principals.writer.id]);
    completed.push('N05 actual panel list/detail/history and ZIP validate/install');

    const noteBefore = notesId;
    await chooseZip(writerPage, fixture('upgrade-v2-broken-migration'));
    const broken = await clickAndResponse(writerPage, '#admin-install',
        response => new URL(response.url()).pathname === '/api/packages' &&
            !new URL(response.url()).search, 400);
    const brokenBody = await broken.json();
    assert.equal(brokenBody.kind, 'upgrade-migration-failed');
    assert.equal(packageRow('notes').id, noteBefore);
    assert.equal(packageRow('notes').version, '1.2.3');
    assert.equal(sql(`SELECT body FROM ext_notes.notes WHERE id=${literal(noteId)}`), noteBody);
    await writerPage.locator('#admin-attempts article[data-outcome="failed"]').last().waitFor();
    await chooseZip(writerPage, fixture('upgrade-v2'));
    const upgraded = await clickAndResponse(writerPage, '#admin-install',
        response => new URL(response.url()).pathname === '/api/packages' &&
            !new URL(response.url()).search, 201);
    notesId = (await upgraded.json()).id;
    assert.notEqual(notesId, noteBefore);
    assert.equal(packageRow('notes').version, '1.3.0');
    assert.equal(sql(`SELECT state FROM plinth.packages WHERE id=${literal(noteBefore)}`), 'SUPERSEDED');
    assert.equal(sql(`SELECT body FROM ext_notes.notes WHERE id=${literal(noteId)}`), noteBody);
    assert.deepEqual(audit('packages.upgrade_completed', 'notes').map(row =>
        row.detail.upgraded_by_user_id), [principals.writer.id]);
    await writerPage.locator('#admin-refresh').click();
    await writerPage.locator(`#admin-package-list button[data-package-id="${notesId}"]`).waitFor();
    await selectPackage(writerPage, notesId);
    await writerPage.locator('#admin-history-load').click();
    await writerPage.locator('#admin-history').getByText('notes 1.3.0', { exact: false }).waitFor();
    assert.match(await writerPage.locator('#admin-history').textContent(), /notes 1\.2\.3/);
    assert.match(await writerPage.locator('#admin-history').textContent(), /notes 1\.4\.0/);
    completed.push('N06 failed migration retains old authority; successful upgrade and observed versions');

    const disabled = await clickAndResponse(writerPage, '#admin-disable',
        response => new URL(response.url()).pathname === `/api/packages/${notesId}` &&
            response.request().method() === 'PATCH', 200);
    assert.equal((await disabled.json()).state, 'DISABLED');
    assert.equal(packageRow('notes').state, 'DISABLED');
    await writerPage.locator('#admin-enable').waitFor();
    const enabled = await clickAndResponse(writerPage, '#admin-enable',
        response => new URL(response.url()).pathname === `/api/packages/${notesId}` &&
            response.request().method() === 'PATCH', 200);
    assert.equal((await enabled.json()).state, 'ACTIVE');
    assert.equal(packageRow('notes').state, 'ACTIVE');
    assert.deepEqual(audit('packages.disabled', 'notes').map(row =>
        row.detail.disabled_by_user_id), [principals.writer.id]);
    assert.deepEqual(audit('packages.enabled', 'notes').map(row =>
        row.detail.enabled_by_user_id), [principals.writer.id]);
    completed.push('N07 panel 200 disable/enable and real operation actors');

    // Deliver two OLD real GET snapshots only after a distinct NEW real server
    // state reaches the next activation. This is a delivery seam, not a fake
    // backend response: stale ACTIVE must never overwrite current DISABLED.
    const visibleNoteState = state => writerPage.waitForFunction(({ id, state }) =>
        document.getElementById('admin-phase')?.textContent === 'active' &&
        document.querySelector(`#admin-package-list button[data-package-id="${id}"]`)
            ?.textContent?.endsWith(`— ${state}`) &&
        document.getElementById('admin-list-error')?.textContent === 'none',
    { id: notesId, state });
    const transitionByOtherActor = async (action, state) => {
        const result = await browserRequest(maintainerPage,
            `/api/packages/${notesId}`, 'PATCH', { action });
        assert.equal(result.status, 200);
        assert.equal(result.body.state, state);
        assert.equal(packageRow('notes').state, state);
    };
    await visibleNoteState('ACTIVE');
    await writerPage.evaluate(id => {
        const control = window.__adminFetchControls;
        control.targetId = id;
        control.oldFetchState = null;
        control.releaseFetch = null;
        control.holdFetch = true;
        if (window.fetch !== window.__adminStableFetch) throw new Error('stable fetch proxy was replaced');
    }, notesId);
    await writerPage.locator('#admin-refresh').click();
    await writerPage.waitForFunction(() => typeof window.__adminFetchControls.releaseFetch === 'function');
    assert.equal(await writerPage.evaluate(() => window.__adminFetchControls.oldFetchState), 'ACTIVE');
    await transitionByOtherActor('disable', 'DISABLED');
    await writerPage.getByRole('button', { name: 'Home', exact: true }).click();
    await writerPage.getByRole('button', { name: 'Administration', exact: true }).click();
    await visibleNoteState('DISABLED');
    await writerPage.evaluate(async () => {
        await window.__adminFetchControls.releaseFetch();
        await new Promise(resolve => setTimeout(resolve, 0));
    });
    await visibleNoteState('DISABLED');
    await transitionByOtherActor('enable', 'ACTIVE');
    await writerPage.locator('#admin-refresh').click();
    await visibleNoteState('ACTIVE');
    await writerPage.evaluate(id => {
        const control = window.__adminFetchControls;
        control.targetId = id;
        control.oldJsonState = null;
        control.releaseJson = null;
        control.holdJson = true;
    }, notesId);
    await writerPage.locator('#admin-refresh').click();
    await writerPage.waitForFunction(() => typeof window.__adminFetchControls.releaseJson === 'function');
    assert.equal(await writerPage.evaluate(() => window.__adminFetchControls.oldJsonState), 'ACTIVE');
    await transitionByOtherActor('disable', 'DISABLED');
    await writerPage.getByRole('button', { name: 'Home', exact: true }).click();
    await writerPage.getByRole('button', { name: 'Administration', exact: true }).click();
    await visibleNoteState('DISABLED');
    const oldJsonSettlement = await writerPage.evaluate(async () => {
        const outcome = await window.__adminFetchControls.releaseJson();
        await new Promise(resolve => setTimeout(resolve, 0));
        return outcome;
    });
    assert(['parsed', 'AbortError'].includes(oldJsonSettlement),
        'retired JSON read must settle as actual parsed data or an abort');
    await visibleNoteState('DISABLED');
    await transitionByOtherActor('enable', 'ACTIVE');
    await writerPage.locator('#admin-refresh').click();
    await visibleNoteState('ACTIVE');
    completed.push('N08 old activation pending fetch and JSON cannot overwrite current panel');

    // Real capability dispatch keeps the v1 slow package in flight. The test
    // wrapper sets a 500 ms upgrade drain window; 3 seconds stays under the
    // extension DB client's 5-second query timeout while exceeding that drain.
    await chooseZip(writerPage, fixture('upgrade-v1-slow'));
    const slowInstalled = await clickAndResponse(writerPage, '#admin-install',
        response => new URL(response.url()).pathname === '/api/packages' &&
            !new URL(response.url()).search, 201);
    slowId = (await slowInstalled.json()).id;
    assert.match(slowId, uuid);
    assert.equal(packageRow('slow').version, '1.0.0');
    const slowRule = sql("SELECT id FROM plinth.rbac_rules WHERE rule='slow.alpha' AND extension_name='slow'");
    assert.match(slowRule, uuid);
    const writerGroup = ownedGroupByRole.get('writer');
    assert.match(writerGroup, uuid);
    sql(`INSERT INTO plinth.group_rules(group_id,rule_id) VALUES
        (${literal(writerGroup)},${literal(slowRule)})`);
    assert.equal(sql(`SELECT count(*) FROM plinth.group_rules
        WHERE group_id=${literal(writerGroup)} AND rule_id=${literal(slowRule)}`), '1');
    // WebSocket authority snapshots are immutable. The new group rule retires
    // the writer's old socket; establish a fresh real shell/panel under the
    // changed authority before starting the in-flight capability call.
    await openAdmin(writerPage, principals.writer);
    await writerPage.locator('#admin-package-file').waitFor();
    let longCallResult = null;
    const longCall = writerPage.evaluate(async () => {
        const clean = value => String(value ?? '').split('\n')[0]
            .replace(/(?:token|password|secret)\s*[:=]\s*\S+/gi, '[redacted]')
            .replace(/[^\x20-\x7e]/g, '?').slice(0, 160);
        try {
            const value = await (await import('@plinth/frontend/sdk'))
                .call('slow.wait', { ms: 3000 });
            return { status: 'fulfilled', value };
        } catch (error) {
            return { status: 'rejected', error: { name: clean(error?.name),
                code: clean(error?.code), message: clean(error?.message) } };
        }
    }).then(result => { longCallResult = result; return result; }, error => {
        const result = { status: 'evaluation-error', error: {
            name: String(error?.name || 'Error').slice(0, 40),
            code: 'page-evaluation-failed', message: 'browser evaluation failed',
        } };
        longCallResult = result;
        return result;
    }); // Attach both handlers immediately: never leave a rejected Node promise.
    // Verify the actual PG sleep started before submitting the upgrade. The
    // observer query excludes itself; no simulator or test seam is invoked.
    await bounded(async () => {
        while (true) {
            const active = Number(sql(`SELECT count(*) FROM pg_stat_activity
                WHERE state='active' AND query LIKE '%SELECT pg_sleep(3)%'
                AND query NOT LIKE '%pg_stat_activity%'`));
            if (active > 0) break;
            if (longCallResult) throw new Error(
                `real slow.wait ended before active PG witness: ${JSON.stringify(longCallResult)}`);
            await new Promise(resolve => setTimeout(resolve, 50));
        }
    }, 'real slow capability dispatch admission', 3000);
    await chooseZip(writerPage, fixture('upgrade-v2-slow'));
    const drainFailure = await clickAndResponse(writerPage, '#admin-install',
        response => new URL(response.url()).pathname === '/api/packages' &&
            !new URL(response.url()).search, 400);
    const drainBody = await drainFailure.json();
    assert.equal(drainBody.kind, 'upgrade-drain-timeout');
    assert.equal(packageRow('slow').id, slowId);
    assert.equal(packageRow('slow').state, 'ACTIVE');
    assert.deepEqual(await bounded(() => longCall, 'real slow capability completion', 7000),
        { status: 'fulfilled', value: { ok: true, slept_ms: 3000 } });
    await writerPage.locator('#admin-attempts article[data-outcome="failed"]').last().waitFor();
    // The failed v2.0.0 USER row retains packages UNIQUE(name,version), while
    // insert_upgrade_row only supports same-version retry for BUNDLED rows.
    // Defer that installer contract gap; N06 already proves a successful UI upgrade.
    const slowDeleted = await browserRequest(maintainerPage,
        `/api/packages/${slowId}?confirm=true`, 'DELETE');
    assert.equal(slowDeleted.status, 204);
    slowId = null;
    assert.equal(packageRow('slow'), null);
    assert.equal(sql(`SELECT count(*) FROM plinth.group_rules
        WHERE group_id=${literal(writerGroup)} AND rule_id=${literal(slowRule)}`), '0');
    // Uninstall removes the package rule and its group grant in one cleanup
    // transaction. Renew the writer's actual shell/WS authority before N10;
    // the old immutable socket snapshot must fail closed after this change.
    await openAdmin(writerPage, principals.writer);
    completed.push('N09 real in-flight drain failure retains old and admitted call completes');

    // Admit exactly one real PATCH, let the server commit, then lose only its
    // response to the panel. Pause its GET reconciliation while another real
    // actor creates the same observed state. Neither a matching row nor that
    // actor's audit may turn the first request into an acknowledged success.
    await selectPackage(writerPage, notesId);
    const beforeUnknownDisabled = audit('packages.disabled', 'notes').map(row =>
        row.detail.disabled_by_user_id);
    const beforeUnknownEnabled = audit('packages.enabled', 'notes').map(row =>
        row.detail.enabled_by_user_id);
    let patchRequests = 0;
    const countPatch = request => {
        if (new URL(request.url()).pathname === `/api/packages/${notesId}` &&
            request.method() === 'PATCH') patchRequests++;
    };
    writerPage.on('request', countPatch);
    await writerPage.evaluate(id => {
        const control = window.__adminFetchControls;
        control.targetId = id;
        control.committedStatus = null;
        control.observedRow = null;
        control.releaseReconcile = null;
        control.losePatch = true;
        control.holdReconcile = true;
        if (window.fetch !== window.__adminStableFetch) throw new Error('stable fetch proxy was replaced');
    }, notesId);
    await clickAndResponse(writerPage, '#admin-disable',
        response => new URL(response.url()).pathname === `/api/packages/${notesId}` &&
            response.request().method() === 'PATCH', 200);
    await writerPage.locator('#admin-attempts article[data-outcome="unknown"]').last().waitFor();
    await writerPage.waitForFunction(() => typeof window.__adminFetchControls.releaseReconcile === 'function');
    assert.equal(await writerPage.evaluate(() => window.__adminFetchControls.committedStatus), 200);
    assert.equal(packageRow('notes').state, 'DISABLED');
    const rbacRunStamp = () => sql(`SELECT coalesce(last_rbac_test_run_at::text,'')
        FROM plinth.packages WHERE id=${literal(notesId)}`);
    const beforeOtherEnableRbacRun = rbacRunStamp();
    const otherEnable = await browserRequest(maintainerPage,
        `/api/packages/${notesId}`, 'PATCH', { action: 'enable' });
    assert.equal(otherEnable.status, 200);
    assert.equal(otherEnable.body.state, 'ACTIVE');
    assert.equal(packageRow('notes').state, 'ACTIVE');
    // Enable schedules a detached RBAC test holding the same name lock as
    // disable. Wait for this run's persisted completion, not a wall-clock delay
    // or the non-null stamp left by a prior install/transition.
    await bounded(async () => {
        while (true) {
            const stamp = rbacRunStamp();
            if (stamp && stamp !== beforeOtherEnableRbacRun) return;
            await new Promise(resolve => setTimeout(resolve, 25));
        }
    }, 'other-actor enable RBAC test completion', 10000);
    const otherDisable = await browserRequest(maintainerPage,
        `/api/packages/${notesId}`, 'PATCH', { action: 'disable' });
    assert.equal(otherDisable.status, 200, `N10 maintainer disable after enable: ${JSON.stringify({
        errorCode: typeof otherDisable.body?.error?.code === 'string' &&
            /^[a-z0-9-]{1,80}$/.test(otherDisable.body.error.code)
            ? otherDisable.body.error.code : null,
        state: packageRow('notes')?.state ?? null,
    })}`);
    assert.equal(packageRow('notes').state, 'DISABLED');
    await writerPage.evaluate(() => window.__adminFetchControls.releaseReconcile());
    await writerPage.locator('#admin-attempts article[data-outcome="unknown"]')
        .last().getByText('Server observation: complete').waitFor();
    assert.deepEqual(await writerPage.evaluate(() => ({
        id: window.__adminFetchControls.observedRow?.id,
        state: window.__adminFetchControls.observedRow?.state,
    })), { id: notesId, state: 'DISABLED' },
    'bounded GET observed an exact matching row but cannot establish mutation attribution');
    assert.equal(patchRequests, 1, 'no automatic duplicate mutation');
    writerPage.off('request', countPatch);
    assert.match(await writerPage.locator('#admin-attempts article[data-outcome="unknown"]')
        .last().textContent(), /Observed rows cannot prove this request committed/);
    assert.deepEqual(audit('packages.disabled', 'notes').map(row =>
        row.detail.disabled_by_user_id).sort(),
    [...beforeUnknownDisabled, principals.writer.id, maintainer.id].sort());
    assert.deepEqual(audit('packages.enabled', 'notes').map(row =>
        row.detail.enabled_by_user_id).sort(), [...beforeUnknownEnabled, maintainer.id].sort());
    const reenabled = await browserRequest(maintainerPage,
        `/api/packages/${notesId}`, 'PATCH', { action: 'enable' });
    assert.equal(reenabled.status, 200);
    await writerPage.locator('#admin-refresh').click();
    await writerPage.locator(`#admin-package-list button[data-package-id="${notesId}"]`).waitFor();
    await selectPackage(writerPage, notesId);
    completed.push('N10 lost committed reply remains unknown despite matching row by other actor');

    await writerPage.locator('#admin-uninstall-name').fill('notes');
    const deleted = await clickAndResponse(writerPage, '#admin-uninstall',
        response => new URL(response.url()).pathname === `/api/packages/${notesId}` &&
            response.request().method() === 'DELETE', 204);
    assert.equal(deleted.status(), 204);
    notesId = null;
    assert.equal(packageRow('notes'), null);
    assert.deepEqual(audit('packages.uninstalled', 'notes').map(row =>
        row.detail.uninstalled_by_user_id), [principals.writer.id]);
    completed.push('N11 confirmed 204 normal uninstall, no live notes generation, audit actor');

    // Admin cannot uninstall itself while its panel remains current; navigate
    // Home first, then use the same real maintainer browser session/API.
    await maintainerPage.getByRole('button', { name: 'Home', exact: true }).click();
    const removedAdmin = await browserRequest(maintainerPage,
        `/api/packages/${adminId}?confirm=true`, 'DELETE');
    assert.equal(removedAdmin.status, 204);
    adminId = null;
    assert.equal(packageRow('admin'), null);
    assert.deepEqual(audit('packages.uninstalled', 'admin').map(row =>
        row.detail.uninstalled_by_user_id), [maintainer.id]);
    completed.push('N12 admin normal uninstall and actor');
} catch (error) { failure = error; }

// Cleanup is independent: one rejected context close cannot skip the others,
// browser close, package uninstall, owned grant removal or wrapper teardown.
for (const name of ['notes', 'slow', 'admin']) {
    try {
        const row = packageRow(name);
        if (!row) continue;
        assert(['ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED'].includes(row.state));
        assert([principals.maintainer.id, principals.writer.id].includes(row.installed_by_user_id),
            'cleanup may target only a package installed by this journey');
        const page = principals.maintainer.page;
        assert(page && !page.isClosed(), 'real maintainer context must remain usable for normal uninstall');
        const removed = await bounded(() => browserRequest(page,
            `/api/packages/${row.id}?confirm=true`, 'DELETE'), `${name} normal uninstall cleanup`, 20000);
        assert.equal(removed.status, 204);
        assert.equal(packageRow(name), null);
        if (name === 'notes') notesId = null;
        if (name === 'slow') slowId = null;
        if (name === 'admin') adminId = null;
    } catch (error) { cleanupErrors.push(error); }
}
for (const principal of Object.values(principals)) {
    try { if (principal.context) await bounded(() => principal.context.close(),
        `${principal.role} context cleanup`, 5000); }
    catch (error) { cleanupErrors.push(error); }
}
try {
    if (browser) { await bounded(() => browser.close(), 'browser cleanup', 5000);
        assert(!browser.isConnected()); }
} catch (error) { cleanupErrors.push(error); }
try {
    // Only exact known, task-owned rows are candidates for fallback removal.
    // A normal confirmed HTTP uninstall is required; never SQL-delete a package.
    if (notesId || adminId || slowId) cleanupErrors.push(new Error(
        'task-owned package remains after failed journey; wrapper must drop only its disposable DB'));
} catch (error) { cleanupErrors.push(error); }
try {
    for (const group of ownedGroups) {
        sql(`DELETE FROM plinth.group_rules WHERE group_id=${literal(group)};
            DELETE FROM plinth.group_members WHERE group_id=${literal(group)};
            DELETE FROM plinth.groups WHERE id=${literal(group)};`);
    }
    const ids = Object.values(principals).map(item => item.id).filter(Boolean);
    if (ids.length && !notesId && !adminId && !slowId) {
        const set = ids.map(literal).join(',');
        // Normal uninstall may deliberately retain failed/superseded history.
        // Remove only this journey's terminal rows after all history/audit
        // assertions, inside the wrapper's disposable database.
        sql(`DELETE FROM plinth.packages WHERE name IN ('admin','notes','slow')
            AND installed_by_user_id IN (${set})
            AND state IN ('INSTALL_FAILED','SUPERSEDED')`);
        sql(`DELETE FROM ext_shell.user_preferences WHERE user_id IN (${set});
            DELETE FROM plinth.sessions WHERE user_id IN (${set});
            DELETE FROM plinth.group_members WHERE user_id IN (${set});
            DELETE FROM plinth.users WHERE id IN (${set});`);
        assert.equal(sql(`SELECT count(*) FROM plinth.users WHERE id IN (${set})`), '0');
    }
} catch (error) { cleanupErrors.push(error); }
if (failure || cleanupErrors.length) throw new AggregateError(
    [...(failure ? [failure] : []), ...cleanupErrors], 'native admin package journey failed');
assert.equal(completed.length, 12);
console.log(JSON.stringify({ adminPackageNativeCases: completed.length, expectedCases: 12,
    actualAuth: true, actualRbacCsrf: true, packageProvenance: 'user',
    archiveSha256: archiveHash, installedModuleSha256: moduleHash,
    installedModulesSha256: moduleHashes,
    cleanup: 'PASS', scope: 'actual kernel/ZIP/installed panel/browser/SQL' }));
