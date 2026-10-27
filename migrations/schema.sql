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

-- Registration invitations contain only a digest of the one-time bearer
-- token. The raw token is returned to its creator once and is never persisted.
CREATE TABLE plinth.registration_invites (
    id                 UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    token_hash         TEXT        UNIQUE NOT NULL,
    created_by_user_id UUID        NOT NULL REFERENCES plinth.users(id),
    created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at         TIMESTAMPTZ NOT NULL,
    revoked_at         TIMESTAMPTZ,
    used_at            TIMESTAMPTZ,
    used_by_user_id    UUID        REFERENCES plinth.users(id),
    CONSTRAINT chk_registration_invites_token_hash
        CHECK (token_hash ~ '^[0-9a-f]{64}$'),
    CONSTRAINT chk_registration_invites_expiry
        CHECK (expires_at > created_at),
    CONSTRAINT chk_registration_invites_use_pair
        CHECK ((used_at IS NULL) = (used_by_user_id IS NULL)),
    CONSTRAINT chk_registration_invites_terminal_state
        CHECK (revoked_at IS NULL OR used_at IS NULL)
);

CREATE INDEX idx_registration_invites_active_expiry
    ON plinth.registration_invites(expires_at)
    WHERE revoked_at IS NULL AND used_at IS NULL;

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
    application_ready    BOOLEAN     NOT NULL DEFAULT FALSE,
    UNIQUE (name, version),
    -- ICD-0.6.1 §4.3: frontend_mount and frontend_entry are NULL together
    -- (headless extension) or non-NULL together (frontend extension). The
    -- pair invariant prevents half-populated frontend rows reaching ACTIVE.
    CONSTRAINT chk_frontend_pair
        CHECK ((frontend_mount IS NULL) = (frontend_entry IS NULL)),
    CONSTRAINT chk_packages_application_ready_state
        CHECK (NOT application_ready OR state IN ('ACTIVE', 'ACTIVE_FLAGGED'))
);

-- At most one installed version per package name at a time. SUPERSEDED
-- rows (0.4.5+) coexist with a new ACTIVE row for the same name and are
-- excluded from this predicate by construction.
CREATE UNIQUE INDEX uniq_packages_name_active
    ON plinth.packages(name)
    WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED', 'DISABLED');

CREATE UNIQUE INDEX uniq_packages_name_application_ready
    ON plinth.packages(name)
    WHERE application_ready;

CREATE UNIQUE INDEX uniq_packages_mount_active
    ON plinth.packages(frontend_mount)
    WHERE frontend_mount IS NOT NULL
      AND state IN ('ACTIVE', 'ACTIVE_FLAGGED');

CREATE INDEX idx_packages_state ON plinth.packages(state);

CREATE INDEX idx_packages_supersedes
    ON plinth.packages(supersedes_id)
    WHERE supersedes_id IS NOT NULL;

-- Kernel-owned, write-once per exact installed generation/capture profile.
-- Existing v2 generations are captured on demand; never backfill guessed hashes.
CREATE TABLE plinth.package_artifact_receipts (
    package_id UUID NOT NULL REFERENCES plinth.packages(id) ON DELETE CASCADE,
    profile INTEGER NOT NULL CHECK (profile = 1),
    artifact_digest TEXT NOT NULL CHECK (artifact_digest ~ '^[0-9a-f]{64}$'),
    declarations_digest TEXT NOT NULL CHECK (declarations_digest ~ '^[0-9a-f]{64}$'),
    record_count INTEGER NOT NULL CHECK (record_count >= 0 AND record_count <= 4096),
    content_bytes BIGINT NOT NULL CHECK (content_bytes >= 0 AND content_bytes <= 52428800),
    metadata_bytes INTEGER NOT NULL CHECK (metadata_bytes >= 32 AND metadata_bytes <= 1048576),
    PRIMARY KEY (package_id, profile)
);

