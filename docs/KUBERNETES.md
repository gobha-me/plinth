# Kubernetes and Traefik deployment

This boundary becomes supported with the first released v0.6.6 image digest;
source on an unreleased `main` branch is not deployment support evidence. The
chart supplies a restrictive StatefulSet, application ClusterIP Service,
headless governing Service, persistent storage, and optional Traefik routing.
It does not install PostgreSQL, issue certificates, create database
credentials, or make a multi-replica topology safe.

Plinth is pre-1.0 software. Start with an isolated namespace, a dedicated
database, and no public route. Establish the first administrator through a
trusted path, verify the deployment, and only then enable the TLS route.

## Prerequisites and release identity

Use Kubernetes 1.35 or newer with a default-deny-capable NetworkPolicy
provider, Helm 3, and a Traefik installation whose CRDs are already present
when Traefik routing is enabled. The live contract is validated against K3s
1.35.8 and 1.37.0 with Traefik 3.7.13; other releases that provide
`traefik.io/v1alpha1` require separate operator validation and are not
currently supported. Supply these operator-owned resources before installing:

- a dedicated PostgreSQL database and superuser login;
- an opaque Secret containing the PostgreSQL connection values;
- storage classes suitable for the data and log claims;
- a TLS Secret and public DNS name when ingress is enabled; and
- the exact OCI digest recorded by a Plinth release.

Supported OCI images start at v0.6.6. Releases v0.6.5 and older have no
supported runtime image. Resolve the release's
`ghcr.io/gobha-me/plinth@sha256:...` reference from its workflow evidence and
verify the provenance before deployment:

```bash
image='ghcr.io/gobha-me/plinth@sha256:REPLACE_WITH_RELEASE_DIGEST'
docker pull "$image"
gh attestation verify "oci://$image" \
  --repo gobha-me/plinth \
  --signer-workflow gobha-me/plinth/.github/workflows/runtime-image.yml
```

The chart is versioned with the repository source rather than published as a
separate mutable chart-registry entry. Use it from the exact source revision
recorded in the verified image, never from an ambient `main` checkout:

```bash
version=$(docker image inspect --format \
  '{{ index .Config.Labels "org.opencontainers.image.version" }}' "$image")
revision=$(docker image inspect --format \
  '{{ index .Config.Labels "org.opencontainers.image.revision" }}' "$image")
git clone --depth 1 --branch "v$version" \
  https://github.com/gobha-me/plinth.git "plinth-$version"
cd "plinth-$version"
test "$(git rev-parse HEAD)" = "$revision"
test "$(tr -d '\r\n' < VERSION)" = "$version"
```

The tag, image revision label, and chart source must agree before rendering.

The chart requires a digest. A mutable tag, `latest`, a major/minor alias, or a
locally rebuilt image is not a supported release identity. The image supports
`linux/amd64` and `linux/arm64`, runs `/usr/local/bin/plinth` directly as PID 1,
and uses numeric UID/GID 10001.

## PostgreSQL authority and capacity

Plinth does not support an unprivileged runtime role followed by a separate
one-shot migration role. The configured kernel account must remain a PostgreSQL
superuser: startup reconciles database-wide extension isolation, creates or
alters restricted extension login roles, maintains an event trigger, and
changes grants and ownership. Use a dedicated database and protect this account
as a high-value secret. `PLINTH_DEV_MODE` must remain `false`; enabling it drops
and recreates Plinth and extension schemas.

The main connection pool defaults to 32. Two more connections back WebSocket
authority, capability and realtime listeners each retain a dedicated
connection, and each active extension may open as many as four restricted-role
connections. Shutdown cancellation also needs a free control connection. Size
PostgreSQL `max_connections` for those consumers, installed extensions,
administrative access, and failure headroom; do not allocate exactly the main
pool size.

