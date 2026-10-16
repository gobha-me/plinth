import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const load = async relative => (await import('data:text/javascript;base64,' +
    Buffer.from(await readFile(new URL(relative, import.meta.url), 'utf8')).toString('base64'))).default;
const storage = await load('../../client/shell/server/platform-storage-v1.js');
const preferences = await load('../../client/shell/server/handlers/preferences.set.js');
const ctx = { user: { id: 'original-user', auth_type: 'session' }, extension: 'shell', call_depth: 3 };
const rejected = code => error => error.code === code;

function mock(query = async () => ({ rows: [] }), exec = async () => ({ row_count: 1 })) {
    const calls = [];
    globalThis.db = {
        async query(sql, params) {
            calls.push({ kind: 'query', sql, params });
            if (sql.includes('platform_contract')) return { rows: [{ storage_version: 1 }] };
            return await query(sql, params);
        },
        async exec(sql, params) {
            calls.push({ kind: 'exec', sql, params });
            return await exec(sql, params);
        },
    };
    return calls;
}

test('fixed own read preserves absence and stored JSON null, under original owner', async () => {
    const calls = mock();
    assert.deepEqual(await storage({ operation: 'floats_read' }, ctx), { present: false });
    assert.equal(calls.length, 2);
    assert(calls[1].sql.includes('CASE WHEN octet_length(value::text) <= 32768 THEN value::text'));
    assert.deepEqual(calls[1].params, ['original-user', 'shell.floats']);
    mock(async () => ({ rows: [{ size: 4, body: 'null' }] }));
    assert.deepEqual(await storage({ operation: 'floats_read' }, ctx), { present: true, value: null });
    const own = mock(async () => ({ rows: [{ size: 26, body: '{"version":1,"choices":[]}' }] }));
    assert.deepEqual(await storage({ operation: 'own_defaults_read' }, ctx),
        { present: true, value: { version: 1, choices: [] } });
    assert.deepEqual(own[1].params, ['original-user', 'shell.handler_defaults']);
});

test('fixed read refuses oversized, malformed or inconsistent data without mutation', async () => {
    for (const row of [{ size: 32769, body: null }, { size: 3, body: '{' },
        { size: -1, body: '{}' }, { size: '2', body: '{}' },
        { size: 2, body: 'x'.repeat(32769) }]) {
        const calls = mock(async () => ({ rows: [row] }));
        await assert.rejects(storage({ operation: 'floats_read' }, ctx), rejected('storage_unavailable'));
        assert.equal(calls.filter(call => call.kind === 'exec').length, 0);
    }
});

test('each operation checks the storage marker and cannot use a supplied version bypass', async () => {
    let writes = 0;
    globalThis.db = {
        query: async () => ({ rows: [{ storage_version: 2 }] }),
        exec: async () => { writes++; return { row_count: 1 }; },
    };
    for (const operation of ['floats_read', 'floats_commit', 'own_defaults_read',
        'admin_default_read', 'admin_default_put', 'admin_default_delete', 'resolve_admin_candidate']) {
        await assert.rejects(storage({ operation, storage_version: 1, bypass: true }, ctx),
            rejected('storage_unavailable'));
    }
    assert.equal(writes, 0);
});

test('fixed commit waits for actual DB completion and guards stored bytes before DML', async () => {
    let finish;
    const pending = new Promise(resolve => { finish = resolve; });
    let entered;
    const seen = new Promise(resolve => { entered = resolve; });
    const calls = mock(undefined, async () => { entered(); return await pending; });
    let acknowledged = false;
    const task = storage({ operation: 'floats_commit', layout: { version: 1, floats: [] },
        user_id: 'not-the-owner' }, ctx).then(result => { acknowledged = true; return result; });
    await seen;
    assert.equal(acknowledged, false);
    const write = calls.find(call => call.kind === 'exec');
    assert(write.sql.includes('WHERE octet_length(($3::jsonb)::text) <= 32768'));
    assert.deepEqual(write.params, ['original-user', 'shell.floats', '{"version":1,"floats":[]}']);
    finish({ row_count: 1 });
    assert.deepEqual(await task, { saved: true });
});

test('fixed commit never acknowledges an oversize/refused/uncertain database write', async () => {
    mock(undefined, async () => ({ row_count: 0 }));
    await assert.rejects(storage({ operation: 'floats_commit', layout: { version: 1, floats: [] } }, ctx),
        rejected('payload_too_large'));
    mock(undefined, async () => { throw { code: 'connection_lost' }; });
    await assert.rejects(storage({ operation: 'floats_commit', layout: { version: 1, floats: [] } }, ctx),
        rejected('connection_lost'));
    const calls = mock();
    await assert.rejects(storage({ operation: 'floats_commit', layout: { version: 1, floats: [], extra: true } }, ctx),
        rejected('invalid_argument'));
    assert.equal(calls.filter(call => call.kind === 'exec').length, 0);
});

test('resolver read is a single logical candidate and management paging is bounded', async () => {
    const candidate = { application_id: 'notes', panel_id: 'preview' };
    const calls = mock(async () => ({ rows: [candidate] }));
    assert.deepEqual(await storage({ operation: 'resolve_admin_candidate', content_type: 'text/plain' }, ctx),
        { candidate });
    assert.deepEqual(calls[1].params, ['text/plain']);
    assert(!calls[1].sql.includes('user_id'));
    const rows = ['application/json', 'text/plain'].map(content_type => ({ content_type, ...candidate }));
    const paged = mock(async () => ({ rows }));
    assert.deepEqual(await storage({ operation: 'admin_default_read', limit: 1 }, ctx),
        { entries: [rows[0]], next_after: 'application/json' });
    assert.deepEqual(paged[1].params, ['', 2]);
    for (const invalid of ['Text/Plain', 'text/*', 'text/plain; charset=utf-8', '/plain']) {
        await assert.rejects(storage({ operation: 'resolve_admin_candidate', content_type: invalid }, ctx),
            rejected('invalid_argument'));
    }
});

test('generic preference defense blocks float writes, deletes and forged bypass flags before SQL', async () => {
    const calls = mock();
    for (const value of [undefined, null, { version: 1, floats: [] }]) {
        await assert.rejects(preferences({ key: 'shell.floats', value, bypass: true,
            validated: true, operation: 'floats_commit' }, ctx), rejected('invalid_argument'));
    }
    assert.equal(calls.length, 0);
    assert.deepEqual(await preferences({ key: 'shell.theme', value: 'dark' }, ctx), { ok: true });
    assert.equal(calls.length, 1);
});

test('own handler defaults reject duplicate/noncanonical/unknown/bounded shapes', async () => {
    const choice = { content_type: 'text/plain', application_id: 'notes', panel_id: 'preview' };
    const valid = { version: 1, choices: [choice] };
    const calls = mock();
    assert.deepEqual(await preferences({ key: 'shell.handler_defaults', value: valid }, ctx), { ok: true });
    for (const value of [null, { ...valid, extra: true }, { ...valid, version: 2 },
        { ...valid, choices: [choice, choice] }, { ...valid, choices: Array(129).fill(choice) },
        { ...valid, choices: [{ ...choice, content_type: 'Text/Plain' }] },
        { ...valid, choices: [{ ...choice, extra: true }] }]) {
        await assert.rejects(preferences({ key: 'shell.handler_defaults', value }, ctx), rejected('invalid_argument'));
    }
    assert.equal(calls.length, 1);
});
