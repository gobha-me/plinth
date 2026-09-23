#!/usr/bin/env python3
"""Exercise the exact Plinth candidate through a task-owned K3s/Traefik stack."""

from __future__ import annotations

import argparse
import atexit
import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import secrets
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

try:
    import yaml
except ImportError as error:  # pragma: no cover - actionable CI preflight
    raise SystemExit("PyYAML is required for the K3d lifecycle test") from error


ROOT = Path(__file__).resolve().parents[2]
CHART = ROOT / "deploy" / "helm" / "plinth"
K3S_MIN_IMAGE = (
    "docker.io/rancher/k3s:v1.35.8-k3s1@"
    "sha256:59fe491fd3b73204e499e40b325240d85c42c7189c3ae50150d37b78243f3b32"
)
K3S_MAX_IMAGE = (
    "docker.io/rancher/k3s:v1.37.0-k3s1@"
    "sha256:d621019df980a5925854bb67f05c6947822bb56acf45e7974d0258565f52ff53"
)
K3S_TRAEFIK_CHART_VERSION = {
    "min": "40.1.4+up40.1.0",
    "max": "41.4.2+up41.4.0",
}
REGISTRY_IMAGE = (
    "docker.io/library/registry:3.0.0@"
    "sha256:5b12b22f21522fe69443079df04c0f3f42cb9977857f3c20404386652a6c6d8e"
)
POSTGRES_IMAGE = (
    "docker.io/pgvector/pgvector:pg16@"
    "sha256:ccc6e83d6e35e931dc7c5def2022729d5a6c370318d099181995567ff1fb4d6b"
)
CURL_IMAGE = (
    "docker.io/curlimages/curl:8.16.0@"
    "sha256:5a91ea0c9c3ee27b4abe657b68cf6bf0676afa13b236b3bda34283cb3924d4f6"
)
TRAEFIK_IMAGE_REPOSITORY = "rancher/mirrored-library-traefik"
TRAEFIK_IMAGE_TAG = "3.7.13"
TRAEFIK_IMAGE_DIGEST = (
    "sha256:96780238b1bbda5a9bb997f4307ce69e798ad1cf6eb7f2dcc0a440823467d199"
)
POSTGRES_PASSWORD = "fake-issue36-postgres-password"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def _containerd_task_exit_event(line):
    """Decode one `k3s ctr events` line, whose timestamp contains spaces."""
    marker = " k8s.io /tasks/exit "
    if marker not in line:
        return None
    try:
        event = json.loads(line.split(marker, 1)[1])
    except json.JSONDecodeError:
        return None
    return event if isinstance(event, dict) else None


