# Configuration

Plinth layers configuration in this order:

1. secure built-in defaults;
2. an optional JSON file named explicitly with `--config`;
3. environment variables;
4. command-line host, port, and development-mode overrides.

Copy `config.json.example` to a local file. Its `plinth` database password
matches the disposable development database in `docker/docker-compose.yml` and
must be replaced before deployment. The example is strict JSON: comments and
trailing commas are not accepted. When `--config` is present, Plinth exits
before starting any service if the file is missing, unreadable, malformed, or
has a non-object root.

Running without `--config` is supported. It uses environment variables over
defaults that bind only to `127.0.0.1` and disable account registration.

## First administrator

First-administrator bootstrap is separate from ordinary registration. Before
starting Plinth for the first time, generate a high-entropy one-time secret and
provide it only through `PLINTH_BOOTSTRAP_TOKEN`. It must contain 32–256 bytes;
generate it from a cryptographically secure random source (for example,
`openssl rand -hex 32`). Do not put it in JSON, a
command line, an image, or source control. With Plinth still reachable only
through a trusted path, send:

```http
POST /api/auth/bootstrap
Content-Type: application/json

{"bootstrap_token":"<secret>","username":"admin","password":"<password>"}
```

The winning request creates the user and administrator membership atomically
and returns `201`. A missing, wrong, or unconfigured secret returns
`403 bootstrap_denied`; after any real user exists an attempt with the still
configured valid authority returns `409 bootstrap_closed`. Removing the
authority makes later attempts return `403 bootstrap_denied`. Concurrent
requests cannot create multiple first
administrators. Remove `PLINTH_BOOTSTRAP_TOKEN` and restart immediately after
success. Ordinary `/api/auth/register` requests never receive administrator
membership and cannot bootstrap an empty installation.

## Local registration

Registration defaults to disabled and is configured as a bounded policy:

```json
{
  "registration": {
    "mode": "disabled",
    "max_accounts": 1000,
    "source_attempts": 5,
    "subject_attempts": 5,
    "global_attempts": 100,
    "window_seconds": 60,
    "invite_ttl_seconds": 86400
  }
}
```

- `disabled` rejects new registration with `registration_unavailable`.
- `invite` requires an unexpired, unrevoked, single-use invite. Only a SHA-256
  digest is stored; the raw 43-character token is returned once when an
  administrator creates it.
- `open` admits registration without an invite, subject to the same bounds.

Every syntactically valid invite/open submission returns
`202 {"status":"processed"}` whether it created an account or was rejected.
This prevents the public response from disclosing an existing or disabled
username, invite validity, or the account ceiling. Source, submitted-subject-digest,
and global attempt limits are enforced before Argon2; `max_accounts` is a hard
total ceiling. The source and subject settings also configure login admission,
and the source/global settings protect bootstrap attempts. Reverse-proxy
deployments must pair a high bounded proxy-hop source ceiling with a trusted
per-external-source edge limiter, as the supported Helm chart does. Invalid
JSON, unknown fields, and attempts to submit `email` or
`real_name` return `400`. Username and password hash are the only
user-supplied identity data Plinth stores for a local account.

Administrators create, list, and revoke invites through
`POST`/`GET /api/auth/invites` and `DELETE /api/auth/invites/{id}`. Credential
recovery uses `POST /api/auth/recovery`; it replaces the password hash and
revokes every session and PAT for the target account, but deliberately does not
clear `disabled_at`. Recovery serializes with login and PAT issuance for that
account, so credentials admitted under the old authority cannot remain valid
after recovery returns. Plinth has no email-based reset and no automatic
persistent account lock, because either would require extra personal data or
provide a targeted denial-of-service primitive.

Changing the mode to `disabled` requires a normal restart and affects only new
registration. Existing users can still log in, and existing valid sessions,
PATs, and WebSocket credentials remain valid unless the account itself is
disabled or the credential is independently revoked or expired.

## Environment variables

The following variables override file and built-in values:

- `PLINTH_PG_HOST`
- `PLINTH_PG_PORT`
- `PLINTH_PG_USER`
- `PLINTH_PG_PASSWORD`
- `PLINTH_PG_DATABASE`
- `PLINTH_PG_POOL_SIZE`
- `PLINTH_MIGRATIONS_DIR`
- `PLINTH_DEV_MODE`
- `PLINTH_REGISTRATION_MODE` (`disabled`, `invite`, or `open`)
- `PLINTH_BOOTSTRAP_TOKEN` (bootstrap authority only; never stored)
- `PLINTH_NODE_ID`

