// Actual groups transport/controller and shipping withCsrf; platform I/O is
// controlled. These units do not establish native authority or package proof.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import vm from 'node:vm';

const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const admin = resolve(repo, 'client/admin/client/groups');
const shell = resolve(repo, 'client/shell/client');
const paths = [resolve(admin, 'api.js'), resolve(admin, 'controller.js'),
    ...['sdk.js', 'data-query.js', 'data-controller.js'].map(path => resolve(shell, path))];
const sources = new Map(await Promise.all(paths.map(async path => [path, await readFile(path, 'utf8')])));
const plain = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));
const id = '11111111-1111-4111-8111-111111111111';
const otherId = '22222222-2222-4222-8222-222222222222';
const thirdId = '44444444-4444-4444-8444-444444444444';
const userId = 'abcdefab-1234-4234-8234-abcdefabcdef';
const row = { id, name: 'team', built_in: false, created_at: '2026-10-24 00:00:00+00' };
const detail = { ...row, member_count: 0 };
const other = { ...row, id: otherId, name: 'other' };
const actorA = { user: { id: 'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa', username: 'alice' },
    session: { id: 'a1111111-1111-4111-8111-111111111111' } };
const actorB = { user: { id: 'bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb', username: 'bob' },
    session: { id: 'b1111111-1111-4111-8111-111111111111' } };
const response = (status, body) => ({ status, json: async () => body });
const error = (status, code, unknown = false) => Object.assign(new Error(code),
    { status, code, unknown, dispatched: true });
function deferred() {
    let resolvePromise;
    const promise = new Promise(resolveValue => { resolvePromise = resolveValue; });
    return { promise, resolve: resolvePromise };
}
async function until(predicate) {
    for (let index = 0; index < 100; index++) {
        if (predicate()) return;
        await Promise.resolve();
    }
    assert.fail('controlled promise sequence did not reach its expected boundary');
}

async function fixture() {
    const document = { cookie: 'plinth_csrf=fake-A' };
    const context = vm.createContext({ URL, URLSearchParams, Headers, AbortController,
        DOMException, TextEncoder, document,
        window: { location: { href: 'https://plinth.test/app/', origin: 'https://plinth.test',
            protocol: 'https:', host: 'plinth.test' } },
        fetch() { throw new Error('unexpected global fetch'); }, queueMicrotask,
        console: { error() {} },
        setTimeout() { throw new Error('unexpected transport timer'); }, clearTimeout() {},
    });
    const hooks = new vm.SyntheticModule(['useEffect', 'useRef', 'useState'], function () {
        for (const name of ['useEffect', 'useRef', 'useState']) this.setExport(name,
            () => { throw new Error('unexpected framework hook in unit'); });
    }, { context, identifier: 'platform:preact/hooks' });
    const preact = new vm.SyntheticModule(['h'], function () {
        this.setExport('h', () => { throw new Error('unexpected rendering in unit'); });
    }, { context, identifier: 'platform:preact' });
    const modules = new Map();
    const load = path => {
        assert(sources.has(path), `unapproved module import ${path}`);
        if (!modules.has(path)) modules.set(path, new vm.SourceTextModule(sources.get(path),
            { context, identifier: path }));
        return modules.get(path);
    };
    const link = (specifier, importer) => {
        if (specifier === '@plinth/frontend/sdk') return load(resolve(shell, 'sdk.js'));
        if (specifier === 'preact/hooks') return hooks;
        if (specifier === 'preact') return preact;
        assert(specifier.startsWith('./') || specifier.startsWith('../'));
        return load(resolve(dirname(importer.identifier), specifier));
    };
    const api = load(resolve(admin, 'api.js')), controller = load(resolve(admin, 'controller.js'));
    await api.link(link); await api.evaluate();
    if (controller.status === 'unlinked') await controller.link(link);
    await controller.evaluate();
    return { api: api.namespace, controller: controller.namespace, document };
}

