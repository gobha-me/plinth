// Fixed, self-contained platform module, NOT an ordinary capability handler.
// Only the kernel's guarded service selects operations. No caller may supply
// a user, module path, SQL, table, contract version or authorization flag.
// Original caller identity/depth and the shell DB role are preserved.

const APP = /^[a-z][a-z0-9-]{1,63}$/;
const PANEL = /^[a-z][a-z0-9_-]{0,63}$/;
const MIME = /^[a-z0-9][a-z0-9!#$&^_.+-]*\/[a-z0-9][a-z0-9!#$&^_.+-]*$/;

function fail(code = 'storage_unavailable') {
    throw { code, message: 'fixed shell storage unavailable' };
}

function bytes(text) {
    let size = 0;
    for (let i = 0; i < text.length; i++) {
        const unit = text.charCodeAt(i);
        if (unit === 0) fail('invalid_argument');
        if (unit < 0x80) size++;
        else if (unit < 0x800) size += 2;
        else if (unit >= 0xd800 && unit <= 0xdbff) {
            const next = text.charCodeAt(++i);
            if (!(next >= 0xdc00 && next <= 0xdfff)) fail('invalid_argument');
            size += 4;
        } else {
            if (unit >= 0xdc00 && unit <= 0xdfff) fail('invalid_argument');
            size += 3;
        }
    }
    return size;
}

function literalType(value) {
    return typeof value === 'string' && value.length <= 128 && MIME.test(value);
}

function handler(value) {
    return value !== null && typeof value === 'object' && !Array.isArray(value)
        && Object.keys(value).length === 2
        && typeof value.application_id === 'string' && APP.test(value.application_id)
        && typeof value.panel_id === 'string' && PANEL.test(value.panel_id);
}

async function readOwn(key, ctx) {
    // CASE ensures an oversized JSONB body never crosses the DB bridge. The
    // length is computed on the server, not after materializing value in JS.
    const result = await db.query(
        'SELECT octet_length(value::text) AS size, ' +
        'CASE WHEN octet_length(value::text) <= 32768 THEN value::text ' +
        'ELSE NULL END AS body FROM ext_shell.user_preferences ' +
        'WHERE user_id = $1 AND key = $2', [ctx.user.id, key]);
    if (result.rows.length === 0) return { present: false };
    if (result.rows.length !== 1) fail();
    const row = result.rows[0];
    if (!Number.isInteger(row.size) || row.size < 0 || row.size > 32768
            || typeof row.body !== 'string' || bytes(row.body) > 32768) fail();
    try {
        return { present: true, value: JSON.parse(row.body) };
    } catch (_) {
        fail();
    }
}

export default async function platformStorage(args, ctx) {
    if (args === null || typeof args !== 'object' || Array.isArray(args)
            || typeof args.operation !== 'string' || typeof ctx?.user?.id !== 'string'
            || ctx.user.id.length === 0) fail('invalid_argument');
    const operation = args.operation;
    if (!['floats_read', 'floats_commit', 'own_defaults_read', 'admin_default_read',
        'admin_default_put', 'admin_default_delete', 'resolve_admin_candidate'].includes(operation)) {
        fail('invalid_argument');
    }
    const marker = await db.query(
        'SELECT storage_version FROM ext_shell.platform_contract WHERE singleton = true', []);
    if (marker.rows.length !== 1 || marker.rows[0].storage_version !== 1) fail();

    if (operation === 'floats_read') return await readOwn('shell.floats', ctx);
    if (operation === 'own_defaults_read') return await readOwn('shell.handler_defaults', ctx);
    if (operation === 'floats_commit') {
        // The service has already validated the full #43 snapshot and all
        // ephemeral current-generation/policy/schema projection preconditions.
        const layout = args.layout;
        if (layout === null || typeof layout !== 'object' || Array.isArray(layout)
                || Object.keys(layout).length !== 2 || layout.version !== 1
                || !Array.isArray(layout.floats) || layout.floats.length > 5) {
            fail('invalid_argument');
        }
        const body = JSON.stringify(layout);
        if (bytes(body) > 32768) fail('payload_too_large');
        const result = await db.exec(
            'INSERT INTO ext_shell.user_preferences (user_id, key, value) ' +
            'SELECT $1, $2, $3::jsonb WHERE octet_length(($3::jsonb)::text) <= 32768 ' +
            'ON CONFLICT (user_id, key) DO UPDATE ' +
            'SET value = EXCLUDED.value, updated_at = now()',
            [ctx.user.id, 'shell.floats', body]);
        // Refuse before DML if PostgreSQL's stored JSON text would exceed the
        // read envelope. Never acknowledge a write this contract cannot read.
        if (result.row_count === 0) fail('payload_too_large');
        if (result.row_count !== 1) fail();
        return { saved: true };
    }

    if (operation === 'admin_default_read' && args.content_type === undefined) {
        const after = args.after ?? '';
        const limit = args.limit ?? 64;
        if (typeof after !== 'string' || (after !== '' && !literalType(after))
                || !Number.isInteger(limit) || limit < 1 || limit > 64) fail('invalid_argument');
        const result = await db.query(
            'SELECT content_type, application_id, panel_id FROM ext_shell.default_apps ' +
            'WHERE content_type COLLATE "C" > $1 COLLATE "C" ' +
            'ORDER BY content_type COLLATE "C" LIMIT $2', [after, limit + 1]);
        if (result.rows.length > limit + 1) fail();
        const rows = result.rows.slice(0, limit);
        for (const row of result.rows) {
            if (!literalType(row.content_type)
                    || !handler({ application_id: row.application_id, panel_id: row.panel_id })) fail();
        }
        return { entries: rows, next_after: result.rows.length > limit
            ? rows[rows.length - 1].content_type : null };
    }
    if (!literalType(args.content_type)) fail('invalid_argument');
    if (operation === 'admin_default_put') {
        if (!handler(args.handler)) fail('invalid_argument');
        const result = await db.exec(
            'INSERT INTO ext_shell.default_apps (content_type, application_id, panel_id) ' +
            'VALUES ($1, $2, $3) ON CONFLICT (content_type) DO UPDATE SET ' +
            'application_id = EXCLUDED.application_id, panel_id = EXCLUDED.panel_id',
            [args.content_type, args.handler.application_id, args.handler.panel_id]);
        if (result.row_count !== 1) fail();
        return { saved: true };
    }
    if (operation === 'admin_default_delete') {
        const result = await db.exec(
            'DELETE FROM ext_shell.default_apps WHERE content_type = $1', [args.content_type]);
        if (result.row_count !== 0 && result.row_count !== 1) fail();
        return { deleted: result.row_count === 1 };
    }
    const result = await db.query(
        'SELECT application_id, panel_id FROM ext_shell.default_apps WHERE content_type = $1',
        [args.content_type]);
    if (result.rows.length > 1) fail();
    if (result.rows.length === 0) return { candidate: null };
    if (!handler(result.rows[0])) fail();
    return { candidate: result.rows[0] };
}