Production secrets belong in the deployment environment or its secret manager,
not in a committed configuration file. `dev_mode` performs a destructive schema
reset and must remain disabled outside disposable development databases.

## Network exposure

The built-in HTTP server does not terminate TLS. Bind to loopback and use a
reverse proxy that supplies TLS, request-size limits, and timeouts. A proxy may
send forwarded-client headers, but Plinth currently uses the socket peer for
auditing and login throttling; it does not trust those headers as client
identity or browser authority. If an orchestrator requires Plinth to listen on
all container interfaces, constrain exposure at the published port, firewall,
or network-policy layer.

The sample Compose file publishes Plinth on host loopback even though the
process listens on all interfaces inside its container network.

### Browser origin behind TLS

When TLS terminates at a reverse proxy, set `browser_origin` to the browser's
exact public serialized origin, for example `https://plinth.example` or
`https://plinth.example:8443`. Preserve that public authority in the upstream
`Host` header. Plinth uses this one authority for cookie-authenticated unsafe
HTTP requests, login/registration browser checks, and WebSocket upgrades.
`Forwarded` and `X-Forwarded-*` headers never establish browser authority.

The origin must be lowercase absolute HTTP(S), with no credentials, trailing
slash, path, query, or fragment. An invalid value prevents startup. The legacy
`ws_browser_origin` key remains an alias for compatibility; if both keys are
present they must be identical or startup fails. Leaving `browser_origin`
empty is correct only when the scheme seen by Plinth plus the exact `Host`
header is also the browser-visible origin. In particular, an HTTPS browser in
front of an HTTP upstream must configure the public HTTPS origin.

## Supported runtime image

The supported OCI image begins with v0.6.6 and is published as
`ghcr.io/gobha-me/plinth:vMAJOR.MINOR.PATCH` for `linux/amd64` and
`linux/arm64`. No image exists for v0.6.5 or older, and no mutable `latest`,
major, or minor alias is published. Deploy the digest reference reported by the
release workflow, not an inferred tag.

The process runs as numeric UID/GID 10001 with `/var/lib/plinth` as its home
and working directory. The image declares persistent volumes at
`/var/lib/plinth/data` and `/var/lib/plinth/logs`; the operator must ensure that
mounted paths are writable by 10001:10001. The installed binary and immutable
assets are:

- `/usr/local/bin/plinth`
- `/usr/local/share/plinth/migrations`
- `/usr/local/share/plinth/bundled/shell.zip`
- `/usr/local/share/doc/plinth`

`PLINTH_MIGRATIONS_DIR` already points to the installed migrations directory.
The entry point is the Plinth binary and the default command is
`serve --host 0.0.0.0`; publish port 8080 only on a trusted interface or behind
the TLS proxy described above. Supply database settings and secrets at runtime
through the deployment secret manager. Never bake a configuration file,
credential, extension data, or logs into a derived image.

For example, with `image` set to the exact release digest and the PostgreSQL
variables already exported:

```bash
docker run --rm --name plinth \
  --publish 127.0.0.1:8080:8080 \
  --volume plinth-data:/var/lib/plinth/data \
  --volume plinth-logs:/var/lib/plinth/logs \
  --env PLINTH_PG_HOST --env PLINTH_PG_PORT \
  --env PLINTH_PG_USER --env PLINTH_PG_PASSWORD \
  --env PLINTH_PG_DATABASE \
  "$image"
```

The container changes only the process/network boundary. The first
administrator, registration, reverse-proxy origin, database isolation, and
non-development requirements in this document still apply.

## Kubernetes and Traefik

Beginning with the first released v0.6.6 image digest, the supported chart is
`deploy/helm/plinth`. It deploys one instance from an exact release digest,
consumes an existing PostgreSQL Secret, mounts persistent
data and logs, uses a ClusterIP Service and default-deny network posture, and
optionally creates a TLS Traefik route. It does not install PostgreSQL or create
credentials or certificates.

See [Kubernetes and Traefik deployment](KUBERNETES.md) before rendering or
installing the chart. That document is authoritative for Host/origin handling,
superuser database requirements, storage semantics, probe limitations,
sequential replacement, lifecycle bounds, validation, upgrade, and removal.
