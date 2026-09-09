-- Reconciled on every startup, before package code or ingress can run.
-- Passwords never appear in application-generated SQL or diagnostics.
DO $authority$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_roles
                   WHERE rolname = current_user AND rolsuper) THEN
        RAISE EXCEPTION USING ERRCODE = '42501',
            MESSAGE = 'extension database isolation requires a superuser bootstrap account to maintain its DDL guard';
    END IF;
END
$authority$;

CREATE TABLE IF NOT EXISTS plinth.extension_database_credentials (
    extension_name TEXT PRIMARY KEY,
    schema_name TEXT UNIQUE NOT NULL,
    role_name TEXT UNIQUE NOT NULL,
    password TEXT NOT NULL
);
REVOKE ALL ON plinth.extension_database_credentials FROM PUBLIC;

-- Reconciled for existing databases as well as fresh schema creation. Raw
-- PostgreSQL NOTIFY has no channel ACL, so its payload is only an untrusted
-- wake hint. Authoritative envelopes live here and are writable solely through
-- this kernel-owned function. The advisory xact lock is acquired before the
-- BIGSERIAL value is allocated and held through caller commit, making ids
-- monotonic in commit order even when emitters use caller-owned transactions.
CREATE TABLE IF NOT EXISTS plinth.realtime_outbox (
    id          BIGSERIAL   PRIMARY KEY,
    payload     JSONB       NOT NULL,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
CREATE INDEX IF NOT EXISTS realtime_outbox_created_at_idx
    ON plinth.realtime_outbox (created_at);
REVOKE ALL ON plinth.realtime_outbox FROM PUBLIC;
REVOKE ALL ON SEQUENCE plinth.realtime_outbox_id_seq FROM PUBLIC;

CREATE OR REPLACE FUNCTION plinth.enqueue_realtime_event(envelope JSONB)
RETURNS BIGINT LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $enqueue$
DECLARE
    event_id BIGINT;
BEGIN
    IF envelope IS NULL OR jsonb_typeof(envelope) <> 'object' THEN
        RAISE EXCEPTION USING ERRCODE = '22023',
            MESSAGE = 'realtime envelope must be a JSON object';
    END IF;
    PERFORM pg_advisory_xact_lock(4643982238346725961::BIGINT);
    INSERT INTO plinth.realtime_outbox(payload)
        VALUES (envelope) RETURNING id INTO event_id;
    PERFORM pg_notify('plinth:realtime', event_id::TEXT);
    RETURN event_id;
END
$enqueue$;
REVOKE ALL ON FUNCTION plinth.enqueue_realtime_event(JSONB) FROM PUBLIC;

-- Deferred callbacks must never survive a guarded migration and run under
-- the privileged transaction committer. This check includes temporary objects.
CREATE OR REPLACE FUNCTION plinth.assert_extension_immediate_constraints(role_id OID)
RETURNS VOID LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $immediate$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_catalog.pg_trigger trigger JOIN pg_catalog.pg_class relation ON relation.oid=trigger.tgrelid
        WHERE trigger.tgdeferrable AND relation.relowner=role_id)
        OR EXISTS (SELECT 1 FROM pg_catalog.pg_constraint constraint_row JOIN pg_catalog.pg_class relation ON relation.oid=constraint_row.conrelid
        WHERE constraint_row.condeferrable AND relation.relowner=role_id) THEN
        RAISE EXCEPTION USING ERRCODE='42501', MESSAGE='extension database objects must use immediate constraints and triggers';
    END IF;
END
$immediate$;
REVOKE ALL ON FUNCTION plinth.assert_extension_immediate_constraints(OID) FROM PUBLIC;

CREATE OR REPLACE FUNCTION plinth.extension_immediate_ddl_guard()
RETURNS EVENT_TRIGGER LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $ddl_guard$
BEGIN
    IF current_user ~ '^px_[0-9a-f]{60}$' THEN
        PERFORM plinth.assert_extension_immediate_constraints(current_user::pg_catalog.regrole::pg_catalog.oid);
    END IF;
END
$ddl_guard$;
REVOKE ALL ON FUNCTION plinth.extension_immediate_ddl_guard() FROM PUBLIC;
DROP EVENT TRIGGER IF EXISTS plinth_extension_immediate_ddl;
CREATE EVENT TRIGGER plinth_extension_immediate_ddl ON ddl_command_end
    EXECUTE FUNCTION plinth.extension_immediate_ddl_guard();
