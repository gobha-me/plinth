-- Schema-qualified DDL so the table lands in `ext_notes` regardless of
-- the admin connection's search_path. The migration system does not
-- (currently) `SET search_path` before applying — see the spawned task
-- "Fix migrations search_path schema-isolation bug" for the production
-- follow-up. Using `ext_notes.notes_comments` here keeps cleanup
-- (`drop_schema_and_migrations` → DROP SCHEMA CASCADE) able to reclaim
-- the table on uninstall, which D.* / U.* tests in
-- `lifecycle_transitions_test.cpp` exercise after running upgrades.
CREATE TABLE IF NOT EXISTS ext_notes.notes_comments (
    id      TEXT PRIMARY KEY,
    note_id TEXT NOT NULL,
    body    TEXT NOT NULL
);
