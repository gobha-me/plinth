// Sleep for ms milliseconds via db.query(pg_sleep) then return.
// Used by ICD-0.4.5 X.08 / X.09 drain-window tests; the await holds
// the upgrade-drain DispatchGuard for the full interval per
// resolution.cpp:387 + drain.hpp:11-19.
export default async function wait({ ms }) {
    const seconds = Number(ms) / 1000.0;
    await db.query('SELECT pg_sleep(' + seconds + ')');
    return { ok: true, slept_ms: Number(ms) };
}