async function owned() {
    const modules = await fixture();
    const state = { actor: actorA, groups: new Map([[id, { ...detail }],
        [otherId, { ...other, member_count: 0 }]]), next: 0 };
    const calls = [], emitted = [], ended = [], handlers = {};
    const defaults = {
        session: async () => plain(state.actor),
        list: async () => ({ groups: [...state.groups.values()].map(value => {
            const { member_count, ...group } = value; return plain(group);
        }) }),
        detail: async target => {
            if (!state.groups.has(target)) throw error(404, 'group_not_found');
            return plain(state.groups.get(target));
        },
        create: async fields => {
            const value = { ...row, ...plain(fields), id: [thirdId, userId][state.next++], member_count: 0 };
            state.groups.set(value.id, value);
            const { member_count, ...group } = value; return group;
        },
        update: async (target, fields) => {
            const value = { ...state.groups.get(target), ...plain(fields) };
            state.groups.set(target, value);
            const { member_count, ...group } = value; return group;
        },
        remove: async target => { state.groups.delete(target); return { status: 'deleted' }; },
        addMember: async (target, user) => {
            state.groups.get(target).member_count++;
            return { group_id: target, user_id: user, added_at: '2026-10-24 00:01:00+00' };
        },
        removeMember: async target => {
            state.groups.get(target).member_count--;
            return { status: 'removed' };
        },
    };
    const api = Object.fromEntries(Object.entries(defaults).map(([name, implementation]) =>
        [name, async (...args) => {
            calls.push({ name, args });
            return (handlers[name] || implementation)(...args);
        }]));
    const controller = modules.controller.createGroupsController({ api,
        publish: snapshot => emitted.push(plain(snapshot)), onSessionEnd: reason => ended.push(reason) });
    return { ...modules, state, calls, emitted, ended, handlers, defaults, model: controller,
        async ready() { await controller.activate(); await controller.select(id); calls.length = 0; } };
}

test('U01 native list/detail/session shapes and exact GET paths are read without mutation authority', async () => {
    const { api } = await fixture(), seen = [];
    const client = api.createGroupsApi({ fetch: async (url, options) => {
        seen.push({ url, options });
        return response(200, url === '/api/auth/session' ? actorA : url === '/api/groups' ?
            { groups: [row] } : detail);
    } });
    assert.deepEqual(plain(await client.session()), actorA);
    assert.deepEqual(plain(await client.list()), { groups: [row] });
    assert.deepEqual(plain(await client.detail(id)), detail);
    assert.deepEqual(seen.map(item => item.url), ['/api/auth/session', '/api/groups', `/api/groups/${id}`]);
    for (const item of seen) {
        assert.equal(item.options.credentials, 'same-origin'); assert.equal(item.options.cache, 'no-store');
        assert.equal(new Headers(item.options.headers).has('X-Plinth-CSRF'), false);
    }
});

test('U02 create and update send explicit blank descriptions and validate their own acknowledgements', async () => {
    const { api } = await fixture(), seen = [];
    const client = api.createGroupsApi({ fetch: async (url, options) => {
        seen.push({ url, options }); return response(options.method === 'POST' ? 201 : 200, row);
    } });
    assert.deepEqual(plain(await client.create({ name: 'team' })), row);
    assert.deepEqual(plain(await client.update(id, { name: 'team', description: '' })), row);
    assert.deepEqual(seen.map(item => [item.options.method, item.options.body]),
        [['POST', '{"name":"team","description":""}'], ['PUT', '{"name":"team","description":""}']]);
    assert.equal(new Headers(seen[1].options.headers).get('Content-Type'), 'application/json');
    assert.equal(new Headers(seen[1].options.headers).has('If-Match'), false);
});