The current database configuration has no PostgreSQL TLS mode, root CA, or
client certificate fields, so the chart cannot claim PostgreSQL certificate
identity verification or require end-to-end database TLS. An unspecified
libpq SSL mode is not an enforceable `verify-full` policy. Keep PostgreSQL on an
authenticated, isolated network or provide encryption through an
operator-controlled network layer; if policy requires Plinth itself to verify
PostgreSQL TLS, this release is unsupported. Do not smuggle connection-string
options through a Secret key.

The chart consumes an existing Secret rather than accepting credentials as
Helm values. This prevents a password from being copied into rendered output,
source control, shell history, or the Helm release record. Restrict Secret read
access to the cluster controllers that must materialize the Pod. The workload
service account has no API token or RBAC and must not receive Kubernetes API
permission to read Secrets; only the selected values are exposed in its Pod
environment. Backups must protect both PostgreSQL and the
`plinth.extension_database_credentials` table as credential material.

## Configuration and public authority

The chart mounts a non-secret JSON configuration with `listen_host` set to
`0.0.0.0` and passes it explicitly with `plinth serve --config ...`. Database
values then override the file through `PLINTH_PG_*` variables sourced from the
Secret. Do not put the database password in the JSON file.

When Traefik terminates TLS, `browser_origin` must be the browser's exact public
serialized origin, for example `https://plinth.example` or
`https://plinth.example:8443`. It must be lowercase and contain no credentials,
path, query, fragment, trailing slash, or explicit default port. Traefik must
preserve the same public authority in the upstream `Host` header. Plinth
requires both `Origin` and `Host` to match for cookie-authenticated mutations
and browser WebSocket upgrades. The supported route therefore keeps
`passHostHeader=true`. `Forwarded` and `X-Forwarded-*` never establish that
authority; setting `X-Forwarded-Proto: https` is not a substitute for
`browser_origin`.

The chart creates only a TLS route on the configured secure Traefik entry point
and references an existing certificate Secret. It does not create a plaintext
route, certificate issuer, or HTTP-to-HTTPS redirect; those remain explicit
controller-level policy.

The `/ws/events` route is a long-lived WebSocket and must not receive a request
buffer or ordinary in-flight HTTP limit. The package upload route accepts a ZIP
of at most 50 MiB and can expand at most 100 MiB. Its Traefik request-body limit
must exceed 50 MiB to allow multipart framing; the supported chart uses a
64 MiB boundary for that route and a smaller limit for ordinary requests. A
dedicated two-request package concurrency limit runs before buffering so the
accepted multipart bodies remain within the 128 MiB upload scratch volume.
Extension execution can run for 30 seconds, while the WebSocket heartbeat uses
a 30-second interval and a 10-second timeout. Proxy response and idle limits
must exceed those bounds. Package installation and migrations do not have a
single global application deadline, so they use a separate 15-minute
response-header budget instead of the ordinary 60-second budget. Exceeding that
proxy budget does not cancel server-side work: treat the outcome as
indeterminate, inspect package state, and never retry blindly.

Traefik forwards client-address headers, but Plinth currently records and
throttles the socket peer address. Behind a conventional proxy this is the
Traefik Pod address: five failed logins within 60 seconds can therefore affect
all clients sharing that peer, and audit rows do not contain the original
client address. Treat the audit address as the proxy hop. The chart's body and
in-flight limits do not provide per-client brute-force protection. Operators
that require per-client controls must place a separately managed trusted edge
limiter ahead of Traefik.
Do not configure Plinth to trust arbitrary forwarded headers; a future narrowly
scoped trusted-proxy contract must land before those headers become an identity
source.

## Storage and Pod security

The image writes installed packages beneath `/var/lib/plinth/data`, uses
`/var/lib/plinth/data/staging` for extraction, and writes rotating logs beneath
`/var/lib/plinth/logs`. Data, its `extensions` directory, and staging must be on
one filesystem. Package activation relies on POSIX symlinks and atomic
`rename(2)`; mounting staging separately, using an object-store filesystem, or
using storage without those semantics is unsupported. Drogon's multipart
receive buffer uses a bounded Pod-ephemeral volume at
`/var/lib/plinth/uploads`; it never substitutes for package staging. The log
sink retains up to five 10 MiB rotated files in addition to current output.

