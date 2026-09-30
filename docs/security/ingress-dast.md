# Isolated ingress DAST

This is source-candidate validation, not certification of a published release,
a dogfood deployment, or an existing service. The harness accepts an exact
locally built candidate image whose revision and version labels match the
current clean checkout. Before provisioning and after cleanup, it rejects
tracked/staged changes, untracked checkout inputs, changed Git identity, and
untracked chart files even when Git ignores them. This check does not reject
ignored dependency/build caches outside the chart. These boundary checks do not
lock the shared checkout against a concurrent writer: run from an isolated
worktree and never change it during a scan. It does not publish that image or
authorize scanning any external target. This document describes the gate; it
records no live scan or green result.

## Owned deployment and scanner

`tests/deployment/ingress_dast_test.py` creates a disposable k3d deployment
using the real chart, PostgreSQL, installed shell, and digest-pinned Traefik.
It generates fixture TLS for `plinth.test`, bootstraps the synthetic
`issue36-admin` account, removes bootstrap authority, and leaves public
registration disabled. No production credentials or data belong in this lane.
The existing deployment harness supplies the pinned min/max Kubernetes and
Traefik identities; there is no fallback to a public cluster or mutable ingress.

The reviewed scanner is the linux/amd64 image:

```text
ghcr.io/zaproxy/zaproxy:2.17.0@sha256:781a2bdaea47324e7bab583e2263f21d257b0aee61ed51521a5be45f5f5081ef
```

The running ZAP version must be `2.17.0`, with release add-ons `ascanrules`
`83.0.0` and `pscanrules` `76.0.0`. Automatic scanner updates are disabled.
Missing or different rules fail the gate rather than silently reducing coverage.
The scanner is an owned, bounded container with a loopback-only control API and
proxy. Browser targets are restricted to
`https://plinth.test:<owned-port>` through `http://127.0.0.1:<owned-proxy-port>`.
The harness has no arbitrary-target CLI option. Never retarget it to a deployed
service, reuse real cookies, or expose the scanner API.

## What must actually be observed

The browser runs real Chromium and the unchanged installed shell and canonical
SDK through HTTPS Traefik and ZAP. It does not replace authentication, HTTP
responses, WebSockets, modules, or hooks with mocks. It records the actual root
navigation status, then exercises sign-in, Home, native session/registration/
catalog requests, missing-CSRF rejection, and an authorized SDK capability
request. Cookie checks require a Secure, HttpOnly, Strict session cookie and a
Secure, readable, Strict CSRF cookie. Native theme and scale preference writes
must succeed and visibly apply without CSP violations. Real connected and subscription-grant
frames are required; ordinary keyboard sign-out must close the original socket
and make the session endpoint return 401.

HTTP controls exercise anonymous access, disabled registration, wrong Host,
native fixture certificate validation, cross-origin and missing-CSRF rejection,
authorized capability access, shipped executable-script CSP, and bounded
malformed/traversal requests. Separate controls exercise the chart's body-size
limit, authentication burst limit, and package concurrent-admission limit.
Normal responses and HTTPS edge denials must retain the intended MIME types,
`nosniff`, bounded HSTS, and no Server banner. The bundled shell explicitly
restricts resource, framing, base and form origins while preserving its
documented inline-style support. Custom frontend policies are unchanged.
The actual bodyless WebSocket 101 must retain kernel `nosniff` and omit its
Server banner, alongside the native protocol/authentication witnesses below.
Pinned Traefik's raw hijack bypasses its ordinary response-header modifiers:
the gate does not claim HSTS on that 101 or impose a document CSP/MIME policy
on it. Normal HTTP upgrade denials still receive the chart's full TLS policy.
Traefik JSON access logs must independently show the exact owned WebSocket
router's completion and edge-generated authentication/package 429 responses,
not unrelated routes or backend substitutes. For the pinned fast HTTP proxy,
hijacking writes the handshake directly and leaves both logged capture statuses
unset (zero). Zero is not reclassified as 101 or accepted for another route.
The gate separately requires exactly one actual proxied 101 with valid native
origin, upgrade headers and key/acceptance matching, plus the browser's native
authenticated subscription grant and closure proof.
The two pinned bundled chart versions use separately tested logging schemas.
The new ready ingress pod and its Deployment must both contain the exact
reviewed access-log arguments. Only downstream status, origin status and router
name are retained as request-derived fields, alongside the pinned formatter's
fixed timestamp, empty message and info level. Paths, headers and query values
are dropped. A successful backend login must retain its explicit origin status
before missing origin status may identify a Traefik-generated rejection, as the
pinned engine omits that field for locally handled responses.

