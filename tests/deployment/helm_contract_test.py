#!/usr/bin/env python3
"""Render the supported Helm chart and enforce its fail-closed contract."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

try:
    import yaml
except ImportError as error:  # pragma: no cover - actionable CI preflight
    raise SystemExit("PyYAML is required for Helm contract validation") from error


ROOT = Path(__file__).resolve().parents[2]
CHART = ROOT / "deploy" / "helm" / "plinth"
RELEASE = "contract"
NAMESPACE = "plinth-contract"
DIGEST = "sha256:" + "a" * 64
SCHEMA_COMMIT = "1360e239a56dcf2e5c7f99e61ccbaca1ea07036a"
SCHEMA_URL = (
    "https://raw.githubusercontent.com/yannh/kubernetes-json-schema/"
    + SCHEMA_COMMIT
    + "/{{.NormalizedKubernetesVersion}}-standalone{{.StrictSuffix}}/"
      "{{.ResourceKind}}{{.KindSuffix}}.json"
)
KUBERNETES_VERSIONS = ("1.35.0", "1.36.0", "1.37.0")


class UniqueKeyLoader(yaml.SafeLoader):
    """SafeLoader that rejects duplicate keys instead of silently overriding."""


def construct_mapping(loader, node, deep=False):
    mapping = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in mapping:
            raise ValueError(f"duplicate YAML key: {key!r}")
        mapping[key] = loader.construct_object(value_node, deep=deep)
    return mapping


UniqueKeyLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, construct_mapping
)


def command(argv, *, input_text=None, expected=0):
    result = subprocess.run(
        [str(item) for item in argv],
        cwd=ROOT,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=180,
    )
    if result.returncode != expected:
        raise RuntimeError(
            f"command returned {result.returncode}, expected {expected}: "
            + " ".join(str(item) for item in argv)
            + "\nstdout:\n"
            + result.stdout
            + "\nstderr:\n"
            + result.stderr
        )
    return result


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def valid_values():
    return {
        "image": {
            "repository": "registry.invalid/plinth",
            "digest": DIGEST,
            "pullPolicy": "IfNotPresent",
            "pullSecrets": [],
        },
        "public": {"host": "plinth.example.test", "port": 8443},
        "registration": {"enabled": False},
        "database": {
            "existingSecret": "plinth-database",
            "keys": {
                "host": "host",
                "port": "port",
                "user": "user",
                "password": "password",
                "database": "database",
            },
            "poolSize": 4,
        },
        "persistence": {
            "data": {
                "existingClaim": "",
                "size": "1Gi",
                "accessModes": ["ReadWriteOnce"],
                "storageClassName": "standard",
                "retain": True,
            },
            "logs": {
                "existingClaim": "",
                "size": "1Gi",
                "accessModes": ["ReadWriteOnce"],
                "storageClassName": "standard",
                "retain": True,
            },
        },
        "resources": {
            "requests": {
                "cpu": "100m",
                "memory": "256Mi",
                "ephemeral-storage": "128Mi",
            },
            "limits": {
                "cpu": "1",
                "memory": "1Gi",
                "ephemeral-storage": "1Gi",
            },
        },
        "traefik": {
            "enabled": True,
            "ingressClassName": "traefik",
            "entryPoints": ["websecure"],
            "tls": {"secretName": "plinth-tls"},
            "limits": {
                "defaultRequestBodyBytes": 1048576,
                "maxRequestBodyBytes": 67108864,
                "memoryRequestBodyBytes": 262144,
                "maxInFlightRequests": 32,
                "maxPackageInFlightRequests": 2,
            },
            "forwardingTimeouts": {
                "dialTimeout": "5s",
                "responseHeaderTimeout": "65s",
                "packageResponseHeaderTimeout": "15m",
                "idleConnTimeout": "90s",
            },
        },
        "networkPolicy": {
            "enabled": True,
            "traefik": {
                "peers": [{
                    "namespaceSelector": {
                        "matchLabels": {"kubernetes.io/metadata.name": "kube-system"}
                    },
                    "podSelector": {
                        "matchLabels": {"app.kubernetes.io/name": "traefik"}
                    },
                }],
            },
            "dns": {
                "peers": [{
                    "namespaceSelector": {
                        "matchLabels": {"kubernetes.io/metadata.name": "kube-system"}
                    },
                    "podSelector": {"matchLabels": {"app": "coredns"}},
                }],
            },
            "database": {
                "port": 5432,
                "peers": [{
                    "namespaceSelector": {
                        "matchLabels": {"kubernetes.io/metadata.name": NAMESPACE}
                    },
                    "podSelector": {"matchLabels": {"app": "postgres"}},
                }],
                "ipBlocks": [],
            },
        },
        "rolloutNonce": "contract-v1",
    }


def write_values(directory, values, name="values.yaml"):
    path = Path(directory) / name
    path.write_text(yaml.safe_dump(values, sort_keys=False), encoding="utf-8")
    return path


def render(helm, directory, values, name="values.yaml", release=RELEASE):
    path = write_values(directory, values, name)
    return command(
        [
            helm,
            "template",
            release,
            CHART,
            "--namespace",
            NAMESPACE,
            "--kube-version",
            "1.36.0",
            "--values",
            path,
        ]
    ).stdout


def documents(rendered):
    loaded = list(yaml.load_all(rendered, Loader=UniqueKeyLoader))
    result = [item for item in loaded if item is not None]
    identities = set()
    for item in result:
        require(isinstance(item, dict), "rendered document is not a mapping")
        identity = (
            item.get("apiVersion"),
            item.get("kind"),
            item.get("metadata", {}).get("namespace", NAMESPACE),
            item.get("metadata", {}).get("name"),
        )
        require(identity[-1], f"resource has no name: {identity}")
        require(identity not in identities, f"duplicate resource identity: {identity}")
        identities.add(identity)
    return result


def one(items, kind, *, name=None):
    matches = [
        item
        for item in items
        if item.get("kind") == kind
        and (name is None or item["metadata"]["name"] == name)
    ]
    require(len(matches) == 1, f"expected one {kind} {name or ''}, got {len(matches)}")
    return matches[0]


def all_kind(items, kind):
    return [item for item in items if item.get("kind") == kind]


def verify_core_contract(items):
    require(not all_kind(items, "Secret"), "chart must never render credentials")
    expected_name = RELEASE + "-plinth"

    account = one(items, "ServiceAccount", name=expected_name)
    require(account.get("automountServiceAccountToken") is False,
            "service account token automount must be disabled")

    service = one(items, "Service", name=expected_name)
    require(service["spec"].get("type") == "ClusterIP",
            "only isolated ClusterIP exposure is supported")
    require("nodePort" not in json.dumps(service["spec"]),
            "service must not allocate node ports")
    headless = one(items, "Service", name=expected_name + "-headless")
    require(headless["spec"].get("type") == "ClusterIP",
            "governing Service must remain internal")
    require(headless["spec"].get("clusterIP") == "None",
            "StatefulSet requires a headless governing Service")
    require("nodePort" not in json.dumps(headless["spec"]),
            "headless Service must not allocate node ports")

    stateful_set = one(items, "StatefulSet", name=expected_name)
    spec = stateful_set["spec"]
    require(spec.get("replicas") == 1, "Plinth workload must remain single-replica")
    require(spec.get("serviceName") == expected_name + "-headless",
            "StatefulSet must bind its identity to the headless Service")
    require(spec.get("podManagementPolicy") == "OrderedReady",
            "StatefulSet must preserve ordered at-most-one replacement")
    require(spec.get("updateStrategy", {}).get("type") == "RollingUpdate",
            "StatefulSet must use an explicit rolling replacement")

    template = spec["template"]
    pod = template["spec"]
    require(pod.get("serviceAccountName") == expected_name,
            "pod must use the chart-owned unprivileged service account")
    require(pod.get("automountServiceAccountToken") is False,
            "pod must independently disable token automount")
    require(pod.get("terminationGracePeriodSeconds", 0) >= 60,
            "termination grace must exceed Plinth's 50-second watchdog")
    pod_security = pod.get("securityContext", {})
    require(pod_security.get("runAsNonRoot") is True, "pod must require non-root")
    require(pod_security.get("runAsUser") == 10001, "pod UID must match the image")
    require(pod_security.get("runAsGroup") == 10001, "pod GID must match the image")
    require(pod_security.get("fsGroup") == 10001, "persistent storage needs image GID")
    require(pod_security.get("seccompProfile", {}).get("type") == "RuntimeDefault",
            "pod must use RuntimeDefault seccomp")

    containers = pod.get("containers", [])
    require(len(containers) == 1, "deployment must have exactly one application container")
    container = containers[0]
    require(container.get("image") == "registry.invalid/plinth@" + DIGEST,
            "runtime image must be composed from repository plus exact digest")
    require(container.get("imagePullPolicy") == "IfNotPresent",
            "unexpected image pull policy")
    require(container.get("args") == [
        "serve", "--config", "/etc/plinth/config.json"
    ], "container must use the generated production configuration")
    security = container.get("securityContext", {})
    require(security.get("allowPrivilegeEscalation") is False,
            "privilege escalation must be disabled")
    require(security.get("readOnlyRootFilesystem") is True,
            "root filesystem must be read-only")
    require(security.get("runAsNonRoot") is True, "container must require non-root")
    require(security.get("capabilities", {}).get("drop") == ["ALL"],
            "all Linux capabilities must be dropped")

    probes = [container.get(name, {}) for name in (
        "startupProbe", "readinessProbe", "livenessProbe"
    )]
    for probe in probes:
        require(probe.get("httpGet", {}).get("path") == "/healthz",
                "every probe must use the production health endpoint")
        require(probe.get("httpGet", {}).get("port") in (8080, "http"),
                "every probe must target the application port")
        require(probe.get("timeoutSeconds", 0) > 0, "probe timeout must be bounded")
        require(probe.get("failureThreshold", 0) > 0,
                "probe failure threshold must be explicit")

    resources = container.get("resources", {})
    for boundary in ("requests", "limits"):
        require(set(resources.get(boundary, {})) >= {
            "cpu", "memory", "ephemeral-storage"
        }, f"resource {boundary} must cover CPU, memory, and ephemeral storage")

    env = {entry["name"]: entry for entry in container.get("env", [])}
    require("envFrom" not in container, "database secret must not inject arbitrary keys")
    for variable, key in {
        "PLINTH_PG_HOST": "host",
        "PLINTH_PG_PORT": "port",
        "PLINTH_PG_USER": "user",
        "PLINTH_PG_PASSWORD": "password",
        "PLINTH_PG_DATABASE": "database",
    }.items():
        reference = env.get(variable, {}).get("valueFrom", {}).get("secretKeyRef", {})
        require(reference == {"name": "plinth-database", "key": key},
                f"{variable} must reference only the declared Secret key")
    require(env.get("PLINTH_DEV_MODE", {}).get("value") in ("false", False),
            "development reset must be explicitly disabled")
    require(env.get("PLINTH_REGISTRATION_ENABLED", {}).get("value") in (
        "false", False
    ), "registration must default closed in the contract fixture")

    mounts = {mount["name"]: mount for mount in container.get("volumeMounts", [])}
    require(mounts.get("config", {}).get("readOnly") is True,
            "generated configuration must be mounted read-only")
    require(mounts.get("data", {}).get("mountPath") == "/var/lib/plinth/data",
            "data storage mount is missing")
    require(mounts.get("logs", {}).get("mountPath") == "/var/lib/plinth/logs",
            "log storage mount is missing")
    require(mounts.get("tmp", {}).get("mountPath") == "/tmp",
            "read-only root needs an explicit temporary volume")
    require(
        mounts.get("uploads", {}).get("mountPath") == "/var/lib/plinth/uploads",
        "read-only root needs writable Drogon upload staging",
    )
    volumes = {
        volume["name"]: volume
        for volume in pod.get("volumes", [])
    }
    require(
        volumes.get("uploads", {}).get("emptyDir", {}).get("sizeLimit") == "128Mi",
        "Drogon upload staging is not bounded ephemeral storage",
    )
    require("checksum/config" in template.get("metadata", {}).get("annotations", {}),
            "ConfigMap changes must trigger a replacement")

    config_map = one(items, "ConfigMap", name=expected_name + "-config")
    config = json.loads(config_map["data"]["config.json"])
    require(config.get("browser_origin") == "https://plinth.example.test:8443",
            "public TLS origin must include the externally visible port")
    require(config.get("dev_mode") is False, "generated config must disable dev mode")
    require(config.get("registration_enabled") is False,
            "generated config must disable registration by default")

    claims = all_kind(items, "PersistentVolumeClaim")
    require({claim["metadata"]["name"] for claim in claims} == {
        expected_name + "-data", expected_name + "-logs"
    }, "chart must create the two requested persistent claims")
    for claim in claims:
        require(claim["metadata"].get("annotations", {}).get(
            "helm.sh/resource-policy"
        ) == "keep", "retained persistent claims need Helm keep policy")
        require(claim["spec"].get("accessModes") == ["ReadWriteOnce"],
                "unexpected PVC access mode")

    policy = one(items, "NetworkPolicy", name=expected_name)
    require(set(policy["spec"].get("policyTypes", [])) == {"Ingress", "Egress"},
            "network policy must isolate ingress and egress")
    require(policy["spec"].get("ingress") == [{
        "from": [{
            "namespaceSelector": {"matchLabels": {
                "kubernetes.io/metadata.name": "kube-system"
            }},
            "podSelector": {"matchLabels": {
                "app.kubernetes.io/name": "traefik"
            }},
        }],
        "ports": [{"port": 8080, "protocol": "TCP"}],
    }], "network policy ingress must select only Traefik on TCP 8080")
    require(policy["spec"].get("egress") == [
        {
            "to": [{
                "namespaceSelector": {"matchLabels": {
                    "kubernetes.io/metadata.name": "kube-system"
                }},
                "podSelector": {"matchLabels": {"app": "coredns"}},
            }],
            "ports": [
                {"port": 53, "protocol": "UDP"},
                {"port": 53, "protocol": "TCP"},
            ],
        },
        {
            "to": [{
                "namespaceSelector": {"matchLabels": {
                    "kubernetes.io/metadata.name": NAMESPACE
                }},
                "podSelector": {"matchLabels": {"app": "postgres"}},
            }],
            "ports": [{"port": 5432, "protocol": "TCP"}],
        },
    ], "network policy egress must select only DNS and PostgreSQL peers")


def verify_traefik_contract(items):
    expected_name = RELEASE + "-plinth"
    routes = all_kind(items, "IngressRoute")
    require(len(routes) == 3, "expected separate WebSocket, package, and default routes")
    require(len(all_kind(items, "Middleware")) == 4,
            "expected body-limit and in-flight middlewares")
    transports = {
        item["metadata"]["name"]: item for item in all_kind(items, "ServersTransport")
    }
    require(set(transports) == {expected_name, expected_name + "-package"},
            "expected ordinary and package-specific backend transports")
    forwarding = transports[expected_name]["spec"].get("forwardingTimeouts", {})
    require(forwarding == {
        "dialTimeout": "5s",
        "responseHeaderTimeout": "65s",
        "idleConnTimeout": "90s",
    }, "Traefik forwarding timeouts drifted")
    require(
        transports[expected_name + "-package"]["spec"].get(
            "forwardingTimeouts", {}
        ) == {
            "dialTimeout": "5s",
            "responseHeaderTimeout": "15m",
            "idleConnTimeout": "90s",
        },
        "package forwarding timeout must allow bounded long migrations",
    )

    route_by_name = {item["metadata"]["name"]: item for item in routes}
    require(set(route_by_name) == {
        expected_name + "-ws", expected_name + "-packages", expected_name + "-http"
    }, "Traefik route names drifted")
    for route in routes:
        require(route["metadata"].get("annotations", {}).get(
            "kubernetes.io/ingress.class"
        ) == "traefik", "routes must select the configured ingress class")
        require("ingressClassName" not in route["spec"],
                "IngressRoute spec.ingressClassName is not supported by K3s Traefik CRDs")
        require(route["spec"].get("entryPoints") == ["websecure"],
                "routes must bind only to the TLS entrypoint")
        require(route["spec"].get("tls", {}).get("secretName") == "plinth-tls",
                "routes must require the configured TLS secret")
        for rule in route["spec"].get("routes", []):
            for service in rule.get("services", []):
                require(service.get("passHostHeader") is True,
                        "Traefik must preserve the browser-visible Host authority")
                expected_transport = (
                    expected_name + "-package"
                    if route["metadata"]["name"] == expected_name + "-packages"
                    else expected_name
                )
                require(service.get("serversTransport") == expected_transport,
                        "route selected the wrong bounded backend transport")

    ws_rule = route_by_name[expected_name + "-ws"]["spec"]["routes"][0]
    require("/ws/events" in ws_rule.get("match", ""),
            "WebSocket route must be exact and highest priority")
    require(not ws_rule.get("middlewares"),
            "WebSocket upgrades must not pass through buffering middleware")
    default_rule = route_by_name[expected_name + "-http"]["spec"]["routes"][0]
    require(
        [item.get("name") for item in default_rule.get("middlewares", [])] == [
            expected_name + "-inflight",
            expected_name + "-default-body",
        ],
        "default concurrency must be enforced before request buffering",
    )
    package_rule = route_by_name[expected_name + "-packages"]["spec"]["routes"][0]
    require("/api/packages" in package_rule.get("match", ""),
            "package route must carry the larger upload budget")
    require(len(package_rule.get("middlewares", [])) == 2,
            "package route must enforce body and in-flight limits")
    require(
        [item.get("name") for item in package_rule["middlewares"]] == [
            expected_name + "-package-inflight",
            expected_name + "-package-body",
        ],
        "package concurrency must be enforced before request buffering",
    )

    middleware_by_name = {
        item["metadata"]["name"]: item for item in all_kind(items, "Middleware")
    }
    default_buffer = middleware_by_name[expected_name + "-default-body"]["spec"]["buffering"]
    package_buffer = middleware_by_name[expected_name + "-package-body"]["spec"]["buffering"]
    inflight = middleware_by_name[expected_name + "-inflight"]["spec"]["inFlightReq"]
    package_inflight = middleware_by_name[
        expected_name + "-package-inflight"
    ]["spec"]["inFlightReq"]
    require(default_buffer.get("maxRequestBodyBytes") == 1048576,
            "default body limit drifted")
    require(package_buffer.get("maxRequestBodyBytes") == 67108864,
            "package body limit must exceed Plinth's 50 MiB package ceiling")
    require(default_buffer.get("memRequestBodyBytes") == 262144,
            "request buffering memory limit drifted")
    require(inflight.get("amount") == 32, "in-flight request limit drifted")
    require(package_inflight.get("amount") == 2,
            "package in-flight limit exceeds upload scratch capacity")


def verify_existing_claim_and_isolated_renders(helm, directory, base):
    values = copy.deepcopy(base)
    values["persistence"]["data"]["existingClaim"] = "existing-data"
    values["persistence"]["logs"]["existingClaim"] = "existing-logs"
    items = documents(render(helm, directory, values, "existing.yaml"))
    require(not all_kind(items, "PersistentVolumeClaim"),
            "existing-claim mode must not create or own PVCs")
    deployment = one(items, "StatefulSet", name=RELEASE + "-plinth")
    volumes = {
        volume["name"]: volume for volume in deployment["spec"]["template"]["spec"]["volumes"]
    }
    require(volumes["data"]["persistentVolumeClaim"]["claimName"] == "existing-data",
            "existing data claim is not mounted")
    require(volumes["logs"]["persistentVolumeClaim"]["claimName"] == "existing-logs",
            "existing logs claim is not mounted")

    values = copy.deepcopy(base)
    values["traefik"]["enabled"] = False
    items = documents(render(helm, directory, values, "isolated.yaml"))
    for kind in ("IngressRoute", "Middleware", "ServersTransport"):
        require(not all_kind(items, kind),
                f"Traefik-disabled render unexpectedly contains {kind}")
    require(one(items, "Service", name=RELEASE + "-plinth")["spec"].get(
        "type"
    ) == "ClusterIP",
            "isolated render must remain ClusterIP-only")
    require(one(items, "NetworkPolicy")["spec"].get("ingress") == [],
            "isolated render must deny all Pod-network ingress")


def verify_yaml_string_boundaries(helm, directory, base):
    values = copy.deepcopy(base)
    values["database"]["existingSecret"] = "true"
    values["database"]["keys"].update({
        "host": "null", "port": "yes", "user": "off",
        "password": "on", "database": "no",
    })
    values["persistence"]["data"]["existingClaim"] = "true"
    values["persistence"]["logs"]["existingClaim"] = "null"
    values["traefik"]["tls"]["secretName"] = "on"
    items = documents(render(
        helm, directory, values, "ambiguous-strings.yaml", release="true"
    ))

    workload = one(items, "StatefulSet", name="true-plinth")
    labels = workload["metadata"]["labels"]
    require(labels.get("app.kubernetes.io/instance") == "true",
            "release-name label changed YAML type")
    container = workload["spec"]["template"]["spec"]["containers"][0]
    env = {entry["name"]: entry for entry in container["env"]}
    expected = {
        "PLINTH_PG_HOST": ("true", "null"),
        "PLINTH_PG_PORT": ("true", "yes"),
        "PLINTH_PG_USER": ("true", "off"),
        "PLINTH_PG_PASSWORD": ("true", "on"),
        "PLINTH_PG_DATABASE": ("true", "no"),
    }
    for variable, (secret, key) in expected.items():
        reference = env[variable]["valueFrom"]["secretKeyRef"]
        require(reference == {"name": secret, "key": key},
                f"{variable} Secret reference changed YAML type")
    volumes = {
        volume["name"]: volume
        for volume in workload["spec"]["template"]["spec"]["volumes"]
    }
    require(volumes["data"]["persistentVolumeClaim"]["claimName"] == "true",
            "data claim name changed YAML type")
    require(volumes["logs"]["persistentVolumeClaim"]["claimName"] == "null",
            "log claim name changed YAML type")
    for route in all_kind(items, "IngressRoute"):
        require(route["spec"]["tls"]["secretName"] == "on",
                "TLS Secret name changed YAML type")


def verify_network_peer_replacement(helm, directory, base):
    values = copy.deepcopy(base)
    values["networkPolicy"]["database"].update(
        peers=[],
        ipBlocks=[{"cidr": "10.0.0.0/8", "except": ["10.1.0.0/16"]}],
    )
    items = documents(render(helm, directory, values, "ip-block-database.yaml"))
    policy = one(items, "NetworkPolicy", name=RELEASE + "-plinth")
    database_rule = policy["spec"]["egress"][1]
    require(database_rule == {
        "to": [{
            "ipBlock": {"cidr": "10.0.0.0/8", "except": ["10.1.0.0/16"]}
        }],
        "ports": [{"port": 5432, "protocol": "TCP"}],
    }, "database peer-array override retained an unintended default selector")

    values = copy.deepcopy(base)
    values["networkPolicy"]["database"].update(
        peers=[], ipBlocks=[{"cidr": "2001:db8::/32", "except": ["2001:db8:1::/48"]}],
    )
    render(helm, directory, values, "ipv6-database.yaml")


def verify_fail_closed_inputs(helm, directory, base):
    cases = []

    def add(name, mutate):
        values = copy.deepcopy(base)
        mutate(values)
        cases.append((name, values))

    add("missing-digest", lambda value: value["image"].update(digest=""))
    add("malformed-digest", lambda value: value["image"].update(digest="sha256:abcd"))
    add("tagged-repository", lambda value: value["image"].update(
        repository="registry.invalid/plinth:latest"
    ))
    add("embedded-digest", lambda value: value["image"].update(
        repository="registry.invalid/plinth@" + DIGEST
    ))
    add("repository-url", lambda value: value["image"].update(
        repository="https://registry.invalid/plinth"
    ))
    add("repository-leading-slash", lambda value: value["image"].update(
        repository="/registry.invalid/plinth"
    ))
    add("repository-empty-component", lambda value: value["image"].update(
        repository="registry.invalid//plinth"
    ))
    add("missing-database-secret", lambda value: value["database"].update(
        existingSecret=""
    ))
    add("invalid-public-host", lambda value: value["public"].update(
        host="https://plinth.example.test/path"
    ))
    add("missing-tls-secret", lambda value: value["traefik"]["tls"].update(
        secretName=""
    ))
    add("package-limit-not-above-payload", lambda value: value["traefik"]["limits"].update(
        maxRequestBodyBytes=67108863
    ))
    add("package-limit-above-supported-boundary", lambda value: value[
        "traefik"
    ]["limits"].update(maxRequestBodyBytes=67108865))
    add("memory-buffer-too-large", lambda value: value["traefik"]["limits"].update(
        memoryRequestBodyBytes=2097152
    ))
    add("registration-exposed", lambda value: value["registration"].update(
        enabled=True
    ))
    add("empty-public-host", lambda value: value["public"].update(host=""))
    add("empty-traefik-peers", lambda value: value["networkPolicy"][
        "traefik"
    ].update(peers=[]))
    add("empty-dns-peers", lambda value: value["networkPolicy"]["dns"].update(
        peers=[]
    ))
    add("empty-database-peers", lambda value: value["networkPolicy"][
        "database"
    ].update(peers=[], ipBlocks=[]))
    add("invalid-database-cidr", lambda value: value["networkPolicy"][
        "database"
    ].update(peers=[], ipBlocks=[{"cidr": "999.999.999.999/999"}]))
    add("invalid-storage-quantity", lambda value: value["persistence"]["data"].update(
        size="null"
    ))
    add("invalid-resource-quantity", lambda value: value["resources"]["limits"].update(
        memory="true"
    ))

    for name, values in cases:
        path = write_values(directory, values, name + ".yaml")
        result = subprocess.run(
            [
                helm,
                "template",
                RELEASE,
                str(CHART),
                "--namespace",
                NAMESPACE,
                "--values",
                str(path),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=60,
        )
        require(result.returncode != 0, f"unsafe values unexpectedly rendered: {name}")


def verify_kubernetes_schemas(kubeconform, rendered, cache):
    Path(cache).mkdir(parents=True, exist_ok=True)
    for version in KUBERNETES_VERSIONS:
        command(
            [
                kubeconform,
                "-strict",
                "-summary",
                "-ignore-missing-schemas",
                "-kubernetes-version",
                version,
                "-schema-location",
                SCHEMA_URL,
                "-cache",
                cache,
            ],
            input_text=rendered,
        )


def verify_chart_archive(helm, directory, values_path):
    output = Path(directory) / "package"
    output.mkdir()
    command([helm, "package", CHART, "--destination", output])
    archives = list(output.glob("plinth-*.tgz"))
    require(len(archives) == 1, "helm package did not create exactly one archive")
    with tarfile.open(archives[0], "r:gz") as archive:
        for member in archive.getmembers():
            path = Path(member.name)
            require(path.parts and path.parts[0] == "plinth",
                    f"chart archive escaped its root: {member.name}")
            require(".." not in path.parts and not path.is_absolute(),
                    f"unsafe chart archive path: {member.name}")
            require(not member.issym() and not member.islnk(),
                    f"chart archive contains a link: {member.name}")
            require(not re.search(r"(?:^|/)(?:\.env|.*\.(?:pem|key))$", member.name),
                    f"credential-shaped chart asset: {member.name}")
    # Keep the argument live so callers cannot accidentally package different
    # values than those linted without this test noticing the file disappeared.
    require(Path(values_path).is_file(), "lint values disappeared before packaging")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--helm", default="helm")
    parser.add_argument("--kubeconform", default="kubeconform")
    args = parser.parse_args()

    for executable in (args.helm, args.kubeconform):
        require(shutil.which(executable), f"required executable is unavailable: {executable}")
    require(CHART.is_dir(), f"Helm chart is missing: {CHART}")

    with tempfile.TemporaryDirectory(prefix="plinth-helm-contract-") as temporary:
        base = valid_values()
        values_path = write_values(temporary, base)
        for version in KUBERNETES_VERSIONS:
            command([
                args.helm,
                "lint",
                "--strict",
                "--kube-version",
                version,
                "--values",
                values_path,
                CHART,
            ])
        rendered = render(args.helm, temporary, base)
        items = documents(rendered)
        verify_core_contract(items)
        verify_traefik_contract(items)
        verify_existing_claim_and_isolated_renders(args.helm, temporary, base)
        verify_yaml_string_boundaries(args.helm, temporary, base)
        verify_network_peer_replacement(args.helm, temporary, base)
        verify_fail_closed_inputs(args.helm, temporary, base)
        verify_kubernetes_schemas(
            args.kubeconform, rendered, str(Path(temporary) / "schema-cache")
        )
        verify_chart_archive(args.helm, temporary, values_path)

    print("helm contract: lint, schema, security, and render checks passed")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, ValueError, yaml.YAMLError) as error:
        print(f"helm contract: {error}", file=sys.stderr)
        raise SystemExit(1) from error