The claims must be writable by 10001:10001. The default Pod security context is
non-root with that UID/GID and filesystem group, drops every Linux capability,
disallows privilege escalation, uses the runtime-default seccomp profile, and
mounts a writable temporary directory while keeping the image filesystem read
only. Its dedicated service account does not receive an API token. Do not relax
these controls to work around a storage ownership error; configure the storage
class or an explicit operator-approved ownership preparation step instead.

Persistent data survives a normal chart upgrade. Treat uninstall as a distinct
data decision: retain the claims by default, take a PostgreSQL and volume
backup, and delete retained claims only with an explicit operator action after
verifying the backup. A test-only ephemeral installation may opt into claim
deletion in its task-owned namespace.

## Service and network isolation

The application Service is `ClusterIP` on TCP 8080. A separate headless
ClusterIP Service is the StatefulSet's immutable governing service for stable
Pod identity. Neither is an external exposure. The chart never defaults to
`NodePort`, `LoadBalancer`, host networking, or host ports. Traefik routing is
disabled until the operator supplies an exact public host and existing TLS
Secret.

The default NetworkPolicy denies ingress and egress. Enable only:

- ingress to TCP 8080 from the selected Traefik namespace and Pods;
- DNS egress to the cluster DNS service; and
- PostgreSQL egress to the operator-selected namespace/Pods and TCP port.

Keep selectors fail closed. Broad namespace-only ingress, unrestricted CIDR
egress, and an empty PostgreSQL selector are explicit operator overrides, not
secure defaults. Kubernetes health probes originate outside the Pod network on
common implementations; confirm the cluster's policy behavior before relying
on that exception.

## Probes and lifecycle bounds

Plinth currently exposes only `GET /healthz`. Once startup has completed it
returns `200` with `{"status":"ok"}`. The process does not start its listener
until database bootstrap, extension isolation, package reconciliation, and
bundled-shell installation have succeeded, so connection refusal before that
point is a valid startup signal. The chart's startup probe checks every five
seconds and allows 60 consecutive failures, a 300-second failure budget; the
exact production-image verifier allows 45 seconds.

The same endpoint is used for liveness and readiness. It is process health, not
dependency health: a PostgreSQL outage after startup does not make `/healthz`
fail. During SIGTERM the process closes its ingress gate and subsequent requests
receive `503 server_shutting_down` or a refused connection. This makes the Pod
unready during drain but does not provide a distinct dependency-aware readiness
contract. Operators must alert on PostgreSQL and application failures
separately.

The shutdown coordinator has a shared 40-second drain budget and a 50-second
absolute process watchdog. Kubernetes must grant at least 60 seconds before
SIGKILL. The binary must remain PID 1 and receive SIGTERM directly. Do not add a
shell wrapper, `preStop` kill, fixed sleep, or a shorter Pod grace period.

## Single-replica replacement

The supported topology is exactly one replica. Startup mutates PostgreSQL
cluster-wide roles and an event trigger, while package activation mutates the
shared filesystem. Concurrent startup, active-active package installation, and
a shared multi-writer volume are not a supported HA contract.

Upgrades therefore use an `OrderedReady` one-replica StatefulSet rolling
replacement. Its stable ordinal prevents the successor Pod from starting until
the prior Pod identity has terminated, so both kernels do not mutate one
database and claim concurrently. This deliberately creates a maintenance
window. Do not increase replicas, switch to a Deployment, or force-delete the
Pod to hide that downtime; forced deletion can violate the at-most-one identity
guarantee. The planned HA architecture is not implementation evidence.

## Install and verify

Create a task-owned namespace and the operator-managed Secret and TLS material
first. Render the chart locally and inspect it before applying it. Use the chart
README and `values.schema.json` as the exact value-key reference; at minimum the
digest, PostgreSQL Secret name, public origin, storage class, and Traefik
selectors must be deliberate.

