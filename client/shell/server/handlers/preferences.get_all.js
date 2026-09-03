// shell.zip/server/handlers/preferences_get_all.js
// ICD-0.6.1 §7.3 — `shell.preferences.get_all()`.
// Returns every key/value pair for the calling user, sorted by key
// ascending. Empty array if the user has no rows. Per SC1, scope
// is bound to `ctx.user.id`.

export default async function preferences_get_all(_args, ctx) {
    const r = await db.query(
        'SELECT key, value FROM ext_shell.user_preferences ' +
        'WHERE user_id = $1 ORDER BY key ASC',
        [ctx.user.id]
    );
    // db.query returns JSONB columns as their JSON-text repr per
    // db_result_to_json.cpp:186-196 — parse each value to recover the
    // original JS structure before returning to callers.
    return {
        entries: r.rows.map(row => ({
            key:   row.key,
            value: JSON.parse(row.value),
        })),
    };
}