ALTER EVENT TRIGGER plinth_extension_immediate_ddl ENABLE ALWAYS;

CREATE OR REPLACE FUNCTION plinth.provision_extension_database(extension_name TEXT)
RETURNS VOID LANGUAGE plpgsql SET search_path = pg_catalog, pg_temp AS $provision$
<<role_setup>>
DECLARE
    schema_name TEXT := left('ext_' || extension_name, 63);
    role_name TEXT := 'px_' || left(encode(sha256(convert_to(current_database() || ':' || extension_name, 'UTF8')), 'hex'), 60);
    legacy_role TEXT := left('ext_' || extension_name || '_role', 63);
    role_id OID;
    database_id OID;
    secret TEXT;
    item RECORD;
BEGIN
    PERFORM pg_advisory_xact_lock(hashtextextended('plinth-extension-db:' || extension_name, 0));
    IF extension_name !~ '^[a-z][a-z0-9_]{2,62}$' THEN
        RAISE EXCEPTION 'extension identity cannot be represented as an isolated database role';
    END IF;
    SELECT oid INTO database_id FROM pg_database WHERE datname = current_database();
    SELECT oid INTO role_id FROM pg_roles WHERE rolname = role_name;
    IF role_id IS NOT NULL AND EXISTS (
        SELECT 1 FROM pg_shdepend WHERE refclassid = 'pg_authid'::regclass
        AND refobjid = role_id AND dbid NOT IN (0, database_id)
    ) THEN
        RAISE EXCEPTION 'extension database role is shared with another database';
    END IF;
    IF role_name = current_user THEN
        RAISE EXCEPTION 'kernel and extension database identities must differ';
    END IF;
    -- Published migrations can name this historical grant target. Keep their
    -- checksums valid without giving the authenticated runtime any membership
    -- in the cluster-wide alias. Never change an existing alias in another DB.
    IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = legacy_role) THEN
        EXECUTE format('CREATE ROLE %I NOLOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION NOBYPASSRLS', legacy_role);
    END IF;
    IF role_id IS NULL THEN
        EXECUTE format('CREATE ROLE %I LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION NOBYPASSRLS', role_name);
        SELECT oid INTO role_id FROM pg_roles WHERE rolname = role_name;
    ELSE
        EXECUTE format('ALTER ROLE %I LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION NOBYPASSRLS', role_name);
    END IF;
    FOR item IN SELECT parent.rolname FROM pg_auth_members membership
        JOIN pg_roles parent ON parent.oid = membership.roleid
        WHERE membership.member = role_id
    LOOP
        EXECUTE format('REVOKE %I FROM %I CASCADE', item.rolname, role_name);
    END LOOP;
    EXECUTE format('ALTER ROLE %I RESET ALL', role_name);
    INSERT INTO plinth.extension_database_credentials AS credentials
        VALUES (extension_name, schema_name, role_name, replace(gen_random_uuid()::text || gen_random_uuid()::text, '-', ''))
        ON CONFLICT ON CONSTRAINT extension_database_credentials_pkey DO NOTHING;
    SELECT credentials.password INTO STRICT secret
        FROM plinth.extension_database_credentials credentials
        WHERE credentials.extension_name = provision_extension_database.extension_name
        AND credentials.role_name = role_setup.role_name;
    IF EXISTS (SELECT 1 FROM plinth.extension_database_credentials credentials
        WHERE credentials.schema_name = role_setup.schema_name
        AND credentials.extension_name <> provision_extension_database.extension_name) THEN
        RAISE EXCEPTION 'extension schema identity collides after PostgreSQL identifier truncation';
    END IF;
    EXECUTE format('ALTER ROLE %I PASSWORD %L', role_name, secret);

    -- A private application database must not grant arbitrary extension roles
    -- ambient PUBLIC access to application objects or PUBLIC schema creation.
    EXECUTE format('REVOKE CREATE ON DATABASE %I FROM PUBLIC', current_database());
    FOR item IN SELECT nspname FROM pg_namespace
        WHERE nspname IN ('plinth', 'public') OR nspname LIKE 'ext\_%' ESCAPE '\'
    LOOP
        EXECUTE format('REVOKE ALL ON SCHEMA %I FROM PUBLIC, %I', item.nspname, role_name);
        EXECUTE format('REVOKE ALL ON ALL TABLES IN SCHEMA %I FROM PUBLIC, %I', item.nspname, role_name);
        EXECUTE format('REVOKE ALL ON ALL SEQUENCES IN SCHEMA %I FROM PUBLIC, %I', item.nspname, role_name);
        EXECUTE format('REVOKE ALL ON ALL FUNCTIONS IN SCHEMA %I FROM PUBLIC, %I', item.nspname, role_name);
        IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = legacy_role) THEN
            EXECUTE format('REVOKE ALL ON SCHEMA %I FROM %I', item.nspname, legacy_role);
            EXECUTE format('REVOKE ALL ON ALL TABLES IN SCHEMA %I FROM %I', item.nspname, legacy_role);
            EXECUTE format('REVOKE ALL ON ALL SEQUENCES IN SCHEMA %I FROM %I', item.nspname, legacy_role);
            EXECUTE format('REVOKE ALL ON ALL FUNCTIONS IN SCHEMA %I FROM %I', item.nspname, legacy_role);
        END IF;
    END LOOP;
    EXECUTE format('CREATE SCHEMA IF NOT EXISTS %I', schema_name);
    EXECUTE format('GRANT USAGE, CREATE ON SCHEMA %I TO %I', schema_name, role_name);
    EXECUTE format('GRANT USAGE ON SCHEMA plinth TO %I', role_name);
    EXECUTE format('GRANT EXECUTE ON FUNCTION plinth.assert_extension_immediate_constraints(OID) TO %I', role_name);
    EXECUTE format('GRANT SELECT (id, username), REFERENCES (id) ON plinth.users TO %I', role_name);
    -- Repair the old broad column grant as well as table-level grants.
    EXECUTE format('REVOKE ALL (password_hash, created_at, disabled_at, is_test_user) ON plinth.users FROM %I', role_name);

    -- Migrations used to create objects as the kernel. Transfer only objects
    -- in the extension's own schema; never REASSIGN the kernel's entire role.
    FOR item IN SELECT c.relname, c.relkind FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname = schema_name AND c.relkind IN ('r', 'p', 'v', 'm', 'S', 'f')
    LOOP
        EXECUTE format('ALTER %s %I.%I OWNER TO %I',
            CASE item.relkind WHEN 'S' THEN 'SEQUENCE' WHEN 'v' THEN 'VIEW'
                WHEN 'm' THEN 'MATERIALIZED VIEW' WHEN 'f' THEN 'FOREIGN TABLE' ELSE 'TABLE' END,
            schema_name, item.relname, role_name);
    END LOOP;
    FOR item IN SELECT p.oid::regprocedure AS signature FROM pg_proc p
        JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = schema_name
    LOOP
        EXECUTE format('ALTER ROUTINE %s OWNER TO %I', item.signature, role_name);
    END LOOP;
    FOR item IN SELECT t.typname FROM pg_type t JOIN pg_namespace n ON n.oid = t.typnamespace
        WHERE n.nspname = schema_name AND t.typtype IN ('e', 'd')
    LOOP
        EXECUTE format('ALTER TYPE %I.%I OWNER TO %I', schema_name, item.typname, role_name);
    END LOOP;
    PERFORM plinth.assert_extension_immediate_constraints(role_id);
    EXECUTE format('GRANT ALL ON ALL TABLES IN SCHEMA %I TO %I', schema_name, role_name);
    EXECUTE format('GRANT ALL ON ALL SEQUENCES IN SCHEMA %I TO %I', schema_name, role_name);
    EXECUTE format('GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA %I TO %I', schema_name, role_name);
    EXECUTE format('GRANT ALL ON ALL TABLES IN SCHEMA %I TO %I', schema_name, current_user);
    EXECUTE format('GRANT ALL ON ALL SEQUENCES IN SCHEMA %I TO %I', schema_name, current_user);
    EXECUTE format('ALTER DEFAULT PRIVILEGES FOR ROLE %I REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC', role_name);
    EXECUTE format('ALTER DEFAULT PRIVILEGES FOR ROLE %I IN SCHEMA %I GRANT ALL ON TABLES TO %I', role_name, schema_name, current_user);
    EXECUTE format('ALTER DEFAULT PRIVILEGES FOR ROLE %I IN SCHEMA %I GRANT ALL ON SEQUENCES TO %I', role_name, schema_name, current_user);
END
$provision$;
REVOKE ALL ON FUNCTION plinth.provision_extension_database(TEXT) FROM PUBLIC;