This render-only preflight keeps Traefik disabled and does not require a live
database. It deliberately does not claim that placeholder database values can
start Plinth:

```bash
namespace=plinth
release=plinth
digest='sha256:REPLACE_WITH_RELEASE_DIGEST'

helm template "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --set-string image.digest="$digest" > /tmp/plinth-rendered.yaml
```

For installation, create the namespace and database Secret out of band. Put
only non-secret chart settings in a reviewed `operator-values.yaml`, including
the actual storage class and complete database peer or IP-block boundary. The
Secret host must be reachable through that exact NetworkPolicy rule; the
default same-namespace PostgreSQL selector is not an external-DNS example.

```bash
helm upgrade --install "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --values operator-values.yaml \
  --set-string image.digest="$digest" \
  --wait --timeout 5m
kubectl --namespace "$namespace" rollout status statefulset/plinth \
  --timeout 5m
```

An existing Secret can use different key names through
`database.keys.{host,port,user,password,database}`. Existing claims can be
selected independently with `persistence.data.existingClaim` and
`persistence.logs.existingClaim`; otherwise the chart creates retained claims.
`networkPolicy.database.port` is the non-secret numeric egress port and must
equal the value stored under the Secret key selected by `database.keys.port`.
Replace the Traefik, DNS, and database `peers` arrays as complete units when
cluster labels differ; Helm replaces arrays instead of recursively retaining
default selector keys. Set `networkPolicy.database.peers=[]` only when an
explicit `ipBlocks` list supplies the complete database boundary.
If a storage class cannot apply `fsGroup`, prepare ownership through a reviewed
cluster-specific mechanism rather than granting root to the application.

To enable the public route, first create a TLS Secret for the exact DNS name and
select the actual Traefik and PostgreSQL Pods. Then render and inspect the
enabled route before upgrading. Export the release's current operator values so
the render includes existing-claim names, Secret-key mappings, selectors, and
other reviewed overrides:

```bash
host='plinth.example'
tls_secret='plinth-example-tls'
helm get values "$release" --namespace "$namespace" --output yaml \
  > /tmp/plinth-current-values.yaml

helm template "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --values /tmp/plinth-current-values.yaml \
  --set-string image.digest="$digest" \
  --set traefik.enabled=true \
  --set-string public.host="$host" \
  --set-string traefik.tls.secretName="$tls_secret" \
  > /tmp/plinth-traefik-rendered.yaml
helm upgrade "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --reuse-values \
  --set-string image.digest="$digest" \
  --set traefik.enabled=true \
  --set-string public.host="$host" \
  --set-string traefik.tls.secretName="$tls_secret" \
  --wait --timeout 5m
```

The default peer arrays select a Traefik Pod labeled
`app.kubernetes.io/name=traefik` in a
namespace labeled `kubernetes.io/metadata.name=traefik`, cluster DNS labeled
`k8s-app=kube-dns` in `kube-system`, and PostgreSQL labeled
`app.kubernetes.io/name=postgresql` in the release namespace. Replace those
peer arrays when the cluster differs; never broaden them merely to make a
failed connection disappear.

After installation, require all of the following before exposing DNS:

1. the Pod reaches Ready without restart loops;
2. the running image ID resolves to the approved release digest;
3. `/healthz` returns the expected JSON through the ClusterIP Service;
4. the bundled shell is installed on the data claim;
5. the Service has no external address and only the intended NetworkPolicies
   select the Pod;
6. the public TLS route preserves `Host`, rejects a wrong Origin, and upgrades
   `/ws/events`; and
7. a SIGTERM restart exits the old container with status zero inside the
   60-second grace period and the replacement mounts the same package state.

Create the first administrator only through a trusted path. With an empty user
table, Plinth permits that first account even while general registration is
disabled; subsequent registration remains disabled. Port-forwarding the
ClusterIP Service from an administrator workstation avoids temporarily exposing
registration to the Internet. The chart schema refuses a Traefik-enabled render
when `registration.enabled=true`.