test('U03 delete and explicit member actions require native JSON200/201 receipts and canonical UUID paths', async () => {
    const { api } = await fixture(), seen = [];
    const client = api.createGroupsApi({ fetch: async (url, options) => {
        seen.push({ url, options });
        if (options.method === 'POST') return response(201, { group_id: id, user_id: userId, added_at: 'now' });
        return response(200, { status: url.endsWith(userId) ? 'removed' : 'deleted' });
    } });
    assert.deepEqual(plain(await client.remove(id)), { status: 'deleted' });
    assert.equal((await client.addMember(id, userId.toUpperCase())).user_id, userId);
    assert.deepEqual(plain(await client.removeMember(id, userId.toUpperCase())), { status: 'removed' });
    assert.deepEqual(seen.map(item => [item.url, item.options.method]),
        [[`/api/groups/${id}`, 'DELETE'], [`/api/groups/${id}/members`, 'POST'],
            [`/api/groups/${id}/members/${userId}`, 'DELETE']]);
    assert.equal(seen[1].options.body, JSON.stringify({ user_id: userId }));
    const wrong = api.createGroupsApi({ fetch: async () => response(204, {}) });
    await assert.rejects(wrong.remove(id), value => value.unknown && value.code === 'http-error');
});

test('U04 shipped withCsrf reads the current cookie for every mutation and preserves cancellation of reads', async () => {
    const f = await fixture(), seen = [], abort = new AbortController();
    const client = f.api.createGroupsApi({ fetch: async (url, options) => {
        seen.push({ url, options }); return response(options.method === 'POST' ? 201 : 200, row);
    } });
    await client.create({ name: 'team' });
    f.document.cookie = 'plinth_csrf=fake-B';
    await client.update(id, { name: 'team', description: '' });
    assert.deepEqual(seen.map(item => new Headers(item.options.headers).get('X-Plinth-CSRF')), ['fake-A', 'fake-B']);
    const reads = f.api.createGroupsApi({ fetch: async (_url, options) => {
        assert.equal(options.signal, abort.signal); return response(200, detail);
    } });
    await reads.detail(id, { signal: abort.signal });
});

test('U05 invalid identities and byte-bounded fields fail before fetch without trimming or inventing a directory', async () => {
    const { api } = await fixture(); let dispatched = 0;
    const client = api.createGroupsApi({ fetch: async () => { dispatched++; throw new Error('must not dispatch'); } });
    for (const input of [{ name: '' }, { name: 'UPPER' }, { name: ' team' }, { name: 'a'.repeat(65) },
        { name: 'team', description: null }, { name: 'team', description: 'é'.repeat(129) }]) {
        await assert.rejects(client.create(input), value => value.name === 'TypeError');
    }
    await assert.rejects(client.detail('../users'), value => value.name === 'TypeError');
    await assert.rejects(client.addMember(id, 'alice'), value => value.name === 'TypeError');
    assert.equal(dispatched, 0);
    assert.deepEqual(plain(api.validateGroupInput({ name: 'team', description: 'é'.repeat(128) })),
        { name: 'team', description: 'é'.repeat(128) });
});

test('U06 wrong group/member attribution and malformed success cannot become an acknowledged mutation', async () => {
    const { api } = await fixture();
    for (const body of [{ ...row, id: otherId }, { ...row, name: 'different' }, { ...row, description: null }]) {
        const client = api.createGroupsApi({ fetch: async () => response(200, body) });
        await assert.rejects(client.update(id, { name: 'team' }), value => value.unknown && value.code === 'invalid-response');
    }
    const read = api.createGroupsApi({ fetch: async () => response(200, { groups: [row, row] }) });
    await assert.rejects(read.list(), value => !value.unknown && value.code === 'invalid-response');
    const member = api.createGroupsApi({ fetch: async () => response(201,
        { group_id: id, user_id: otherId, added_at: 'now' }) });
    await assert.rejects(member.addMember(id, userId), value => value.unknown && value.code === 'invalid-response');
});

test('U07 structured native rejections retain status/code and never pretend an admitted success', async () => {
    const { api } = await fixture();
    for (const [status, code] of [[400, 'name_invalid'], [403, 'permission_denied'],
        [404, 'group_not_found'], [409, 'group_exists']]) {
        const client = api.createGroupsApi({ fetch: async () => response(status, { error: code, message: 'rejected' }) });
        await assert.rejects(client.create({ name: 'team' }), value => value.status === status &&
            value.code === code && value.dispatched && !value.unknown);
    }
});