Scanner route coverage is derived from actual retained request messages, with
matching HTTPS authority and Host, not from planned control names. Required
labels include ROOT, APP, SESSION, LOGIN, LOGOUT, REGISTER, REGISTRATION, CAP,
WS, PACKAGES, and HEALTH. The browser's own receipt also requires both controls
complete, positive observed route counts, zero error/CSP/unscoped-traffic
counters, and confirmed browser/context cleanup. The harness establishes a
fresh HTTP session after browser logout before authenticated scanning.

The active scan is deliberately narrow: non-recursive GET query probes on
`/app/`, `/api/auth/session`, and `/api/frontend/applications`, each with the
synthetic `issue40_probe` query parameter. Only rules `40012` (reflected XSS)
and `40018` (SQL injection) are enabled, at MEDIUM attack strength and MEDIUM
alert threshold. Query parameters are the injection surface; body and general
header scanning are disabled. These handlers ignore the probe parameter.
Running the SQL-injection rule here is not exhaustive SQL/database coverage,
nor proof that the scanner reaches every application, capability, RBAC, or
mutation boundary. Other routes receive the actual journey/control traffic
and passive analysis, not this active scan.

The pinned XSS rule sends probes to JSON-response routes, but at the unchanged
MEDIUM alert threshold it may filter non-HTML results classified LOW. Probe
traffic is not certification of exploitable JSON XSS, and an absent alert is
not proof that such behavior is impossible. The threshold is not lowered and
routes or rules are not automatically skipped to obtain a passing result.

Each active scan must finish, both enabled rules must complete positive request
counts, and retained attack messages must show both rule IDs, the exact GET
endpoint, and actual responses. For each endpoint, each rule must actually
change the decoded value of the sole `issue40_probe` parameter. Baseline
requests are retained, but a rule tag on an unchanged baseline cannot count as
attack evidence. A stopped scan, skipped rule, absent traffic,
truncated inventory, nonempty passive queue, browser/control error, or unproven
cleanup is red. A scan progress value of 100 alone is insufficient.

## Running the source-candidate gate

Use a Linux amd64 host with Docker and the pinned deployment tools, Python 3
with PyYAML, Node, OpenSSL, curl, and the pinned Playwright Chromium dependency.
Install tools as in [CONTRIBUTING](../../CONTRIBUTING.md) and
[the browser instructions](../../tests/browser/README.md). The exact candidate
runtime image must already be available locally; its OCI revision/version
labels must match `HEAD` and `VERSION`. Prepare the pinned scanner image locally
before running the harness. Do not substitute a published tag for this identity
check.

```bash
.github/scripts/install-deployment-tools.sh all
export PATH="${PLINTH_DEPLOYMENT_TOOLS_DIR:-/tmp/plinth-deployment-tools/bin}:$PATH"
npm ci --prefix tests/browser
(cd tests/browser && npx playwright install --with-deps chromium)
docker pull ghcr.io/zaproxy/zaproxy:2.17.0@sha256:781a2bdaea47324e7bab583e2263f21d257b0aee61ed51521a5be45f5f5081ef
umask 077
dast_evidence_dir="$(mktemp -d /tmp/plinth-issue40-evidence.XXXXXX)"
image='plinth-runtime:exact-candidate'
python3 tests/deployment/ingress_dast_test.py \
  --image "$image" --kubernetes max \
  --report "$dast_evidence_dir/summary.json" \
  --private-report "$dast_evidence_dir/private.json"
```

`--kubernetes min` selects the other existing pinned deployment lane; one lane
does not prove the other ran. Sandboxing remains enabled by default. Only for
an owned local container that cannot run it, explicitly set
`PLINTH_BROWSER_NO_SANDBOX=1`; this is not a production recommendation.

