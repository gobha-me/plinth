# Plinth Helm chart

This chart deploys one Plinth kernel behind an isolated application `ClusterIP`
Service. A second headless Service governs the StatefulSet's stable Pod
identity; neither Service is externally exposed. Traefik exposure is optional
and disabled by default. The chart does not
install PostgreSQL, Traefik, certificates, namespaces, or secret-management
software.

## Requirements

- Kubernetes 1.35 or newer with an enforcing NetworkPolicy implementation.
  The live contract covers the minimum K3s 1.35 line and current K3s 1.37.
- A supported Plinth runtime image (v0.6.6 or newer), selected by the exact
  multi-platform `sha256` digest recorded by its release workflow.
- PostgreSQL reachable through the configured NetworkPolicy.
- A pre-existing database Secret in the release namespace.
- A default StorageClass or two pre-existing PersistentVolumeClaims.
- For public exposure, Traefik 3.7.13 with the `traefik.io/v1alpha1` CRDs and
  a pre-existing TLS Secret in the release namespace. Other Traefik releases
  require separate operator validation and are not currently supported.

The image reference is always rendered as `repository@sha256:digest`. There is
no tag or mutable-alias fallback. `image.digest` intentionally has no default;
an install without an operator-selected release digest fails validation.
The chart is shipped in the repository source. Use `deploy/helm/plinth` from
the exact release commit recorded in that verified image's
`org.opencontainers.image.revision` label; do not mix a release image with an
ambient branch checkout.

## Database Secret

`database.existingSecret` must contain five keys. Their default names are
`host`, `port`, `user`, `password`, and `database`; change the key mappings in
`database.keys` when an external secret manager uses different names. The
chart only emits `secretKeyRef` entries and never copies credential values into
a ConfigMap, annotation, or rendered manifest.

The non-secret `networkPolicy.database.port` must equal the numeric port stored
under `database.keys.port`. By default egress is limited to TCP 5432 and pods
with `app.kubernetes.io/name=postgresql` in the release namespace. Set database
namespace labels or explicit `ipBlocks` for a different topology. Standard
NetworkPolicy cannot select a Service by name or an external database by DNS
name.

## Install without public exposure

Create the namespace, database Secret, and any externally managed claims first.
Then install using the digest copied from the release workflow:

```console
helm upgrade --install plinth deploy/helm/plinth \
  --namespace plinth \
  --set-string image.digest=sha256:REPLACE_WITH_RELEASE_DIGEST
```

The Service remains `ClusterIP`, and the default NetworkPolicy admits no pod
ingress while Traefik is disabled. `kubectl port-forward` remains available for
local bootstrap because node-to-pod traffic is outside the portable isolation
guarantees of Kubernetes NetworkPolicy.

## Traefik TLS exposure

Set all three values together:

```console
helm upgrade --install plinth deploy/helm/plinth \
  --namespace plinth \
  --set-string image.digest=sha256:REPLACE_WITH_RELEASE_DIGEST \
  --set traefik.enabled=true \
  --set public.host=plinth.example \
  --set traefik.tls.secretName=plinth-tls
```

The chart derives the exact browser origin as `https://public.host`, adding
`:public.port` only when the port is not 443. It configures every Traefik route
with `passHostHeader: true`; upstream or controller-wide middleware must not
rewrite `Host`. Plinth does not trust `Forwarded` or `X-Forwarded-*` headers for
browser authority.

Four route groups keep their limits independent:

- `/ws/events` has no buffering or in-flight middleware, so an upgraded socket
  cannot consume an ordinary-request concurrency slot.
- `POST /api/packages` receives the 64 MiB package-upload bound, a dedicated
  two-request concurrency limit applied before buffering, and a separate
  15-minute response-header budget for long migrations. Two accepted 51 MiB
  multipart bodies fit within the 128 MiB upload scratch volume.
