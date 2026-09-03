-- Plinth kernel schema — LIVING DOCUMENT
--
-- This file represents the CURRENT DESIRED STATE of the plinth schema.
-- During milestones 0.1–0.6, edit this file directly when the schema changes.
-- With dev_mode=true, the kernel drops and recreates from this file on startup.
--
-- At milestone 0.7.0, this file becomes migrations/001_baseline.sql
-- and all subsequent changes are numbered immutable migrations.
-- See SESSION-GUIDE.md for details.

CREATE SCHEMA IF NOT EXISTS plinth;

-- ── Migration tracking (bootstraps itself) ───────────────────
CREATE TABLE plinth.migrations (
    extension_name TEXT NOT NULL,
    migration_file TEXT NOT NULL,
    applied_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    checksum       TEXT NOT NULL,
    PRIMARY KEY (extension_name, migration_file)
);

-- ── Identity ─────────────────────────────────────────────────
-- Added in 0.1.2

CREATE TABLE plinth.users (
    id            UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    username      TEXT        UNIQUE NOT NULL,
    password_hash TEXT        NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    disabled_at   TIMESTAMPTZ,
    -- Added in 0.4.7. The RBAC test runner creates ephemeral users (__test_denied_/
    -- __test_allowed_) that must not appear in admin user-listing
    -- queries or trip the first-user bootstrap COUNT. Placeholder
    -- argon2id hash verifies against no password; ephemeral users
    -- authenticate synthetically inside the kernel, never over HTTP.
    is_test_user  BOOLEAN     NOT NULL DEFAULT false
);

-- Partial index for the reconciler's orphaned-user sweep; the set is
-- always a small minority of rows, so a partial index keeps the index
-- tiny without impacting the common lookup (covered by the existing
-- username UNIQUE constraint).
CREATE INDEX idx_users_is_test_user
    ON plinth.users(id)
    WHERE is_test_user = true;

CREATE TABLE plinth.sessions (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id     UUID        NOT NULL REFERENCES plinth.users(id),
    token_hash  TEXT        NOT NULL,
    user_agent  TEXT,
    ip_address  INET,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at  TIMESTAMPTZ NOT NULL DEFAULT (NOW() + INTERVAL '24 hours'),
    revoked_at  TIMESTAMPTZ
);

-- Added in 0.1.3

CREATE TABLE plinth.pats (
    id           UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id      UUID        NOT NULL REFERENCES plinth.users(id),
    name         TEXT        NOT NULL,
    token_hash   TEXT        NOT NULL,
    token_prefix TEXT        NOT NULL,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at   TIMESTAMPTZ,
    last_used_at TIMESTAMPTZ,
    revoked_at   TIMESTAMPTZ
);

-- ── Groups & RBAC ────────────────────────────────────────────
-- Added in 0.1.4

