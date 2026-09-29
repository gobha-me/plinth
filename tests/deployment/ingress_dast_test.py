#!/usr/bin/env python3
"""Scan only an exact candidate in a disposable, monitored Traefik deployment.

Raw engine output stays private. Unexpected alerts, incomplete scans and cleanup
failures are not passing gates. This is source-revision validation, not a
published-release certification or an authorization to scan an existing service.
"""

from __future__ import annotations

import argparse
from contextlib import ExitStack
from datetime import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request

import yaml

from dast_report import REQUIRED_CONTROLS, classify_route, public_report
from k3d_lifecycle_test import Harness, K3S_TRAEFIK_CHART_VERSION, ROOT, free_port, require

sys.path.insert(0, str(ROOT / "tests/browser"))
from process_cleanup import run_browser  # noqa: E402


SCANNER_VERSION = "2.17.0"
SCANNER_DIGEST = "sha256:781a2bdaea47324e7bab583e2263f21d257b0aee61ed51521a5be45f5f5081ef"
SCANNER_IMAGE = f"ghcr.io/zaproxy/zaproxy:{SCANNER_VERSION}@{SCANNER_DIGEST}"
RULE_ADDONS = {"ascanrules": "83.0.0", "pscanrules": "76.0.0"}
ACTIVE_RULES = ("40012", "40018")  # Reflected XSS and SQL injection; GET only.
ACTIVE_PATHS = ("/app/?issue40_probe=read_only",
                "/api/auth/session?issue40_probe=read_only",
                "/api/frontend/applications?issue40_probe=read_only")
ADMIN_NAME = "issue36-admin"
ADMIN_PASSWORD = "fake-password-for-issue36!"
INGRESS_LOG_FIELDS = ("DownstreamStatus", "OriginStatus", "RouterName")
INGRESS_LOG_METADATA = {"time", "msg", "level"}
INGRESS_LOG_ARGUMENTS = {
    "--accesslog=true", "--accesslog.format=json",
    "--accesslog.fields.defaultmode=drop", "--accesslog.fields.headers.defaultmode=drop",
    *(f"--accesslog.fields.names.{name}=keep" for name in INGRESS_LOG_FIELDS),
}


def wait_until(observe, *, timeout, label):
    deadline = time.monotonic() + timeout
    while True:
        result = observe()
        if result:
            return result
        require(time.monotonic() < deadline, f"bounded observation failed: {label}")
        time.sleep(0.2)


def private_json(path, value):
    descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as output:
        json.dump(value, output, sort_keys=True, indent=2)
        output.write("\n")


def ingress_log_time(value):
    if not isinstance(value, str) or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(Z|[+-](?:[01]\d|2[0-3]):[0-5]\d)", value):
        return False
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return False
    return True


def observed_routes(messages, origin):
    """Derive coverage from actual messages, rejecting mismatched authorities."""
    labels = set()
    authority = urllib.parse.urlsplit(origin).netloc
    for message in messages:
        require(isinstance(message, dict), "invalid scanner message")
        header = message.get("requestHeader")
        require(isinstance(header, str), "missing scanner request header")
        lines = header.split("\r\n")
        first = lines[0].split(" ")
        require(len(first) == 3 and first[0] in ("GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS")
                and first[2] in ("HTTP/1.0", "HTTP/1.1", "HTTP/2"),
                "scanner request line is malformed")
        path = first[1]
        require(path.startswith("https://") or
                (path.startswith("/") and not path.startswith("//")),
                "scanner request target is not an owned HTTPS route")
        url = path if path.startswith("https://") else origin + path
        require(urllib.parse.urlsplit(url).netloc == authority,
                "scanner message escaped the owned authority")
        hosts = [line.split(":", 1)[1].strip() for line in lines[1:]
                 if line.lower().startswith("host:")]
        require(hosts == [authority], "scanner message Host differs from owned authority")
        label = classify_route(url, origin)
        if label is not None:
            labels.add(label)
    return labels


