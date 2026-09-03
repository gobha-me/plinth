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

The first registered account becomes an administrator. Keep Plinth bound to
loopback while bootstrapping it:

1. copy and edit `config.json.example`;
2. leave `listen_host` as `127.0.0.1` and temporarily set
   `registration_enabled` to `true`;
3. start Plinth and register the first account locally;
4. set `registration_enabled` to `false` and restart;
5. place an authenticated TLS reverse proxy in front of Plinth before allowing
   traffic from another host.

Do not expose a registration-enabled instance to an untrusted network.

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
- `PLINTH_REGISTRATION_ENABLED`
- `PLINTH_NODE_ID`

Production secrets belong in the deployment environment or its secret manager,
not in a committed configuration file. `dev_mode` performs a destructive schema
reset and must remain disabled outside disposable development databases.

## Network exposure

The built-in HTTP server does not terminate TLS. Bind to loopback and use a
reverse proxy that supplies TLS, request-size limits, timeouts, and appropriate
forwarded-client headers. If an orchestrator requires Plinth to listen on all
container interfaces, constrain exposure at the published port, firewall, or
network-policy layer.

The sample Compose file publishes Plinth on host loopback even though the
process listens on all interfaces inside its container network.