CREATE TABLE plinth.groups (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name        TEXT        UNIQUE NOT NULL,
    description TEXT,
    built_in    BOOLEAN     NOT NULL DEFAULT false,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE plinth.group_members (
    group_id UUID        NOT NULL REFERENCES plinth.groups(id),
    user_id  UUID        NOT NULL REFERENCES plinth.users(id),
    added_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (group_id, user_id)
);

CREATE TABLE plinth.rbac_rules (
    id             UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    rule           TEXT        UNIQUE NOT NULL,
    namespace      TEXT        NOT NULL,
    description    TEXT        NOT NULL,
    extension_name TEXT        NOT NULL,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    orphaned_at    TIMESTAMPTZ,
    test_contract  JSONB
);

CREATE TABLE plinth.group_rules (
    group_id   UUID        NOT NULL REFERENCES plinth.groups(id),
    rule_id    UUID        NOT NULL REFERENCES plinth.rbac_rules(id),
    granted_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (group_id, rule_id)
);

-- ── Capabilities ─────────────────────────────────────────────
-- Added in 0.2.0

CREATE TABLE plinth.capabilities (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    namespace       TEXT        NOT NULL,
    version         INTEGER     NOT NULL CHECK (version > 0),
    function        TEXT        NOT NULL,
    signature       TEXT        NOT NULL,
    provider_type   TEXT        NOT NULL CHECK (provider_type IN ('kernel', 'extension', 'sidecar')),
    extension_name  TEXT,
    scope           TEXT        NOT NULL DEFAULT 'instance' CHECK (scope IN ('instance', 'user')),
    description     TEXT        NOT NULL,
    rbac_rule       TEXT        NOT NULL,
    registered_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    enabled         BOOLEAN     NOT NULL DEFAULT true,
    UNIQUE (namespace, version, function, scope)
);

CREATE INDEX idx_capabilities_lookup
    ON plinth.capabilities (namespace, version, function)
    WHERE enabled = true;

-- ── Packages ─────────────────────────────────────────────────
-- Added in 0.4.4; SUPERSEDED + retired_at + supersedes_id added in 0.4.5

CREATE TABLE plinth.packages (
    id                   UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name                 TEXT        NOT NULL,
    version              TEXT        NOT NULL,
    state                TEXT        NOT NULL CHECK (state IN (
                                         'UPLOADING', 'VALIDATING', 'MIGRATING',
                                         'REGISTERING', 'EXTRACTING', 'ACTIVATING',
                                         'ACTIVE', 'ACTIVE_FLAGGED',
                                         'DISABLED', 'INSTALL_FAILED', 'UNINSTALLING',
                                         'SUPERSEDED'
                                     )),
    provenance           TEXT        NOT NULL CHECK (provenance IN ('bundled', 'user')),
    manifest_json        JSONB       NOT NULL,
    frontend_mount       TEXT,
    frontend_entry       TEXT,
    entry_point          TEXT        NOT NULL,
    manifest_checksum    TEXT        NOT NULL,
    last_install_report  JSONB,
    installed_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    disabled_at          TIMESTAMPTZ,
    uninstalling_at      TIMESTAMPTZ,
    retired_at           TIMESTAMPTZ,
    supersedes_id        UUID REFERENCES plinth.packages(id) ON DELETE SET NULL,
    last_rbac_test_run_at  TIMESTAMPTZ,
    last_rbac_test_result  JSONB,
    installed_by_user_id UUID REFERENCES plinth.users(id),
    UNIQUE (name, version),
    -- ICD-0.6.1 §4.3: frontend_mount and frontend_entry are NULL together
    -- (headless extension) or non-NULL together (frontend extension). The
    -- pair invariant prevents half-populated frontend rows reaching ACTIVE.
    CONSTRAINT chk_frontend_pair
        CHECK ((frontend_mount IS NULL) = (frontend_entry IS NULL))
);

-- At most one installed version per package name at a time. SUPERSEDED
-- rows (0.4.5+) coexist with a new ACTIVE row for the same name and are
-- excluded from this predicate by construction.
CREATE UNIQUE INDEX uniq_packages_name_active
    ON plinth.packages(name)
    WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED');

CREATE UNIQUE INDEX uniq_packages_mount_active
    ON plinth.packages(frontend_mount)
    WHERE frontend_mount IS NOT NULL
      AND state IN ('ACTIVE', 'ACTIVE_FLAGGED');

CREATE INDEX idx_packages_state ON plinth.packages(state);

CREATE INDEX idx_packages_supersedes
    ON plinth.packages(supersedes_id)
    WHERE supersedes_id IS NOT NULL;

CREATE TABLE plinth.panels (
    package_id    UUID        NOT NULL REFERENCES plinth.packages(id) ON DELETE CASCADE,
    panel_id      TEXT        NOT NULL,
    panel_type    TEXT        NOT NULL CHECK (panel_type IN ('primary', 'float', 'settings', 'tray')),
    slot_type     TEXT        CHECK (slot_type IN ('home')),
    declaration   JSONB       NOT NULL,
    registered_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (package_id, panel_id)
);

-- ── Realtime ─────────────────────────────────────────────────
-- Added in 0.5.0; plinth.events + plinth.user_event_cursors added in 0.5.4
-- per ICD-0.5.4 §Schema. plinth.events.seq is the canonical replay cursor
-- (BIGSERIAL PK); plinth.user_event_cursors records each user's last
-- delivered seq for reconnect delta-sync.

CREATE TABLE plinth.events (
    seq         BIGSERIAL   PRIMARY KEY,
    channel     TEXT        NOT NULL,
    payload     JSONB       NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Composite index serves the replay query
-- `WHERE seq > $1 AND channel = ANY($2) ORDER BY seq ASC`
-- per ICD-0.5.4 §OQ6.
CREATE INDEX events_channel_seq_idx ON plinth.events (channel, seq);
-- Serves the cleanup sweep `WHERE created_at < NOW() - INTERVAL ...`.
CREATE INDEX events_created_at_idx  ON plinth.events (created_at);

CREATE TABLE plinth.user_event_cursors (
    user_id    UUID        PRIMARY KEY
                           REFERENCES plinth.users(id) ON DELETE CASCADE,
    last_seq   BIGINT      NOT NULL DEFAULT 0,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- ── Audit ────────────────────────────────────────────────────
-- Added in 0.1.7

CREATE TABLE plinth.audit_log (
    id          UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    timestamp   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    action      TEXT        NOT NULL,
    user_id     UUID,
    session_id  UUID,
    detail      JSONB       NOT NULL,
    ip_address  INET,
    node_id     TEXT        NOT NULL
);

CREATE INDEX idx_audit_log_timestamp ON plinth.audit_log (timestamp DESC);
CREATE INDEX idx_audit_log_action    ON plinth.audit_log (action, timestamp);
CREATE INDEX idx_audit_log_user      ON plinth.audit_log (user_id, timestamp);
