// Native groups journey invoked by the parent after N01–N12. The parent owns
// kernel/browser/database lifetime; this module owns its new fixture resources.
// Importing the module performs no I/O and starts no browser.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { createHash, randomUUID } from 'node:crypto';
import { readFile } from 'node:fs/promises';

export const GROUPS_NATIVE_CASES = Object.freeze([
    'G01 genuine source baseline and uninterrupted admin upgrade',
    'G02 independent groups visibility and API authority',
    'G03 native groups UI lifecycle and exact stored state',
    'G04 built-in protections and owned spare admin membership',
    'G05 explicit UUID membership and authoritative counts',
    'G06 real concurrent uniqueness and audit winners',
    'G07 real authentication CSRF and live-authority denials',
    'G08 committed lost reply stays unknown and retires ownership',
]);
const LEGACY_CASES = Object.freeze([
    'N01 first boot shell-only and exact local ZIP/source bytes',
    'N02 real bootstrap/register/login and no first-boot admin discovery',
    'N03 real authenticated dry run/install, provenance, ownership and audit',
    'N04 five real actors, independent visibility/API grants, RBAC and CSRF',
    'N05 actual panel list/detail/history and ZIP validate/install',
    'N06 failed migration retains old authority; successful upgrade and observed versions',
    'N07 panel 200 disable/enable and real operation actors',
    'N08 old activation pending fetch and JSON cannot overwrite current panel',
    'N09 real in-flight drain failure retains old and admitted call completes',
    'N10 lost committed reply remains unknown despite matching row by other actor',
    'N11 confirmed 204 normal uninstall, no live notes generation, audit actor',
    'N12 admin normal uninstall and actor',
]);
const BASE_COMMIT = '3301b7ee01c2098847d47f3e51d5ff14d28442f3';
const BASE_TREE = '2399eadb48c5d1bc6f41f4bbc77b425ec0d9dc26';
const OLD_MODULES = Object.freeze({
    panel: '8dd9b670591527f453ca8c8ce8414464680d85f5a5ae6630acd0b643e7ac234a',
    api: '809e189dcc1df92b8ea873db241be51d55db92de5e5a0cfc7843770936ab9242',
    controller: '59fc07c37a1d964d12f8b0a7801ee184b32f0bb534cbe1431712016c79ba128d',
});
const MODULE_PATHS = Object.freeze({ panel: 'client/panels/packages.js',
    api: 'client/packages/api.js', controller: 'client/packages/controller.js' });
const GROUP_PATHS = Object.freeze({ panel: 'client/panels/groups.js',
    api: 'client/groups/api.js', controller: 'client/groups/controller.js' });
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const HASH = /^[0-9a-f]{64}$/;
const LIVE = ['ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED', 'UNINSTALLING'];
const literal = value => "'" + String(value).replaceAll("'", "''") + "'";
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const code = body => typeof body?.error === 'string' ? body.error : body?.error?.code || body?.kind;
const frozenCopy = value => {
    const copy = structuredClone(value);
    const freeze = item => {
        if (item && typeof item === 'object') { Object.values(item).forEach(freeze); Object.freeze(item); }
        return item;
    };
    return freeze(copy);
};