```bash
kubectl --namespace "$namespace" port-forward service/plinth 8080:8080
```

## Upgrade and rollback

Before an upgrade, back up PostgreSQL and both persistent claims, verify the new
release digest and attestations, and read the release notes for storage or
schema changes. Upgrade only the digest value and wait for the sequential
replacement to finish. Verify the exact running digest, health response, TLS
authority checks, WebSocket connection, installed package state, and old Pod's
clean exit.

Changing a tag without changing the digest is not an upgrade. Rolling back the
container digest does not roll back PostgreSQL or persistent package state;
perform a rollback only when the release explicitly declares that state
backward-compatible, otherwise restore the matched database and volume backup.
Never run two versions concurrently against one database or claim.

Apply an upgrade by changing only the exact digest and retaining all other
reviewed values. `--reuse-values` avoids accidentally resetting selectors or
existing-claim names, but inspect `helm get values` first because it also
retains every historical operator override:

```bash
new_digest='sha256:REPLACE_WITH_NEW_RELEASE_DIGEST'
helm get values "$release" --namespace "$namespace"
helm upgrade "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --reuse-values \
  --set-string image.digest="$new_digest" \
  --wait --timeout 5m
kubectl --namespace "$namespace" rollout status statefulset/plinth \
  --timeout 5m
```

## Removal

Disable the public route first and allow endpoint removal to propagate. A
StatefulSet deletion does not itself guarantee ordered Pod termination, so
scale the workload to zero, wait for the stable Pod to disappear, and verify
its container exited zero inside the shutdown bound before uninstalling the
release. Confirm that namespaced
Service/Traefik/policy/config objects are absent, and no task-owned resource
remains. Retained claims and the operator-managed Secret, TLS Secret, database,
and backups are intentionally outside normal chart deletion. Inventory them by
exact name and either retain them for recovery or remove them explicitly. Never
use a namespace-wide recursive deletion as a substitute for that inventory in a
shared cluster.

```bash
helm upgrade "$release" deploy/helm/plinth \
  --namespace "$namespace" \
  --reuse-values --set traefik.enabled=false \
  --wait --timeout 5m
kubectl --namespace "$namespace" scale statefulset/plinth --replicas=0
kubectl --namespace "$namespace" wait --for=delete pod/plinth-0 --timeout=65s
# Verify the exact terminated container's exit code through the cluster
# runtime or approved observability path before continuing.
helm uninstall "$release" --namespace "$namespace" --wait --timeout 5m
owned_resources='all,configmap,networkpolicy,serviceaccount'
owned_resources+=',ingressroute.traefik.io,middleware.traefik.io'
owned_resources+=',serverstransport.traefik.io'
kubectl --namespace "$namespace" get "$owned_resources" \
  --selector app.kubernetes.io/instance="$release"
kubectl --namespace "$namespace" get persistentvolumeclaim \
  --selector app.kubernetes.io/instance="$release"
```

The final PVC command is expected to show retained chart claims. The
operator-managed database and TLS Secrets are also expected to remain. Delete
each only by exact name after the retention decision; `helm uninstall` does not
authorize data destruction.

For a disposable validation namespace, test the full sequence: install,
health and TLS/Host/origin checks, a SIGTERM restart, a controlled sequential
replacement with retained volume and database state, Helm uninstall, and
exact-name absence of every task-owned resource. The repository's deployment
integration harness is the canonical executable version of this procedure. It
forces the replacement with a rollout nonce rather than building a second
candidate digest.

```bash
.github/scripts/install-deployment-tools.sh contract
export PATH="${PLINTH_DEPLOYMENT_TOOLS_DIR:-/tmp/plinth-deployment-tools/bin}:$PATH"
python3 tests/deployment/helm_contract_test.py
.github/scripts/install-deployment-tools.sh all
image='plinth-runtime:exact-candidate'
python3 tests/deployment/k3d_lifecycle_test.py --image "$image" --kubernetes min
python3 tests/deployment/k3d_lifecycle_test.py --image "$image" --kubernetes max
```