test('U08 network/nonJSON/unexpected success/5xx remain UNKNOWN while preparation stays undispatched', async () => {
    const { api } = await fixture();
    for (const fetch of [async () => { throw new Error('lost'); },
        async () => ({ status: 201, json: async () => { throw new Error('not JSON'); } }),
        async () => response(200, row), async () => response(503, { error: 'service_unavailable' })]) {
        const client = api.createGroupsApi({ fetch });
        await assert.rejects(client.create({ name: 'team' }), value => value.unknown && value.dispatched);
    }
    let called = false;
    const prepared = api.createGroupsApi({ csrf() { throw new Error('no cookie'); },
        fetch() { called = true; } });
    await assert.rejects(prepared.create({ name: 'team' }), value => !value.dispatched && !value.unknown &&
        value.code === 'request-preparation');
    assert.equal(called, false);
});

test('U09 PREPARED ownership has no I/O and snapshots cannot mutate owned forms or policy', async () => {
    const f = await owned();
    assert.equal(f.model.read().phase, 'prepared');
    assert.equal(f.model.setCreateDraft({ name: 'ignored' }), false);
    assert.equal(await f.model.create(), null);
    const exposed = f.model.read(); exposed.createDraft.name = 'external'; exposed.policy.editMetadata = true;
    assert.equal(f.model.read().createDraft.name, '');
    assert.equal(f.model.read().policy.editMetadata, false);
    assert.equal(f.calls.length, 0);
});

test('U10 activation establishes the observed actor and fences list data with real session reads', async () => {
    const f = await owned(); await f.model.activate();
    assert.deepEqual(f.calls.map(item => item.name), ['session', 'session', 'list', 'session']);
    assert.deepEqual(plain(f.model.read().actor), { id: actorA.user.id, username: 'alice', sessionId: actorA.session.id });
    assert.equal(f.model.read().phase, 'active'); assert.equal(f.model.read().items.length, 2);
    await f.model.activate(); assert.equal(f.calls.length, 4, 'same active owner is not re-created');
});

test('U11 one activation serializes writes and captures form values before later edits', async () => {
    const f = await owned(); await f.ready();
    const blocked = deferred(), started = deferred(); let count = 0;
    f.handlers.create = async fields => {
        if (count++ === 0) { started.resolve(); await blocked.promise; }
        return f.defaults.create(fields);
    };
    f.model.setCreateDraft({ name: 'first', description: '' }); const first = f.model.create();
    await started.promise;
    f.model.setCreateDraft({ name: 'second' }); const second = f.model.create();
    f.model.setCreateDraft({ name: 'later' });
    assert.equal(f.calls.filter(item => item.name === 'create').length, 1);
    blocked.resolve(); await Promise.all([first, second]);
    assert.deepEqual(f.calls.filter(item => item.name === 'create').map(item => plain(item.args[0])),
        [{ name: 'first', description: '' }, { name: 'second', description: '' }]);
    assert.deepEqual(plain(f.model.read().attempts.map(item => item.status)), ['succeeded', 'succeeded']);
});

test('U12 retirement bars queued writes and cannot abort an already admitted mutation into rollback', async () => {
    const f = await owned(); await f.ready(); const blocked = deferred(), started = deferred();
    f.handlers.create = async fields => { started.resolve(); await blocked.promise; return f.defaults.create(fields); };
    f.model.setCreateDraft({ name: 'first' }); const first = f.model.create(); await started.promise;
    f.model.setCreateDraft({ name: 'queued' }); const second = f.model.create();
    const write = f.calls.find(item => item.name === 'create'); assert.equal(write.args[1].signal, undefined);
    f.model.deactivate(); blocked.resolve(); await Promise.all([first, second]);
    assert.equal(f.calls.filter(item => item.name === 'create').length, 1);
    assert.equal(f.model.read().phase, 'inactive'); assert.equal(f.model.read().attempts.length, 0);
    assert.deepEqual(plain(f.model.read().createDraft), { name: '', description: '' });
});

