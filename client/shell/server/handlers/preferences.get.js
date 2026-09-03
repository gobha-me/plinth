// shell.zip/server/handlers/preferences_get.js
// ICD-0.6.1 §7.3 — `shell.preferences.get(key)`.
// Returns { value: <jsonb> } or an object with no `value` member when the key
// is absent. Per SC1, `user_id` is taken from `ctx.user.id` — never
// from args; cross-tenant access is structurally impossible.

export default async function preferences_get({ key }, ctx) {
    if (typeof key !== 'string' || key.length === 0 || key.length > 255) {
        throw { code: 'invalid_argument',
                message: 'key must be 1..255 byte string' };
    }
    const r = await db.query(
        'SELECT value FROM ext_shell.user_preferences ' +
        'WHERE user_id = $1 AND key = $2',
        [ctx.user.id, key]
    );
    if (r.rows.length === 0) {
        // The native JS value is `undefined`, whose JSON object representation
        // is an omitted member. Returning an empty object preserves the
        // absent-vs-stored-null distinction across the C++ JSON bridge.
        return {};
    }
    // db.query returns JSONB columns as their JSON-text repr per
    // db_result_to_json.cpp:186-196 — parse to recover the JS value.
    return { value: JSON.parse(r.rows[0].value) };
}
