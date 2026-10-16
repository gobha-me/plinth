// shell.zip/server/handlers/preferences_set.js
// ICD-0.6.1 §7.4 — `shell.preferences.set(key, value)`.
//
// `value=undefined` deletes the row (OQ3 architect-recommendation).
// `value=null` UPSERTs a JSONB null literal — distinguishable from
// the deletion gesture and from the absent-key state.
// 64 KiB serialised value cap (SC4).
//
// Per-key validation per ICD-0.6.2 §5.5 (defense-in-depth + well-known-
// key contract). The SCHEMA dict carries one entry per `shell.*` key
// with a `validate(value) -> bool` predicate. Unknown keys pass through
// unchecked — extensions own their own keyspace via `<ext>.*`.

const APP_ID = /^[a-z][a-z0-9-]{1,63}$/;
const PANEL_ID = /^[a-z][a-z0-9_-]{0,63}$/;
const CONTENT_TYPE = /^[a-z0-9][a-z0-9!#$&^_.+-]*\/[a-z0-9][a-z0-9!#$&^_.+-]*$/;

function utf8Bytes(text) {
    let size = 0;
    for (let index = 0; index < text.length; index++) {
        const unit = text.charCodeAt(index);
        if (unit < 0x80) size++;
        else if (unit < 0x800) size += 2;
        else if (unit >= 0xd800 && unit <= 0xdbff) {
            const next = text.charCodeAt(++index);
            if (!(next >= 0xdc00 && next <= 0xdfff)) return Infinity;
            size += 4;
        } else {
            if (unit >= 0xdc00 && unit <= 0xdfff) return Infinity;
            size += 3;
        }
    }
    return size;
}

function validHandlerDefaults(value) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)
            || Object.keys(value).length !== 2 || value.version !== 1
            || !Array.isArray(value.choices) || value.choices.length > 128) return false;
    const seen = new Set();
    for (const choice of value.choices) {
        if (choice === null || typeof choice !== 'object' || Array.isArray(choice)
                || Object.keys(choice).length !== 3
                || typeof choice.content_type !== 'string' || choice.content_type.length > 128
                || !CONTENT_TYPE.test(choice.content_type) || seen.has(choice.content_type)
                || typeof choice.application_id !== 'string' || !APP_ID.test(choice.application_id)
                || typeof choice.panel_id !== 'string' || !PANEL_ID.test(choice.panel_id)) return false;
        seen.add(choice.content_type);
    }
    return utf8Bytes(JSON.stringify(value)) <= 32768;
}

function validLauncher(value) {
    if (value === null || typeof value !== 'object' || Array.isArray(value)
            || value.version !== 1) return false;
    if (value.last_application !== null && value.last_application !== undefined
            && (typeof value.last_application !== 'string'
                || !APP_ID.test(value.last_application))) return false;
    if (value.last_panels === null || typeof value.last_panels !== 'object'
            || Array.isArray(value.last_panels)) return false;
    const panels = Object.entries(value.last_panels);
    if (panels.length > 256 || panels.some(([app, panel]) =>
        !APP_ID.test(app) || typeof panel !== 'string' || !PANEL_ID.test(panel))) return false;
    if (!Array.isArray(value.application_order) || value.application_order.length > 256
            || value.application_order.some(app => typeof app !== 'string' || !APP_ID.test(app))) {
        return false;
    }
    return true;
}

const SCHEMA = {
    'shell.theme': {
        validate: (v) => v === 'light' || v === 'dark' || v === 'system',
    },
    'shell.scale_pct': {
        validate: (v) => Number.isInteger(v) && v >= 80 && v <= 175,
    },
    'shell.launcher': {
        validate: validLauncher,
    },
    'shell.handler_defaults': {
        validate: validHandlerDefaults,
    },
};

export default async function preferences_set({ key, value }, ctx) {
    if (typeof key !== 'string' || key.length === 0 || key.length > 255) {
        throw { code: 'invalid_argument',
                message: 'key must be 1..255 byte string' };
    }
    // Includes the missing-value DELETE gesture. Only the guarded fixed
    // platform service may commit a freshly approved float snapshot.
    if (key === 'shell.floats') {
        throw { code: 'invalid_argument',
                message: 'shell.floats requires guarded layout storage' };
    }
    if (Object.prototype.hasOwnProperty.call(SCHEMA, key)
            && value !== undefined
            && !SCHEMA[key].validate(value)) {
        throw { code: 'invalid_argument',
                message: 'value not valid for key ' + key };
    }
    const serialised = JSON.stringify(value);
    if (serialised === undefined) {
        // value === undefined → DELETE row.
        const res = await db.exec(
            'DELETE FROM ext_shell.user_preferences ' +
            'WHERE user_id = $1 AND key = $2',
            [ctx.user.id, key]
        );
        return { ok: true, deleted: (res.row_count ?? 0) > 0 };
    }
    if (serialised.length > 65536) {
        throw { code: 'payload_too_large',
                message: 'value exceeds 64 KiB serialised limit' };
    }
    const result = await db.exec(
        'INSERT INTO ext_shell.user_preferences (user_id, key, value) ' +
        (key === 'shell.handler_defaults'
            ? 'SELECT $1, $2, $3::jsonb WHERE octet_length(($3::jsonb)::text) <= 32768 '
            : 'VALUES ($1, $2, $3::jsonb) ') +
        'ON CONFLICT (user_id, key) DO UPDATE ' +
        'SET value = EXCLUDED.value, updated_at = now()',
        [ctx.user.id, key, serialised]
    );
    if (key === 'shell.handler_defaults' && result.row_count !== 1) {
        throw { code: result.row_count === 0 ? 'payload_too_large' : 'storage_unavailable',
                message: 'handler defaults were not saved' };
    }
    return { ok: true };
}