test('U13 rotated cookie actor cannot admit a queued mutation under the old session', async () => {
    const f = await owned(); await f.ready(); f.model.setCreateDraft({ name: 'never-sent' });
    f.state.actor = actorB; await f.model.create();
    assert.equal(f.calls.filter(item => item.name === 'create').length, 0);
    assert.equal(f.model.read().phase, 'session-ended'); assert.equal(f.model.read().actor, null);
    assert.deepEqual(f.ended, ['session_rotated']);
});

test('U14 read admission and acknowledgement both reject a changed session before publishing data', async () => {
    const pre = await owned(); await pre.ready(); pre.state.actor = actorB; await pre.model.loadList();
    assert.equal(pre.calls.filter(item => item.name === 'list').length, 0);
    const post = await owned(); await post.ready(); const blocked = deferred(), started = deferred();
    post.handlers.list = async () => { started.resolve(); return blocked.promise; };
    const pending = post.model.loadList(); await started.promise;
    post.state.actor = actorB; blocked.resolve({ groups: [{ ...row, name: 'private-b' }] }); await pending;
    assert.equal(post.model.read().phase, 'session-ended'); assert.equal(post.model.read().items.length, 0);
    assert(!JSON.stringify(post.model.read()).includes('private-b'));
});

test('U15 an abort-ignoring GET completion cannot publish after owner deactivation', async () => {
    const f = await owned(); await f.ready(); const blocked = deferred(), started = deferred(); let options;
    f.handlers.list = async input => { options = input; started.resolve(); return blocked.promise; };
    const pending = f.model.loadList(); await started.promise; f.model.deactivate();
    assert.equal(options.signal.aborted, true);
    blocked.resolve({ groups: [{ ...row, name: 'retired-private' }] }); await pending;
    assert.equal(f.model.read().items.length, 0); assert.equal(f.model.read().phase, 'inactive');
    assert(!JSON.stringify(f.model.read()).includes('retired-private'));
});

test('U16 a newer selected group supersedes a late detail request and its edit draft', async () => {
    const f = await owned(); await f.ready(); const blocked = deferred(), started = deferred();
    f.handlers.detail = async target => {
        if (target === id) { started.resolve(); return blocked.promise; }
        return f.defaults.detail(target);
    };
    const pending = f.model.select(id); await started.promise; await f.model.select(otherId);
    blocked.resolve({ ...detail, name: 'stale' }); await pending;
    assert.equal(f.model.read().selectedId, otherId); assert.equal(f.model.read().detail.name, 'other');
    assert.equal(f.model.read().editDraft.name, 'other'); assert.equal(f.model.read().pending.detail, false);
    // A delete queued behind an admitted create must also retire a newer,
    // pre-delete GET for the same selected ID, even if that transport ignores abort.
    const deleted = await owned(); await deleted.ready();
    const createGate = deferred(), createStarted = deferred(), detailGate = deferred(), detailStarted = deferred();
    let detailOptions;
    deleted.handlers.create = async fields => {
        createStarted.resolve(); await createGate.promise; return deleted.defaults.create(fields);
    };
    deleted.model.setCreateDraft({ name: 'created-elsewhere' });
    const creation = deleted.model.create(); await createStarted.promise;
    deleted.model.setDeleteConfirmation('team'); const deletion = deleted.model.remove();
    deleted.handlers.detail = async (_target, options) => {
        detailOptions = options; detailStarted.resolve(); return detailGate.promise;
    };
    const late = deleted.model.select(id); await detailStarted.promise;
    createGate.resolve(); await Promise.all([creation, deletion]);
    assert.equal(detailOptions.signal.aborted, true);
    detailGate.resolve({ ...detail, description: 'pre-delete data' }); await late;
    const final = deleted.model.read();
    assert.equal(final.selectedId, null); assert.equal(final.detail, null);
    assert.deepEqual(plain(final.editDraft), { name: '', description: '' });
    assert.equal(final.pending.detail, false);
});