class Scanner:
    def __init__(self, harness):
        self.harness = harness
        self.name = "plinth-issue40-zap-" + secrets.token_hex(6)
        self.port = free_port()
        self.key = secrets.token_urlsafe(32)
        self.started = False
        self.scans = []
        self.completed = False
        self.creation_attempted = False
        self.owner = secrets.token_hex(16)
        self.passive_remaining = None
        self.scan_evidence = []

    @property
    def proxy(self):
        return f"http://127.0.0.1:{self.port}"

    def api(self, component, kind, action, **parameters):
        query = urllib.parse.urlencode({"apikey": self.key, **parameters})
        url = f"{self.proxy}/JSON/{component}/{kind}/{action}/?{query}"
        # Do not permit ambient proxy environment to redirect the control API.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(url, timeout=15) as response:
            require(response.status == 200, "scanner API rejected a request")
            body = json.load(response)
        require(isinstance(body, dict) and "code" not in body,
                "scanner API did not complete the requested operation")
        return body

    def start(self):
        self.harness.run(["docker", "image", "inspect", SCANNER_IMAGE], timeout=30)
        absent = self.harness.run(["docker", "inspect", self.name], check=False, timeout=15)
        require(absent.returncode != 0 and "no such" in absent.stderr.lower(),
                "scanner name must be absent before task-owned creation")
        self.creation_attempted = True
        self.harness.run([
            "docker", "run", "--detach", "--name", self.name,
            "--label", "plinth.test.owner=" + self.owner,
            "--network", "host", "--add-host", "plinth.test:127.0.0.1",
            "--memory", "1536m", "--memory-swap", "1536m", "--cpus", "1",
            "--env", "JAVA_OPTS=-Xmx768m", SCANNER_IMAGE,
            "zap.sh", "-daemon", "-host", "127.0.0.1", "-port", str(self.port),
            "-config", "api.disablekey=false", "-config", "api.key=" + self.key,
            "-config", "api.addrs.addr.name=127.0.0.1",
            "-config", "api.addrs.addr.regex=false",
            "-config", "autoupdate.checkOnStart=false",
            "-config", "autoupdate.downloadNewRelease=false",
        ], timeout=60)
        self.started = True

        def version():
            try:
                return self.api("core", "view", "version").get("version")
            except (OSError, ValueError, AssertionError, urllib.error.URLError):
                return None

        require(wait_until(version, timeout=90, label="scanner start") == SCANNER_VERSION,
                "running scanner version differs from the pinned image")
        addons = self.api("autoupdate", "view", "installedAddons").get("installedAddons")
        require(isinstance(addons, list), "scanner addon inventory is unavailable")
        rules = [addon for addon in addons if isinstance(addon, dict)
                 and addon.get("id") in RULE_ADDONS]
        require(len(rules) == len(RULE_ADDONS)
                and all(addon.get("version") == RULE_ADDONS[addon["id"]]
                        and addon.get("status") == "release" for addon in rules)
                and {addon["id"] for addon in rules} == set(RULE_ADDONS),
                "scanner rule addon identity differs from the pinned image")
        self.harness.private_details["scannerAddons"] = addons
        self.api("core", "action", "setMode", mode="safe")
        context = self.api("context", "action", "newContext", contextName="issue40")
        self.context_id = context["contextId"]
        self.api("context", "action", "includeInContext", contextName="issue40",
                 regex=re.escape(self.harness.origin) + r"/.*")
        self.api("context", "action", "setContextInScope", contextName="issue40",
                 booleanInScope="true")
        self.api("pscan", "action", "setScanOnlyInScope", onlyInScope="true")

    def install_cookie(self, cookie):
        # Replacement is limited to this exact disposable origin, never a
        # general proxy credential. Its value is not logged or reported.
        self.api("replacer", "action", "addRule", description="issue40-session",
                 enabled="true", matchType="REQ_HEADER", matchRegex="false",
                 matchString="Cookie", replacement=cookie,
                 url=re.escape(self.harness.origin) + r"/.*")

    def active_scan(self):
        policy = "issue40-readonly"
        self.api("ascan", "action", "addScanPolicy", scanPolicyName=policy,
                 attackStrength="MEDIUM", alertThreshold="MEDIUM")
        self.api("ascan", "action", "disableAllScanners", scanPolicyName=policy)
        self.api("ascan", "action", "enableScanners", ids=",".join(ACTIVE_RULES),
                 scanPolicyName=policy)
        self.api("ascan", "action", "setOptionTargetParamsInjectable", Integer="1")
        self.api("ascan", "action", "setOptionAddQueryParam", Boolean="false")
        self.api("ascan", "action", "setOptionScanHeadersAllRequests", Boolean="false")
        self.api("ascan", "action", "setOptionPersistTemporaryMessages", Boolean="true")
        self.api("ascan", "action", "setOptionInjectPluginIdInHeader", Boolean="true")
        for option, expected in (("PersistTemporaryMessages", "true"),
                                 ("InjectPluginIdInHeader", "true"),
                                 ("TargetParamsInjectable", "1"),
                                 ("AddQueryParam", "false"),
                                 ("ScanHeadersAllRequests", "false")):
            require(self.api("ascan", "view", "option" + option).get(option) == expected,
                    "scanner attack options differ from the reviewed scope")
        self.api("ascan", "action", "setOptionThreadPerHost", Integer="1")
        self.api("ascan", "action", "setOptionDelayInMs", Integer="10")
        self.api("core", "action", "setMode", mode="protect")
        active_context = "issue40-get-probes"
        context = self.api("context", "action", "newContext", contextName=active_context)
        self.api("context", "action", "includeInContext", contextName=active_context,
                 regex=re.escape(self.harness.origin)
                 + r"/(?:app/|api/auth/session|api/frontend/applications)(?:\?.*)?")
        self.api("context", "action", "setContextInScope", contextName=active_context,
                 booleanInScope="true")
        for path in ACTIVE_PATHS:
            require(self.harness.request(path)[0] == 200,
                    "reviewed query seed did not reach its authenticated handler")
            scan = self.api("ascan", "action", "scan", url=self.harness.origin + path,
                            recurse="false", inScopeOnly="true",
                            scanPolicyName=policy, method="GET",
                            contextId=context["contextId"])["scan"]
            self.scans.append(scan)
            evidence = {"scanId": scan, "seed": path, "stage": "ALLOCATED"}
            self.scan_evidence.append(evidence)

            def status_complete():
                evidence["status"] = self.api("ascan", "view", "status", scanId=scan)
                return evidence["status"].get("status") == "100"

            wait_until(status_complete, timeout=180, label="active scan")
            scans = self.api("ascan", "view", "scans")
            evidence["scans"] = scans
            progress = self.api("ascan", "view", "scanProgress", scanId=scan)
            evidence["progress"] = progress
            message_ids = self.api("ascan", "view", "messagesIds", scanId=scan)
            evidence["messagesIds"] = message_ids
            evidence["stage"] = "PROGRESS_RECEIVED"
            self.validate_scan_progress(scan, scans, progress, message_ids)
            messages = []
            evidence["messages"] = messages
            ids = message_ids["messagesIds"]
            require(len(ids) < 5000, "active scan message inventory exceeded its bound")
            for offset in range(0, len(ids), 100):
                messages.extend(self.api("core", "view", "messagesById",
                                         ids=",".join(ids[offset:offset + 100]))["messagesById"])
            evidence["stage"] = "MESSAGES_RECEIVED"
            self.validate_active_messages(path, ids, messages)
            evidence["stage"] = "VERIFIED"
        self.api("core", "action", "setMode", mode="safe")
        wait_until(lambda: self.remaining() == 0, timeout=90, label="passive scanner drain")
        self.passive_remaining = self.remaining()
        self.completed = True

    def validate_scan_progress(self, scan, scans, progress, message_ids):
        def positive(value):
            return isinstance(value, str) and re.fullmatch(r"[1-9][0-9]*", value) is not None

        entries = scans.get("scans", [])
        matches = [entry for entry in entries if entry.get("id") == scan]
        require(len(matches) == 1, "active scan terminal record is ambiguous")
        record = matches[0]
        require(record.get("state") == "FINISHED" and record.get("progress") == "100"
                and positive(record.get("reqCount")),
                "active scan stopped or sent no requests")
        hosts = progress.get("scanProgress")
        require(isinstance(hosts, list) and len(hosts) == 2
                and hosts[0] == self.harness.origin and isinstance(hosts[1], dict),
                "active scan progress differs from the owned host")
        plugins = hosts[1].get("HostProcess")
        require(isinstance(plugins, list), "active rule progress is missing")
        observed = {}
        for entry in plugins:
            plugin = entry.get("Plugin") if isinstance(entry, dict) else None
            require(isinstance(plugin, list) and len(plugin) == 7,
                    "active rule progress is malformed")
            if plugin[1] not in ACTIVE_RULES:
                continue
            require(plugin[1] not in observed and plugin[3] == "Complete"
                    and positive(plugin[5]), "enabled active rule did not complete real requests")
            observed[plugin[1]] = plugin
        require(set(observed) == set(ACTIVE_RULES), "enabled active rule was skipped")
        ids = message_ids.get("messagesIds")
        require(isinstance(ids, list) and ids and len(ids) == len(set(ids))
                and all(positive(value) for value in ids),
                "active scan request messages were not retained")

    def validate_active_messages(self, path, ids, messages):
        require(isinstance(messages, list) and len(messages) == len(ids)
                and {message.get("id") for message in messages} == set(ids),
                "active request message inventory is incomplete")
        observed_routes(messages, self.harness.origin)
        seed = urllib.parse.urlsplit(path)
        expected_path = seed.path
        seed_query = urllib.parse.parse_qsl(seed.query, keep_blank_values=True, strict_parsing=True)
        require(len(seed_query) == 1 and seed_query[0][0] == "issue40_probe",
                "active seed differs from the reviewed query input")
        rules = set()
        for message in messages:
            header = message["requestHeader"]
            lines = header.split("\r\n")
            method, target, _ = lines[0].split(" ")
            parsed = urllib.parse.urlsplit(target)
            query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True, strict_parsing=True)
            require(method == "GET" and parsed.path == expected_path
                    and message.get("requestBody") == "",
                    "active request escaped the reviewed GET endpoint")
            require(len(query) == 1 and query[0][0] == "issue40_probe",
                    "active request escaped the reviewed query parameter")
            response = message.get("responseHeader", "")
            require(isinstance(response, str) and re.match(r"^HTTP/\d(?:\.\d)? [1-5][0-9]{2} ", response),
                    "active request has no actual response")
            markers = []
            for line in lines[1:]:
                if line.lower().startswith("x-zap-scan-id:"):
                    rule = line.split(":", 1)[1].strip()
                    require(rule in ACTIVE_RULES, "unexpected active rule sent an attack")
                    markers.append(rule)
            require(len(markers) <= 1, "active message has ambiguous rule attribution")
            # A rule-tagged baseline request is not an injection attempt.
            if query[0][1] != seed_query[0][1]:
                rules.update(markers)
        require(rules == set(ACTIVE_RULES), "enabled rule attack messages were not observed")

    def remaining(self):
        return int(self.api("pscan", "view", "recordsToScan")["recordsToScan"])

    def results(self):
        alerts = self.api("core", "view", "alerts", baseurl=self.harness.origin,
                          start="0", count="5000")["alerts"]
        number = int(self.api("core", "view", "numberOfAlerts",
                              baseurl=self.harness.origin)["numberOfAlerts"])
        require(isinstance(alerts, list) and len(alerts) == number and number <= 5000,
                "scanner alert inventory is truncated or inconsistent")
        messages = self.api("core", "view", "messages", baseurl=self.harness.origin,
                            start="0", count="5000")["messages"]
        require(isinstance(messages, list) and 0 < len(messages) < 5000,
                "scanner HTTP traffic is absent or truncated")
        return alerts, messages

    def cleanup(self):
        if self.creation_attempted:
            existing = self.harness.run(["docker", "inspect", self.name], check=False,
                                        timeout=15)
            if existing.returncode == 0:
                info = json.loads(existing.stdout)[0]
                require(info["Config"].get("Labels", {}).get("plinth.test.owner") == self.owner,
                        "refusing to remove an unowned scanner container")
                self.harness.run(["docker", "rm", "--force", "--volumes", self.name],
                                 timeout=45)
                self.started = False
        absent = self.harness.run(["docker", "inspect", self.name], check=False,
                                  timeout=15)
        require(absent.returncode != 0 and "no such" in absent.stderr.lower(),
                "task-owned scanner absence was not proven")