Both report destinations must be new files in an existing, non-symlink parent
outside the checkout, under `/tmp/plinth-issue40*/`. The harness refuses
overwrites and writes private-permission files. Preserve the generated reports
in your own durable private evidence storage before removing this directory;
never copy raw evidence into Git. Keep the exit status: zero requires the
sanitized report's `COMPLETE` status, all required controls, completed scanner,
zero pending passive records, exact reviewed alert dispositions, and verified
absence of task-owned deployment/scanner resources, networks, volumes, and
temporary files. Cleanup failure remains a failure even if scanning finished.

## Evidence and exact finding triage

`--private-report` retains raw scanner messages/alerts, browser diagnostics,
access logs, errors, and scan progress. Even fixture evidence can contain fake
session tokens, request details, or suspected vulnerability information. Keep
it private and outside Git; never commit it, paste it into a public issue, or
upload it as a CI artifact. The fixed-schema `--report` omits raw URLs,
parameters, headers, bodies, and free-text finding descriptions. Review even
that summary before sharing it; raw evidence is never an attachment to it.

The candidate CI lane runs DAST only on amd64, within the existing 90-minute
job budget; lightweight report-policy and driver unit checks run on both
architectures. It uses the same pinned scanner and an owned
`/tmp/plinth-issue40-ci.*` report directory. Only a successful `COMPLETE`
sanitized summary may be uploaded. Failed/incomplete summaries and all raw
reports are never uploaded; an always-run cleanup step removes its exact
raw/report files, including after failure, and a cleanup failure remains red.
CI raw evidence is therefore not durably retained. An unexpected CI finding
requires reproduction on the exact
candidate locally and private triage before merge; the outside-Git local
private evidence is authoritative for that investigation. This lane grants no
new registry-write, identity-token, or repository-secret permissions.

Every observed alert starts unresolved, regardless of severity. There is no
blanket rule ignore, severity cutoff, or suppression for findings that were not
observed. The version-controlled review producer joins each alert to its unique
actual scanner message and checks every member of each present group against a
finite, source-reviewed predicate. It verifies actual method, authority, route,
status, headers and body against independently captured fixture ownership and
the current candidate's reviewed public bytes. Missing, ambiguous, changed or
unknown evidence remains unresolved. Investigate the exact
group privately, and report suspected vulnerabilities through
[SECURITY.md](../../SECURITY.md), not public issues.

The default run produces matching-only dispositions with these predicates;
it never changes the scanner's original severity or confidence. Accepted
categories distinguish `FALSE_POSITIVE`, `INFORMATIONAL`, `NOT_APPLICABLE` and
`DOCUMENTED_POLICY`: discovery observations are not mislabeled false positives,
and the narrowly verified inline-style policy is not a claim that inline styles
are forbidden. Code-reviewed cookie-deletion, cache, own-session, harmless
public-comment and bodyless-upgrade predicates are not blanket rule exemptions.
New or drifting observations require private investigation and a reviewed
policy change before they can pass unattended CI.

An independently checked private JSON list may be supplied using
`--dispositions /absolute/private/reviewed-dispositions.json`. It must exactly
agree with the records proven by the current run; it cannot override unknown
evidence or authorize a new exception. Each record must
exactly identify `pluginId`, `alertRef`, `risk`, `confidence`, `routeLabel`, and
`param`, with `dispositionId` and `reviewId`. The matching-only `alertRef` must
be a bounded canonical rule/variant string whose rule prefix matches `pluginId`.
A bare rule reference matches only that exact reference, never every variant.
Missing, legacy, malformed, or wildcard references fail closed. Variants are
qualified separately even when their sanitized public group fields coincide;
the reference itself is never emitted in the public report. The finite policy in
`tests/deployment/dast_report.py` binds each accepted review ID to its applicable
disposition, rule, variant, severity and route. Identifiers are not conclusions
on their own and a review cannot match a broader scanner variant. A real finding uses
`PRIVATE_REMEDIATION_REQUIRED` with `PRIVATE_FIX_REQUIRED` and remains red.
Unknown, duplicate, absent-group, malformed, or unresolved dispositions remain
red. Changing this finite policy requires code review, not a broader ignore file.

Do not erase or relabel the original failed run after review. A reviewed rerun
uses fresh report destinations, preserves the original private evidence, and
must still satisfy all traffic, rule, control, and cleanup checks. A passing
source-candidate result does not replace required candidate/exact-merge CI or
the separate maintainer-owned release and dogfood gates.