test('U17 component destruction is terminal/idempotent and cannot reactivate or publish late data', async () => {
    const f = await owned(); await f.ready(); const blocked = deferred(), started = deferred();
    f.handlers.list = async () => { started.resolve(); return blocked.promise; };
    const pending = f.model.loadList(); await started.promise; f.model.destroy();
    const count = f.emitted.length; f.model.destroy(); await f.model.activate();
    blocked.resolve({ groups: [row] }); await pending;
    assert.equal(f.model.read().phase, 'destroyed'); assert.equal(f.emitted.length, count);
    assert.equal(f.model.setMemberId(userId), false);
});

test('U18 session retirement and native401 clear actor drafts selection and prior attempts', async () => {
    const f = await owned(); await f.ready();
    f.model.setCreateDraft({ name: 'typed' }); f.model.setEditDraft({ description: 'private draft' });
    f.model.setMemberId(userId); f.model.setDeleteConfirmation('team');
    f.handlers.list = async () => { throw error(401, 'session_revoked'); };
    await f.model.loadList();
    const state = f.model.read();
    assert.equal(state.phase, 'session-ended'); assert.equal(state.actor, null); assert.equal(state.detail, null);
    assert.equal(state.createDraft.name, ''); assert.equal(state.editDraft.description, '');
    assert.equal(state.memberId, ''); assert.equal(state.deleteConfirmation, ''); assert.equal(state.attempts.length, 0);
    assert.deepEqual(f.ended, ['session_revoked']);
});

test('U19 unavailable actor probes are not fake401 and separate predispatch failure from unknown acknowledgement', async () => {
    const f = await owned(); await f.ready();
    f.handlers.session = async () => { throw error(503, 'service_unavailable'); };
    await f.model.loadList();
    assert.equal(f.model.read().phase, 'active'); assert.equal(f.model.read().readErrors.list.code, 'session-unverified');
    f.model.setCreateDraft({ name: 'not-admitted' }); await f.model.create();
    assert.equal(f.calls.filter(item => item.name === 'create').length, 0);
    assert.equal(f.model.read().attempts[0].status, 'failed'); assert.deepEqual(f.ended, []);
    const ack = await owned(); await ack.ready();
    ack.handlers.create = async fields => {
        const result = await ack.defaults.create(fields);
        ack.handlers.session = async () => { throw error(503, 'service_unavailable'); };
        return result;
    };
    ack.model.setCreateDraft({ name: 'committed' }); await ack.model.create();
    assert.equal(ack.model.read().attempts[0].status, 'unknown'); assert.deepEqual(ack.ended, []);
});

test('U20 explicit reconciliation only observes one GET list/detail and cannot resolve UNKNOWN membership', async () => {
    const f = await owned(); await f.ready(); f.model.setMemberId(userId);
    f.handlers.addMember = async () => { throw error(null, 'transport-error', true); };
    await f.model.addMember();
    assert.equal(f.model.read().attempts[0].status, 'unknown');
    assert.equal(f.calls.filter(item => item.name === 'list').length, 0, 'no automatic reconciliation');
    f.calls.length = 0; await f.model.reconcile(f.model.read().attempts[0].requestId);
    assert.deepEqual(f.calls.filter(item => item.name !== 'session').map(item => item.name), ['list', 'detail']);
    const attempt = f.model.read().attempts[0];
    assert.equal(attempt.status, 'unknown'); assert.equal(attempt.reconciliation, 'complete');
    assert.equal(attempt.observed.detail.member_count, 0); assert.equal(attempt.response, null);
    await f.model.addMember();
    const blocked = deferred(), started = deferred(); let first = true;
    f.handlers.list = async () => {
        if (first) { first = false; started.resolve(); return blocked.promise; }
        throw error(403, 'permission_denied');
    };
    const pending = f.model.reconcile(f.model.read().attempts[1].requestId); await started.promise;
    await f.model.loadList();
    assert.equal(f.model.read().phase, 'active');
    assert(f.model.read().attempts.every(value => value.observed === null && value.response === null));
    blocked.resolve({ groups: [{ ...row, name: 'late-reconcile-private' }] }); await pending;
    assert(!JSON.stringify(f.model.read()).includes('late-reconcile-private'));
    assert(f.model.read().attempts.every(value => value.observed === null && value.reconciliation !== 'pending'));
});

