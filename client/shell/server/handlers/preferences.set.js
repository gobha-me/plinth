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

const SCHEMA = {
    'shell.theme': {
        validate: (v) => v === 'light' || v === 'dark' || v === 'system',
    },
    'shell.scale_pct': {
        validate: (v) => Number.isInteger(v) && v >= 80 && v <= 175,
    },
};

export default async function preferences_set({ key, value }, ctx) {
    if (typeof key !== 'string' || key.length === 0 || key.length > 255) {
        throw { code: 'invalid_argument',
                message: 'key must be 1..255 byte string' };
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
    await db.query(
        'INSERT INTO ext_shell.user_preferences (user_id, key, value) ' +
        'VALUES ($1, $2, $3::jsonb) ' +
        'ON CONFLICT (user_id, key) DO UPDATE ' +
        'SET value = EXCLUDED.value, updated_at = now()',
        [ctx.user.id, key, serialised]
    );
    return { ok: true };
}