class DastHarness(Harness):
    def __init__(self, args):
        super().__init__(args)
        # Replace the still-empty superclass temporary directory before any
        # fixture creation; keep the browser's narrow issue40 ownership check.
        self.temporary.cleanup()
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-issue40-dast-")
        self.root = Path(self.temporary.name)
        for attribute, filename in (("kubeconfig", "kubeconfig"),
                                    ("certificate", "tls.crt"), ("private_key", "tls.key"),
                                    ("cookies", "cookies.txt"), ("values_path", "values.yaml"),
                                    ("traefik_config", "traefik-config.yaml")):
            setattr(self, attribute, self.root / filename)
        self.env["KUBECONFIG"] = str(self.kubeconfig)
        self.cluster = self.cluster.replace("issue36", "issue40")
        self.registry = self.registry.replace("issue36", "issue40")
        self.namespace = self.namespace.replace("issue36", "issue40")
        self.internal_repository = f"k3d-{self.registry}:5000/plinth"
        self.release = "issue40-dast"
        self.scanner = Scanner(self)
        self.controls = {name: False for name in REQUIRED_CONTROLS}
        self.route_labels = set()
        self.raw_alerts = []
        self.raw_messages = []
        self.private_details = {}
        self.cleanup_proof = {}
        self.initial_volumes = set(self.run([
            "docker", "volume", "ls", "--format", "{{.Name}}"], timeout=15).stdout.splitlines())
        self.owned_volumes = set()
        self.infrastructure_attempted = False

    def create_infrastructure(self):
        tag = self.run(["docker", "image", "inspect", self.local_tag], check=False, timeout=15)
        require(tag.returncode != 0
                and tag.stderr.strip().lower() in (
                    f"error response from daemon: no such image: {self.local_tag}",
                    f"error: no such image: {self.local_tag}",
                ),
                "refusing to overwrite an existing registry candidate tag")
        containers = self.run(["docker", "ps", "-a", "--format", "{{.Names}}"],
                              timeout=15).stdout.splitlines()
        networks = self.run(["docker", "network", "ls", "--format", "{{.Name}}"],
                            timeout=15).stdout.splitlines()
        require(not any(name == "k3d-" + self.registry
                        or name.startswith("k3d-" + self.cluster + "-") for name in containers)
                and "k3d-" + self.cluster not in networks,
                "disposable resource names were already in use")
        self.infrastructure_attempted = True
        super().create_infrastructure()

    def values(self, **kwargs):
        values = super().values(**kwargs)
        defaults = yaml.safe_load((ROOT / "deploy/helm/plinth/values.yaml").read_text())
        values["traefik"]["limits"] = defaults["traefik"]["limits"]
        values["traefik"]["forwardingTimeouts"] = defaults["traefik"]["forwardingTimeouts"]
        return values

    def monitor_ingress(self):
        old = json.loads(self.kubectl("get", "pods", "-n", "kube-system", "-l",
                                     "app.kubernetes.io/name=traefik", "-o", "json").stdout)
        old_uids = {pod["metadata"]["uid"] for pod in old["items"]}
        config = json.loads(self.kubectl("get", "helmchartconfig/traefik", "-n",
                                        "kube-system", "-o", "json").stdout)
        values = yaml.safe_load(config["spec"]["valuesContent"])
        chart_version = K3S_TRAEFIK_CHART_VERSION[self.args.kubernetes]
        names = dict.fromkeys(INGRESS_LOG_FIELDS, "keep")
        # The two pinned bundled charts have different logging schemas. Keep
        # only the fields used below: paths, query values and headers are dropped.
        if chart_version == "40.1.4+up40.1.0":
            values["logs"] = {"access": {
                "enabled": True, "format": "json",
                "fields": {"general": {"defaultmode": "drop", "names": names},
                           "headers": {"defaultmode": "drop"}},
            }}
        elif chart_version == "41.4.2+up41.4.0":
            values["accessLog"] = {
                "enabled": True, "format": "json",
                "fields": {"defaultMode": "drop", "names": names,
                           "headers": {"defaultMode": "drop"}},
            }
        else:
            raise AssertionError("unreviewed ingress logging chart schema")
        config["spec"]["valuesContent"] = yaml.safe_dump(values)
        self.apply_json(config)

        def changed():
            pods = json.loads(self.kubectl("get", "pods", "-n", "kube-system", "-l",
                                          "app.kubernetes.io/name=traefik", "-o", "json").stdout)
            for pod in pods["items"]:
                if (pod["metadata"]["uid"] not in old_uids
                        and any(condition.get("status") == "True"
                                and condition.get("type") == "Ready"
                                for condition in pod.get("status", {}).get("conditions", []))
                        and pod.get("status", {}).get("phase") == "Running"):
                    self.validate_ingress_arguments(pod["spec"]["containers"])
                    deployment = json.loads(self.kubectl(
                        "get", "deployment/traefik", "-n", "kube-system", "-o", "json").stdout)
                    self.validate_ingress_arguments(
                        deployment["spec"]["template"]["spec"]["containers"])
                    return True
            return False

        wait_until(changed, timeout=180, label="monitored ingress rollout")
        self.private_details["ingressConfiguration"] = {
            "chartVersion": chart_version, "arguments": sorted(INGRESS_LOG_ARGUMENTS),
        }

    @staticmethod
    def validate_ingress_arguments(containers):
        require(isinstance(containers, list) and len(containers) == 1,
                "unexpected monitored ingress containers")
        container = containers[0]
        require(isinstance(container, dict) and container.get("name") == "traefik",
                "invalid monitored ingress container")
        args = container.get("args")
        require(isinstance(args, list) and all(isinstance(arg, str) for arg in args),
                "monitored ingress arguments missing")
        access_args = [arg for arg in args if arg.lower().startswith("--accesslog")]
        require(len(access_args) == len(INGRESS_LOG_ARGUMENTS)
                and set(access_args) == INGRESS_LOG_ARGUMENTS,
                "actual ingress logging arguments do not match the reviewed allowlist")
        env = container.get("env", [])
        require(container.get("envFrom", []) == []
                and isinstance(env, list) and all(
            isinstance(item, dict) and isinstance(item.get("name"), str)
            and not item["name"].upper().startswith("TRAEFIK_ACCESSLOG") for item in env),
            "ingress logging environment override is not reviewed")

    def request(self, path, *, method="GET", body=None, headers=None, cookies=False):
        require(path.startswith("/") and not path.startswith("//")
                and "\r" not in path and "\n" not in path, "invalid owned route")
        context = ssl.create_default_context(cafile=str(self.certificate))
        if self.scanner.started:
            # Only the ephemeral pinned scanner's interception certificate is
            # ignored. Native Host/TLS authority is independently checked with
            # the real fixture certificate through curl, never through this MITM.
            connection = http.client.HTTPSConnection(
                "127.0.0.1", self.scanner.port, timeout=15,
                context=ssl._create_unverified_context())
            connection.set_tunnel(self.host, self.https_port)
        else:
            connection = http.client.HTTPSConnection("127.0.0.1", self.https_port,
                                                    timeout=15, context=context)
            connection.sock = self.connect_tls(context)
        effective = {"Host": f"{self.host}:{self.https_port}", **(headers or {})}
        if cookies:
            effective["Cookie"] = self.cookie_header()
        try:
            connection.request(method, path, body=body, headers=effective)
            response = connection.getresponse()
            data = response.read(2 * 1024 * 1024 + 1)
            require(len(data) <= 2 * 1024 * 1024, "unbounded HTTP response")
            return response.status, dict(response.getheaders()), data
        finally:
            connection.close()

    def connect_tls(self, context=None):
        context = context or ssl.create_default_context(cafile=str(self.certificate))
        peer = socket.create_connection(("127.0.0.1", self.https_port), timeout=15)
        try:
            secured = context.wrap_socket(peer, server_hostname=self.host)
            secured.settimeout(15)
            return secured
        except BaseException:
            peer.close()
            raise

    def cookie_header(self):
        return "; ".join(f"{name}={self.cookie(name)}"
                         for name in ("plinth_session", "plinth_csrf"))

    def browser(self):
        env = self.env.copy()
        env.update(PLINTH_DAST_ORIGIN=self.origin,
                   PLINTH_DAST_PROXY=self.scanner.proxy,
                   PLINTH_DAST_USERNAME=ADMIN_NAME, PLINTH_DAST_PASSWORD=ADMIN_PASSWORD,
                   PLINTH_DAST_RESULT_FILE=str(self.root / "browser-private.json"))
        with (self.root / "browser-private.log").open("w") as log:
            run_browser(["node", str(ROOT / "tests/browser/ingress-dast.mjs")],
                        timeout=120, env=env, cwd=ROOT, stdout=log,
                        stderr=subprocess.STDOUT)
        result = json.loads((self.root / "browser-private.json").read_text())
        self.private_details["browser"] = result
        self.validate_browser_receipt(result)
        self.controls["BROWSER_AUTH"] = self.controls["WS_UPGRADE"] = True

    @staticmethod
    def validate_browser_receipt(result):
        require(isinstance(result, dict)
                and result.get("schema") == "plinth.ingress-browser.v1"
                and result.get("completed") is True and result.get("stage") == "COMPLETE",
                "real browser receipt is incomplete")
        require(all(isinstance(result.get(name), dict) for name in
                    ("controls", "cleanup", "counts", "routes", "proof")),
                "real browser receipt sections are malformed")
        require(set(result.get("controls", {})) == {"BROWSER_AUTH", "WS_UPGRADE"}
                and all(value is True for value in result["controls"].values())
                and set(result.get("cleanup", {})) == {"contextClosed", "browserClosed"}
                and all(value is True for value in result["cleanup"].values()),
                "real browser controls or cleanup did not complete")
        counts = result.get("counts", {})
        require(set(counts) == {"pageErrors", "cspViolations", "failedRequests",
                               "externalRequestsBlocked", "externalResponses", "invalidWsFrames"}
                and all(type(value) is int and value == 0 for value in counts.values()),
                "real browser had errors or unscoped traffic")
        routes = result.get("routes", {})
        for label in ("ROOT", "APP", "SESSION", "LOGIN", "LOGOUT", "REGISTRATION", "CAP", "WS"):
            require(isinstance(routes.get(label), dict), "browser route receipt is malformed")
            count = routes.get(label, {}).get("requests")
            require(type(count) is int and count > 0, "browser route was not observed")
        proof = result.get("proof", {})
        websocket = proof.get("websocket", {})
        require(isinstance(websocket, dict) and type(websocket.get("connections")) is int
                and websocket["connections"] == 1
                and websocket.get("connected") is True and websocket.get("applicationGrant") is True,
                "browser did not receive an authenticated subscription grant")
        logout = proof.get("logout", {})
        require(isinstance(logout, dict) and type(logout.get("sessionStatus")) is int
                and logout["sessionStatus"] == 401
                and logout.get("originalSocketClosed") is True,
                "browser logout did not retire its session and WebSocket")

    def verify_http(self):
        session = self.request("/api/auth/session")[0]
        require(session == 401, "anonymous session request was accepted")
        status, _, body = self.request("/api/auth/registration")
        require(status == 200 and json.loads(body)["mode"] == "disabled",
                "public registration policy differs from the isolated chart")
        shell_version = (ROOT / "VERSION").read_text().strip()
        for path in ("/app/../../config.json", f"/ext/shell/{shell_version}/../../config.json",
                     "/api/frontend/applications?probe=%3Cscript%3E", "/api/unknown"):
            status, _, body = self.request(path)
            require(status in (400, 401, 403, 404, 405)
                    and b'"database"' not in body and b"<script>" not in body,
                    "malformed/traversal HTTP control did not fail closed")
        code, status, _ = self.curl("/healthz", host_header="wrong-authority.invalid")
        require(code == 0 and status == "404", "wrong Host authority reached the kernel")
        code, status, _ = self.curl("/healthz")
        require(code == 0 and status == "200", "native TLS certificate authority validation failed")
        status, _, body = self.request("/api/auth/logout", method="POST", cookies=True,
                                       headers={"Origin": "https://cross-origin.invalid"})
        require(status == 403 and json.loads(body)["error"] == "csrf_failed",
                "cross-origin mutation was accepted")
        headers = {"Origin": self.origin, "Content-Type": "application/json"}
        args = json.dumps({"args": {"key": "shell.launcher"}})
        status, _, body = self.request("/api/cap/shell.preferences.get", method="POST",
                                       body=args, headers=headers, cookies=True)
        require(status == 403 and json.loads(body)["error"] == "csrf_failed",
                "missing CSRF capability control was accepted")
        headers["X-Plinth-CSRF"] = self.cookie("plinth_csrf")
        status, _, body = self.request("/api/cap/shell.preferences.get", method="POST",
                                       body=args, headers=headers, cookies=True)
        require(status == 200 and json.loads(body)["ok"] is True,
                "valid capability control did not reach the real handler")
        for path in ("/app/", "/api/frontend/sdk.js", "/healthz"):
            status, headers, _ = self.request(path)
            require(status in (200, 302), "expected surface unavailable")
            if path == "/app/":
                policy = headers.get("Content-Security-Policy", "")
                require("script-src 'self'" in policy and "connect-src 'self'" in policy
                        and "'unsafe-eval'" not in policy
                        and "'unsafe-inline'" not in policy.split(";")[0],
                        "shipped executable-script CSP differs from the browser contract")
                self.private_details["shippedCsp"] = policy
        self.controls["HTTP_SURFACE"] = self.controls["SECURITY_HEADERS"] = True
        self.controls["TLS_AUTHORITY"] = True
        self.route_labels.update(("HEALTH", "REGISTRATION", "CAP", "SESSION", "APP"))
        status, _, body = self.request("/api/auth/register", method="POST",
                                       body=json.dumps({"username": "issue40-disabled",
                                                        "password": ADMIN_PASSWORD}),
                                       headers={"Origin": self.origin,
                                                "Content-Type": "application/json"})
        require(status == 403 and json.loads(body)["error"] == "registration_unavailable",
                "disabled registration was not exercised before the rate probe")

    def verify_request_limits(self):
        defaults = yaml.safe_load((ROOT / "deploy/helm/plinth/values.yaml").read_text())
        limit = defaults["traefik"]["limits"]["defaultRequestBodyBytes"]
        status, _, _ = self.request("/api/cap/shell.preferences.get", method="POST",
                                    body=b"x" * (limit + 1))
        require(status == 413, "default ingress request boundary was not enforced")
        self.controls["REQUEST_LIMITS"] = True

    def verify_concurrency_limits(self):
        # Package admission precedes buffering. Hold two incomplete, bounded
        # bodies so a third request must be rejected without invoking installer.
        with ExitStack() as sockets:
            held = []
            for _ in range(2):
                peer = sockets.enter_context(self.connect_tls())
                peer.sendall(("POST /api/packages HTTP/1.1\r\n"
                              f"Host: {self.host}:{self.https_port}\r\n"
                              f"Origin: {self.origin}\r\n"
                              f"Cookie: {self.cookie_header()}\r\n"
                              f"X-Plinth-CSRF: {self.cookie('plinth_csrf')}\r\n"
                              "Content-Type: application/octet-stream\r\n"
                              "Content-Length: 32\r\n\r\n").encode("ascii") + b"x")
                held.append(peer)
            # Each held request must remain outstanding rather than already
            # having been rejected; never convert a closed socket into success.
            for peer in held:
                peer.settimeout(0.2)
                try:
                    unexpected = peer.recv(1)
                except TimeoutError:
                    continue
                raise AssertionError("held package request ended before the limit probe")
            status, _, _ = self.request("/api/packages", method="POST", body=b"x")
            require(status == 429, "package concurrent admission bound was not enforced")
        self.route_labels.add("PACKAGES")
        self.controls["CONCURRENCY_LIMITS"] = True

    def verify_rate_limits(self):
        payload = json.dumps({"username": ADMIN_NAME, "password": ADMIN_PASSWORD})
        statuses = []
        for _ in range(6):
            status, _, body = self.request("/api/auth/login", method="POST", body=payload,
                                           headers={"Origin": self.origin,
                                                    "Content-Type": "application/json"})
            statuses.append(status)
            require(status in (200, 429), "rate probe failed outside the edge token bucket")
            if status == 429:
                require(b"rate_limited" not in body, "kernel throttling substituted for edge admission")
        require(429 in statuses, "six immediate logins exceeded the five-token edge burst")
        require(self.request("/healthz")[0] == 200, "rate probe affected target health")
        self.route_labels.add("LOGIN")
        self.controls["RATE_LIMITS"] = True

    def observe_ingress(self):
        raw = self.kubectl("logs", "deployment/traefik", "-n", "kube-system",
                           "--tail=4000").stdout
        entries = []
        for line in raw.splitlines():
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(item, dict) and "DownstreamStatus" in item:
                entries.append(item)
        # Preserve actual private records even when a subsequent witness fails.
        self.private_details["ingressAccessLog"] = entries
        require(entries and len(entries) < 4000, "ingress monitor absent or truncated")
        require(all(set(item) <= set(INGRESS_LOG_FIELDS) | INGRESS_LOG_METADATA
                    and INGRESS_LOG_METADATA <= set(item)
                    and item["level"] == "info" and item["msg"] == ""
                    and ingress_log_time(item["time"])
                    and type(item.get("DownstreamStatus")) is int
                    and 100 <= item["DownstreamStatus"] <= 599
                    and ("OriginStatus" not in item
                         or (type(item["OriginStatus"]) is int
                             and 0 <= item["OriginStatus"] <= 599))
                    and ("RouterName" not in item
                         or isinstance(item["RouterName"], str)) for item in entries),
                "ingress monitor fields do not match the reviewed allowlist")
        routers = self.expected_ingress_routers()
        # Pinned Traefik omits OriginStatus when it handles an edge response.
        # First prove it retained the explicit status of a real backend login;
        # only then may absence distinguish edge rejection from backend 429.
        require(any(item["DownstreamStatus"] == 200
                    and item.get("OriginStatus") == 200
                    and item.get("RouterName") == routers["LOGIN"] for item in entries),
                "ingress monitor did not retain a backend authentication status")
        require(any(item["DownstreamStatus"] == 101
                    and item.get("RouterName") == routers["WS"] for item in entries),
                "ingress did not observe actual WebSocket upgrade")
        require(any(item["DownstreamStatus"] == 429
                    and item.get("RouterName") == routers["LOGIN"]
                    and ("OriginStatus" not in item or item["OriginStatus"] == 0)
                    for item in entries),
                "edge authentication rejection was not independently monitored")
        require(any(item["DownstreamStatus"] == 429
                    and item.get("RouterName") == routers["PACKAGES"]
                    and ("OriginStatus" not in item or item["OriginStatus"] == 0)
                    for item in entries),
                "edge package admission rejection was not independently monitored")
        self.controls["INGRESS_MONITOR"] = True

    def expected_ingress_routers(self):
        # Match the unchanged chart's three exact rules and pinned Traefik's
        # default legacy CRD naming, including namespace and provider identity.
        rules = {
            "WS": ("ws", f"Host(`{self.host}`) && Path(`/ws/events`)"),
            "LOGIN": ("auth", f"Host(`{self.host}`) && Path(`/api/auth/login`) && Method(`POST`)"),
            "PACKAGES": ("packages", f"Host(`{self.host}`) && Path(`/api/packages`) && Method(`POST`)"),
        }
        return {label: f"{self.namespace}-{self.release}-plinth-{kind}-"
                       f"{hashlib.sha256(rule.encode()).hexdigest()[:20]}@kubernetescrd"
                for label, (kind, rule) in rules.items()}

    def execute_dast(self):
        self.create_infrastructure()
        self.monitor_ingress()
        self.create_namespace_dependencies()
        self.install_chart()
        self.bootstrap_admin()
        self.remove_bootstrap_authority()
        self.enable_public_route()
        self.scanner.start()
        self.browser()
        # New valid session after the real browser logout; no stale-session
        # credentials are reused for scanner traffic.
        self.verify_login()
        self.verify_http()
        self.scanner.install_cookie(self.cookie_header())
        self.verify_scanner_session()
        self.scanner.active_scan()
        self.verify_scanner_session()
        self.verify_request_limits()
        self.verify_concurrency_limits()
        self.verify_rate_limits()
        require(self.request("/healthz")[0] == 200, "scan target is no longer healthy")
        self.scanner.passive_remaining = self.scanner.remaining()
        wait_until(lambda: self.scanner.remaining() == 0, timeout=90,
                   label="final passive scanner drain")
        self.scanner.passive_remaining = self.scanner.remaining()
        self.raw_alerts, self.raw_messages = self.scanner.results()
        # Route coverage comes from observed engine messages, not a planned
        # control list or a direct-backend mock. Query values stay private.
        self.route_labels = observed_routes(self.raw_messages, self.origin)
        self.observe_ingress()
        self.verify_clean_removal()

    def verify_scanner_session(self):
        status, _, body = self.request("/api/auth/session")
        require(status == 200 and json.loads(body).get("user", {}).get("username") == ADMIN_NAME,
                "scanner cookie did not authenticate the exact fake account")

    def diagnostics(self):
        # The base harness's diagnostic printing is appropriate for its fake
        # lifecycle fixtures, but a scanner finding must not be published by
        # printing command arguments, payloads, headers or raw access records.
        pass

    def retain_private_failure_evidence(self):
        for name in ("browser-private.json", "browser-private.log"):
            path = self.root / name
            if path.is_file():
                require(path.stat().st_size <= 4 * 1024 * 1024,
                        "private browser artifact exceeded its bound")
                self.private_details[name] = path.read_text(encoding="utf-8", errors="replace")
        self.private_details["activeScans"] = self.scanner.scan_evidence
        if self.scanner.started and not self.raw_messages:
            try:
                self.raw_alerts, self.raw_messages = self.scanner.results()
            except BaseException:
                self.private_details["captureError"] = traceback.format_exc()

    def inventory_owned_volumes(self):
        names = self.run(["docker", "ps", "-a", "--format", "{{.Names}}"],
                         timeout=15).stdout.splitlines()
        owned = [name for name in names if name == "k3d-" + self.registry
                 or name.startswith("k3d-" + self.cluster + "-")
                 or name == self.scanner.name]
        for name in owned:
            info = json.loads(self.run(["docker", "inspect", name], timeout=15).stdout)[0]
            for mount in info.get("Mounts", []):
                if mount.get("Type") == "volume" and mount.get("Name") not in self.initial_volumes:
                    volume = mount["Name"]
                    require(re.fullmatch(r"[A-Za-z0-9_.-]+", volume), "invalid owned volume identity")
                    self.owned_volumes.add(volume)

    def remove_owned_volumes(self):
        for name in sorted(self.owned_volumes):
            existing = self.run(["docker", "volume", "inspect", name], check=False, timeout=15)
            if existing.returncode != 0:
                require("no such" in existing.stderr.lower(), "volume absence check failed")
                continue
            users = self.run(["docker", "ps", "-a", "--filter", "volume=" + name,
                              "--format", "{{.Names}}"], timeout=15).stdout.strip()
            require(not users, "task volume still mounted; do not remove it")
            self.run(["docker", "volume", "rm", name], timeout=15)
        remaining = set(self.run(["docker", "volume", "ls", "--format", "{{.Name}}"],
                                 timeout=15).stdout.splitlines())
        return len(self.owned_volumes & remaining)

    def cleanup(self):
        """Always attempt every independently owned stage, even after timeout."""
        if self.cleanup_complete:
            return
        errors = []

        def attempt(label, operation):
            try:
                result = operation()
                if result is not None and result.returncode != 0:
                    raise RuntimeError("owned cleanup command failed")
            except BaseException:
                errors.append({"stage": label, "error": traceback.format_exc()})

        for watcher in list(self._exit_watchers):
            attempt("EXIT_OBSERVER", lambda watcher=watcher: self._stop_container_exit_watch(watcher))
        if self._exit_watchers:
            errors.append({"stage": "EXIT_OBSERVER", "error": "owned exit observers remain"})
        if self.infrastructure_attempted:
            if self.kubeconfig.is_file():
                if self.chart_installed:
                    attempt("HELM", lambda: self.helm(
                        "uninstall", self.release, "-n", self.namespace,
                        "--wait", "--timeout", "90s", timeout=110))
                if self.namespace_created:
                    attempt("NAMESPACE", lambda: self.kubectl(
                        "delete", "namespace", self.namespace, "--ignore-not-found=true",
                        "--wait=true", "--timeout=90s", timeout=110))
            attempt("CLUSTER", lambda: self.run(
                [self.args.k3d, "cluster", "delete", self.cluster], timeout=180))
            attempt("REGISTRY", lambda: self.run(
                [self.args.k3d, "registry", "delete", self.registry], timeout=120))

            def remove_tag():
                found = self.run(["docker", "image", "inspect", self.local_tag],
                                 check=False, timeout=15)
                if found.returncode == 0:
                    self.run(["docker", "image", "rm", self.local_tag], timeout=30)
                else:
                    require("no such" in found.stderr.lower(), "owned image absence unproven")

            attempt("LOCAL_TAG", remove_tag)
        attempt("TEMPORARY", self.temporary.cleanup)
        self.private_details["cleanupErrors"] = errors
        require(not errors, "owned cleanup stages failed; see private evidence")
        self.cleanup_complete = True