test('U21 every built-in metadata field is read-only and everyone membership remains virtual', async () => {
    const f = await owned(); f.state.groups.set(id, { ...detail, name: 'admin', built_in: true }); await f.ready();
    assert.deepEqual(plain(f.model.read().policy),
        { editMetadata: false, deleteGroup: false, editMembers: true, virtualMembership: false });
    assert.equal(f.model.setEditDraft({ name: 'renamed', description: 'changed' }), false);
    await f.model.update(); f.model.setDeleteConfirmation('admin'); await f.model.remove();
    assert.equal(f.calls.filter(item => ['update', 'remove'].includes(item.name)).length, 0);
    f.state.groups.set(id, { ...detail, name: 'everyone', built_in: true }); await f.model.select(id);
    f.model.setMemberId(userId); await f.model.addMember(); await f.model.removeMember();
    assert.equal(f.model.read().policy.virtualMembership, true);
    assert.equal(f.calls.filter(item => ['addMember', 'removeMember'].includes(item.name)).length, 0);
    assert.match(f.model.read().formError.message, /virtual/);
});

test('U22 explicit admin membership uses a target UUID and actual refreshed count without a new policy', async () => {
    const f = await owned(); f.state.groups.set(id, { ...detail, name: 'admin', built_in: true }); await f.ready();
    f.model.setMemberId(userId.toUpperCase()); await f.model.addMember();
    assert.equal(f.model.read().detail.member_count, 1); await f.model.removeMember();
    assert.equal(f.model.read().detail.member_count, 0);
    assert.deepEqual(f.calls.filter(item => ['addMember', 'removeMember'].includes(item.name))
        .map(item => item.args.slice(0, 2)), [[id, userId], [id, userId]]);
    assert.deepEqual(plain(f.model.read().attempts.map(item => item.status)), ['succeeded', 'succeeded']);
});

test('U23 a conflict retains the operator draft until explicit refresh instead of inventing OCC', async () => {
    const f = await owned(); await f.ready(); f.model.setEditDraft({ name: 'conflicting', description: 'my draft' });
    f.handlers.update = async () => { throw error(409, 'group_exists'); };
    await f.model.update();
    assert.equal(f.model.read().attempts[0].status, 'failed');
    assert.deepEqual(plain(f.model.read().editDraft), { name: 'conflicting', description: 'my draft' });
    f.state.groups.set(id, { ...detail, name: 'external' });
    assert.equal(f.model.read().detail.name, 'team');
    await f.model.select(id); assert.equal(f.model.read().editDraft.name, 'external');
    assert.equal(f.calls.filter(item => item.name === 'update').length, 1);
    const denied = await owned(); await denied.ready();
    denied.model.setEditDraft({ description: 'previously authorized detail' }); await denied.model.update();
    assert.equal(denied.model.read().attempts[0].response.description, 'previously authorized detail');
    denied.handlers.list = async () => { throw error(403, 'permission_denied'); };
    await denied.model.loadList();
    assert.equal(denied.model.read().attempts[0].status, 'succeeded', 'denial does not invent rollback');
    assert.equal(denied.model.read().attempts[0].response, null);
    assert.equal(denied.model.read().detail, null); assert.equal(denied.model.read().phase, 'active');
});