- Exact `POST /api/auth/login` and `POST /api/auth/register` routes receive a
  dedicated per-source Traefik token bucket before the ordinary in-flight and
  body limits. It defaults to five requests per 60 seconds with a burst of
  five; tune the bounded `traefik.limits.authRate*` values only after reviewing
  the application-level source, subject, and global limits. The chart raises
  the kernel source ceiling to 1000 because the kernel sees the shared Traefik
  Pod; the edge bucket remains the per-external-source control.
- Other HTTP requests receive the smaller 1 MiB bound plus the in-flight limit.

Ordinary HTTP and WebSocket handshakes use the shorter backend transport;
package installation uses its separately bounded transport. All routes use the
same TLS certificate. The chart creates no plaintext Traefik route; configure
any HTTP-to-HTTPS redirect at the controller boundary.

## First administrator

For first bootstrap, keep `registration.mode=disabled`, keep Traefik disabled,
set `registration.bootstrapSecret.name` to an existing Secret, and use a local
port-forward. The selected Secret key is projected only into
`PLINTH_BOOTSTRAP_TOKEN`; it is not copied into the ConfigMap or Helm release
values. Remove the Secret reference after the first administrator exists.
The chart rejects any nonempty bootstrap Secret name while Traefik exposure is
enabled, so bootstrap authority cannot be published accidentally.

Upgrades using `--reuse-values` accept the prior chart's
`registration.enabled=false` shape and resolve every missing registration
field to the current closed, bounded defaults. The legacy value `true` remains
invalid; select `registration.mode=invite` or `open` explicitly after bootstrap.

## Storage, replacement, and removal

The chart mounts separate data and log claims. Package staging remains under
`/var/lib/plinth/data/staging` on the data claim so atomic installation renames
never cross filesystems. Drogon's multipart receive buffer uses a separate,
bounded ephemeral volume at `/var/lib/plinth/uploads`; it is not package state
and is discarded with the Pod. Generated claims carry `helm.sh/resource-policy: keep`
by default. Set the relevant `retain` value to false before installation only
when Helm uninstall should delete that claim.

Plinth is deliberately fixed at one replica. An `OrderedReady` StatefulSet
binds its stable ordinal to the chart's headless governing Service and uses
rolling replacement so the prior kernel terminates
before its successor starts against the same claims. This causes a bounded
interruption and existing WebSocket clients reconnect. Never force-delete the
Pod: bypassing StatefulSet identity safety can create two live kernels. The
60-second Pod termination grace exceeds Plinth's 50-second shutdown watchdog.

An externally managed Secret change does not alter the rendered StatefulSet.
After rotating it, change `rolloutNonce` to force the controlled replacement.
On root-squashed or unusual storage, provision the claims so UID/GID 10001 can
write both mount points; `fsGroup` cannot repair every storage backend.

For removal, first disable Traefik exposure and wait for the public route to
close. Scale the StatefulSet to zero, wait for the Pod to disappear, and verify
that the exact container exited cleanly within the shutdown bound before
running `helm uninstall`. Follow the complete inventory and data-retention
procedure in [Kubernetes removal](../../../docs/KUBERNETES.md#removal).

## NetworkPolicy portability

The default policy permits only selected Traefik ingress, selected DNS egress,
and selected PostgreSQL egress. Each `peers` array replaces atomically during
Helm values merging, so custom labels do not retain hidden default keys.
Namespace and pod selectors inside one peer are combined, not alternatives.
Replace the peer arrays for the installed controller, DNS provider, and
database. A NetworkPolicy object provides no enforcement when the cluster's
network plugin does not implement it, and host-network traffic may be observed
as node traffic by some plugins.

## Render checks

For a non-installing syntax check, supply a syntactically valid test digest:

```console
digest="sha256:0000000000000000000000000000000000000000000000000000000000000000"
helm lint --strict deploy/helm/plinth --set-string image.digest="$digest"
helm template plinth deploy/helm/plinth --set-string image.digest="$digest"
```

Use the real release digest for every deployment.