def source_identity():
    """Reject changed checkout inputs, including ignored chart additions."""
    status = subprocess.check_output([
        "git", "status", "--porcelain=v1", "--untracked-files=all",
        "--ignore-submodules=none",
    ], cwd=ROOT, text=True)
    require(not status.strip(), "source-candidate checkout must be clean")
    chart_additions = subprocess.check_output([
        "git", "ls-files", "--others", "--", "deploy/helm/plinth",
    ], cwd=ROOT, text=True)
    require(not chart_additions.strip(), "source-candidate chart contains untracked inputs")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    tree = subprocess.check_output(["git", "rev-parse", "HEAD^{tree}"], cwd=ROOT, text=True).strip()
    return revision, tree


def run(args):
    os.umask(0o077)
    require(platform_is_amd64(), "DAST uses reviewed linux/amd64 scanner identity")
    for executable in ("docker", "curl", "openssl", "node", args.helm, args.kubectl, args.k3d):
        require(shutil.which(executable), "required DAST executable is unavailable")
    revision, tree = source_identity()
    version = (ROOT / "VERSION").read_text().strip()
    inspected = json.loads(subprocess.check_output(["docker", "image", "inspect", args.image], text=True))[0]
    labels = inspected["Config"]["Labels"]
    require(labels.get("org.opencontainers.image.revision") == revision
            and labels.get("org.opencontainers.image.version") == version,
            "runtime image must match the exact current source revision/version")
    require(not args.report.exists(), "refusing to overwrite a report")
    require(not args.private_report.exists(), "refusing to overwrite private evidence")
    for destination in (args.report, args.private_report):
        absolute = destination.absolute()
        require(absolute.parent.is_dir() and absolute.parent.resolve() == absolute.parent
                and re.match(r"^/tmp/plinth-issue40[^/]*/", str(absolute)),
                "report destination must be an owned non-repository path")
    harness = DastHarness(args)
    error = None
    cleaned = False
    try:
        harness.execute_dast()
    except BaseException:
        error = traceback.format_exc()
    finally:
        try:
            harness.retain_private_failure_evidence()
        except BaseException:
            error = error or "private evidence capture failed"
        try:
            harness.inventory_owned_volumes()
        except BaseException:
            error = error or "owned volume inventory failed"
        try:
            harness.scanner.cleanup()
        except BaseException:
            error = error or "owned scanner cleanup failed"
        try:
            harness.cleanup()
        except BaseException:
            error = error or "owned deployment cleanup failed"
        try:
            remaining_volumes = harness.remove_owned_volumes()
            cleaned = harness.cleanup_complete and remaining_volumes == 0
        except BaseException:
            error = error or "owned volume cleanup failed"
            remaining_volumes = None
        try:
            require(source_identity() == (revision, tree),
                    "source-candidate identity changed during the scan")
        except BaseException:
            error = error or "source-candidate checkout changed during the scan"
        # Cleanup diagnostics must survive, including a failure of an earlier
        # independent stage. All raw traffic remains outside the repository.
        raw = {"details": harness.private_details, "alerts": harness.raw_alerts,
               "messages": harness.raw_messages, "error": error,
               "sourceRevision": revision, "sourceTree": tree}
        try:
            private_json(args.private_report, raw)
        except BaseException:
            error = error or "private evidence retention failed"
    containers = subprocess.check_output(["docker", "ps", "-a", "--format", "{{.Names}}"], text=True).splitlines()
    networks = subprocess.check_output(["docker", "network", "ls", "--format", "{{.Name}}"], text=True).splitlines()
    remaining = sum(name == harness.scanner.name or name == "k3d-" + harness.registry
                    or name.startswith("k3d-" + harness.cluster + "-") for name in containers)
    remaining_networks = int("k3d-" + harness.cluster in networks)
    cleanup = {"verified": cleaned and error is None,
               "namespaceAbsent": not harness.namespace_created,
               "clusterAbsent": remaining == 0,
               "registryAbsent": "k3d-" + harness.registry not in containers,
               "scannerContainerAbsent": harness.scanner.name not in containers,
               "temporaryFilesAbsent": not harness.root.exists(),
               "remainingContainers": remaining, "remainingNetworks": remaining_networks,
               "remainingVolumes": remaining_volumes}
    dispositions = json.loads(args.dispositions.read_text()) if args.dispositions else []
    scanner = {"completed": harness.scanner.completed and error is None,
               "version": SCANNER_VERSION,
               "passiveRecordsRemaining": harness.scanner.passive_remaining,
               "httpMessages": len(harness.raw_messages),
               "observedRoutes": sorted(harness.route_labels), "targetOrigin": harness.origin}
    identity = {"sourceRevision": revision, "sourceTree": tree,
                "imageDigest": harness.candidate_digest or "sha256:" + "0" * 64,
                "scannerDigest": SCANNER_DIGEST, "scannerVersion": SCANNER_VERSION}
    report = public_report(identity, [{"id": name, "passed": passed}
                                     for name, passed in harness.controls.items()], scanner,
                           harness.raw_alerts, dispositions, cleanup)
    private_json(args.report, report)
    print("ingress DAST: " + ("complete" if report.get("status") == "COMPLETE"
                              else "incomplete; private triage/evidence required"))
    return 0 if report.get("status") == "COMPLETE" else 1


def platform_is_amd64():
    import platform
    return platform.machine() in ("x86_64", "amd64")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, help="exact locally built candidate image")
    parser.add_argument("--kubernetes", choices=("min", "max"), default="max")
    parser.add_argument("--report", required=True, type=Path, help="new sanitized JSON destination")
    parser.add_argument("--private-report", required=True, type=Path,
                        help="new private raw destination; never publish/upload")
    parser.add_argument("--dispositions", type=Path,
                        help="reviewed exact finding groups, not blanket rule suppressions")
    parser.add_argument("--helm", default="helm")
    parser.add_argument("--kubectl", default="kubectl")
    parser.add_argument("--k3d", default="k3d")
    args = parser.parse_args()

    def interrupted(signum, _frame):
        raise KeyboardInterrupt(f"owned scan interrupted by signal {signum}")

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, interrupted)
    try:
        return run(args)
    except BaseException:
        # Never expose raw scanner errors, request bodies, credentials or
        # suspected vulnerability details through public CI stdout/stderr.
        print("ingress DAST: incomplete; private triage/evidence required", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
