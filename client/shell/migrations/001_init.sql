-- shell.zip/migrations/001_init.sql
-- Initial migration for the bundled shell extension. Creates the
-- ext_shell.user_preferences table and reserves ext_shell.default_apps
-- for the 0.6.6 tray / content-type resolution milestone.
--
-- Per ICD-0.6.1 §6, ICD-0.4.3 §Schema + GRANT Contract.

CREATE TABLE ext_shell.user_preferences (
    user_id     UUID         NOT NULL,
    key         TEXT         NOT NULL,
    value       JSONB        NOT NULL,
    updated_at  TIMESTAMPTZ  NOT NULL DEFAULT now(),

    PRIMARY KEY (user_id, key),

    CONSTRAINT fk_user_preferences_user_id
        FOREIGN KEY (user_id)
        REFERENCES plinth.users (id)
        ON DELETE CASCADE,

    CONSTRAINT chk_key_nonempty
        CHECK (length(key) BETWEEN 1 AND 255),

    CONSTRAINT chk_value_size
        CHECK (octet_length(value::text) <= 65536)
);

CREATE INDEX idx_user_preferences_updated_at
    ON ext_shell.user_preferences (updated_at);

GRANT SELECT, INSERT, UPDATE, DELETE
    ON ext_shell.user_preferences
    TO ext_shell_role;

-- Reservation: 0.6.6 will add ext_shell.default_apps via a
-- 002_add_default_apps.sql migration.  (scope, user_id, content_type)
-- PK; FK on plinth.users(id) ON DELETE CASCADE for user-scope rows;
-- admin-scope rows have user_id = NULL.  Do NOT create this table in
-- 0.6.1 — it has no consumer yet.
-- See ICD-0.6.6-tray-content-type-navigation.md §default_apps.
