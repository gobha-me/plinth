# Extension database authority

Extension SQL uses a separate PostgreSQL login, with `current_user` and
`session_user` both bound to that extension. `RESET ROLE`, transaction control,
`set_config`, and disabling the search-path convenience wrapper cannot restore
kernel authority. Standalone queries, executions, and batches all select the
same authenticated extension client; batches pin its transaction.

`RuntimePool` owns the extension database clients. Contexts and in-flight
operations retain that ownership. The existing shutdown coordinator drains
runtime leases and async operations before destroying runtime pools and their
database clients. No process-global pool or additional shutdown handler exists.
Each extension pool has at most four connections and a five-second query
timeout. The host-only connection configuration is never part of `config.get()`.

Startup applies `migrations/extension_database.sql` before package code or
ingress. It provisions and reconciles installed extension schemas, restricted
login roles, memberships, object ownership, and grants. Installation and trusted
host-created runtime contexts use the same provisioning operation. The kernel
database account must be a PostgreSQL superuser to maintain the database-wide
DDL guard, create roles, change object ownership, and set default privileges.
This strengthens the earlier CREATE/CREATEROLE-only installation requirement;
the supplied Docker and CI PostgreSQL accounts already satisfy it. Environments
that prohibit superuser bootstrap are unsupported and fail explicitly before
isolation reconciliation. Provisioning failure
aborts startup or rejects the extension operation. It never falls back to the
kernel account.

Roles use `px_` followed by 60 hexadecimal SHA-256 characters derived from the
database name and full extension name. This fits PostgreSQL's 63-byte identifier
limit and separates the cluster-wide role namespace across application
databases. The full extension name and actual schema name remain in kernel-only
metadata. Ambiguous schema names after PostgreSQL identifier truncation fail
closed. Existing conventional `ext_<name>_role` grants are revoked in the current
database, while ownership moves to the new role; other databases are untouched.

Every role has `NOSUPERUSER`, `NOCREATEDB`, `NOCREATEROLE`, `NOINHERIT`,
`NOREPLICATION`, and `NOBYPASSRLS`, and no parent-role memberships. It can create
and use objects in its own schema. Shared user lookup grants only `SELECT` on
`plinth.users(id, username)` and `REFERENCES(id)` for foreign keys. It cannot read
password hashes, credentials, sessions, or other extensions' schemas. PUBLIC
application-schema privileges and PUBLIC function execution are removed.

PostgreSQL provides no producer ACL for `NOTIFY`, so notification payloads are
never an authority boundary. Capability notifications are coalescible hints
that trigger a full reload from `plinth.capabilities`. Realtime envelopes are
inserted by the kernel into `plinth.realtime_outbox`; the wire notification
carries only the ordered row id. Extension roles cannot read or write the
outbox or execute its enqueue function, and replayed or forged hints cannot
alter the listener's monotonic cursor or dispatch an event.

Random login secrets are generated inside PostgreSQL and stored in
`plinth.extension_database_credentials`, accessible only to the kernel account.
Application-generated provisioning SQL contains no secret values. Runtime
clients fetch the secret through the kernel client and authenticate directly as
the restricted login. Startup restores the recorded credential without rotating
it during ordinary upgrades or invalidating existing connections. Database
backups must protect this table with the same access controls as kernel
configuration and identity data.

Package migrations remain inside their existing per-file or caller-owned
transaction. Each execution creates a restricted-owner `SECURITY DEFINER` guard,
invokes it once, and drops it before commit. The guard is never committed for a
later privileged invocation: an extension owner could otherwise change its
security mode between calls. PostgreSQL rejects role/session authorization and
transaction-control changes within the guard. A kernel-owned `ddl_command_end`
event trigger rejects every deferrable trigger or constraint on a relation owned
by an extension role, including temporary relations. The check runs after each
DDL command, before later statements can enqueue a callback; ALTER operations
and dynamic SQL receive the same protection. Startup reconciliation and each
migration invocation also reject pre-existing deferrable objects. Existing
packages must convert these to immediate constraints before upgrading.

Merely executing `SET CONSTRAINTS ALL IMMEDIATE` is insufficient: a trigger can
defer another trigger while the first queue is being drained. Rejecting the
deferred objects preserves the caller's transaction without allowing extension
callbacks to survive into privileged commit. Ordinary immediate constraints,
triggers, and foreign keys remain supported. The event trigger is enabled for
all replication modes; extension roles cannot change or disable it. Migrations
must use schema-qualified names and cannot defer work beyond the guarded call.
Isolation functions put `pg_temp` explicitly after `pg_catalog`, and guard
catalog reads are qualified, so temporary catalog-named objects cannot hide
deferred work or redirect subsequent provisioning queries.

This contract supersedes the historical `NOLOGIN`/`SET ROLE` sketch and broad
`plinth.users` grant in ICD-0.4.3. Search-path settings are name resolution,
not the authorization boundary. Tests use privileged fixture/admin sessions and
the same restricted runtime logins as production, including adversarial SQL and
deferred-trigger migrations.

Historical migrations may still grant their own table privileges to
`ext_<name>_role`. Provisioning preserves this grant target, creating a `NOLOGIN`
role only when absent and leaving any existing cluster-wide role settings
unchanged. The runtime receives no membership in that role, and the alias has
no schema access in this database. This preserves published migration checksums
without restoring the old authorization model.
