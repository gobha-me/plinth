#!/usr/bin/env python3
"""Check public-release provenance and generate Plinth's CycloneDX SBOM."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import uuid


ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = ROOT / "third_party" / "dependencies.json"
SBOM = ROOT / "sbom.cdx.json"


def workflow_job(text: str, name: str) -> str:
    """Return one top-level Actions job without requiring a YAML dependency."""
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\s*$.*?(?=^  [A-Za-z0-9_-]+:\s*$|\Z)",
        text,
    )
    return match.group(0) if match else ""


def check_runtime_image_workflow() -> list[str]:
    errors: list[str] = []
    path = ROOT / ".github" / "workflows" / "runtime-image.yml"
    if not path.is_file():
        return [".github/workflows/runtime-image.yml is missing"]

    text = path.read_text(encoding="utf-8")
    if re.search(r"^\s*pull_request_target:\s*$", text, re.MULTILINE):
        errors.append("runtime image workflow must not use pull_request_target")
    if re.search(r"ghcr\.io/gobha-me/plinth:(?:latest|v?\d+|v?\d+\.\d+)\b", text):
        errors.append("runtime image workflow contains a mutable image alias")

    workflow_permissions = text.partition("\njobs:")[0]
    if "permissions:\n  contents: read" not in workflow_permissions:
        errors.append("runtime image workflow must default to contents: read")
    if re.search(r"^\s+contents:\s*write\s*$", text, re.MULTILINE):
        errors.append("runtime image workflow must never grant contents: write")

    candidate = workflow_job(text, "candidate")
    preflight = workflow_job(text, "release_preflight")
    publish = workflow_job(text, "publish")
    for name, block in (
        ("candidate", candidate),
        ("release_preflight", preflight),
        ("publish", publish),
    ):
        if not block:
            errors.append(f"runtime image workflow is missing the {name} job")

    write_permission = re.compile(
        r"^\s+(?:packages|attestations|id-token):\s*write\s*$", re.MULTILINE
    )
    if write_permission.search(workflow_permissions):
        errors.append("runtime image workflow-level permissions must be read-only")
    for name, block in (("candidate", candidate), ("release_preflight", preflight)):
        if write_permission.search(block):
            errors.append(f"runtime image {name} job has publication permissions")
    if "actions: read" not in preflight:
        errors.append("runtime image release preflight is missing actions: read")
    if "actions: read" in candidate or "actions: read" in publish:
        errors.append("actions: read must be limited to runtime image release preflight")
    for permission in ("packages: write", "attestations: write", "id-token: write"):
        if permission not in publish:
            errors.append(f"runtime image publish job is missing {permission}")
    if "environment: release" not in publish:
        errors.append("runtime image publish job is missing the release environment")
    if "vars.PLINTH_RELEASE_APPROVAL_GATE" not in preflight:
        errors.append("runtime image preflight is missing the approval configuration gate")
    if publish.count("create-storage-record: false") != 2:
        errors.append("runtime image attestations must disable storage records")

    required_candidate = (
        "--target server-tests",
        "--target runtime",
        ".github/scripts/check-runtime-image.sh",
        "tests/browser/run-production.py --image",
        "tests/browser/run-bundled-upgrade.py --image",
    )
    for marker in required_candidate:
        if marker not in candidate:
            errors.append(f"runtime image candidate job is missing {marker}")
    if "push: true" in candidate or "docker/login-action" in candidate:
        errors.append("runtime image candidate job must build without registry access")

    required_release = (
        "PLINTH_RELEASE=1",
        "push-by-digest=true",
        "actions/attest@",
        "gh attestation verify",
        "git merge-base --is-ancestor",
        "actions/workflows/$workflow/runs",
        "plinth-anonymous-docker",
        "v0.6.6 is the first release eligible",
        "tonistiigi/binfmt:qemu-v9.2.2@sha256:",
        "moby/buildkit:v0.33.0@sha256:",
        "version: v0.37.1",
        "PLINTH_EXPECTED_ARCH=arm64",
        "PLINTH_EXPECTED_ARCH=amd64",
    )
    release_contract = preflight + publish
    for marker in required_release:
        if marker not in release_contract:
            errors.append(f"runtime image release contract is missing {marker}")
    return errors


def check_kubernetes_deployment() -> list[str]:
    """Require the supported chart, operator contract, tests, and pinned CI."""
    errors: list[str] = []
    chart = ROOT / "deploy" / "helm" / "plinth"
    required_files = (
        chart / "Chart.yaml",
        chart / "README.md",
        chart / "values.yaml",
        chart / "values.schema.json",
        chart / "templates" / "NOTES.txt",
        chart / "templates" / "_helpers.tpl",
        chart / "templates" / "configmap.yaml",
        chart / "templates" / "statefulset.yaml",
        chart / "templates" / "networkpolicy.yaml",
        chart / "templates" / "persistentvolumeclaims.yaml",
        chart / "templates" / "service.yaml",
        chart / "templates" / "service-headless.yaml",
        chart / "templates" / "serviceaccount.yaml",
        chart / "templates" / "traefik-ingressroute.yaml",
        chart / "templates" / "traefik-middlewares.yaml",
        chart / "templates" / "traefik-serverstransport.yaml",
        ROOT / "docs" / "KUBERNETES.md",
        ROOT / "tests" / "deployment" / "helm_contract_test.py",
        ROOT / "tests" / "deployment" / "k3d_lifecycle_test.py",
        ROOT / ".github" / "scripts" / "install-deployment-tools.sh",
    )
    for path in required_files:
        if not path.is_file():
            errors.append(
                f"supported deployment file is missing: {path.relative_to(ROOT)}"
            )
    if errors:
        return errors

    values = (chart / "values.yaml").read_text(encoding="utf-8")
    schema_text = (chart / "values.schema.json").read_text(encoding="utf-8")
    try:
        schema = json.loads(schema_text)
    except json.JSONDecodeError as error:
        return [f"deploy/helm/plinth/values.schema.json is invalid: {error}"]
    if schema.get("additionalProperties") is not False:
        errors.append("Helm values schema must reject unknown top-level values")
    for marker in (
        '"pattern": "^sha256:[0-9a-f]{64}$"',
        '"existingSecret"',
        '"registration"',
        '"networkPolicy"',
        '"databasePeer"',
        '"rolloutNonce"',
        '"maxRequestBodyBytes"',
        '"authRateAverage"',
        '"authRatePeriodSeconds"',
        '"responseHeaderTimeout"',
    ):
        if marker not in schema_text:
            errors.append(f"Helm values schema is missing {marker}")
    for marker in (
        "digest: \"\"",
        "existingSecret:",
        "poolSize: 32",
        "database:\n    port: 5432",
        "retain: true",
        "enabled: false",
        "defaultRequestBodyBytes: 1048576",
        "maxRequestBodyBytes: 67108864",
        "authRateAverage: 5",
        "authRateBurst: 5",
        "authRatePeriodSeconds: 60",
        "sourceAttempts: 1000",
        "responseHeaderTimeout: 60s",
        "packageResponseHeaderTimeout: 15m",
        "idleConnTimeout: 90s",
    ):
        if marker not in values:
            errors.append(f"Helm secure defaults are missing {marker}")
    if re.search(r"(?m)^\s*tag:\s*", values):
        errors.append("Helm values must not expose an image tag fallback")

    templates = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((chart / "templates").glob("*"))
        if path.is_file()
    )
    required_template_markers = (
        'printf "%s@%s" $repository $digest',
        "replicas: 1",
        "kind: StatefulSet",
        "podManagementPolicy: OrderedReady",
        "updateStrategy:",
        "terminationGracePeriodSeconds: 60",
        "runAsUser: 10001",
        "runAsGroup: 10001",
        "readOnlyRootFilesystem: true",
        "allowPrivilegeEscalation: false",
        "automountServiceAccountToken: false",
        "path: /healthz",
        "type: ClusterIP",
        "clusterIP: None",
        "policyTypes:",
        "helm.sh/resource-policy: keep",
        "/var/lib/plinth/data",
        "/var/lib/plinth/logs",
        "/var/lib/plinth/uploads",
        "PLINTH_PG_PASSWORD",
        "PLINTH_DEV_MODE",
        "PLINTH_REGISTRATION_MODE",
        "PLINTH_BOOTSTRAP_TOKEN",
        "browser_origin",
        "passHostHeader: true",
        "Path(`/api/auth/login`)",
        "Path(`/api/auth/register`)",
        "rateLimit:",
        "port: {{ .Values.networkPolicy.database.port }}",
    )
    for marker in required_template_markers:
        if marker not in templates:
            errors.append(f"Helm deployment contract is missing {marker}")
    if re.search(
        r"(?m)^\s*(?:type:\s*(?:NodePort|LoadBalancer)|"
        r"hostNetwork:\s*true|hostPort:)",
        templates,
    ):
        errors.append(
            "Helm chart must not expose a node, load balancer, or host network"
        )

    deployment_doc = (ROOT / "docs" / "KUBERNETES.md").read_text(
        encoding="utf-8"
    )
    for marker in (
        "ghcr.io/gobha-me/plinth@sha256:",
        "Traefik 3.7.13",
        'org.opencontainers.image.revision',
        'git clone --depth 1 --branch',
        'test "$(git rev-parse HEAD)" = "$revision"',
        'test "$(tr -d \'\\r\\n\' < VERSION)" = "$version"',
        "headless governing Service",
        "scale statefulset/plinth --replicas=0",
        "wait --for=delete pod/plinth-0",
        "passHostHeader=true",
        "only a TLS route",
        "Forwarded` and `X-Forwarded-*` never establish",
        "dedicated Traefik token bucket",
        "authRatePeriodSeconds",
        "registration.enabled=false",
        "must remain a PostgreSQL",
        "no PostgreSQL TLS mode, root CA",
        "unsupported",
        "networkPolicy.database.port",
        "one replica",
        "OrderedReady",
        "force-delete",
        "GET /healthz",
        "300-second failure budget",
        "50-second",
        "absolute process watchdog",
        "grant at least 60 seconds before",
        "tests/deployment/helm_contract_test.py",
        "tests/deployment/k3d_lifecycle_test.py",
        "helm uninstall",
        "serverstransport.traefik.io",
    ):
        if marker not in deployment_doc:
            errors.append(
                f"docs/KUBERNETES.md is missing deployment contract: {marker}"
            )

    contract_command = "python3 tests/deployment/helm_contract_test.py"
    live_commands = (
        'python3 tests/deployment/k3d_lifecycle_test.py --image "$image" --kubernetes min',
        'python3 tests/deployment/k3d_lifecycle_test.py --image "$image" --kubernetes max',
    )
    ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    runtime_ci = (ROOT / ".github" / "workflows" / "runtime-image.yml").read_text(
        encoding="utf-8"
    )
    quality_job = workflow_job(ci, "quality")
    candidate_job = workflow_job(runtime_ci, "candidate")
    if re.search(
        rf"(?m)^\s*{re.escape(contract_command)}\s*$", quality_job
    ) is None:
        errors.append("CI is missing the non-cluster Helm contract test invocation")
    for live_command in live_commands:
        if re.search(
            rf"(?m)^\s*{re.escape(live_command)}\s*$", candidate_job
        ) is None:
            errors.append(
                "Runtime image CI is missing the live k3d lifecycle invocation: "
                + live_command
            )
    if re.search(
        r"(?m)^\s*\.github/scripts/install-deployment-tools\.sh contract\s*$",
        quality_job,
    ) is None:
        errors.append("CI is missing the pinned deployment-tool contract installer")
    if re.search(
        r"(?m)^\s*\.github/scripts/install-deployment-tools\.sh all\s*$",
        candidate_job,
    ) is None:
        errors.append("Runtime image CI is missing the pinned deployment-tool installer")
    for marker in (
        "matrix.arch == 'amd64'",
        'image="plinth-runtime:amd64-$GITHUB_RUN_ID"',
    ):
        if marker not in candidate_job:
            errors.append(f"Runtime image live deployment job is missing {marker}")

    contract_test = (
        ROOT / "tests" / "deployment" / "helm_contract_test.py"
    ).read_text(encoding="utf-8")
    if re.search(
        r'(?m)^SCHEMA_COMMIT = "[0-9a-f]{40}"$', contract_test
    ) is None:
        errors.append("non-cluster Helm validation schema is not commit-pinned")

    live_test = (
        ROOT / "tests" / "deployment" / "k3d_lifecycle_test.py"
    ).read_text(encoding="utf-8")
    for variable in (
        "K3S_MIN_IMAGE",
        "K3S_MAX_IMAGE",
        "REGISTRY_IMAGE",
        "POSTGRES_IMAGE",
        "CURL_IMAGE",
    ):
        assignment = re.search(
            rf'(?ms)^{variable} = \(\n(.*?)^\)$', live_test
        )
        if (
            assignment is None
            or re.search(r':[A-Za-z0-9._-]+@', assignment.group(1)) is None
            or re.search(r'sha256:[0-9a-f]{64}', assignment.group(1)) is None
        ):
            errors.append(f"live lifecycle {variable} is not tag-and-digest pinned")
    traefik_tag = re.search(
        r'(?m)^TRAEFIK_IMAGE_TAG = "[0-9]+\.[0-9]+\.[0-9]+"$', live_test
    )
    if traefik_tag is None:
        errors.append("live lifecycle Traefik tag is not version-pinned")
    else:
        expected_traefik = "Traefik " + traefik_tag.group(0).split('"')[1]
        if expected_traefik not in deployment_doc:
            errors.append(
                "deployment docs do not match the live Traefik version: "
                + expected_traefik
            )
    if re.search(
        r'(?ms)^TRAEFIK_IMAGE_DIGEST = \(\n.*?'
        r'sha256:[0-9a-f]{64}"\n\)$',
        live_test,
    ) is None:
        errors.append("live lifecycle Traefik image is not digest-pinned")

    installer_path = (
        ROOT / ".github" / "scripts" / "install-deployment-tools.sh"
    )
    installer = installer_path.read_text(encoding="utf-8")
    for variable in (
        "HELM_VERSION",
        "KUBECTL_VERSION",
        "K3D_VERSION",
        "KUBECONFORM_VERSION",
    ):
        if re.search(rf"(?m)^{variable}=v\d+\.\d+\.\d+$", installer) is None:
            errors.append(f"deployment tool installer does not pin {variable}")
    for architecture in ("helm", "kubectl", "k3d", "kubeconform"):
        assignments = re.findall(
            rf"(?m)^\s*{architecture}_sha256=([0-9a-f]{{64}})$", installer
        )
        if len(assignments) != 2:
            errors.append(
                f"deployment tool installer must pin two {architecture} SHA-256 values"
            )
    if "sha256sum --check --status" not in installer:
        errors.append("deployment tool installer must verify downloaded SHA-256 values")

    return errors


def load_dependencies() -> list[dict[str, object]]:
    document = json.loads(DEPENDENCIES.read_text(encoding="utf-8"))
    if document.get("schema") != 1 or not isinstance(document.get("components"), list):
        raise ValueError("third_party/dependencies.json has an unsupported schema")
    return document["components"]


def component_purl(component: dict[str, object]) -> str:
    source = str(component["source"])
    repository = source.removeprefix("https://github.com/")
    revision = str(component.get("commit", component["version"]))
    return f"pkg:github/{repository}@{revision}"


def make_sbom(components: list[dict[str, object]]) -> str:
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    canonical = json.dumps(components, sort_keys=True, separators=(",", ":"))
    serial = uuid.uuid5(uuid.NAMESPACE_URL, f"plinth:{version}:{canonical}")
    entries = []
    for component in components:
        entry: dict[str, object] = {
            "type": "library",
            "name": component["name"],
            "version": component["version"],
            "licenses": [{"license": {"id": component["license"]}}],
            "purl": component_purl(component),
            "externalReferences": [
                {"type": "vcs", "url": component["source"]}
            ],
            "properties": [
                {"name": "plinth:dependency-kind", "value": component["kind"]}
            ],
        }
        if "sha256" in component:
            entry["hashes"] = [{"alg": "SHA-256", "content": component["sha256"]}]
        entries.append(entry)
    document = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "plinth",
                "version": version,
                "licenses": [{"license": {"id": "MIT"}}],
            }
        },
        "components": entries,
    }
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def tracked_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [ROOT / item.decode() for item in result.stdout.split(b"\0") if item]


def check(components: list[dict[str, object]]) -> list[str]:
    errors: list[str] = []
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if re.fullmatch(r"\d+\.\d+\.\d+", version) is None:
        errors.append("VERSION is not MAJOR.MINOR.PATCH")

    dockerfile = (ROOT / "docker" / "Dockerfile").read_text(encoding="utf-8")
    docker_versions = re.findall(
        r"^ARG PLINTH_VERSION=([^\s#]+)", dockerfile, re.MULTILINE
    )
    if not docker_versions:
        errors.append("docker/Dockerfile must default PLINTH_VERSION")
    for docker_version in docker_versions:
        if docker_version != version:
            errors.append(
                "docker/Dockerfile PLINTH_VERSION default must match VERSION"
            )

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for component in components:
        license_path = ROOT / str(component.get("license_path", ""))
        if not license_path.is_file():
            errors.append(f"{component['name']}: license text is missing")
        kind = component.get("kind")
        if kind == "fetchcontent":
            commit = str(component.get("commit", ""))
            if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
                errors.append(f"{component['name']}: commit is not a full Git hash")
            elif f"GIT_TAG {commit}" not in cmake:
                errors.append(f"{component['name']}: CMake pin does not match inventory")
        elif kind == "vendored":
            path = ROOT / str(component.get("path", ""))
            if not path.is_file():
                errors.append(f"{component['name']}: vendored file is missing")
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != component.get("sha256"):
                errors.append(f"{component['name']}: vendored SHA-256 does not match")
        else:
            errors.append(f"{component['name']}: unknown dependency kind {kind!r}")

    manifest = json.loads(
        (ROOT / "client" / "shell" / "manifest.json").read_text(encoding="utf-8")
    )
    if manifest.get("version") != version:
        errors.append("client/shell/manifest.json version must match VERSION")
    if manifest.get("license") != "MIT":
        errors.append("client/shell/manifest.json must declare MIT")

    for path in tracked_files():
        if not path.is_file() or not (path.parts[-1].endswith((".cpp", ".hpp"))):
            continue
        if "SPDX-License-Identifier: Apache-2.0" in path.read_text(
            encoding="utf-8", errors="replace"
        ):
            errors.append(f"{path.relative_to(ROOT)} still declares Apache-2.0")

    for workflow in (ROOT / ".github" / "workflows").glob("*.yml"):
        workflow_text = workflow.read_text(encoding="utf-8")
        for action in re.findall(r"^\s*-?\s*uses:\s*([^\s#]+)", workflow_text, re.MULTILINE):
            if action.startswith("./"):
                continue
            revision = action.rsplit("@", maxsplit=1)[-1]
            if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
                errors.append(f"{workflow.relative_to(ROOT)}: action is not SHA-pinned: {action}")

    errors.extend(check_runtime_image_workflow())
    errors.extend(check_kubernetes_deployment())

    forbidden_text = (
        ".claude/plans/",
        "claude-design-handoff",
        "plinth.xcaliber",
        "/home/jeff/",
    )
    for path in tracked_files():
        if not path.is_file() or path.suffix.lower() not in {
            ".md",
            ".yml",
            ".yaml",
            ".json",
        }:
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        for token in forbidden_text:
            if token in content:
                errors.append(f"{path.relative_to(ROOT)} contains private path {token}")

    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", readme):
        if "://" not in target and not (ROOT / target).is_file():
            errors.append(f"README.md link target is missing: {target}")

    dockerignore_path = ROOT / ".dockerignore"
    dockerignore_rules = (
        [
            line.strip()
            for line in dockerignore_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        if dockerignore_path.is_file()
        else []
    )
    dockerignore = set(dockerignore_rules)
    for required_pattern in (
        ".git",
        ".env",
        ".env.*",
        "config.json",
        "config.yml",
        "config.yaml",
        "AGENTS.override.md",
        "*.pem",
        "*.key",
    ):
        if required_pattern not in dockerignore:
            errors.append(f".dockerignore must exclude {required_pattern}")

    # Docker resolves exceptions by the last matching rule. Require explicit
    # root and recursive forms for local control/credential paths, and keep
    # every one after the final allowlist exception so a broad `!tests/**` (or
    # a future equivalent) cannot silently re-include a nested secret.
    fail_closed_patterns = (
        ".git",
        "**/.git",
        "**/.git/**",
        ".env",
        "**/.env",
        ".env.*",
        "**/.env.*",
        "AGENTS.override.md",
        "**/AGENTS.override.md",
        "*.pem",
        "**/*.pem",
        "*.key",
        "**/*.key",
        "**/node_modules",
        "**/node_modules/**",
        "**/__pycache__",
        "**/__pycache__/**",
        "**/*.pyc",
    )
    last_allow = max(
        (index for index, rule in enumerate(dockerignore_rules)
         if rule.startswith("!")),
        default=-1,
    )
    for pattern in fail_closed_patterns:
        positions = [
            index for index, rule in enumerate(dockerignore_rules)
            if rule == pattern
        ]
        if not positions:
            errors.append(f".dockerignore must fail-closed exclude {pattern}")
        elif max(positions) <= last_allow:
            errors.append(
                f".dockerignore exclusion {pattern} must follow every allow rule"
            )

    approved_configs = {
        "!client/shell/config.json",
        *{
            "!" + str(path.relative_to(ROOT))
            for path in (ROOT / "tests" / "fixtures").rglob("config.json")
        },
    }
    configured_exceptions = {
        rule for rule in dockerignore_rules
        if rule.startswith("!") and rule.endswith(
            ("/config.json", "/config.yml", "/config.yaml")
        )
    }
    if configured_exceptions != approved_configs:
        errors.append(
            ".dockerignore config exceptions must exactly match versioned manifests"
        )
    for pattern in ("config.json", "config.yml", "config.yaml"):
        if pattern not in dockerignore:
            errors.append(f".dockerignore must fail-closed exclude {pattern}")
        recursive = "**/" + pattern
        if recursive not in dockerignore:
            errors.append(f".dockerignore must fail-closed exclude {recursive}")

    expected_sbom = make_sbom(components)
    if not SBOM.is_file() or SBOM.read_text(encoding="utf-8") != expected_sbom:
        errors.append("sbom.cdx.json is stale; run tools/public_readiness.py --write-sbom")
    for required in ("LICENSE", "THIRD_PARTY_NOTICES.md", ".gitleaks.toml"):
        if not (ROOT / required).is_file():
            errors.append(f"{required} is missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-sbom", action="store_true")
    args = parser.parse_args()
    components = load_dependencies()
    if args.write_sbom:
        SBOM.write_text(make_sbom(components), encoding="utf-8")
    errors = check(components)
    for error in errors:
        print(f"public-readiness: {error}", file=sys.stderr)
    if errors:
        return 1
    print(
        "public-readiness: dependency, license, workflow, deployment, and SBOM "
        "checks passed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