// Existing parent helpers are injected rather than importing its executing
// N01–N12 script. login receives a new principal clone and assigns its own
// context/page. Originals and their outer teardown ownership remain intact.
export async function runGroupsAdminJourney({ baseURL, browser, principals,
    sql, jsonSql, request, upload, login, bounded,
    baselineArchive, baselineIdentity, currentArchive, currentIdentity, legacyProof }) {
    for (const helper of [sql, jsonSql, request, upload, login, bounded]) assert.equal(typeof helper, 'function');
    assert(baseURL && browser && principals.maintainer?.page);
    const control = principals.maintainer;
    const page = control.page;
    const legacy = frozenCopy(legacyProof);
    const baseline = frozenCopy(baselineIdentity), current = frozenCopy(currentIdentity);
    assert.deepEqual(legacy.cases, LEGACY_CASES);
    assert.equal(legacy.count, 12);
    assert.equal(legacy.finalState, 'uninstalled_at_N12');
    assert.equal(legacy.version, '0.1.1');
    assert.equal(current.version, '0.1.1', 'expected current fixture version');
    assert.equal(baseline.version, '0.1.0');
    assert.equal(baseline.commit, BASE_COMMIT); assert.equal(baseline.tree, BASE_TREE);
    assert.deepEqual(baseline.moduleHashes, OLD_MODULES);
    assert.equal(legacy.archiveSha256, current.archiveSha256);
    assert.deepEqual(legacy.moduleHashes, current.moduleHashes);
    assert.equal(legacy.actor.userId, control.id);
    for (const id of [legacy.packageId, legacy.actor.userId, legacy.actor.sessionId, legacy.uninstallAudit.id])
        assert.match(id, UUID);
    assert.equal(legacy.uninstallAudit.action, 'packages.uninstalled');
    assert.equal(legacy.uninstallAudit.packageId, legacy.packageId);
    assert.equal(legacy.uninstallAudit.uninstalledByUserId, legacy.actor.userId);
    const ownedContexts = [], ownedGroups = new Set(), ownedPackages = new Set();
    const completed = [], cleanupErrors = [];
    let failure, oldId, currentId, adminBuiltinId, writerFixture;
    let writer, visible, denied, actorControl, actorWriter;
    const label = randomUUID().replaceAll('-', '').slice(0, 16);
    const groupName = role => `g51_${label}_${role}`;
    const packageRows = () => jsonSql(`SELECT coalesce(json_agg(p ORDER BY installed_at),'[]'::json)::text
        FROM (SELECT id,name,version,state,supersedes_id,installed_by_user_id,installed_at
        FROM plinth.packages WHERE name='admin') p`);
    const groupRow = id => jsonSql(`SELECT coalesce((SELECT row_to_json(g) FROM
        (SELECT id,name,description,built_in FROM plinth.groups WHERE id=${literal(id)}) g),'null'::json)::text`);
    const memberCount = id => Number(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(id)}`));
    const call = (p, path, method = 'GET', body = null) => bounded(
        () => request(p, path, method, body), `groups ${method} ${path}`, 15000);
    const status = (response, expected, expectedCode) => {
        assert.equal(response.status, expected);
        if (expectedCode) assert.equal(code(response.body), expectedCode);
        return response.body;
    };
    async function observeActor(principal) {
        const session = status(await call(principal.page, '/api/auth/session'), 200);
        assert.equal(session.user.id, principal.id);
        assert.match(session.session.id, UUID);
        const row = jsonSql(`SELECT row_to_json(s)::text FROM (SELECT user_id,
            host(ip_address) AS ip FROM plinth.sessions WHERE id=${literal(session.session.id)}) s`);
        assert.equal(row.user_id, principal.id);
        assert.equal(typeof row.ip, 'string'); assert(row.ip.length > 0);
        return { userId: principal.id, username: session.user.username,
            sessionId: session.session.id, ipAddress: row.ip };
    }
    async function fresh(role) {
        const source = principals[role]; assert(source?.id && source.password);
        const clone = { ...source, role: `groups-${role}`, context: null, page: null };
        ownedContexts.push(clone); // Track partial login too; never overwrite the parent's principal.
        await bounded(() => login(clone), `groups ${role} real login`, 15000);
        return clone;
    }
    async function waitUntil(check, description) {
        // These predicates are synchronous, individually bounded SQL reads.
        // Await the finite loop itself: a raced timeout must not leave a poller
        // issuing SQL or timers after this journey has entered cleanup.
        const deadline = Date.now() + 5000;
        while (true) {
            if (check()) return;
            const remaining = deadline - Date.now();
            if (remaining <= 0) throw new Error(`${description} exceeded its observation deadline`);
            await new Promise(resolve => setTimeout(resolve, Math.min(25, remaining)));
        }
    }
    function auditRows(action, id) {
        return jsonSql(`SELECT coalesce(json_agg(a ORDER BY timestamp,id),'[]'::json)::text FROM
            (SELECT id,action,user_id,session_id,host(ip_address) AS ip_address,detail,detail_encoding,timestamp
            FROM plinth.audit_log WHERE action=${literal(action)}
            AND detail_encoding=0 AND detail->>'group_id'=${literal(id)}) a`);
    }
    async function groupAudit(action, id, count, actor, targetUserId = null) {
        let rows;
        await waitUntil(() => { rows = auditRows(action, id); return rows.length >= count; }, `${action} owned audit settlement`);
        assert.equal(rows.length, count);
        for (const row of rows) {
            assert.equal(row.user_id, actor.userId); assert.equal(row.session_id, actor.sessionId);
            assert.equal(row.ip_address, actor.ipAddress); assert.equal(row.detail_encoding, 0);
            assert.equal(row.detail.group_id, id);
            if (targetUserId) assert.equal(row.detail.target_user_id, targetUserId);
            else assert.equal(typeof row.detail.group_name, 'string');
        }
        return rows;
    }
    function addFixtureRule(group, rule) {
        assert.equal(sql(`SELECT count(*) FROM plinth.rbac_rules WHERE rule=${literal(rule)} AND orphaned_at IS NULL`), '1');
        sql(`INSERT INTO plinth.group_rules(group_id,rule_id)
            SELECT ${literal(group)},id FROM plinth.rbac_rules WHERE rule=${literal(rule)} ON CONFLICT DO NOTHING`);
    }
    function fixtureGroup(role, userId, rules) {
        const id = randomUUID(); ownedGroups.add(id);
        sql(`INSERT INTO plinth.groups(id,name,description) VALUES (${literal(id)},${literal(groupName(role))},'owned groups journey fixture');
            INSERT INTO plinth.group_members(group_id,user_id) VALUES (${literal(id)},${literal(userId)})`);
        for (const rule of rules) addFixtureRule(id, rule);
        return id;
    }
    async function createOwned(name, description = '') {
        assert.equal(sql(`SELECT count(*) FROM plinth.groups WHERE name=${literal(name)}`), '0');
        const result = status(await call(page, '/api/groups', 'POST', { name, description }), 201);
        assert.match(result.id, UUID); ownedGroups.add(result.id);
        assert.equal(groupRow(result.id).name, name);
        return result;
    }
    async function applications(p) { return status(await call(p, '/api/frontend/applications'), 200).applications; }
    async function openGroups(principal) {
        const p = principal.page;
        await p.reload(); await p.getByRole('heading', { name: 'Home', exact: true }).waitFor();
        const application = (await applications(p)).find(item => item.id === 'admin');
        assert(application && Array.isArray(application.panels));
        assert(application.panels.some(panel => panel.id === 'groups'), 'Groups must be authorized in the native catalog');
        await p.getByRole('button', { name: 'Administration', exact: true }).click();
        if (application.panels.length > 1) {
            const tab = p.getByRole('tab', { name: 'Groups', exact: true });
            await tab.waitFor({ state: 'visible', timeout: 15000 });
            await tab.click({ timeout: 15000 });
        } else {
            assert.deepEqual(application.panels.map(panel => panel.id), ['groups']);
        }
        await p.locator('#admin-groups-panel:visible').waitFor();
        await p.waitForFunction(username =>
            document.querySelector('#groups-phase')?.dataset.phase === 'active' &&
            document.querySelector('#groups-actor')?.textContent.includes(username), principal.username);
    }
    async function selectGroup(p, row) {
        await p.locator('#groups-filter').fill(row.name);
        await p.locator(`[data-group-id="${row.id}"]`).click();
        await p.waitForFunction(id => document.querySelector('#groups-detail')?.textContent.includes(id), row.id);
    }
    async function uiChange(p, selector, path, method, expected, outcome = 'succeeded') {
        const before = await p.locator('#groups-attempts article').count();
        const results = await Promise.allSettled([
            p.waitForResponse(r => new URL(r.url()).pathname === path && r.request().method() === method,
                { timeout: 15000 }),
            p.locator(selector).click({ timeout: 15000 }),
        ]);
        for (const result of results) if (result.status === 'rejected') throw result.reason;
        const actual = results[0].value;
        assert.equal(actual.status(), expected);
        const body = await actual.json();
        // Capture newly created resources before waiting on the UI oracle so
        // an acknowledgement/render regression cannot bypass normal cleanup.
        if (path === '/api/groups' && method === 'POST' && expected === 201) {
            assert.match(body.id, UUID);
            ownedGroups.add(body.id);
        }
        await p.waitForFunction(({ before, outcome }) => {
            const rows = [...document.querySelectorAll('#groups-attempts article')];
            return rows.length === before + 1 && rows.at(-1)?.dataset.outcome === outcome;
        }, { before, outcome });
        return { response: actual, body };
    }
    async function servedModules(p, application, panelId, paths, expected) {
        const panel = application.panels.find(item => item.id === panelId); assert(panel);
        for (const [key, path] of Object.entries(paths)) {
            const url = key === 'panel' ? panel.module_url :
                new URL(`../${panelId === 'groups' ? 'groups' : 'packages'}/${key}.js`, baseURL + panel.module_url).pathname;
            const result = await p.evaluate(async url => {
                const response = await fetch(url, { credentials: 'same-origin', cache: 'no-store' });
                return { status: response.status, text: await response.text() };
            }, url);
            assert.equal(result.status, 200, path);
            assert.equal(digest(Buffer.from(result.text)), expected[key], `${path} installed served source bytes`);
        }
    }
    async function archiveModules(path, paths, expected, currentSource) {
        for (const [key, member] of Object.entries(paths)) {
            assert.match(expected[key], HASH);
            const bytes = execFileSync('unzip', ['-p', path, member], { timeout: 10000, stdio: ['ignore', 'pipe', 'pipe'] });
            assert.equal(digest(bytes), expected[key], `${member} archive binding`);
            if (currentSource) assert.equal(digest(await readFile(new URL(`../../client/admin/${member}`, import.meta.url))),
                expected[key], `${member} independently read current source binding`);
        }
    }

    try {
        // All pre-G entry assertions precede any attempt to install or clean up.
        await page.getByRole('heading', { name: 'Home', exact: true }).waitFor();
        assert.equal(await page.locator('#admin-packages-panel:visible, #admin-groups-panel:visible').count(), 0);
        assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE id=${literal(legacy.packageId)}`), '0');
        assert.deepEqual(packageRows(), [], 'G entry requires all-state admin absence, not cleanup to manufacture it');
        assert.equal((await applications(page)).some(app => app.id === 'admin'), false);
        actorControl = await observeActor(control); assert.deepEqual(actorControl, legacy.actor);
        const oldAudit = jsonSql(`SELECT row_to_json(a)::text FROM (SELECT id,action,detail,detail_encoding
            FROM plinth.audit_log WHERE id=${literal(legacy.uninstallAudit.id)}) a`);
        assert.equal(oldAudit.action, 'packages.uninstalled'); assert.equal(oldAudit.detail_encoding, 0);
        assert.equal(oldAudit.detail.id, legacy.packageId); assert.equal(oldAudit.detail.name, 'admin');
        assert.equal(oldAudit.detail.version, '0.1.1');
        assert.equal(oldAudit.detail.uninstalled_by_user_id, control.id);
        // Package transition audit records the installer actor in detail. It
        // does not supply the groups handlers' explicit session/IP metadata.
        const manifest = JSON.parse(await readFile(new URL('../../client/admin/manifest.json', import.meta.url), 'utf8'));
        assert.equal(manifest.version, '0.1.1', 'expected current source fixture version');
        for (const [path, expected] of [[baselineArchive, baseline.archiveSha256], [currentArchive, current.archiveSha256]]) {
            assert.match(expected, HASH); assert.equal(digest(await readFile(path)), expected);
        }
        await archiveModules(baselineArchive, MODULE_PATHS, OLD_MODULES, false);
        await archiveModules(currentArchive, MODULE_PATHS, current.moduleHashes, true);
        await archiveModules(currentArchive, GROUP_PATHS, current.groupsModuleHashes, true);
        const installedOld = status(await bounded(() => upload(page, baselineArchive), 'baseline install', 20000), 201);
        oldId = installedOld.id; assert.match(oldId, UUID); ownedPackages.add(oldId);
        assert.notEqual(oldId, legacy.packageId);
        assert.equal(installedOld.version, '0.1.0'); assert.equal(installedOld.state, 'ACTIVE');
        assert.equal(packageRows().find(row => row.id === oldId)?.state, 'ACTIVE');
        const same = await bounded(() => upload(page, baselineArchive), 'same-version rejection', 15000);
        status(same, 409, 'upgrade-version-not-newer');
        assert.equal(packageRows().find(row => row.id === oldId)?.state, 'ACTIVE');
        const oldRule = sql("SELECT id FROM plinth.rbac_rules WHERE rule='plinth.admin.packages' AND extension_name='admin' AND orphaned_at IS NULL");
        assert.match(oldRule, UUID);
        assert.equal(sql(`SELECT count(*) FROM plinth.group_rules WHERE rule_id=${literal(oldRule)}`), '0');
        const controlFixture = fixtureGroup('control', control.id, ['plinth.admin.packages']);
        const retainedGroup = groupRow(controlFixture);
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(controlFixture)}
            AND user_id=${literal(control.id)}`), '1');
        const oldApp = (await applications(page)).find(app => app.id === 'admin');
        assert.equal(oldApp.generation, oldId); assert.equal(oldApp.version, '0.1.0');
        assert.deepEqual(oldApp.panels.map(panel => panel.id), ['packages']);
        await servedModules(page, oldApp, 'packages', MODULE_PATHS, OLD_MODULES);
        // No uninstall, disable, row deletion or version rewriting is allowed
        // between this baseline first install and the real newer-version POST.
        const installedNew = status(await bounded(() => upload(page, currentArchive), 'admin upgrade', 20000), 201);
        currentId = installedNew.id; assert.match(currentId, UUID); ownedPackages.add(currentId);
        assert.notEqual(currentId, oldId); assert.notEqual(currentId, legacy.packageId);
        const rows = packageRows(), previous = rows.find(row => row.id === oldId), upgraded = rows.find(row => row.id === currentId);
        assert.equal(rows.length, 2); assert.equal(previous.state, 'SUPERSEDED'); assert.equal(previous.version, '0.1.0');
        assert.equal(upgraded.state, 'ACTIVE'); assert.equal(upgraded.version, '0.1.1');
        assert.equal(upgraded.supersedes_id, oldId); assert.equal(upgraded.installed_by_user_id, control.id);
        assert.equal(sql("SELECT id FROM plinth.rbac_rules WHERE rule='plinth.admin.packages' AND orphaned_at IS NULL"), oldRule);
        assert.equal(sql(`SELECT count(*) FROM plinth.group_rules WHERE group_id=${literal(controlFixture)} AND rule_id=${literal(oldRule)}`), '1');
        assert.deepEqual(groupRow(controlFixture), retainedGroup, 'ordinary fixture state survives the package upgrade');
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(controlFixture)}
            AND user_id=${literal(control.id)}`), '1');
        assert.equal(sql("SELECT count(*) FROM plinth.group_rules gr JOIN plinth.rbac_rules r ON r.id=gr.rule_id WHERE r.rule='plinth.admin.groups'"), '0');
        const newApp = (await applications(page)).find(app => app.id === 'admin');
        assert.equal(newApp.generation, currentId); assert.equal(newApp.version, '0.1.1');
        // No groups rule was granted, but kernel.admin gives this maintainer
        // universal panel visibility. Non-admin grant isolation is tested next.
        assert.deepEqual(newApp.panels.map(panel => panel.id), ['packages', 'groups']);
        await servedModules(page, newApp, 'packages', MODULE_PATHS, current.moduleHashes);
        completed.push(GROUPS_NATIVE_CASES[0]);

        const rules = jsonSql(`SELECT coalesce(json_agg(r ORDER BY rule),'[]'::json)::text FROM
            (SELECT id,rule,namespace,extension_name FROM plinth.rbac_rules WHERE extension_name='admin' AND orphaned_at IS NULL) r`);
        assert.deepEqual(rules.map(row => [row.rule, row.namespace, row.extension_name]),
            [['plinth.admin.groups', 'plinth', 'admin'], ['plinth.admin.packages', 'plinth', 'admin']]);
        const groupRule = rules.find(row => row.rule === 'plinth.admin.groups').id;
        assert.equal(sql(`SELECT count(*) FROM plinth.group_rules WHERE rule_id=${literal(groupRule)}`), '0');
        addFixtureRule(controlFixture, 'plinth.admin.groups');
        writerFixture = fixtureGroup('writer', principals.writer.id, ['plinth.admin.groups', 'kernel.admin']);
        fixtureGroup('visible', principals.visible.id, ['plinth.admin.groups']);
        writer = await fresh('writer'); visible = await fresh('visible'); denied = await fresh('denied');
        actorWriter = await observeActor(writer);
        const currentApp = (await applications(page)).find(app => app.id === 'admin');
        assert.deepEqual(currentApp.panels.map(panel => [panel.id, panel.title]),
            [['packages', 'Administration'], ['groups', 'Groups']]);
        await servedModules(page, currentApp, 'groups', GROUP_PATHS, current.groupsModuleHashes);
        const visibleApp = (await applications(visible.page)).find(app => app.id === 'admin');
        assert.deepEqual(visibleApp.panels.map(panel => panel.id), ['groups']);
        assert.equal((await applications(denied.page)).some(app => app.id === 'admin'), false);
        await openGroups(visible);
        await visible.page.locator('#groups-list-error[data-error-code="permission_denied"]').waitFor();
        assert.equal(await visible.page.locator('#groups-list [data-group-id]').count(), 0);
        status(await call(visible.page, '/api/groups'), 403, 'permission_denied');
        status(await call(denied.page, '/api/groups'), 403, 'permission_denied');
        completed.push(GROUPS_NATIVE_CASES[1]);

        await openGroups(control);
        const uiName = groupName('ui');
        assert.equal(sql(`SELECT count(*) FROM plinth.groups WHERE name=${literal(uiName)}`), '0');
        await page.locator('#groups-create-name').fill(uiName);
        await page.locator('#groups-create-description').fill('');
        const made = await uiChange(page, '#groups-create', '/api/groups', 'POST', 201);
        assert.match(made.body.id, UUID); ownedGroups.add(made.body.id);
        assert.deepEqual(made.response.request().postDataJSON(), { name: uiName, description: '' });
        await selectGroup(page, { id: made.body.id, name: uiName });
        assert.equal(await page.locator('#groups-edit-description').inputValue(), '');
        assert.equal(groupRow(made.body.id).description, null);
        const renamed = groupName('edited');
        await page.locator('#groups-edit-name').fill(renamed);
        await page.locator('#groups-edit-description').fill('Reviewed description');
        await uiChange(page, '#groups-update', `/api/groups/${made.body.id}`, 'PUT', 200);
        assert.deepEqual(groupRow(made.body.id), { id: made.body.id, name: renamed,
            description: 'Reviewed description', built_in: false });
        await page.locator('#groups-edit-description').fill('');
        const cleared = await uiChange(page, '#groups-update', `/api/groups/${made.body.id}`, 'PUT', 200);
        assert.deepEqual(cleared.response.request().postDataJSON(), { name: renamed, description: '' });
        assert.equal(groupRow(made.body.id).description, null);
        const detail = status(await call(page, `/api/groups/${made.body.id}`), 200);
        assert.equal(Object.hasOwn(detail, 'description'), false);
        await page.waitForFunction(name => document.querySelector('#groups-selected-title')?.textContent === name, renamed);
        await page.locator('#groups-delete-name').fill(renamed);
        await uiChange(page, '#groups-delete', `/api/groups/${made.body.id}`, 'DELETE', 200);
        assert.equal(groupRow(made.body.id), null); ownedGroups.delete(made.body.id);
        await groupAudit('group.created', made.body.id, 1, actorControl);
        await groupAudit('group.updated', made.body.id, 2, actorControl);
        await groupAudit('group.deleted', made.body.id, 1, actorControl);
        completed.push(GROUPS_NATIVE_CASES[2]);

        const builtins = jsonSql("SELECT json_agg(g ORDER BY name)::text FROM (SELECT id,name,description,built_in FROM plinth.groups WHERE name IN ('admin','everyone')) g");
        assert.equal(builtins.length, 2); assert(builtins.every(row => row.built_in));
        adminBuiltinId = builtins.find(row => row.name === 'admin').id;
        const everyoneId = builtins.find(row => row.name === 'everyone').id;
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(control.id)}`), '1');
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(writer.id)}`), '0');
        for (const row of builtins) {
            for (const fields of [{ name: groupName('builtin') }, { description: 'not allowed' },
                { name: groupName('builtin'), description: 'not allowed' }])
                status(await call(page, `/api/groups/${row.id}`, 'PUT', fields), 403, 'permission_denied');
            status(await call(page, `/api/groups/${row.id}`, 'DELETE'), 403, 'permission_denied');
            assert.deepEqual(groupRow(row.id), row);
        }
        status(await call(page, `/api/groups/${everyoneId}/members`, 'POST', { user_id: writer.id }), 409, 'immutable_membership');
        status(await call(page, `/api/groups/${everyoneId}/members/${writer.id}`, 'DELETE'), 409, 'immutable_membership');
        await selectGroup(page, builtins.find(row => row.name === 'everyone'));
        assert.equal(await page.locator('#groups-member-add').isDisabled(), true);
        assert.equal(await page.locator('#groups-edit-name').isEditable(), false);
        assert.equal(await page.locator('#groups-edit-description').isEditable(), false);
        assert.match(await page.locator('#groups-virtual-membership').textContent(), /not a count of all identities/);
        await selectGroup(page, builtins.find(row => row.name === 'admin'));
        await page.locator('#groups-member-id').fill(writer.id);
        assert.equal(await page.locator('#groups-member-add').isDisabled(), true);
        await page.locator('#groups-admin-member-confirm').fill(writer.id);
        await uiChange(page, '#groups-member-add', `/api/groups/${adminBuiltinId}/members`, 'POST', 201);
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(writer.id)}`), '1');
        await uiChange(page, '#groups-member-remove', `/api/groups/${adminBuiltinId}/members/${writer.id}`, 'DELETE', 200);
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(writer.id)}`), '0');
        assert.equal(sql(`SELECT count(*) FROM plinth.group_members WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(control.id)}`), '1');
        await groupAudit('group.member_added', adminBuiltinId, 1, actorControl, writer.id);
        await groupAudit('group.member_removed', adminBuiltinId, 1, actorControl, writer.id);
        completed.push(GROUPS_NATIVE_CASES[3]);

        const members = await createOwned(groupName('members'));
        await openGroups(control); await selectGroup(page, members);
        assert.equal(memberCount(members.id), 0);
        await page.locator('#groups-member-id').fill(writer.id);
        const added = await uiChange(page, '#groups-member-add', `/api/groups/${members.id}/members`, 'POST', 201);
        assert.equal(added.body.group_id, members.id); assert.equal(added.body.user_id, writer.id);
        assert.equal(memberCount(members.id), 1);
        assert.equal(status(await call(page, `/api/groups/${members.id}`), 200).member_count, 1);
        status(await call(page, `/api/groups/${members.id}/members`, 'POST', { user_id: writer.id }), 409, 'already_member');
        const absentUser = randomUUID();
        assert.equal(sql(`SELECT count(*) FROM plinth.users WHERE id=${literal(absentUser)}`), '0');
        status(await call(page, `/api/groups/${members.id}/members`, 'POST', { user_id: absentUser }), 404, 'user_not_found');
        await uiChange(page, '#groups-member-remove', `/api/groups/${members.id}/members/${writer.id}`, 'DELETE', 200);
        status(await call(page, `/api/groups/${members.id}/members/${writer.id}`, 'DELETE'), 404, 'not_a_member');
        assert.equal(memberCount(members.id), 0);
        assert.equal(status(await call(page, `/api/groups/${members.id}`), 200).member_count, 0);
        await groupAudit('group.member_added', members.id, 1, actorControl, writer.id);
        await groupAudit('group.member_removed', members.id, 1, actorControl, writer.id);
        completed.push(GROUPS_NATIVE_CASES[4]);

        const raceName = groupName('race');
        assert.equal(sql(`SELECT count(*) FROM plinth.groups WHERE name=${literal(raceName)}`), '0');
        const actors = [control, writer], identities = [actorControl, actorWriter];
        const creationResults = await Promise.allSettled(actors.map(actor => call(actor.page,
            '/api/groups', 'POST', { name: raceName, description: '' })));
        for (const result of creationResults) {
            if (result.status === 'fulfilled' && result.value.status === 201) {
                assert.match(result.value.body.id, UUID);
                ownedGroups.add(result.value.body.id);
            }
        }
        for (const result of creationResults) if (result.status === 'rejected') throw result.reason;
        const creates = creationResults.map(result => result.value);
        assert.deepEqual(creates.map(result => result.status).sort(), [201, 409]);
        const winner = creates.findIndex(result => result.status === 201), raceId = creates[winner].body.id;
        assert.match(raceId, UUID); ownedGroups.add(raceId);
        status(creates[1 - winner], 409, 'group_exists');
        assert.equal(sql(`SELECT count(*) FROM plinth.groups WHERE name=${literal(raceName)}`), '1');
        await groupAudit('group.created', raceId, 1, identities[winner]);
        const additionResults = await Promise.allSettled(actors.map(actor => call(actor.page,
            `/api/groups/${raceId}/members`, 'POST', { user_id: writer.id })));
        for (const result of additionResults) if (result.status === 'rejected') throw result.reason;
        const additions = additionResults.map(result => result.value);
        assert.deepEqual(additions.map(result => result.status).sort(), [201, 409]);
        const memberWinner = additions.findIndex(result => result.status === 201);
        status(additions[1 - memberWinner], 409, 'already_member');
        assert.equal(memberCount(raceId), 1);
        await groupAudit('group.member_added', raceId, 1, identities[memberWinner], writer.id);
        completed.push(GROUPS_NATIVE_CASES[5]);

        const anonymous = { context: null, page: null }; ownedContexts.push(anonymous);
        anonymous.context = await browser.newContext(); anonymous.page = await anonymous.context.newPage();
        anonymous.page.setDefaultTimeout(15000);
        await anonymous.page.goto(baseURL + '/app/');
        await anonymous.page.getByRole('heading', { name: 'Sign in to Plinth', exact: true }).waitFor();
        const blockedName = groupName('blocked');
        status(await call(anonymous.page, '/api/groups'), 401);
        status(await call(anonymous.page, '/api/groups', 'POST', { name: blockedName, description: '' }), 401);
        const actorVisible = await observeActor(visible);
        async function deniedAudit(actor, issue) {
            const rows = () => jsonSql(`SELECT coalesce(json_agg(a),'[]'::json)::text FROM
                (SELECT id,user_id,session_id,host(ip_address) AS ip_address,detail FROM plinth.audit_log
                WHERE action='rbac.denied' AND user_id=${literal(actor.userId)} AND session_id=${literal(actor.sessionId)}
                AND detail_encoding=0 AND detail->>'path'='/api/groups' AND detail->>'method'='POST') a`);
            const before = new Set(rows().map(row => row.id)); await issue();
            let addedRows;
            await waitUntil(() => { addedRows = rows().filter(row => !before.has(row.id)); return addedRows.length > 0; }, 'owned RBAC denial audit settlement');
            assert.equal(addedRows.length, 1);
            assert.equal(addedRows[0].user_id, actor.userId);
            assert.equal(addedRows[0].session_id, actor.sessionId);
            assert.equal(addedRows[0].ip_address, actor.ipAddress);
            assert.equal(addedRows[0].detail.path, '/api/groups');
            assert.equal(addedRows[0].detail.method, 'POST');
            assert.equal(addedRows[0].detail.reason, 'permission_denied');
            assert.deepEqual(addedRows[0].detail.required_rules, ['kernel.admin']);
        }
        await deniedAudit(actorVisible, async () => status(await call(visible.page, '/api/groups', 'POST',
            { name: blockedName, description: '' }), 403, 'permission_denied'));
        const csrf = await page.evaluate(async name => {
            const response = await fetch('/api/groups', { method: 'POST', credentials: 'same-origin',
                headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name, description: '' }) });
            return { status: response.status, body: await response.json() };
        }, blockedName);
        status(csrf, 403, 'csrf_failed');
        sql(`DELETE FROM plinth.group_rules WHERE group_id=${literal(writerFixture)}
            AND rule_id=(SELECT id FROM plinth.rbac_rules WHERE rule='kernel.admin')`);
        await deniedAudit(actorWriter, async () => status(await call(writer.page, '/api/groups', 'POST',
            { name: blockedName, description: '' }), 403, 'permission_denied'));
        assert.equal((await observeActor(writer)).sessionId, actorWriter.sessionId, '403 is not a fake logout');
        await openGroups(writer);
        await writer.page.locator('#groups-list-error[data-error-code="permission_denied"]').waitFor();
        assert.equal(await writer.page.locator('#groups-detail').count(), 0);
        assert.equal(sql(`SELECT count(*) FROM plinth.groups WHERE name=${literal(blockedName)}`), '0');
        addFixtureRule(writerFixture, 'kernel.admin');
        completed.push(GROUPS_NATIVE_CASES[6]);

        const uncertain = await createOwned(groupName('unknown'));
        await writer.page.addInitScript(() => {
            const actualFetch = window.fetch.bind(window);
            const control = { id: null, lose: false, holdDetail: false, writes: 0,
                committedStatus: null, releaseDetail: null, heldId: null };
            window.__groupsNativeDelivery = control;
            window.fetch = async (url, options) => {
                const path = new URL(url instanceof Request ? url.url : url, location.href).pathname;
                const method = options?.method || (url instanceof Request ? url.method : 'GET');
                if (path === `/api/groups/${control.id}` && method === 'PUT') {
                    control.writes++;
                    const response = await actualFetch(url, options);
                    if (control.lose && response.status === 200) {
                        control.lose = false; control.committedStatus = response.status;
                        throw new TypeError('controlled lost reply after real acknowledged write');
                    }
                    return response;
                }
                if (control.holdDetail && path === `/api/groups/${control.id}` && method === 'GET') {
                    control.holdDetail = false;
                    const response = await actualFetch(url, options);
                    control.heldId = (await response.clone().json()).id;
                    return new Promise(resolve => { control.releaseDetail = () => resolve(response); });
                }
                return actualFetch(url, options);
            };
        });
        await openGroups(writer); await selectGroup(writer.page, uncertain);
        await writer.page.evaluate(id => { const c = window.__groupsNativeDelivery; c.id = id; c.lose = true; }, uncertain.id);
        await writer.page.locator('#groups-edit-description').fill('Committed request with withheld reply');
        await uiChange(writer.page, '#groups-update', `/api/groups/${uncertain.id}`, 'PUT', 200, 'unknown');
        assert.equal(groupRow(uncertain.id).description, 'Committed request with withheld reply');
        assert.deepEqual(await writer.page.evaluate(() => ({ status: window.__groupsNativeDelivery.committedStatus,
            writes: window.__groupsNativeDelivery.writes })), { status: 200, writes: 1 });
        await groupAudit('group.updated', uncertain.id, 1, actorWriter);
        const observedRequests = [];
        const observed = r => observedRequests.push({ path: new URL(r.url()).pathname, method: r.method() });
        writer.page.on('request', observed);
        try {
            await writer.page.locator('[id^="groups-observe-"]').click();
            await writer.page.locator('[data-outcome="unknown"][data-reconciliation="complete"]').waitFor();
        } finally { writer.page.off('request', observed); }
        assert(observedRequests.every(r => r.method === 'GET'));
        assert.equal(observedRequests.filter(r => r.path === '/api/groups').length, 1);
        assert.equal(observedRequests.filter(r => r.path === `/api/groups/${uncertain.id}`).length, 1);
        assert.equal(await writer.page.locator('[data-outcome="succeeded"]').count(), 0);
        assert.equal(await writer.page.evaluate(() => window.__groupsNativeDelivery.writes), 1);
        await writer.page.evaluate(() => { window.__groupsNativeDelivery.holdDetail = true; });
        await writer.page.locator(`[data-group-id="${uncertain.id}"]`).click();
        await writer.page.waitForFunction(id => window.__groupsNativeDelivery.heldId === id &&
            typeof window.__groupsNativeDelivery.releaseDetail === 'function', uncertain.id);
        await writer.page.getByRole('button', { name: 'Home', exact: true }).click();
        await writer.page.evaluate(() => window.__groupsNativeDelivery.releaseDetail());
        await writer.page.waitForFunction(() => document.querySelector('#groups-phase')?.dataset.phase === 'inactive');
        assert.equal(await writer.page.locator('#groups-detail').count(), 0);
        assert.equal(await writer.page.locator('#groups-attempts article').count(), 0);
        assert.equal(await writer.page.evaluate(() => window.__groupsNativeDelivery.writes), 1);
        completed.push(GROUPS_NATIVE_CASES[7]);
    } catch (error) { failure = error; }
    finally {
        // Retire UI owners before settling deferred native reads/closing only
        // new contexts. The control maintainer stays alive for normal cleanup.
        for (const principal of ownedContexts) {
            try {
                if (principal.page && !principal.page.isClosed()) {
                    const home = principal.page.getByRole('button', { name: 'Home', exact: true });
                    if (await home.count() && await home.isVisible()) await bounded(() => home.click(), 'groups owned Home cleanup', 5000);
                    await principal.page.evaluate(() => window.__groupsNativeDelivery?.releaseDetail?.());
                }
            } catch (error) { cleanupErrors.push(error); }
            try { if (principal.context) await bounded(() => principal.context.close(), 'groups owned context cleanup', 5000); }
            catch (error) { cleanupErrors.push(error); }
        }
        try {
            if (adminBuiltinId && writer?.id && Number(sql(`SELECT count(*) FROM plinth.group_members
                WHERE group_id=${literal(adminBuiltinId)} AND user_id=${literal(writer.id)}`)) > 0)
                status(await call(page, `/api/groups/${adminBuiltinId}/members/${writer.id}`, 'DELETE'), 200);
        } catch (error) { cleanupErrors.push(error); }
        for (const id of ownedGroups) {
            try {
                const row = groupRow(id); if (!row) continue;
                assert.equal(row.built_in, false);
                status(await call(page, `/api/groups/${id}`, 'DELETE'), 200);
                assert.equal(groupRow(id), null);
            } catch (error) { cleanupErrors.push(error); }
        }
        try {
            const home = page.getByRole('button', { name: 'Home', exact: true });
            if (await home.count() && await home.isVisible()) await bounded(() => home.click(), 'groups control Home cleanup', 5000);
            for (const row of packageRows().filter(row => LIVE.includes(row.state))) {
                assert(ownedPackages.has(row.id), 'cleanup cannot adopt an untracked live admin package');
                assert.equal(row.installed_by_user_id, control.id);
                status(await call(page, `/api/packages/${row.id}?confirm=true`, 'DELETE'), 204);
                assert.equal(sql(`SELECT count(*) FROM plinth.packages WHERE id=${literal(row.id)}`), '0');
            }
            assert.equal(packageRows().filter(row => LIVE.includes(row.state)).length, 0);
            assert.equal((await applications(page)).some(app => app.id === 'admin'), false);
            // Baseline SUPERSEDED history is legitimate after uninstall. The
            // parent's later exact-owned terminal-history teardown is separate.
            assert(packageRows().every(row => ownedPackages.has(row.id) && ['SUPERSEDED', 'INSTALL_FAILED'].includes(row.state)));
        } catch (error) { cleanupErrors.push(error); }
    }
    if (failure || cleanupErrors.length) throw new AggregateError(
        [...(failure ? [failure] : []), ...cleanupErrors], 'native groups journey or owned cleanup failed');
    assert.deepEqual(completed, GROUPS_NATIVE_CASES);
    return { groupsAdminNativeCases: 8, expectedCases: 8, cases: [...completed],
        actualAuth: true, actualRbacCsrf: true, legacyProof: legacy,
        baseline: { commit: BASE_COMMIT, tree: BASE_TREE, name: 'admin', version: '0.1.0',
            archiveSha256: baseline.archiveSha256, moduleHashes: { ...OLD_MODULES }, packageId: oldId },
        upgraded: { name: 'admin', version: '0.1.1', archiveSha256: current.archiveSha256,
            groupsModuleHashes: { ...current.groupsModuleHashes }, packageId: currentId, supersedesId: oldId },
        cleanup: 'PASS' };
}