-- HTTP exposure requires explicit validated install provenance. These empty
-- tables grant no claims to historical manifests. Restrictive foreign keys
-- preserve retained generation ownership until acknowledged retirement.
CREATE TABLE plinth.package_http_claim_receipts (
    generation_receipt_id UUID PRIMARY KEY,
    package_id UUID NOT NULL UNIQUE,
    capture_profile INTEGER NOT NULL CHECK (capture_profile = 1),
    name TEXT NOT NULL CHECK (name COLLATE "C" ~ '^[a-z][a-z0-9-]{1,63}$'),
    version TEXT NOT NULL CHECK (octet_length(version) BETWEEN 1 AND 256 AND version COLLATE "C" ~ '^[0-9A-Za-z.+-]+$'),
    provenance TEXT NOT NULL CHECK (provenance IN ('user','bundled')),
    schema SMALLINT NOT NULL CHECK (schema = 1),
    artifact_digest TEXT NOT NULL CHECK (artifact_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    registered_declarations_digest TEXT NOT NULL CHECK (registered_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    http_declarations_digest TEXT NOT NULL CHECK (http_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    adapter_descriptor_digest TEXT NOT NULL CHECK (adapter_descriptor_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    handler_mode TEXT NOT NULL CHECK (handler_mode IN ('quickjs','sidecar','bundled_native')),
    http_prefixes TEXT[] NOT NULL CHECK (array_ndims(http_prefixes) = 1 AND array_lower(http_prefixes,1) = 1 AND cardinality(http_prefixes) BETWEEN 1 AND 64 AND array_position(http_prefixes,NULL) IS NULL AND octet_length(array_to_string(http_prefixes,E'\n')) <= 16447),
    unauthenticated_prefixes TEXT[] NOT NULL CHECK ((cardinality(unauthenticated_prefixes) = 0 OR (array_ndims(unauthenticated_prefixes) = 1 AND array_lower(unauthenticated_prefixes,1) = 1)) AND cardinality(unauthenticated_prefixes) BETWEEN 0 AND 64 AND array_position(unauthenticated_prefixes,NULL) IS NULL AND octet_length(array_to_string(unauthenticated_prefixes,E'\n')) <= 16447 AND unauthenticated_prefixes <@ http_prefixes),
    adapter_binding_key TEXT NOT NULL CHECK (octet_length(adapter_binding_key) BETWEEN 1 AND 256),
    module_entry TEXT CHECK ((handler_mode = 'quickjs' AND module_entry IS NOT NULL AND module_entry = 'server/http.js') OR (handler_mode <> 'quickjs' AND module_entry IS NULL)),
    UNIQUE (package_id,generation_receipt_id),
    FOREIGN KEY (package_id,capture_profile) REFERENCES plinth.package_artifact_receipts(package_id,profile) ON DELETE RESTRICT
);

CREATE TABLE plinth.package_http_claim_reservations (
    package_id UUID PRIMARY KEY,
    http_prefixes TEXT[] NOT NULL CHECK (array_ndims(http_prefixes) = 1 AND array_lower(http_prefixes,1) = 1 AND cardinality(http_prefixes) BETWEEN 1 AND 64 AND array_position(http_prefixes,NULL) IS NULL AND octet_length(array_to_string(http_prefixes,E'\n')) <= 16447),
    http_declarations_digest TEXT NOT NULL CHECK (http_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    generation_receipt_id UUID,
    phase TEXT NOT NULL CHECK (phase IN ('HELD','RETIRING')),
    FOREIGN KEY (package_id) REFERENCES plinth.packages(id) ON DELETE RESTRICT,
    FOREIGN KEY (package_id,generation_receipt_id) REFERENCES plinth.package_http_claim_receipts(package_id,generation_receipt_id) ON DELETE RESTRICT
);

CREATE TABLE plinth.package_http_adapter_bindings (
    adapter_epoch UUID PRIMARY KEY,
    generation_receipt_id UUID NOT NULL,
    owner_epoch UUID NOT NULL,
    clock_domain_id UUID NOT NULL,
    adapter_descriptor_digest TEXT NOT NULL CHECK (adapter_descriptor_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
    retired_at TIMESTAMPTZ,
    FOREIGN KEY (generation_receipt_id) REFERENCES plinth.package_http_claim_receipts(generation_receipt_id) ON DELETE RESTRICT
);

-- A pending HTTP dispatch pins its exact receipt until owned terminal
-- settlement. The singleton gate serializes the bounded pending count.
CREATE TABLE plinth.extension_http_pending_gate (
    singleton BOOLEAN PRIMARY KEY DEFAULT true CHECK(singleton)
);
INSERT INTO plinth.extension_http_pending_gate VALUES(true);

CREATE TABLE plinth.extension_http_dispatch_obligations (
    dispatch_id UUID PRIMARY KEY,
    binding JSONB NOT NULL CHECK(jsonb_typeof(binding)='object' AND octet_length(binding::text)<=8192),
    owner_epoch UUID NOT NULL,
    fence_version BIGINT NOT NULL CHECK(fence_version>0),
    revision BIGINT NOT NULL DEFAULT 1 CHECK(revision>0),
    authorization_state TEXT NOT NULL DEFAULT 'prepared' CHECK(authorization_state IN ('prepared','armed','dispatched','unknown','aborted')),
    choice JSONB CHECK(choice IS NULL OR (jsonb_typeof(choice)='object' AND octet_length(choice::text)<=1024)),
    wire TEXT NOT NULL DEFAULT 'not_attempted' CHECK(wire IN ('not_attempted','attempt_started','write_completed','write_failed','unknown')),
    physical_entry TEXT NOT NULL DEFAULT 'unknown' CHECK(physical_entry IN ('known_entered','known_not_entered','unknown')),
    entered_at_ns NUMERIC(20,0) CHECK(entered_at_ns IS NULL OR entered_at_ns BETWEEN 0 AND 18446744073709551615),
    live_generation_receipt_id UUID REFERENCES plinth.package_http_claim_receipts(generation_receipt_id) ON DELETE RESTRICT,
    projection_confirmed BOOLEAN NOT NULL DEFAULT false,
    pending BOOLEAN NOT NULL DEFAULT true,
    retained BOOLEAN NOT NULL DEFAULT false,
    settlement_resolved BOOLEAN NOT NULL DEFAULT false,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK((physical_entry='known_entered')=(entered_at_ns IS NOT NULL)),
    CHECK(NOT projection_confirmed OR authorization_state='dispatched'),
    CHECK(live_generation_receipt_id IS NULL OR (live_generation_receipt_id::text IS NOT DISTINCT FROM (binding#>>'{context,extension,generation_receipt_id}'))),
    CHECK(NOT pending OR live_generation_receipt_id IS NOT NULL),
    CHECK(live_generation_receipt_id IS NOT NULL OR (NOT pending AND retained AND settlement_resolved)),
    CHECK(pending OR (retained AND settlement_resolved))
);
CREATE INDEX extension_http_pending_package ON
    plinth.extension_http_dispatch_obligations((binding->>'package_name'))
    WHERE pending;

CREATE TABLE plinth.panels (
    package_id    UUID        NOT NULL REFERENCES plinth.packages(id) ON DELETE CASCADE,
    panel_id      TEXT        NOT NULL,
    panel_type    TEXT        NOT NULL CHECK (panel_type IN ('primary', 'float', 'settings', 'tray')),
    slot_type     TEXT        CHECK (slot_type IN ('home')),
    declaration   JSONB       NOT NULL,
    registered_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    validated_format_version SMALLINT NOT NULL DEFAULT 0,
    logical_install_order NUMERIC(20,0),
    CONSTRAINT chk_panels_validated_format_version
        CHECK (validated_format_version IN (0, 2)),
    CONSTRAINT chk_panels_logical_install_order
        CHECK (logical_install_order IS NULL OR
               (validated_format_version = 2 AND panel_type = 'float' AND
                logical_install_order >= 1 AND
                logical_install_order <= 18446744073709551615)),
    PRIMARY KEY (package_id, panel_id)
);

-- Allocated only in the transaction that first commits an installed ACTIVE
-- generation. Its row lock remains held through COMMIT; generation rows carry
-- their own rank copies, independently of predecessor garbage collection.
CREATE TABLE plinth.handler_install_order_allocator (
    id SMALLINT PRIMARY KEY DEFAULT 1,
    last_value NUMERIC(20,0) NOT NULL DEFAULT 0,
    CONSTRAINT chk_handler_install_order_allocator_id CHECK (id = 1),
    CONSTRAINT chk_handler_install_order_allocator_value
        CHECK (last_value >= 0 AND last_value <= 18446744073709551615)
);
INSERT INTO plinth.handler_install_order_allocator (id, last_value) VALUES (1, 0);

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

-- Kernel-owned realtime transport. PostgreSQL NOTIFY carries only the row id;
-- listeners load and validate the envelope from this protected table. The
-- transaction-scoped advisory lock in enqueue_realtime_event serializes id
-- allocation with commit order, so each listener can reject replayed hints by
-- advancing one monotonic cursor.
CREATE TABLE plinth.realtime_outbox (
    id          BIGSERIAL   PRIMARY KEY,
    payload     JSONB       NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX realtime_outbox_created_at_idx
    ON plinth.realtime_outbox (created_at);

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
    detail_encoding SMALLINT NOT NULL DEFAULT 0
        CONSTRAINT chk_audit_log_detail_encoding CHECK (detail_encoding IN (0, 1)),
    ip_address  INET,
    node_id     TEXT        NOT NULL
);

CREATE INDEX idx_audit_log_timestamp ON plinth.audit_log (timestamp DESC);
CREATE INDEX idx_audit_log_action    ON plinth.audit_log (action, timestamp);
CREATE INDEX idx_audit_log_user      ON plinth.audit_log (user_id, timestamp);