def parse_containerd_exit_event(line, expected_container_id):
    """Return a CRI-compatible status for only the target container's init exit."""
    event = _containerd_task_exit_event(line)
    if event is None or event.get("container_id") != expected_container_id \
            or event.get("id") != expected_container_id:
        return None
    # TaskExit is proto3: encoding/json omits the zero-valued exit_status.
    exit_code = event.get("exit_status", 0)
    finished_at = event.get("exited_at")
    if isinstance(exit_code, bool) or not isinstance(exit_code, int) \
            or exit_code < 0 or not isinstance(finished_at, str):
        return None
    timestamp = re.fullmatch(
        r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.\d{1,9})?Z",
        finished_at,
    )
    if timestamp is None:
        return None
    try:
        datetime.strptime(timestamp.group(1), "%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return None
    return {
        "state": "CONTAINER_EXITED",
        "exitCode": exit_code,
        "finishedAt": finished_at,
        "id": expected_container_id,
    }


class Harness:
    def __init__(self, args):
        token = secrets.token_hex(4)
        self.args = args
        self.cluster = "plinth-issue36-" + token
        self.registry = "plinth-issue36-reg-" + token
        self.namespace = "plinth-issue36-" + token
        self.release = "issue36-live"
        self.host = "plinth.test"
        self.https_port = free_port()
        self.registry_port = free_port()
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-issue36-live-")
        self.root = Path(self.temporary.name)
        self.kubeconfig = self.root / "kubeconfig"
        self.certificate = self.root / "tls.crt"
        self.private_key = self.root / "tls.key"
        self.cookies = self.root / "cookies.txt"
        self.values_path = self.root / "values.yaml"
        self.traefik_config = self.root / "traefik-config.yaml"
        self.bootstrap_secret_name = "plinth-bootstrap"
        self.bootstrap_token = secrets.token_urlsafe(48)
        self.local_tag = f"127.0.0.1:{self.registry_port}/plinth:candidate"
        self.internal_repository = f"k3d-{self.registry}:5000/plinth"
        self.candidate_digest = ""
        self.cluster_created = False
        self.registry_created = False
        self.namespace_created = False
        self.chart_installed = False
        self.cleanup_complete = False
        self.failed = False
        self._exit_watchers = []
        self.env = os.environ.copy()
        self.env["KUBECONFIG"] = str(self.kubeconfig)

    def run(self, argv, *, input_text=None, timeout=180, check=True, env=None):
        result = subprocess.run(
            [str(item) for item in argv],
            cwd=ROOT,
            env=env or self.env,
            input=input_text,
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        if check and result.returncode != 0:
            raise RuntimeError(
                f"command failed ({result.returncode}): "
                + " ".join(str(item) for item in argv)
                + "\nstdout:\n"
                + result.stdout
                + "\nstderr:\n"
                + result.stderr
            )
        return result

    def kubectl(self, *arguments, **kwargs):
        return self.run([self.args.kubectl, *arguments], **kwargs)

    def helm(self, *arguments, **kwargs):
        return self.run([self.args.helm, *arguments], **kwargs)

    def wait_for_resource(self, resource, namespace, *, timeout=240):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.kubectl(
                "get", resource, "-n", namespace, check=False, timeout=15,
            )
            if last.returncode == 0:
                return
            time.sleep(0.5)
        raise RuntimeError(
            f"{namespace}/{resource} was not created within {timeout}s\n"
            + (last.stderr if last is not None else "")
        )

    def create_infrastructure(self):
        require(platform.machine() in ("x86_64", "amd64"),
                "the live lane uses reviewed linux/amd64 image digests")
        self.run(["docker", "image", "inspect", self.args.image], timeout=30)

        self.run([
            self.args.k3d,
            "registry",
            "create",
            self.registry,
            "--image",
            REGISTRY_IMAGE,
            "--port",
            f"127.0.0.1:{self.registry_port}",
        ], timeout=120)
        self.registry_created = True

        self.run(["docker", "tag", self.args.image, self.local_tag], timeout=30)
        deadline = time.monotonic() + 30
        last_push_error = ""
        while True:
            try:
                pushed = self.run(
                    ["docker", "push", self.local_tag], timeout=5, check=False,
                )
                if pushed.returncode == 0:
                    break
                last_push_error = (
                    "stdout:\n" + pushed.stdout + "\nstderr:\n" + pushed.stderr
                )
            except subprocess.TimeoutExpired as error:
                last_push_error = f"push attempt timed out: {error}"
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "candidate push did not become ready within 30s\n"
                    + last_push_error
                )
            time.sleep(0.5)
        match = re.search(r"digest:\s*(sha256:[0-9a-f]{64})", pushed.stdout + pushed.stderr)
        require(match is not None, "registry push did not report an immutable digest")
        self.candidate_digest = match.group(1)

        self.traefik_config.write_text(
            """apiVersion: helm.cattle.io/v1
kind: HelmChartConfig
metadata:
  name: traefik
  namespace: kube-system
spec:
  valuesContent: |-
    image:
      repository: rancher/mirrored-library-traefik
      tag: \"3.7.13\"
      digest: sha256:96780238b1bbda5a9bb997f4307ce69e798ad1cf6eb7f2dcc0a440823467d199
""",
            encoding="utf-8",
        )
        self.run([
            self.args.k3d,
            "cluster",
            "create",
            self.cluster,
            "--image",
            K3S_MIN_IMAGE if self.args.kubernetes == "min" else K3S_MAX_IMAGE,
            "--registry-use",
            f"k3d-{self.registry}:5000",
            "--port",
            f"127.0.0.1:{self.https_port}:443@loadbalancer",
            "--k3s-arg",
            "--disable=traefik@server:0",
            "--kubeconfig-update-default=false",
            "--wait",
            "--timeout",
            "240s",
        ], timeout=300)
        self.cluster_created = True
        kubeconfig = self.run([
            self.args.k3d, "kubeconfig", "get", self.cluster
        ], timeout=30).stdout
        self.kubeconfig.write_text(kubeconfig, encoding="utf-8")
        self.kubeconfig.chmod(0o600)

        # Keep the packaged component disabled until the digest override is
        # present. The exact K3s image still carries immutable internal chart
        # archives; submit matching HelmChart objects only after proving that
        # no default Traefik reconciliation started.
        for resource in (
            "helmchart.helm.cattle.io/traefik-crd",
            "helmchart.helm.cattle.io/traefik",
            "job/helm-install-traefik-crd",
            "job/helm-install-traefik",
            "deployment/traefik",
        ):
            absent = self.kubectl(
                "get", resource, "-n", "kube-system", check=False,
            )
            require(
                absent.returncode != 0 and "(NotFound)" in absent.stderr,
                f"{resource} was present or its absence could not be proven "
                f"before the digest pin\nstdout:\n{absent.stdout}"
                f"\nstderr:\n{absent.stderr}",
            )
        pods_before_pin = json.loads(self.kubectl(
            "get", "pods", "-n", "kube-system", "-o", "json",
        ).stdout)["items"]
        traefik_pods_before_pin = [
            pod for pod in pods_before_pin
            if pod["metadata"]["name"].startswith("helm-install-traefik-")
            or pod["metadata"].get("labels", {}).get(
                "app.kubernetes.io/name"
            ) == "traefik"
        ]
        require(not traefik_pods_before_pin,
                "a Traefik installer or application pod ran before its digest pin")
        self.kubectl("apply", "-f", self.traefik_config)
        chart_version = K3S_TRAEFIK_CHART_VERSION[self.args.kubernetes]
        node = f"k3d-{self.cluster}-server-0"
        for chart_name in ("traefik-crd", "traefik"):
            archive = (
                "/var/lib/rancher/k3s/server/static/charts/"
                f"{chart_name}-{chart_version}.tgz"
            )
            self.run(["docker", "exec", node, "test", "-f", archive], timeout=30)
        chart_base = "https://%{KUBERNETES_API}%/static/charts/"
        bundled_traefik = [{
            "apiVersion": "helm.cattle.io/v1",
            "kind": "HelmChart",
            "metadata": {"name": "traefik-crd", "namespace": "kube-system"},
            "spec": {
                "forceConflicts": True,
                "failurePolicy": "retry",
                "chart": chart_base + f"traefik-crd-{chart_version}.tgz",
            },
        }, {
            "apiVersion": "helm.cattle.io/v1",
            "kind": "HelmChart",
            "metadata": {"name": "traefik", "namespace": "kube-system"},
            "spec": {
                "forceConflicts": True,
                "failurePolicy": "retry",
                "chart": chart_base + f"traefik-{chart_version}.tgz",
                "set": {"global.systemDefaultRegistry": ""},
                "valuesContent": """deployment:
  podAnnotations:
    prometheus.io/port: \"8082\"
    prometheus.io/scrape: \"true\"
providers:
  kubernetesIngress:
    publishedService:
      enabled: true
priorityClassName: system-cluster-critical
tolerations:
  - key: CriticalAddonsOnly
    operator: Exists
  - key: node-role.kubernetes.io/control-plane
    operator: Exists
    effect: NoSchedule
service:
  spec:
    ipFamilyPolicy: PreferDualStack
""",
            },
        }]
        self.kubectl(
            "apply", "-f", "-",
            input_text=yaml.safe_dump_all(bundled_traefik, sort_keys=False),
        )

        # K3d's --wait covers the K3s API, while packaged HelmChart jobs keep
        # reconciling after the node becomes ready. Wait for the Deployment to
        # exist before asking rollout status to follow it.
        self.wait_for_resource("deployment/traefik", "kube-system")
        self.kubectl(
            "rollout", "status", "deployment/traefik", "-n", "kube-system",
            "--timeout=240s", timeout=260,
        )
        deadline = time.monotonic() + 240
        image = ""
        while time.monotonic() < deadline:
            traefik = json.loads(self.kubectl(
                "get", "deployment/traefik", "-n", "kube-system", "-o", "json"
            ).stdout)
            image = traefik["spec"]["template"]["spec"]["containers"][0]["image"]
            if TRAEFIK_IMAGE_DIGEST in image:
                break
            time.sleep(0.5)
        require(TRAEFIK_IMAGE_REPOSITORY in image,
                f"unexpected Traefik image repository: {image}")
        require(TRAEFIK_IMAGE_DIGEST in image,
                f"Traefik deployment is not digest-pinned: {image}")
        self.kubectl(
            "rollout", "status", "deployment/traefik", "-n", "kube-system",
            "--timeout=240s", timeout=260,
        )
        pod = json.loads(self.kubectl(
            "get", "pods", "-n", "kube-system",
            "-l", "app.kubernetes.io/name=traefik", "-o", "json"
        ).stdout)["items"]
        require(len(pod) == 1, "expected one task-cluster Traefik pod")
        image_id = pod[0]["status"]["containerStatuses"][0]["imageID"]
        require(TRAEFIK_IMAGE_DIGEST in image_id,
                f"running Traefik image ID drifted: {image_id}")
        version = self.kubectl(
            "exec", "-n", "kube-system", pod[0]["metadata"]["name"],
            "--", "traefik", "version",
        ).stdout
        require(
            re.search(rf"(?m)^Version:\s+{re.escape(TRAEFIK_IMAGE_TAG)}$", version),
            f"running Traefik version drifted:\n{version}",
        )

    def apply_json(self, document):
        self.kubectl("apply", "-f", "-", input_text=json.dumps(document))

    def create_namespace_dependencies(self):
        self.kubectl("create", "namespace", self.namespace)
        self.namespace_created = True
        self.kubectl(
            "label", "namespace", self.namespace,
            "plinth.gobha.me/test=issue36", "--overwrite",
        )

        self.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-days", "1", "-subj", f"/CN={self.host}",
            "-addext", f"subjectAltName=DNS:{self.host}",
            "-keyout", self.private_key, "-out", self.certificate,
        ], timeout=30)
        self.kubectl(
            "create", "secret", "tls", "plinth-tls",
            "--cert", self.certificate, "--key", self.private_key,
            "-n", self.namespace,
        )

        database_secret = {
            "apiVersion": "v1",
            "kind": "Secret",
            "metadata": {"name": "plinth-database", "namespace": self.namespace},
            "type": "Opaque",
            "stringData": {
                "host": "postgres",
                "port": "5432",
                "user": "plinth",
                "password": POSTGRES_PASSWORD,
                "database": "plinth",
            },
        }
        self.apply_json(database_secret)

        bootstrap_secret = {
            "apiVersion": "v1",
            "kind": "Secret",
            "metadata": {
                "name": self.bootstrap_secret_name,
                "namespace": self.namespace,
                "labels": {"plinth.gobha.me/test": "issue36"},
            },
            "type": "Opaque",
            "stringData": {"bootstrap-token": self.bootstrap_token},
        }
        self.apply_json(bootstrap_secret)

        postgres = {
            "apiVersion": "v1",
            "kind": "List",
            "items": [
                {
                    "apiVersion": "v1",
                    "kind": "Service",
                    "metadata": {"name": "postgres", "namespace": self.namespace},
                    "spec": {
                        "selector": {"app": "postgres"},
                        "ports": [{"name": "postgres", "port": 5432, "targetPort": 5432}],
                    },
                },
                {
                    "apiVersion": "apps/v1",
                    "kind": "Deployment",
                    "metadata": {"name": "postgres", "namespace": self.namespace},
                    "spec": {
                        "replicas": 1,
                        "selector": {"matchLabels": {"app": "postgres"}},
                        "template": {
                            "metadata": {"labels": {"app": "postgres"}},
                            "spec": {
                                "automountServiceAccountToken": False,
                                "securityContext": {
                                    "seccompProfile": {"type": "RuntimeDefault"}
                                },
                                "containers": [{
                                    "name": "postgres",
                                    "image": POSTGRES_IMAGE,
                                    "imagePullPolicy": "IfNotPresent",
                                    "env": [
                                        {"name": "POSTGRES_USER", "value": "plinth"},
                                        {"name": "POSTGRES_PASSWORD", "valueFrom": {
                                            "secretKeyRef": {
                                                "name": "plinth-database", "key": "password"
                                            }
                                        }},
                                        {"name": "POSTGRES_DB", "value": "plinth"},
                                    ],
                                    "ports": [{"containerPort": 5432, "name": "postgres"}],
                                    "readinessProbe": {
                                        "exec": {"command": [
                                            "pg_isready", "-U", "plinth", "-d", "plinth"
                                        ]},
                                        "periodSeconds": 2,
                                        "timeoutSeconds": 2,
                                        "failureThreshold": 30,
                                    },
                                    "resources": {
                                        "requests": {"cpu": "50m", "memory": "128Mi"},
                                        "limits": {"cpu": "1", "memory": "512Mi"},
                                    },
                                    "volumeMounts": [{
                                        "name": "data", "mountPath": "/var/lib/postgresql/data"
                                    }],
                                }],
                                "volumes": [{"name": "data", "emptyDir": {}}],
                            },
                        },
                    },
                },
            ],
        }
        self.apply_json(postgres)
        self.kubectl(
            "rollout", "status", "deployment/postgres", "-n", self.namespace,
            "--timeout=180s", timeout=200,
        )

    def values(
        self, *, registration, nonce, traefik_enabled,
        bootstrap_secret_name="",
    ):
        require(
            not bootstrap_secret_name or not traefik_enabled,
            "bootstrap authority must never be configured with Traefik exposure",
        )
        return {
            "image": {
                "repository": self.internal_repository,
                "digest": self.candidate_digest,
                "pullPolicy": "IfNotPresent",
                "pullSecrets": [],
            },
            "public": {"host": self.host, "port": self.https_port},
            "registration": {
                "mode": "open" if registration else "disabled",
                "maxAccounts": 1000,
                "sourceAttempts": 1000,
                "subjectAttempts": 5,
                "globalAttempts": 100,
                "windowSeconds": 60,
                "inviteTtlSeconds": 86400,
                "bootstrapSecret": {
                    "name": bootstrap_secret_name,
                    "key": "bootstrap-token",
                },
            },
            "database": {
                "existingSecret": "plinth-database",
                "keys": {
                    "host": "host", "port": "port", "user": "user",
                    "password": "password", "database": "database",
                },
                "poolSize": 4,
            },
            "persistence": {
                "data": {
                    "existingClaim": "", "size": "1Gi",
                    "accessModes": ["ReadWriteOnce"],
                    "storageClassName": "local-path", "retain": False,
                },
                "logs": {
                    "existingClaim": "", "size": "1Gi",
                    "accessModes": ["ReadWriteOnce"],
                    "storageClassName": "local-path", "retain": False,
                },
            },
            "resources": {
                "requests": {
                    "cpu": "100m", "memory": "256Mi", "ephemeral-storage": "128Mi"
                },
                "limits": {
                    "cpu": "2", "memory": "1Gi", "ephemeral-storage": "1Gi"
                },
            },
            "traefik": {
                "enabled": traefik_enabled,
                "ingressClassName": "traefik",
                "entryPoints": ["websecure"],
                "tls": {"secretName": "plinth-tls"},
                "limits": {
                    "defaultRequestBodyBytes": 1024,
                    "maxRequestBodyBytes": 67108864,
                    "memoryRequestBodyBytes": 512,
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
                        "namespaceSelector": {"matchLabels": {
                            "kubernetes.io/metadata.name": "kube-system"
                        }},
                        "podSelector": {"matchLabels": {
                            "app.kubernetes.io/name": "traefik"
                        }},
                    }],
                },
                "dns": {
                    "peers": [{
                        "namespaceSelector": {"matchLabels": {
                            "kubernetes.io/metadata.name": "kube-system"
                        }},
                        "podSelector": {"matchLabels": {"k8s-app": "kube-dns"}},
                    }],
                },
                "database": {
                    "port": 5432,
                    "peers": [{
                        "namespaceSelector": {"matchLabels": {
                            "kubernetes.io/metadata.name": self.namespace
                        }},
                        "podSelector": {"matchLabels": {"app": "postgres"}},
                    }],
                    "ipBlocks": [],
                },
            },
            "rolloutNonce": nonce,
        }

    def write_values(self, values):
        self.values_path.write_text(
            yaml.safe_dump(values, sort_keys=False), encoding="utf-8"
        )

    def install_chart(self):
        # Install the supported isolated boundary first. The administrator is
        # bootstrapped through a local API-server port-forward before any
        # Internet-facing Traefik object exists.
        self.write_values(self.values(
            registration=False,
            nonce="install",
            traefik_enabled=False,
            bootstrap_secret_name=self.bootstrap_secret_name,
        ))
        self.helm(
            "upgrade", "--install", self.release, CHART,
            "--namespace", self.namespace,
            "--values", self.values_path,
            "--atomic", "--wait", "--timeout", "5m",
            timeout=330,
        )
        self.chart_installed = True
        self.kubectl(
            "rollout", "status", "statefulset/" + self.workload_name,
            "-n", self.namespace, "--timeout=240s", timeout=260,
        )
        exposed = self.kubectl(
            "get", "ingressroute.traefik.io,middleware.traefik.io,"
            "serverstransport.traefik.io",
            "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release}",
            "-o", "name",
        )
        require(
            not exposed.stdout.strip(),
            "isolated bootstrap unexpectedly created Traefik exposure",
        )

    @property
    def workload_name(self):
        return self.release + "-plinth"

    def active_pod(self):
        response = json.loads(self.kubectl(
            "get", "pods", "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release},app.kubernetes.io/name=plinth",
            "-o", "json",
        ).stdout)
        active = [
            item for item in response["items"]
            if item["metadata"].get("deletionTimestamp") is None
        ]
        require(len(active) == 1, f"expected one active Plinth pod, got {len(active)}")
        require(any(
            condition.get("type") == "Ready" and condition.get("status") == "True"
            for condition in active[0].get("status", {}).get("conditions", [])
        ), "active Plinth pod is not ready")
        container = next(
            (
                item
                for item in active[0]["spec"].get("containers", [])
                if item.get("name") == "plinth"
            ),
            None,
        )
        require(container is not None, "active Pod has no Plinth container spec")
        require(
            container.get("image")
            == f"{self.internal_repository}@{self.candidate_digest}",
            f"Pod spec did not retain the exact candidate digest: {container}",
        )
        status = next(
            (
                item
                for item in active[0].get("status", {}).get("containerStatuses", [])
                if item.get("name") == "plinth"
            ),
            None,
        )
        require(status is not None, "active Pod has no Plinth container status")
        require(
            self.candidate_digest in status.get("imageID", ""),
            f"running Plinth image ID drifted: {status.get('imageID', '')}",
        )
        return active[0]

    def pvc_uids(self):
        response = json.loads(self.kubectl(
            "get", "pvc", "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release}", "-o", "json"
        ).stdout)
        result = {
            item["metadata"]["name"]: item["metadata"]["uid"]
            for item in response["items"]
        }
        require(len(result) == 2, f"expected data and log PVCs, got {result}")
        return result

    def bound_pv_names(self):
        response = json.loads(self.kubectl(
            "get", "pvc", "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release}", "-o", "json"
        ).stdout)
        names = [item.get("spec", {}).get("volumeName", "") for item in response["items"]]
        require(len(names) == 2 and all(names),
                f"expected two bound task-owned persistent volumes, got {names}")
        return names

    @property
    def origin(self):
        return f"https://{self.host}:{self.https_port}"

    def isolated_bootstrap_request(self):
        """POST bootstrap authority through a local port-forward without logging it."""
        port = free_port()
        process = subprocess.Popen(
            [
                self.args.kubectl,
                "port-forward",
                "-n",
                self.namespace,
                "service/" + self.workload_name,
                f"{port}:8080",
                "--address=127.0.0.1",
            ],
            cwd=ROOT,
            env=self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    stdout, stderr = process.communicate(timeout=5)
                    raise RuntimeError(
                        "kubectl port-forward exited before bootstrap"
                        f"\nstdout:\n{stdout}\nstderr:\n{stderr}"
                    )
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                        break
                except OSError:
                    time.sleep(0.25)
            else:
                raise RuntimeError("kubectl port-forward did not become ready")

            payload = json.dumps({
                "bootstrap_token": self.bootstrap_token,
                "username": "issue36-admin",
                "password": "fake-password-for-issue36!",
            })
            request_path = self.root / ("bootstrap-request-" + secrets.token_hex(4))
            descriptor = os.open(
                request_path,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                0o600,
            )
            with os.fdopen(descriptor, "w", encoding="utf-8") as request_file:
                request_file.write(payload)
            output = self.root / "bootstrap-response"
            origin = f"http://127.0.0.1:{port}"
            try:
                result = self.run(
                    [
                        "curl", "--silent", "--show-error",
                        "--output", output, "--write-out", "%{http_code}",
                        "--request", "POST",
                        "--header", "Content-Type: application/json",
                        "--header", "Origin: " + origin,
                        "--data-binary", "@" + str(request_path),
                        origin + "/api/auth/bootstrap",
                    ],
                    check=False,
                    timeout=30,
                )
            finally:
                request_path.unlink(missing_ok=True)
            body = output.read_text(
                encoding="utf-8", errors="replace"
            ) if output.exists() else ""
            try:
                response_error = json.loads(body).get("error", "")
            except (json.JSONDecodeError, AttributeError):
                response_error = "invalid_json"
            return result.returncode, result.stdout.strip(), response_error
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

    def bootstrap_admin(self):
        """Create the first administrator through the isolated Service only."""
        code, status, response_error = self.isolated_bootstrap_request()
        require(
            code == 0 and status == "201",
            "isolated first-administrator bootstrap failed: "
            f"status={status} error={response_error}",
        )

    def remove_bootstrap_authority(self):
        """Restart isolated without bootstrap authority before public exposure."""
        old_pod = self.active_pod()
        old_runtime = self.container_runtime_identity(old_pod)
        old_exit = self.start_container_exit_watch(*old_runtime)
        self.write_values(self.values(
            registration=False,
            nonce="bootstrap-removed",
            traefik_enabled=False,
        ))
        started = time.monotonic()
        self.helm(
            "upgrade", self.release, CHART,
            "--namespace", self.namespace,
            "--values", self.values_path,
            "--atomic", "--wait", "--timeout", "5m",
            timeout=330,
        )
        elapsed = time.monotonic() - started
        require(elapsed < 330,
                f"bootstrap-authority restart exceeded bound: {elapsed:.1f}s")
        old_status = self.finish_container_exit_watch(old_exit)
        replacement = self.active_pod()
        require(
            replacement["metadata"]["uid"] != old_pod["metadata"]["uid"],
            "removing bootstrap authority did not replace the isolated Pod",
        )
        self.verify_ordered_transition(old_status, replacement)
        container = replacement["spec"]["containers"][0]
        env_names = {item["name"] for item in container.get("env", [])}
        require("PLINTH_BOOTSTRAP_TOKEN" not in env_names,
                "replacement Pod retained bootstrap authority")

        exposed = self.kubectl(
            "get", "ingressroute.traefik.io,middleware.traefik.io,"
            "serverstransport.traefik.io",
            "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release}",
            "-o", "name",
        )
        require(not exposed.stdout.strip(),
                "bootstrap-authority restart created Traefik exposure")
        self.kubectl(
            "delete", "secret/" + self.bootstrap_secret_name,
            "-n", self.namespace, "--wait=true", "--timeout=30s",
            timeout=40,
        )
        absent = self.kubectl(
            "get", "secret/" + self.bootstrap_secret_name,
            "-n", self.namespace, check=False,
        )
        require(absent.returncode != 0 and "NotFound" in absent.stderr,
                "task-owned bootstrap Secret survived authority removal")
        code, status, response_error = self.isolated_bootstrap_request()
        require(
            code == 0 and status == "403" and response_error == "bootstrap_denied",
            "removed bootstrap authority remained usable: "
            f"status={status} error={response_error}",
        )

    def enable_public_route(self):
        old_pod = self.active_pod()
        old_runtime = self.container_runtime_identity(old_pod)
        old_exit = self.start_container_exit_watch(*old_runtime)
        self.write_values(self.values(
            registration=False, nonce="public", traefik_enabled=True
        ))
        self.helm(
            "upgrade", self.release, CHART,
            "--namespace", self.namespace,
            "--values", self.values_path,
            "--atomic", "--wait", "--timeout", "5m",
            timeout=330,
        )
        old_status = self.finish_container_exit_watch(old_exit)
        replacement = self.active_pod()
        require(
            replacement["metadata"]["uid"] != old_pod["metadata"]["uid"],
            "enabling the public origin did not replace the isolated Pod",
        )
        self.verify_ordered_transition(old_status, replacement)
        self.wait_for_https()

    def curl(
        self,
        path,
        *,
        data=None,
        data_file=None,
        form_file=None,
        origin=None,
        host_header=None,
        csrf=False,
        cookie_jar=None,
        use_cookies=False,
    ):
        output = self.root / ("curl-" + secrets.token_hex(4))
        argv = [
            "curl", "--silent", "--show-error", "--cacert", self.certificate,
            "--resolve", f"{self.host}:{self.https_port}:127.0.0.1",
            "--output", output, "--write-out", "%{http_code}",
        ]
        require(
            sum(item is not None for item in (data, data_file, form_file)) <= 1,
            "curl accepts only one request body source",
        )
        if data is not None:
            argv.extend(["--request", "POST", "--header", "Content-Type: application/json",
                         "--data-binary", data])
        if data_file is not None:
            argv.extend([
                "--request", "POST",
                "--header", "Content-Type: application/octet-stream",
                "--data-binary", "@" + str(data_file),
            ])
        if form_file is not None:
            argv.extend([
                "--request", "POST",
                "--form", "package=@" + str(form_file) + ";filename=package.zip",
            ])
        if origin is not None:
            argv.extend(["--header", "Origin: " + origin])
        if host_header is not None:
            argv.extend(["--header", "Host: " + host_header])
        if csrf:
            token = self.cookie("plinth_csrf")
            require(token, "CSRF-protected request has no CSRF cookie")
            argv.extend(["--header", "X-Plinth-CSRF: " + token])
        if cookie_jar is not None:
            argv.extend(["--cookie-jar", cookie_jar])
        if use_cookies:
            argv.extend(["--cookie", self.cookies])
        argv.append(self.origin + path)
        result = self.run(argv, timeout=30, check=False)
        body = output.read_text(encoding="utf-8", errors="replace") if output.exists() else ""
        return result.returncode, result.stdout.strip(), body

    def wait_for_https(self):
        deadline = time.monotonic() + 120
        last = None
        while time.monotonic() < deadline:
            last = self.curl("/healthz")
            if last[0] == 0 and last[1] == "200":
                return
            time.sleep(0.5)
        raise AssertionError(f"TLS health endpoint did not become ready: {last}")

    def verify_runtime_boundary(self):
        self.wait_for_https()
        pod = self.active_pod()
        pod_name = pod["metadata"]["name"]
        self.kubectl(
            "exec", "-n", self.namespace, pod_name, "--", "sh", "-euc",
            "test \"$(id -u)\" = 10001; "
            "test ! -e /var/run/secrets/kubernetes.io/serviceaccount/token; "
            "! touch /root-filesystem-must-be-read-only",
        )
        self.kubectl(
            "exec", "-n", self.namespace, pod_name, "--", "sh", "-euc",
            "if timeout 5 openssl s_client "
            "-connect kubernetes.default.svc:443 </dev/null "
            ">/tmp/egress-probe 2>&1; then "
            "echo 'unauthorized Kubernetes API egress succeeded' >&2; exit 42; fi",
        )

        wrong_credentials = json.dumps({
            "username": "issue36-rejected",
            "password": "fake-password-for-issue36!",
        })
        code, status, body = self.curl(
            "/api/auth/register", data=wrong_credentials,
            origin="https://cross-origin.invalid",
        )
        require(code == 0 and status == "403" and "csrf_failed" in body,
                f"wrong browser origin did not fail closed: {status} {body}")

        code, status, _ = self.curl(
            "/healthz", host_header="wrong-authority.invalid"
        )
        require(
            code == 0 and status == "404",
            f"Traefik accepted the wrong Host authority: {status}",
        )

        code, status, body = self.curl(
            "/api/auth/register", data=wrong_credentials, origin=self.origin
        )
        require(
            code == 0 and status == "403" and "registration_unavailable" in body,
            f"public registration was not closed after bootstrap: {status} {body}",
        )

        credentials = json.dumps({
            "username": "issue36-admin",
            "password": "fake-password-for-issue36!",
        })
        code, status, body = self.curl(
            "/api/auth/login", data=credentials, origin=self.origin,
            cookie_jar=self.cookies,
        )
        require(code == 0 and status == "200",
                f"TLS login failed: {status} {body}")
        require(self.cookie("plinth_session"), "login did not issue a session cookie")

        oversized = json.dumps({"padding": "x" * 2048})
        code, status, _ = self.curl(
            "/api/auth/login", data=oversized, origin=self.origin
        )
        require(code == 0 and status == "413",
                f"Traefik default request limit returned {status}, expected 413")

        route_probe = self.root / "route-probe.zip"
        with route_probe.open("wb") as upload:
            upload.truncate(2048)
        def is_plinth_invalid_zip_response(status, body):
            try:
                parsed = json.loads(body)
            except json.JSONDecodeError:
                return False
            return (
                status == "400"
                and parsed.get("state") == "INSTALL_FAILED"
                and parsed.get("failed_at_stage") == "UPLOADING"
                and parsed.get("kind") == "not-a-zip"
            )

        route_deadline = time.monotonic() + 30
        last_route_response = None
        while True:
            code, status, body = self.curl(
                "/api/packages", form_file=route_probe, origin=self.origin,
                csrf=True, use_cookies=True,
            )
            last_route_response = (code, status, body)
            if code == 0 and is_plinth_invalid_zip_response(status, body):
                break
            require(
                time.monotonic() < route_deadline,
                "package route did not become ready with its larger upload budget: "
                + repr(last_route_response),
            )
            time.sleep(0.5)

        maximum_package = self.root / "maximum-package.zip"
        with maximum_package.open("wb") as upload:
            upload.truncate(50 * 1024 * 1024)
        code, status, body = self.curl(
            "/api/packages",
            form_file=maximum_package,
            origin=self.origin,
            csrf=True,
            use_cookies=True,
        )
        require(
            code == 0 and is_plinth_invalid_zip_response(status, body),
            "50 MiB multipart package did not reach Plinth's installer: "
            + repr((code, status, body)),
        )

        above_package_limit = self.root / "above-package-limit.bin"
        with above_package_limit.open("wb") as upload:
            upload.truncate(67108865)
        code, status, _ = self.curl(
            "/api/packages", data_file=above_package_limit, origin=self.origin
        )
        require(
            code == 0 and status == "413",
            f"Traefik package request limit returned {status}, expected 413",
        )

        self.verify_websocket()
        self.verify_network_policy_denial()

    def cookie(self, name):
        if not self.cookies.is_file():
            return ""
        for line in self.cookies.read_text(encoding="utf-8").splitlines():
            # Netscape cookie jars encode HttpOnly cookies with this prefix;
            # the remainder is still a normal seven-field cookie record.
            if line.startswith("#HttpOnly_"):
                line = line.removeprefix("#HttpOnly_")
            elif not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) == 7 and fields[5] == name:
                return fields[6]
        return ""

    def verify_websocket(self):
        context = ssl.create_default_context(cafile=str(self.certificate))
        with socket.create_connection(("127.0.0.1", self.https_port), timeout=10) as peer:
            with context.wrap_socket(peer, server_hostname=self.host) as secured:
                secured.settimeout(10)
                key = base64.b64encode(os.urandom(16)).decode("ascii")
                host = f"{self.host}:{self.https_port}"
                request = (
                    "GET /ws/events HTTP/1.1\r\n"
                    f"Host: {host}\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    f"Sec-WebSocket-Key: {key}\r\n"
                    "Sec-WebSocket-Version: 13\r\n"
                    f"Origin: {self.origin}\r\n"
                    f"Cookie: plinth_session={self.cookie('plinth_session')}\r\n\r\n"
                )
                secured.sendall(request.encode("ascii"))
                buffered = bytearray()
                while b"\r\n\r\n" not in buffered:
                    require(len(buffered) < 16384, "unbounded WebSocket response headers")
                    chunk = secured.recv(4096)
                    require(chunk, "Traefik closed the WebSocket during upgrade")
                    buffered.extend(chunk)
                header, remaining = bytes(buffered).split(b"\r\n\r\n", 1)
                require(header.startswith(b"HTTP/1.1 101 "),
                        f"WebSocket upgrade failed through Traefik: {header!r}")

                def receive(length):
                    nonlocal remaining
                    while len(remaining) < length:
                        chunk = secured.recv(4096)
                        require(chunk, "WebSocket closed before the connected frame")
                        remaining += chunk
                    result, remaining = remaining[:length], remaining[length:]
                    return result

                first, second = receive(2)
                require(first == 0x81, "expected a final unmasked JSON text frame")
                length = second & 0x7F
                if length == 126:
                    length = struct.unpack("!H", receive(2))[0]
                elif length == 127:
                    length = struct.unpack("!Q", receive(8))[0]
                require(length < 65536, "unbounded WebSocket authentication frame")
                message = json.loads(receive(length))
                require(message.get("type") == "connected",
                        f"unexpected WebSocket authentication response: {message}")

    def verify_network_policy_denial(self):
        attacker = {
            "apiVersion": "v1",
            "kind": "Pod",
            "metadata": {
                "name": "attacker", "namespace": self.namespace,
                "labels": {"app": "attacker"},
            },
            "spec": {
                "automountServiceAccountToken": False,
                "restartPolicy": "Never",
                "securityContext": {"seccompProfile": {"type": "RuntimeDefault"}},
                "containers": [{
                    "name": "attacker", "image": CURL_IMAGE,
                    "imagePullPolicy": "IfNotPresent",
                    "command": ["sleep", "600"],
                    "securityContext": {
                        "allowPrivilegeEscalation": False,
                        "capabilities": {"drop": ["ALL"]},
                        "runAsNonRoot": True,
                        # The pinned curl image declares the symbolic user
                        # curl_user. Kubernetes cannot prove that a symbolic
                        # image user is non-root without an explicit UID.
                        "runAsUser": 101,
                        "runAsGroup": 102,
                    },
                    "resources": {
                        "requests": {"cpu": "10m", "memory": "16Mi"},
                        "limits": {"cpu": "100m", "memory": "64Mi"},
                    },
                }],
            },
        }
        self.apply_json(attacker)
        self.kubectl(
            "wait", "--for=condition=Ready", "pod/attacker", "-n", self.namespace,
            "--timeout=120s", timeout=140,
        )
        denied = self.kubectl(
            "exec", "-n", self.namespace, "attacker", "--",
            "curl", "--silent", "--show-error", "--fail", "--max-time", "4",
            f"http://{self.workload_name}:8080/healthz",
            check=False, timeout=20,
        )
        require(denied.returncode != 0,
                "unlabelled pod bypassed the default-deny ingress policy")

    def verify_restart(self):
        before = self.active_pod()
        before_uid = before["metadata"]["uid"]
        before_name = before["metadata"]["name"]
        runtime = self.container_runtime_identity(before)
        exit_watch = self.start_container_exit_watch(*runtime)
        claims = self.pvc_uids()
        marker = "issue36-" + secrets.token_hex(8)
        self.kubectl(
            "exec", "-n", self.namespace, before_name, "--", "sh", "-euc",
            f"printf '%s' '{marker}' > /var/lib/plinth/data/lifecycle-marker",
        )

        started = time.monotonic()
        self.kubectl(
            "delete", "pod", before_name, "-n", self.namespace,
            "--wait=true", "--timeout=65s", timeout=75,
        )
        elapsed = time.monotonic() - started
        require(elapsed < 65, f"pod shutdown exceeded its Kubernetes bound: {elapsed:.1f}s")
        old_status = self.finish_container_exit_watch(exit_watch)
        self.kubectl(
            "rollout", "status", "statefulset/" + self.workload_name,
            "-n", self.namespace, "--timeout=240s", timeout=260,
        )
        after = self.active_pod()
        require(after["metadata"]["uid"] != before_uid,
                "pod deletion did not create a distinct replacement")
        require(self.pvc_uids() == claims, "persistent claims changed across restart")
        observed = self.kubectl(
            "exec", "-n", self.namespace, after["metadata"]["name"], "--",
            "cat", "/var/lib/plinth/data/lifecycle-marker",
        ).stdout
        require(observed == marker, "persistent data did not survive pod restart")
        self.verify_ordered_transition(old_status, after)
        self.wait_for_https()
        self.verify_login()
        return claims, after["metadata"]["uid"]

    def container_runtime_identity(self, pod):
        statuses = pod.get("status", {}).get("containerStatuses", [])
        status = next((item for item in statuses if item.get("name") == "plinth"), None)
        require(status is not None, "Plinth container status is unavailable")
        container_id = status.get("containerID", "")
        require(container_id.startswith("containerd://"),
                f"unexpected Plinth container runtime ID: {container_id}")
        node = pod.get("spec", {}).get("nodeName", "")
        require(node.startswith("k3d-" + self.cluster + "-"),
                f"Plinth pod ran on an unexpected node: {node}")
        return node, container_id.removeprefix("containerd://")

    def start_container_exit_watch(self, node, container_id):
        """Subscribe to containerd before exit; CRI records may be GC'd at once."""
        self.inspect_running_container(node, container_id)
        token = secrets.token_hex(8)
        process = subprocess.Popen(
            ["docker", "exec", "-e", f"PLINTH_EXIT_WATCH_TOKEN={token}",
             node, "sh", "-ec",
             "printf 'PLINTH_WATCH_PID=%s\\n' \"$$\"; "
             "exec k3s ctr --namespace k8s.io events"],
            cwd=ROOT, env=self.env, text=True, bufsize=1,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        watcher = {
            "node": node, "container_id": container_id, "token": token,
            "process": process, "node_pid": None, "status": None,
            "last": None, "stderr": "", "sentinels": {},
            "header_ready": threading.Event(), "status_ready": threading.Event(),
        }
        self._exit_watchers.append(watcher)

        def read_events():
            for line in process.stdout:
                if line.startswith("PLINTH_WATCH_PID="):
                    raw = line.removeprefix("PLINTH_WATCH_PID=").strip()
                    if raw.isdecimal():
                        watcher["node_pid"] = int(raw)
                        watcher["header_ready"].set()
                    continue
                event = _containerd_task_exit_event(line)
                if event is None:
                    watcher["last"] = line.strip()[:300]
                    continue
                if event.get("container_id") != container_id:
                    continue
                sentinel = watcher["sentinels"].get(event.get("id"))
                if sentinel is not None and event.get("exit_status", 0) == 0:
                    sentinel.set()
                status = parse_containerd_exit_event(line, container_id)
                if status is not None:
                    watcher["status"] = status
                    watcher["status_ready"].set()
            if watcher["status"] is None:
                watcher["last"] = "containerd event stream closed: " + str(watcher["last"])
            watcher["header_ready"].set()
            watcher["status_ready"].set()

        def read_errors():
            for line in process.stderr:
                watcher["stderr"] = (watcher["stderr"] + line)[-1000:]

        watcher["reader"] = threading.Thread(target=read_events, daemon=True)
        watcher["error_reader"] = threading.Thread(target=read_errors, daemon=True)
        watcher["reader"].start()
        watcher["error_reader"].start()
        try:
            require(watcher["header_ready"].wait(5) and watcher["node_pid"] is not None,
                    "containerd exit observer did not start: " + watcher["stderr"])
            # A PID header only proves the client was launched. An exec child
            # exit from this exact container proves its subscription is live.
            for attempt in range(5):
                sentinel_id = f"plinth-watch-{token}-{attempt}"
                seen = threading.Event()
                watcher["sentinels"][sentinel_id] = seen
                probe = self.run(
                    ["docker", "exec", node, "k3s", "ctr", "--namespace",
                     "k8s.io", "tasks", "exec", "--exec-id", sentinel_id,
                     container_id, "/bin/true"],
                    check=False, timeout=10,
                )
                require(probe.returncode == 0,
                        "containerd exit observer probe failed: "
                        + probe.stderr[:500])
                if seen.wait(2):
                    return watcher
                require(process.poll() is None,
                        "containerd exit observer closed before readiness: "
                        + watcher["stderr"])
            raise AssertionError(
                "containerd exit observer never saw its readiness probe: "
                + str(watcher["last"]) + " " + watcher["stderr"]
            )
        except BaseException:
            self._stop_container_exit_watch(watcher)
            raise

    def _stop_container_exit_watch(self, watcher):
        """Stop both the node-side ctr client and the host Docker exec client."""
        process = watcher["process"]
        node_pid = watcher["node_pid"]
        stop_error = None

        def node_client(*action):
            # Exit 1 means this PID is gone or was reused by another process.
            # Never signal a process without its task-specific environment tag.
            return self.run(
                ["docker", "exec", watcher["node"], "sh", "-ec",
                 "if ! test -r \"/proc/$1/environ\"; then exit 1; fi; "
                 "tr '\\000' '\\n' < \"/proc/$1/environ\" "
                 "| grep -Fx -- \"PLINTH_EXIT_WATCH_TOKEN=$2\" >/dev/null "
                 "|| exit 1; " + " ".join(action),
                 "sh", str(node_pid), watcher["token"]],
                check=False, timeout=10,
            )

        try:
            if node_pid is not None:
                stopped = node_client('kill -TERM "$1"')
                if stopped.returncode not in (0, 1):
                    stop_error = (
                        "task-owned containerd event client could not be stopped: "
                        + stopped.stderr[:500]
                    )
        except subprocess.TimeoutExpired as error:
            stop_error = "containerd event client stop timed out: " + str(error)
        try:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if node_pid is not None:
                    try:
                        forced = node_client('kill -KILL "$1"')
                        if forced.returncode not in (0, 1):
                            stop_error = (
                                "task-owned containerd event client force-stop failed: "
                                + forced.stderr[:500]
                            )
                    except subprocess.TimeoutExpired as error:
                        stop_error = "containerd event client force-stop timed out: " + str(error)
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if node_pid is not None:
                try:
                    still_alive = node_client("true")
                    if still_alive.returncode == 0:
                        stop_error = "task-owned containerd event client remains alive"
                    elif still_alive.returncode != 1:
                        stop_error = (
                            "containerd event client absence check failed: "
                            + still_alive.stderr[:500]
                        )
                except subprocess.TimeoutExpired as error:
                    stop_error = "containerd event client absence check timed out: " + str(error)
        finally:
            watcher["reader"].join(5)
            watcher["error_reader"].join(5)
            if watcher["reader"].is_alive() or watcher["error_reader"].is_alive():
                stop_error = stop_error or "containerd exit observer reader did not stop"
            if stop_error is None and watcher in self._exit_watchers:
                self._exit_watchers.remove(watcher)
        require(stop_error is None, stop_error)

    def finish_container_exit_watch(self, watcher):
        watcher["status_ready"].wait(15)
        self._stop_container_exit_watch(watcher)
        status = watcher["status"]
        require(
            status is not None and status.get("state") == "CONTAINER_EXITED",
            "containerd exit observer did not see the terminated Plinth init: "
            + repr(watcher["last"]) + " " + watcher["stderr"],
        )
        require(status.get("exitCode") == 0,
                f"Plinth container did not exit cleanly: {status}")
        self.timestamp_ns(status, "finishedAt")
        return status

    @staticmethod
    def timestamp_ns(status, key):
        raw = status.get(key, 0)
        try:
            value = int(raw)
        except (TypeError, ValueError):
            match = re.fullmatch(
                r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.(\d{1,9}))?Z",
                str(raw),
            )
            if match is None:
                value = 0
            else:
                seconds = datetime.strptime(
                    match.group(1), "%Y-%m-%dT%H:%M:%S"
                ).replace(tzinfo=timezone.utc)
                fraction = (match.group(2) or "").ljust(9, "0")
                value = int(seconds.timestamp()) * 1_000_000_000 + int(fraction or 0)
        require(value > 0, f"container has no {key} timestamp: {status}")
        return value

    def inspect_running_container(self, node, container_id):
        inspected = self.run(
            ["docker", "exec", node, "crictl", "inspect", container_id],
            timeout=15,
        )
        status = json.loads(inspected.stdout)["status"]
        require(status.get("state") == "CONTAINER_RUNNING",
                f"replacement Plinth container is not running: {status}")
        self.timestamp_ns(status, "startedAt")
        return status

    def verify_ordered_transition(self, old_status, new_pod):
        new_runtime = self.container_runtime_identity(new_pod)
        new_status = self.inspect_running_container(*new_runtime)
        require(
            self.timestamp_ns(old_status, "finishedAt")
            <= self.timestamp_ns(new_status, "startedAt"),
            "replacement container started before the prior kernel finished: "
            f"old={old_status['finishedAt']} new={new_status['startedAt']}",
        )

    def verify_login(self):
        credentials = json.dumps({
            "username": "issue36-admin",
            "password": "fake-password-for-issue36!",
        })
        code, status, body = self.curl(
            "/api/auth/login", data=credentials, origin=self.origin,
            cookie_jar=self.cookies,
        )
        require(code == 0 and status == "200",
                f"persisted account login failed: {status} {body}")

    def verify_sequential_rollout(self, claims, old_uid):
        old_pod = self.active_pod()
        require(old_pod["metadata"]["uid"] == old_uid,
                "active pod changed before the controlled rollout")
        old_runtime = self.container_runtime_identity(old_pod)
        old_exit = self.start_container_exit_watch(*old_runtime)
        self.write_values(self.values(
            registration=False, nonce="replacement", traefik_enabled=True
        ))
        process = subprocess.Popen(
            [
                self.args.helm, "upgrade", self.release, str(CHART),
                "--namespace", self.namespace,
                "--values", str(self.values_path),
                "--atomic", "--wait", "--timeout", "5m",
            ],
            cwd=ROOT,
            env=self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 330
        violation = ""
        while process.poll() is None and time.monotonic() < deadline:
            response = self.kubectl(
                "get", "pods", "-n", self.namespace,
                "-l", f"app.kubernetes.io/instance={self.release},app.kubernetes.io/name=plinth",
                "-o", "json", check=False, timeout=15,
            )
            if response.returncode == 0:
                pods = json.loads(response.stdout)["items"]
                active = [pod for pod in pods if not pod["metadata"].get("deletionTimestamp")]
                if len(active) > 1:
                    violation = "rolling replacement created more than one active pod"
                    break
                old = next((pod for pod in pods if pod["metadata"]["uid"] == old_uid), None)
                new = [pod for pod in pods if pod["metadata"]["uid"] != old_uid]
                if old and not old["metadata"].get("deletionTimestamp") and new:
                    violation = "replacement pod was created before old-pod termination began"
                    break
                if old and new and any(
                    any(condition.get("type") == "Ready" and condition.get("status") == "True"
                        for condition in pod.get("status", {}).get("conditions", []))
                    for pod in new
                ):
                    violation = "replacement became ready while the old pod still existed"
                    break
            time.sleep(0.25)
        if violation:
            process.terminate()
        if process.poll() is None:
            process.kill()
            stdout, stderr = process.communicate(timeout=10)
            raise AssertionError(
                "Helm rollout exceeded 330 seconds\nstdout:\n" + stdout
                + "\nstderr:\n" + stderr
            )
        stdout, stderr = process.communicate(timeout=10)
        require(not violation, violation)
        require(process.returncode == 0,
                f"Helm rolling replacement failed\nstdout:\n{stdout}\nstderr:\n{stderr}")
        old_status = self.finish_container_exit_watch(old_exit)

        after = self.active_pod()
        require(after["metadata"]["uid"] != old_uid,
                "Helm upgrade did not replace the pod")
        require(self.pvc_uids() == claims, "persistent claims changed across rollout")
        observed = self.kubectl(
            "exec", "-n", self.namespace, after["metadata"]["name"], "--",
            "cat", "/var/lib/plinth/data/lifecycle-marker",
        ).stdout
        require(observed.startswith("issue36-"), "persistent marker vanished during rollout")
        self.verify_ordered_transition(old_status, after)
        self.wait_for_https()
        self.verify_login()

        credentials = json.dumps({
            "username": "issue36-closed",
            "password": "fake-password-for-issue36!",
        })
        code, status, body = self.curl(
            "/api/auth/register", data=credentials, origin=self.origin
        )
        require(code == 0 and status == "403" and "registration_unavailable" in body,
                f"registration did not close after rollout: {status} {body}")

    def verify_clean_removal(self):
        persistent_volumes = self.bound_pv_names()

        # Remove Internet ingress first and wait for the configuration-driven
        # ordered replacement before beginning the final shutdown.
        public_pod = self.active_pod()
        public_runtime = self.container_runtime_identity(public_pod)
        public_exit = self.start_container_exit_watch(*public_runtime)
        self.write_values(self.values(
            registration=False, nonce="removal", traefik_enabled=False
        ))
        self.helm(
            "upgrade", self.release, CHART,
            "--namespace", self.namespace,
            "--values", self.values_path,
            "--atomic", "--wait", "--timeout", "5m",
            timeout=330,
        )
        public_status = self.finish_container_exit_watch(public_exit)
        isolated_pod = self.active_pod()
        require(
            isolated_pod["metadata"]["uid"] != public_pod["metadata"]["uid"],
            "disabling Traefik did not apply the isolated configuration",
        )
        self.verify_ordered_transition(public_status, isolated_pod)
        exposed = self.kubectl(
            "get", "ingressroute.traefik.io,middleware.traefik.io,"
            "serverstransport.traefik.io",
            "-n", self.namespace,
            "-l", f"app.kubernetes.io/instance={self.release}",
            "-o", "name",
        )
        require(not exposed.stdout.strip(),
                "public Traefik resources survived the isolation upgrade")
        route_deadline = time.monotonic() + 30
        while True:
            code, status, _ = self.curl("/healthz")
            if not (code == 0 and status == "200"):
                break
            require(
                time.monotonic() < route_deadline,
                "public HTTPS route remained reachable after isolation",
            )
            time.sleep(0.5)

        # StatefulSet deletion itself does not promise ordered Pod shutdown.
        # Scale to zero, prove the exact final container exited cleanly, and
        # only then remove the release metadata and remaining objects.
        final_pod = self.active_pod()
        final_runtime = self.container_runtime_identity(final_pod)
        final_exit = self.start_container_exit_watch(*final_runtime)
        removal_started_ns = time.time_ns()
        self.kubectl(
            "scale", "statefulset/" + self.workload_name,
            "-n", self.namespace, "--replicas=0",
        )
        self.kubectl(
            "wait", "--for=delete", "pod/" + final_pod["metadata"]["name"],
            "-n", self.namespace, "--timeout=65s", timeout=75,
        )
        final_status = self.finish_container_exit_watch(final_exit)
        removal_duration_ns = (
            self.timestamp_ns(final_status, "finishedAt") - removal_started_ns
        )
        require(
            0 <= removal_duration_ns < 65 * 1_000_000_000,
            "final Plinth shutdown exceeded the 65-second removal bound",
        )

        self.helm(
            "uninstall", self.release, "-n", self.namespace,
            "--wait", "--timeout", "180s", timeout=200,
        )
        self.chart_installed = False

        deadline = time.monotonic() + 60
        resources = None
        endpoint_output = ""
        endpoint_results = []
        remaining_volumes = []
        volume_errors = []
        while time.monotonic() < deadline:
            resources = self.kubectl(
                "get",
                "pods,statefulsets.apps,controllerrevisions.apps,service,"
                "serviceaccount,configmap,persistentvolumeclaim,"
                "networkpolicy.networking.k8s.io,ingressroute.traefik.io,"
                "middleware.traefik.io,serverstransport.traefik.io",
                "-n", self.namespace,
                "-l", f"app.kubernetes.io/instance={self.release}",
                "-o", "name", check=False,
            )
            endpoint_results = [
                self.kubectl(
                    "get", "endpointslice.discovery.k8s.io",
                    "-n", self.namespace,
                    "-l", "kubernetes.io/service-name=" + service,
                    "-o", "name", check=False,
                )
                for service in (
                    self.workload_name,
                    self.workload_name + "-headless",
                )
            ]
            endpoint_output = "".join(item.stdout for item in endpoint_results)
            remaining_volumes = []
            volume_errors = []
            for name in persistent_volumes:
                result = self.kubectl("get", "pv/" + name, check=False)
                if result.returncode == 0:
                    remaining_volumes.append(name)
                elif "NotFound" not in result.stderr:
                    volume_errors.append(name + ": " + result.stderr.strip())
            if (
                resources.returncode == 0
                and all(item.returncode == 0 for item in endpoint_results)
                and not resources.stdout.strip()
                and not endpoint_output.strip()
                and not remaining_volumes
                and not volume_errors
            ):
                break
            time.sleep(0.5)
        require(
            resources is not None
            and resources.returncode == 0
            and all(item.returncode == 0 for item in endpoint_results)
            and not resources.stdout.strip()
            and not endpoint_output.strip()
            and not remaining_volumes,
            "Helm uninstall left release-owned resources:\n"
            + (resources.stdout if resources is not None else "")
            + endpoint_output
            + ("persistent volumes: " + ", ".join(remaining_volumes)
               if remaining_volumes else ""),
        )
        require(
            not volume_errors,
            "persistent-volume absence checks failed:\n" + "\n".join(volume_errors),
        )
        releases = json.loads(self.helm(
            "list", "-n", self.namespace, "--output", "json"
        ).stdout)
        require(not releases, "Helm release metadata survived uninstall")

        self.kubectl(
            "delete", "namespace", self.namespace,
            "--wait=true", "--timeout=180s", timeout=200,
        )
        self.namespace_created = False
        absent = self.kubectl("get", "namespace", self.namespace, check=False)
        require(absent.returncode != 0 and "NotFound" in absent.stderr,
                "task-owned namespace still exists after deletion")

    def diagnostics(self):
        if not self.cluster_created or not self.kubeconfig.is_file():
            return
        commands = [
            [self.args.kubectl, "get", "pods,statefulsets,services,pvc,networkpolicy", "-A", "-o", "wide"],
            [self.args.kubectl, "get", "events", "-A", "--sort-by=.lastTimestamp"],
            [self.args.kubectl, "logs", "-n", self.namespace,
             "statefulset/" + self.workload_name, "--tail=200"],
            [self.args.kubectl, "logs", "-n", "kube-system",
             "deployment/traefik", "--tail=200"],
        ]
        for argv in commands:
            result = self.run(argv, check=False, timeout=30)
            print("diagnostic: " + " ".join(argv), file=sys.stderr)
            print(result.stdout, file=sys.stderr)
            print(result.stderr, file=sys.stderr)

    def cleanup(self):
        if self.cleanup_complete:
            return
        errors = []
        for watcher in list(self._exit_watchers):
            try:
                self._stop_container_exit_watch(watcher)
            except (RuntimeError, AssertionError) as error:
                errors.append("containerd exit observer cleanup failed: " + str(error))
        if self.cluster_created and self.kubeconfig.is_file():
            if self.chart_installed:
                result = self.helm(
                    "uninstall", self.release, "-n", self.namespace,
                    "--wait", "--timeout", "90s", check=False, timeout=110,
                )
                if result.returncode != 0:
                    errors.append("emergency Helm uninstall failed: " + result.stderr)
            if self.namespace_created:
                result = self.kubectl(
                    "delete", "namespace", self.namespace,
                    "--wait=true", "--timeout=90s", check=False, timeout=110,
                )
                if result.returncode != 0 and "NotFound" not in result.stderr:
                    errors.append("emergency namespace deletion failed: " + result.stderr)
        result = self.run(
            [self.args.k3d, "cluster", "delete", self.cluster],
            check=False, timeout=180,
        )
        if result.returncode != 0:
            errors.append("cluster cleanup failed: " + result.stderr)
        self.cluster_created = False
        result = self.run(
            [self.args.k3d, "registry", "delete", self.registry],
            check=False, timeout=120,
        )
        if result.returncode != 0:
            errors.append("registry cleanup failed: " + result.stderr)
        self.registry_created = False
        local_image = self.run(
            ["docker", "image", "inspect", self.local_tag],
            check=False,
            timeout=30,
        )
        if local_image.returncode == 0:
            removed = self.run(
                ["docker", "image", "rm", self.local_tag],
                check=False,
                timeout=60,
            )
            if removed.returncode != 0:
                errors.append("candidate image-tag cleanup failed: " + removed.stderr)

        containers = self.run([
            "docker", "ps", "-a", "--format", "{{.Names}}"
        ], check=False, timeout=30).stdout.splitlines()
        leaked = [
            name for name in containers
            if name == "k3d-" + self.registry
            or name.startswith("k3d-" + self.cluster + "-")
        ]
        if leaked:
            errors.append("task-owned Docker containers remain: " + ", ".join(leaked))
        networks = self.run([
            "docker", "network", "ls", "--format", "{{.Name}}"
        ], check=False, timeout=30).stdout.splitlines()
        if "k3d-" + self.cluster in networks:
            errors.append("task-owned Docker network remains: k3d-" + self.cluster)

        self.temporary.cleanup()
        if errors:
            raise RuntimeError("\n".join(errors))
        self.cleanup_complete = True

    def execute(self):
        self.create_infrastructure()
        self.create_namespace_dependencies()
        self.install_chart()
        self.bootstrap_admin()
        self.remove_bootstrap_authority()
        self.enable_public_route()
        self.verify_runtime_boundary()
        claims, pod_uid = self.verify_restart()
        self.verify_sequential_rollout(claims, pod_uid)
        self.verify_clean_removal()
        print(
            "k3d lifecycle: install, TLS authority, limits, restart, rollout, "
            "shutdown, and cleanup checks passed"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True,
                        help="exact locally built amd64 runtime image tag")
    parser.add_argument(
        "--kubernetes", choices=("min", "max"), default="max",
        help="exercise the minimum or maximum pinned supported Kubernetes line",
    )
    parser.add_argument("--helm", default="helm")
    parser.add_argument("--kubectl", default="kubectl")
    parser.add_argument("--k3d", default="k3d")
    args = parser.parse_args()

    for executable in ("docker", "curl", "openssl", args.helm, args.kubectl, args.k3d):
        require(shutil.which(executable), f"required executable is unavailable: {executable}")
    require(CHART.is_dir(), f"Helm chart is missing: {CHART}")

    harness = Harness(args)
    atexit.register(harness.cleanup)

    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    try:
        harness.execute()
    except BaseException:
        harness.failed = True
        harness.diagnostics()
        raise
    finally:
        harness.cleanup()


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, ValueError, yaml.YAMLError) as error:
        print(f"k3d lifecycle: {error}", file=sys.stderr)
        raise SystemExit(1) from error