test('U24 fresh activation takes a new actor without inheriting old admitted-write results or drafts', async () => {
    const f = await owned(); await f.ready(); const blocked = deferred(), started = deferred();
    f.handlers.create = async fields => { started.resolve(); await blocked.promise; return f.defaults.create(fields); };
    f.model.setCreateDraft({ name: 'old-intent' }); const pending = f.model.create(); await started.promise;
    f.model.deactivate(); f.state.actor = actorB; await f.model.activate();
    f.model.setCreateDraft({ name: 'fresh-intent' }); blocked.resolve(); await pending;
    const state = f.model.read();
    assert.equal(state.actor.id, actorB.user.id); assert.equal(state.actor.sessionId, actorB.session.id);
    assert.equal(state.createDraft.name, 'fresh-intent'); assert.equal(state.attempts.length, 0);
    assert(!state.items.some(group => group.name === 'old-intent'));
    assert.equal(state.detail, null); assert.equal(state.selectedId, null);
});

test('U25 a session postflight404 cannot turn an observed existing group into absent reconciliation', async () => {
    const f = await owned(); await f.ready();
    f.handlers.update = async (target, fields) => {
        await f.defaults.update(target, fields);
        throw error(0, 'reply_lost', true);
    };
    f.model.setEditDraft({ description: 'committed-with-reply-lost' });
    await f.model.update();
    const attempt = f.model.read().attempts[0];
    assert.equal(attempt.status, 'unknown');
    assert.equal(f.state.groups.get(id).description, 'committed-with-reply-lost');
    let sessionProbes = 0, detailReads = 0, failedPostflight = false;
    f.handlers.session = async () => {
        sessionProbes++;
        if (sessionProbes === 4) {
            failedPostflight = true;
            throw error(404, 'session_not_found');
        }
        return plain(actorA); // A later successful probe cannot recast the failed postflight as group404.
    };
    f.handlers.detail = async target => {
        assert.equal(target, id); detailReads++;
        return plain(f.state.groups.get(target)); // The group request succeeded with an existing row.
    };
    const observed = await f.model.reconcile(attempt.requestId);
    const final = f.model.read().attempts[0];
    assert.equal(failedPostflight, true); assert.equal(detailReads, 1);
    assert.equal(observed, null);
    assert.equal(final.status, 'unknown', 'session failure does not prove the mutation failed or committed');
    assert.equal(final.reconciliation, 'failed');
    assert.equal(final.reconciliationError.code, 'session-unverified');
    assert(final.observed === null || final.observed.detail === null);
    assert(!f.emitted.some(snapshot => snapshot.attempts.some(value =>
        value.requestId === attempt.requestId &&
        (value.reconciliation === 'complete' || value.observed?.detail?.absent === true))));

    // Positive control: a genuine detail-request404 with verified session is
    // an absence observation, while the admitted mutation still stays UNKNOWN.
    const absent = await owned(); await absent.ready();
    absent.handlers.update = async (target, fields) => {
        await absent.defaults.update(target, fields);
        throw error(0, 'reply_lost', true);
    };
    absent.model.setEditDraft({ description: 'unknown-before-external-removal' });
    await absent.model.update();
    const unknown = absent.model.read().attempts[0];
    assert.equal(unknown.status, 'unknown');
    absent.state.groups.delete(id); // Existing default detail handler now rejects with group_not_found404.
    absent.calls.length = 0;
    const observation = await absent.model.reconcile(unknown.requestId);
    assert.deepEqual(plain(observation.detail), { absent: true });
    const observedAttempt = absent.model.read().attempts[0];
    assert.equal(observedAttempt.status, 'unknown');
    assert.equal(observedAttempt.reconciliation, 'complete');
    assert.equal(observedAttempt.reconciliationError, null);
    assert.deepEqual(plain(observedAttempt.observed.detail), { absent: true });
    assert.equal(absent.calls.filter(value => value.name === 'detail').length, 1);
    assert.equal(absent.calls.filter(value => value.name === 'update').length, 0, 'observation never retries a mutation');
});
