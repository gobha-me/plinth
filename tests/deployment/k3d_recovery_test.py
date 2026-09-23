#!/usr/bin/env python3
"""Recover a quiesced K3s installation, then upgrade and restore a matched rollback."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import select
import shutil
import socket
import subprocess
import sys
import time
import zipfile

from k3d_lifecycle_test import (
    CHART,
    POSTGRES_IMAGE,
    POSTGRES_PASSWORD,
    ROOT,
    Harness,
    require,
)


RESTORE_ADMIN = "plinth_restore_test_admin"
NODE_BROWSER = r"""
import { chromium } from 'playwright';
import { createInterface } from 'node:readline';

const browser = await chromium.launch({headless: true,
  args: ['--no-proxy-server',
         '--host-resolver-rules=MAP plinth.test 127.0.0.1']});
const context = await browser.newContext({ignoreHTTPSErrors: true});
await context.addCookies([
  {name: 'plinth_session', value: process.env.PLINTH_RECOVERY_SESSION,
   url: process.env.PLINTH_RECOVERY_ORIGIN, httpOnly: true, secure: true,
   sameSite: 'Strict'},
  {name: 'plinth_csrf', value: process.env.PLINTH_RECOVERY_CSRF,
   url: process.env.PLINTH_RECOVERY_ORIGIN, secure: true,
   sameSite: 'Strict'},
]);
const page = await context.newPage();
const response = await page.goto(process.env.PLINTH_RECOVERY_ORIGIN + '/app/');
if (response.status() !== 200) throw new Error('deployed browser shell did not load');
await page.evaluate(async () => {
  const socket = new WebSocket(location.origin.replace(/^http/, 'ws') + '/ws/events');
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('WebSocket subscribe timeout')), 15000);
    socket.onmessage = event => {
      const frame = JSON.parse(event.data);
      if (frame.type === 'connected') {
        socket.send(JSON.stringify({type: 'subscribe',
          channels: ['plinth:data:ext_shell.user_preferences'], since_seq: 0}));
      } else if (frame.type === 'subscribed' &&
                 frame.channels.includes('plinth:data:ext_shell.user_preferences')) {
        clearTimeout(timer);
        resolve();
      }
    };
    socket.onerror = () => reject(new Error('browser WebSocket failed'));
  });
  window.recoverySocket = socket;
});
console.log('ready');
let request;
const commands = createInterface({input: process.stdin});
for await (const line of commands) {
  if (line.trim() === 'begin') {
    request = page.evaluate(() => fetch('/api/cap/shell.preferences.set', {
      method: 'POST',
      headers: {'Content-Type': 'application/json',
                'X-Plinth-CSRF': document.cookie.match(/(?:^|; )plinth_csrf=([^;]+)/)?.[1] || ''},
      body: JSON.stringify({args: {key: 'recovery_blocked'}}),
    }).then(response => response.status)).catch(error => error.name);
    console.log('issued');
  } else if (line.trim() === 'stop') {
    await context.close();
    await browser.close();
    break;
  } else if (line.startsWith('replay ')) {
    const since = Number(line.slice(7));
    const frames = await page.evaluate(async sinceSeq => {
      const socket = new WebSocket(location.origin.replace(/^http/, 'ws') + '/ws/events');
      return await new Promise((resolve, reject) => {
        const replayed = [];
        const timer = setTimeout(() => reject(new Error('WebSocket replay timeout')), 15000);
        socket.onmessage = event => {
          const frame = JSON.parse(event.data);
          if (frame.type === 'connected') {
            socket.send(JSON.stringify({type: 'subscribe',
              channels: ['plinth:data:ext_shell.user_preferences'], since_seq: sinceSeq}));
          } else if (frame.type === 'replay') {
            replayed.push(frame.envelope.seq);
          } else if (frame.type === 'replay_done') {
            clearTimeout(timer);
            socket.close();
            resolve(replayed);
          }
        };
        socket.onerror = () => reject(new Error('browser replay WebSocket failed'));
      });
    }, since);
    console.log('replay ' + JSON.stringify(frames));
  }
}
"""


class RecoveryHarness(Harness):
    def __init__(self, args):
        super().__init__(args)
        token = secrets.token_hex(4)
        self.cluster = "plinth-issue38-" + token
        self.registry = "plinth-issue38-reg-" + token
        self.namespace = "plinth-issue38-source-" + token
        self.release = "issue38-live"
        self.local_tag = f"127.0.0.1:{self.registry_port}/plinth:candidate"
        self.internal_repository = f"k3d-{self.registry}:5000/plinth"
        self.source_namespace = self.namespace
        self.restore_namespace = "plinth-issue38-restored-" + token
        self.rollback_namespace = "plinth-issue38-rollback-" + token
        self.extra_namespaces = set()
        self.previous_local_tag = f"127.0.0.1:{self.registry_port}/plinth:previous"
        self.current_digest = ""
        self.previous_digest = ""
        self.current_revision = ""
        self.previous_revision = ""

    def image_revision(self, image):
        result = self.run([
            "docker", "image", "inspect", "--format",
            '{{index .Config.Labels "org.opencontainers.image.revision"}}', image,
        ], timeout=30).stdout.strip()
        require(re.fullmatch(r"[0-9a-f]{40}", result) is not None,
                "image lacks an exact source revision label")
        return result

    def publish_previous(self):
        self.current_digest = self.candidate_digest
        self.current_revision = self.image_revision(self.args.image)
        self.previous_revision = self.image_revision(self.args.previous_image)
        require(self.current_revision != self.previous_revision,
                "candidate and previous image have the same source revision")
        previous_root = self.args.previous_chart.parents[2]
        previous_chart_revision = self.run(
            ["git", "-C", previous_root, "rev-parse", "HEAD"], timeout=15,
        ).stdout.strip()
        require(previous_chart_revision == self.previous_revision,
                "previous chart does not match previous image source revision")
        current_source_revision = self.run(
            ["git", "rev-parse", "HEAD"], timeout=15,
        ).stdout.strip()
        require(current_source_revision == self.current_revision,
                "current chart does not match current image source revision")
        self.run(["docker", "tag", self.args.previous_image, self.previous_local_tag],
                 timeout=30)
        pushed = self.run(["docker", "push", self.previous_local_tag], timeout=120)
        match = re.search(r"digest:\s*(sha256:[0-9a-f]{64})",
                          pushed.stdout + pushed.stderr)
        require(match is not None, "previous push did not report an immutable digest")
        self.previous_digest = match.group(1)
        require(self.previous_digest != self.current_digest,
                "candidate and previous image resolved to the same digest")
        self.candidate_digest = self.previous_digest

    def install_chart(self):
        self.write_values(self.previous_values("source-bootstrap", False))
        self.helm("upgrade", "--install", self.release,
                  self.args.previous_chart, "--namespace", self.namespace,
                  "--values", self.values_path, "--atomic", "--wait",
                  "--timeout", "5m", timeout=330)
        self.chart_installed = True
        self.kubectl("rollout", "status", "statefulset/" + self.workload_name,
                     "-n", self.namespace, "--timeout=240s", timeout=260)

    def previous_values(self, nonce, exposed):
        values = self.values(registration=False, nonce=nonce,
                             traefik_enabled=exposed)
        values["registration"] = {"enabled": False}
        return values

    def bootstrap_admin(self):
        # The prior revision permits only first-account registration through
        # local port-forward with registration.enabled=false.
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process = subprocess.Popen(
            [self.args.kubectl, "port-forward", "-n", self.namespace,
             "service/" + self.workload_name, f"{port}:8080",
             "--address=127.0.0.1"],
            cwd=ROOT, env=self.env, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                require(process.poll() is None,
                        "bootstrap port-forward exited unexpectedly")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                        break
                except OSError:
                    time.sleep(0.25)
            else:
                raise AssertionError("bootstrap port-forward did not become ready")
            request = self.root / "previous-bootstrap.json"
            descriptor = os.open(request, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                                 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                json.dump({"username": "issue36-admin",
                           "password": "fake-password-for-issue36!"}, output)
            origin = f"http://127.0.0.1:{port}"
            result = self.run(
                ["curl", "--silent", "--show-error", "--output", "/dev/null",
                 "--write-out", "%{http_code}", "--request", "POST",
                 "--header", "Content-Type: application/json",
                 "--header", "Origin: " + origin,
                 "--data-binary", "@" + str(request),
                 origin + "/api/auth/register"], check=False, timeout=30,
            )
            request.unlink(missing_ok=True)
            require(result.returncode == 0 and result.stdout.strip() == "201",
                    "prior-revision isolated administrator bootstrap failed: "
                    + result.stdout.strip())
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=5)

    def remove_bootstrap_authority(self):
        # The historical chart never projects a bootstrap Secret. Remove the
        # test harness's unused Secret before enabling public routing.
        container = self.active_pod()["spec"]["containers"][0]
        require("PLINTH_BOOTSTRAP_TOKEN" not in
                {entry["name"] for entry in container.get("env", [])},
                "historical Pod unexpectedly projected bootstrap authority")
        self.kubectl("delete", "secret/" + self.bootstrap_secret_name,
                     "-n", self.namespace, "--wait=true", "--timeout=30s",
                     timeout=40)
        absent = self.kubectl("get", "secret/" + self.bootstrap_secret_name,
                              "-n", self.namespace, check=False)
        require(absent.returncode != 0 and "NotFound" in absent.stderr,
                "bootstrap Secret survived source authority removal")

    def enable_public_route(self):
        old = self.active_pod()
        watcher = self.start_container_exit_watch(
            *self.container_runtime_identity(old))
        self.write_values(self.previous_values("source-public", True))
        self.helm("upgrade", self.release, self.args.previous_chart,
                  "--namespace", self.namespace, "--values", self.values_path,
                  "--atomic", "--wait", "--timeout", "5m", timeout=330)
        old_status = self.finish_container_exit_watch(watcher)
        new = self.active_pod()
        require(new["metadata"]["uid"] != old["metadata"]["uid"],
                "public exposure did not replace source Pod")
        self.verify_ordered_transition(old_status, new)
        self.wait_for_https()

    def postgres_pod(self):
        result = json.loads(self.kubectl(
            "get", "pods", "-n", self.namespace, "-l", "app=postgres", "-o", "json"
        ).stdout)["items"]
        require(len(result) == 1, "expected one PostgreSQL Pod")
        return result[0]["metadata"]["name"]

    def sql(self, statement, *, role="plinth", database="plinth"):
        result = self.kubectl(
            "exec", "-n", self.namespace, self.postgres_pod(), "--",
            "psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-U", role,
            "-d", database, "-c", statement, timeout=30,
        )
        return result.stdout.strip()

    def seed_installation(self):
        self.verify_login()
        fixture = ROOT / "tests/fixtures/install_lifecycle/valid-install"
        archive = self.root / "notes.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
            for path in sorted(fixture.rglob("*")):
                if path.is_file():
                    package.write(path, path.relative_to(fixture))
        code, status, body = self.curl(
            "/api/packages", form_file=archive, origin=self.origin,
            csrf=True, use_cookies=True,
        )
        require(code == 0 and status == "201" and json.loads(body)["state"] == "ACTIVE",
                f"notes package installation failed: {status} {body}")
        self.sql(
            "INSERT INTO plinth.users(id,username,password_hash) "
            "SELECT '00000000-0000-4000-8000-000000000038'::uuid,"
            "'recovery_fixture',password_hash FROM plinth.users "
            "WHERE username='issue36-admin'; "
            "INSERT INTO plinth.groups(name) VALUES ('recovery_operators'); "
            "INSERT INTO plinth.group_members(group_id,user_id) "
            "SELECT id,'00000000-0000-4000-8000-000000000038'::uuid "
            "FROM plinth.groups WHERE name='recovery_operators'; "
            "INSERT INTO plinth.group_rules(group_id,rule_id) "
            "SELECT g.id,r.id FROM plinth.groups g CROSS JOIN plinth.rbac_rules r "
            "WHERE g.name='recovery_operators' AND r.rule='notes.read'; "
            "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
            "SELECT id,'theme','\"dark\"'::jsonb FROM plinth.users "
            "WHERE username='issue36-admin'; "
            "INSERT INTO ext_notes.notes(id,body) VALUES "
            "('recovery-note','downstream data survives restore')"
        )
        pod = self.active_pod()["metadata"]["name"]
        self.kubectl(
            "exec", "-n", self.namespace, pod, "--", "sh", "-euc",
            "printf 'package-data-marker' > /var/lib/plinth/data/recovery-marker; "
            "printf 'log-marker' > /var/lib/plinth/logs/recovery-marker",
        )

    def snapshot(self):
        queries = {
            "users": "SELECT coalesce(json_agg((id,username,disabled_at) ORDER BY username)::text,'[]') FROM plinth.users WHERE NOT is_test_user",
            "group_members": "SELECT coalesce(json_agg((g.name,u.username) ORDER BY g.name,u.username)::text,'[]') FROM plinth.group_members m JOIN plinth.groups g ON g.id=m.group_id JOIN plinth.users u ON u.id=m.user_id",
            "grants": "SELECT coalesce(json_agg((g.name,r.rule) ORDER BY g.name,r.rule)::text,'[]') FROM plinth.group_rules gr JOIN plinth.groups g ON g.id=gr.group_id JOIN plinth.rbac_rules r ON r.id=gr.rule_id",
            "packages": "SELECT coalesce(json_agg((id,name,version,state,provenance,supersedes_id) ORDER BY name,version)::text,'[]') FROM plinth.packages WHERE state IN ('ACTIVE','ACTIVE_FLAGGED')",
            "preferences": "SELECT coalesce(json_agg((user_id,key,value) ORDER BY user_id,key)::text,'[]') FROM ext_shell.user_preferences",
            "downstream": "SELECT coalesce(json_agg((id,body) ORDER BY id)::text,'[]') FROM ext_notes.notes",
            "extension_roles": "SELECT coalesce(json_agg((extension_name,role_name) ORDER BY extension_name)::text,'[]') FROM plinth.extension_database_credentials",
        }
        return {name: json.loads(self.sql(query)) for name, query in queries.items()}

    def verify_retained_user(self):
        admin_cookies = self.cookies
        self.cookies = self.root / "retained-user-cookies"
        try:
            code, status, body = self.curl(
                "/api/auth/login", data=json.dumps({
                    "username": "recovery_fixture",
                    "password": "fake-password-for-issue36!",
                }), origin=self.origin, cookie_jar=self.cookies,
            )
            require(code == 0 and status == "200",
                    f"restored user cannot log in: {status} {body}")
            code, status, body = self.curl(
                "/api/cap/notes.read",
                data=json.dumps({"args": {"id": "recovery-note"}}),
                origin=self.origin, csrf=True, use_cookies=True,
            )
            require(code == 0 and status == "200" and "recovery-note" in body,
                    f"restored user lost notes.read grant: {status} {body}")
        finally:
            self.cookies = admin_cookies

    def verify_files(self):
        pod = self.active_pod()["metadata"]["name"]
        for claim, expected in (("data", "package-data-marker"),
                                ("logs", "log-marker")):
            observed = self.kubectl(
                "exec", "-n", self.namespace, pod, "--", "cat",
                f"/var/lib/plinth/{claim}/recovery-marker",
            ).stdout
            require(observed == expected, f"{claim} claim lost its marker")
        active = self.kubectl(
            "exec", "-n", self.namespace, pod, "--", "readlink",
            "/var/lib/plinth/data/extensions/notes/active",
        ).stdout.strip()
        require(active == "1.2.3", "restored package active pointer drifted")

    def _binary_command(self, argv, *, stage, output=None, input_path=None,
                        timeout=120):
        require((output is None) != (input_path is None),
                "binary transfer needs one input or output file")
        stream = None
        try:
            if output is not None:
                descriptor = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                stream = os.fdopen(descriptor, "wb")
                result = subprocess.run(
                    [str(part) for part in argv], cwd=ROOT, env=self.env,
                    stdout=stream, stderr=subprocess.PIPE, timeout=timeout,
                )
            else:
                stream = open(input_path, "rb")
                result = subprocess.run(
                    [str(part) for part in argv], cwd=ROOT, env=self.env,
                    stdin=stream, stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, timeout=timeout,
                )
            if result.returncode != 0:
                diagnostic = self.root / f"transfer-{stage}.stderr"
                descriptor = os.open(diagnostic, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                                     0o600)
                with os.fdopen(descriptor, "wb") as error_file:
                    error_file.write(result.stderr)
                raise AssertionError(
                    f"binary transfer {stage} failed with exit "
                    f"{result.returncode}; stderr retained privately until cleanup")
        finally:
            if stream is not None:
                stream.close()

    def helper(self):
        name = "recovery-volume-helper"
        claims = {kind: self.workload_name + "-" + kind for kind in ("data", "logs")}
        self.apply_json({
            "apiVersion": "v1", "kind": "Pod",
            "metadata": {"name": name, "namespace": self.namespace,
                         "labels": {"plinth.gobha.me/test": "issue38-helper"}},
            "spec": {
                "automountServiceAccountToken": False,
                "restartPolicy": "Never",
                "securityContext": {"seccompProfile": {"type": "RuntimeDefault"}},
                "containers": [{
                    "name": "archive", "image": POSTGRES_IMAGE,
                    "command": ["sleep", "3600"],
                    "volumeMounts": [{"name": kind, "mountPath": "/" + kind}
                                     for kind in claims],
                    "resources": {"requests": {"cpu": "25m", "memory": "64Mi"},
                                  "limits": {"cpu": "1", "memory": "256Mi"}},
                }],
                "volumes": [{"name": kind,
                             "persistentVolumeClaim": {"claimName": claim}}
                            for kind, claim in claims.items()],
            },
        })
        self.kubectl("wait", "--for=condition=Ready", "pod/" + name,
                     "-n", self.namespace, "--timeout=120s", timeout=135)
        return name

    def remove_helper(self, name):
        self.kubectl("delete", "pod/" + name, "-n", self.namespace,
                     "--wait=true", "--timeout=90s", timeout=105)

    def stop_plinth(self):
        pod = self.active_pod()
        watcher = self.start_container_exit_watch(
            *self.container_runtime_identity(pod))
        started = time.monotonic()
        self.kubectl("scale", "statefulset/" + self.workload_name,
                     "-n", self.namespace, "--replicas=0")
        self.kubectl("wait", "--for=delete", "pod/" + pod["metadata"]["name"],
                     "-n", self.namespace, "--timeout=65s", timeout=75)
        require(time.monotonic() - started < 65, "quiesce exceeded Pod bound")
        self.finish_container_exit_watch(watcher)

    def backup(self):
        self.stop_plinth()
        artifacts = {}
        pgpod = self.postgres_pod()
        for name, command in (
            ("globals", ["pg_dumpall", "-U", "plinth", "--globals-only"]),
            ("database", ["pg_dump", "-U", "plinth", "-d", "plinth",
                          "--format=custom", "--create"]),
        ):
            path = self.root / (name + ".backup")
            self._binary_command(
                [self.args.kubectl, "exec", "-n", self.namespace, pgpod,
                 "--", *command], stage=f"backup-{name}", output=path,
            )
            require(path.stat().st_size > 100, f"{name} backup is empty")
            artifacts[name] = path
        helper = self.helper()
        try:
            for kind in ("data", "logs"):
                path = self.root / (kind + ".tar")
                self._binary_command(
                    [self.args.kubectl, "exec", "-n", self.namespace, helper,
                     "--", "tar", "-C", "/" + kind, "--numeric-owner",
                     "-cf", "-", "."],
                    stage=f"backup-{kind}", output=path,
                )
                require(path.stat().st_size > 100, f"{kind} backup is empty")
                artifacts[kind] = path
        finally:
            self.remove_helper(helper)
        artifacts["sha256"] = {
            name: hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in artifacts.items()
        }
        return artifacts

    def delete_current_namespace(self):
        if self.chart_installed:
            self.helm("uninstall", self.release, "-n", self.namespace,
                      "--wait", "--timeout", "180s", timeout=200)
            self.chart_installed = False
        if self.namespace_created:
            old = self.namespace
            self.kubectl("delete", "namespace", old, "--wait=true",
                         "--timeout=180s", timeout=200)
            absent = self.kubectl("get", "namespace", old, check=False)
            require(absent.returncode != 0 and "NotFound" in absent.stderr,
                    "task namespace survived deletion")
            self.namespace_created = False

    def create_restore_namespace(self, name):
        require(not self.namespace_created and not self.chart_installed,
                "old namespace is still active at restore")
        self.namespace = name
        self.kubectl("create", "namespace", name)
        self.namespace_created = True
        self.kubectl("label", "namespace", name,
                     "plinth.gobha.me/test=issue38", "--overwrite")
        self.kubectl("create", "secret", "tls", "plinth-tls", "--cert",
                     self.certificate, "--key", self.private_key, "-n", name)
        self.apply_json({
            "apiVersion": "v1", "kind": "Secret",
            "metadata": {"name": "plinth-database", "namespace": name},
            "type": "Opaque",
            "stringData": {"host": "postgres", "port": "5432",
                           "user": "plinth", "password": POSTGRES_PASSWORD,
                           "database": "plinth"},
        })
        for kind in ("data", "logs"):
            self.apply_json({
                "apiVersion": "v1", "kind": "PersistentVolumeClaim",
                "metadata": {"name": self.workload_name + "-" + kind,
                             "namespace": name,
                             "labels": {"plinth.gobha.me/test": "issue38",
                                        "app.kubernetes.io/instance": self.release}},
                "spec": {
                    "accessModes": ["ReadWriteOnce"],
                    "storageClassName": "local-path",
                    "resources": {"requests": {"storage": "1Gi"}},
                },
            })
        self.apply_json({
            "apiVersion": "v1", "kind": "Service",
            "metadata": {"name": "postgres", "namespace": name},
            "spec": {"selector": {"app": "postgres"},
                     "ports": [{"name": "postgres", "port": 5432,
                                "targetPort": 5432}]},
        })
        self.apply_json({
            "apiVersion": "apps/v1", "kind": "Deployment",
            "metadata": {"name": "postgres", "namespace": name},
            "spec": {
                "replicas": 1,
                "selector": {"matchLabels": {"app": "postgres"}},
                "template": {
                    "metadata": {"labels": {"app": "postgres"}},
                    "spec": {
                        "automountServiceAccountToken": False,
                        "securityContext": {"seccompProfile": {"type": "RuntimeDefault"}},
                        "containers": [{
                            "name": "postgres", "image": POSTGRES_IMAGE,
                            "env": [
                                {"name": "POSTGRES_USER", "value": RESTORE_ADMIN},
                                {"name": "POSTGRES_DB", "value": RESTORE_ADMIN},
                                {"name": "POSTGRES_PASSWORD", "valueFrom": {
                                    "secretKeyRef": {"name": "plinth-database",
                                                     "key": "password"}}},
                            ],
                            "readinessProbe": {"exec": {"command": [
                                "pg_isready", "-U", RESTORE_ADMIN,
                                "-d", RESTORE_ADMIN]},
                                "periodSeconds": 2, "timeoutSeconds": 2,
                                "failureThreshold": 30},
                            "resources": {
                                "requests": {"cpu": "50m", "memory": "128Mi"},
                                "limits": {"cpu": "1", "memory": "512Mi"}},
                            "volumeMounts": [{"name": "database",
                                              "mountPath": "/var/lib/postgresql/data"}],
                        }],
                        "volumes": [{"name": "database", "emptyDir": {}}],
                    },
                },
            },
        })
        self.kubectl("rollout", "status", "deployment/postgres", "-n", name,
                     "--timeout=180s", timeout=200)

    def restore(self, artifacts):
        for name, path in artifacts.items():
            if name != "sha256":
                require(path.is_file() and path.stat().st_size > 100,
                        f"missing {name} backup artifact")
                require(hashlib.sha256(path.read_bytes()).hexdigest()
                        == artifacts["sha256"][name],
                        f"{name} backup hash changed")
        pod = self.postgres_pod()
        self._binary_command(
            [self.args.kubectl, "exec", "-i", "-n", self.namespace, pod,
             "--", "psql", "-X", "-v", "ON_ERROR_STOP=1", "-U",
             RESTORE_ADMIN, "-d", RESTORE_ADMIN],
            stage="restore-globals", input_path=artifacts["globals"],
        )
        self._binary_command(
            [self.args.kubectl, "exec", "-i", "-n", self.namespace, pod,
             "--", "pg_restore", "-U", RESTORE_ADMIN, "-d", RESTORE_ADMIN,
             "--create", "--exit-on-error"],
            stage="restore-database", input_path=artifacts["database"],
        )
        require(self.sql("SELECT current_database()") == "plinth",
                "database restore did not create Plinth database")
        helper = self.helper()
        try:
            for kind in ("data", "logs"):
                self._binary_command(
                    [self.args.kubectl, "exec", "-i", "-n", self.namespace,
                     helper, "--", "tar", "-C", "/" + kind,
                     "--numeric-owner", "-xpf", "-"],
                    stage=f"restore-{kind}", input_path=artifacts[kind],
                )
        finally:
            self.remove_helper(helper)

    def install_restored(self, digest, nonce, chart):
        self.candidate_digest = digest
        values = (self.previous_values(nonce, True)
                  if chart == self.args.previous_chart else
                  self.values(registration=False, nonce=nonce,
                              traefik_enabled=True))
        for kind in ("data", "logs"):
            values["persistence"][kind]["existingClaim"] = (
                self.workload_name + "-" + kind)
        self.write_values(values)
        self.helm("upgrade", "--install", self.release, chart,
                  "--namespace", self.namespace, "--values", self.values_path,
                  "--atomic", "--wait", "--timeout", "5m", timeout=330)
        self.chart_installed = True
        self.kubectl("rollout", "status", "statefulset/" + self.workload_name,
                     "-n", self.namespace, "--timeout=240s", timeout=260)
        self.active_pod()
        self.wait_for_https()
        self.verify_login()
        self.verify_retained_user()
        self.verify_files()

    def verify_snapshot(self, expected):
        observed = self.snapshot()
        require(observed == expected,
                "restored identity, grant, package, preference, or downstream data drifted: "
                + ", ".join(name for name in expected
                            if expected[name] != observed[name]))

    def upgrade_current(self, expected):
        old = self.active_pod()
        old_id = old["metadata"]["uid"]
        watcher = self.start_container_exit_watch(
            *self.container_runtime_identity(old))
        claims = self.pvc_uids()
        self.candidate_digest = self.current_digest
        values = self.values(registration=False, nonce="current-revision",
                             traefik_enabled=True)
        for kind in ("data", "logs"):
            values["persistence"][kind]["existingClaim"] = (
                self.workload_name + "-" + kind)
        self.write_values(values)
        self.helm("upgrade", self.release, CHART,
                  "--namespace", self.namespace, "--values", self.values_path,
                  "--reset-values", "--atomic", "--wait", "--timeout", "5m",
                  timeout=330)
        old_status = self.finish_container_exit_watch(watcher)
        new = self.active_pod()
        require(new["metadata"]["uid"] != old_id,
                "versioned digest upgrade did not replace the Pod")
        self.verify_ordered_transition(old_status, new)
        require(self.pvc_uids() == claims, "upgrade replaced retained claims")
        self.wait_for_https()
        self.verify_login()
        self.verify_retained_user()
        self.verify_snapshot(expected)
        self.verify_files()

    @staticmethod
    def _read_marker(process, marker, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            require(process.poll() is None,
                    f"browser exited before {marker}")
            ready, _, _ = select.select([process.stdout], [], [],
                                        min(1, deadline - time.monotonic()))
            if ready:
                line = process.stdout.readline().strip()
                if line == marker:
                    return
                raise AssertionError(f"unexpected browser marker: {line!r}")
        raise TimeoutError(f"browser did not report {marker}")

    def _wait_sql(self, statement, expected, *, stage, timeout=15):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.sql(statement)
            if last == expected:
                return
            time.sleep(0.1)
        raise AssertionError(
            f"database barrier {stage} did not reach {expected}: {last}")

    def _wait_restarted(self, old_container_id):
        deadline = time.monotonic() + 120
        last = ""
        while time.monotonic() < deadline:
            try:
                self.kubectl("rollout", "status",
                             "statefulset/" + self.workload_name,
                             "-n", self.namespace, "--timeout=10s", timeout=15)
                pod = self.active_pod()
                _, container_id = self.container_runtime_identity(pod)
                if container_id != old_container_id:
                    return pod
                last = "old container is still active"
            except (AssertionError, RuntimeError) as error:
                last = str(error)
            time.sleep(0.5)
        raise AssertionError("Pod supervisor did not restart Plinth: " + last)

    def _wait_shutdown_started(self, node, container_id):
        # The coordinator emits this after it has closed ingress. In
        # particular, a Pod deletion timestamp alone does not prove that
        # kubelet delivered SIGTERM to Plinth's PID 1.
        deadline = time.monotonic() + 15
        last = ""
        while time.monotonic() < deadline:
            try:
                result = self.run(
                    ["docker", "exec", node, "crictl", "logs", container_id],
                    check=False, timeout=3,
                )
            except subprocess.TimeoutExpired:
                last = "CRI log poll timed out"
                continue
            if "shutdown: ingress gate closed" in result.stdout:
                return
            last = result.stderr.strip() if result.returncode else "marker pending"
            time.sleep(0.1)
        raise AssertionError(
            "exact old container did not enter the shutdown coordinator: " + last
        )

    def verify_active_signal(self, signame):
        require(signame in ("SIGINT", "SIGTERM"), "unsupported signal")
        self.verify_login()
        user = self.sql("SELECT id::text FROM plinth.users "
                        "WHERE username='issue36-admin'")
        require(re.fullmatch(r"[0-9a-f-]{36}", user) is not None,
                "administrator identity is missing")
        prior_seq = self.sql(
            "SELECT coalesce(max(seq),0) FROM plinth.events "
            "WHERE channel='plinth:data:ext_shell.user_preferences'"
        )
        self.sql(
            "INSERT INTO ext_shell.user_preferences(user_id,key,value) "
            f"VALUES ('{user}'::uuid,'recovery_blocked','\"before\"'::jsonb), "
            f"('{user}'::uuid,'recovery_control','true'::jsonb) "
            "ON CONFLICT (user_id,key) DO UPDATE SET value=EXCLUDED.value"
        )
        code, status, body = self.curl(
            "/api/cap/shell.preferences.set",
            data=json.dumps({"args": {"key": "recovery_control"}}),
            origin=self.origin, csrf=True, use_cookies=True,
        )
        require(code == 0 and status == "200" and
                json.loads(body).get("value", {}).get("deleted") is True,
                f"control preference deletion failed with HTTP {status}")
        self._wait_sql(
            "SELECT (coalesce(max(seq),0) > " + prior_seq + ")::int "
            "FROM plinth.events WHERE channel="
            "'plinth:data:ext_shell.user_preferences'",
            "1", stage="seed-event", timeout=20,
        )
        before_seq = self.sql(
            "SELECT coalesce(max(seq),0) FROM plinth.events "
            "WHERE channel='plinth:data:ext_shell.user_preferences'"
        )
        pod = self.active_pod()
        old_runtime = self.container_runtime_identity(pod)
        browser_env = self.env | {
            "PLINTH_RECOVERY_ORIGIN": self.origin,
            "PLINTH_RECOVERY_SESSION": self.cookie("plinth_session"),
            "PLINTH_RECOVERY_CSRF": self.cookie("plinth_csrf"),
        }
        require(browser_env["PLINTH_RECOVERY_SESSION"] and
                browser_env["PLINTH_RECOVERY_CSRF"],
                "browser has no authenticated session/CSRF cookies")
        browser = subprocess.Popen(
            ["node", "--input-type=module", "-e", NODE_BROWSER],
            cwd=ROOT / "tests/browser", env=browser_env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        lock = None
        watcher = None
        try:
            self._read_marker(browser, "ready", 45)
            lock = subprocess.Popen(
                [self.args.kubectl, "exec", "-i", "-n", self.namespace,
                 self.postgres_pod(), "--", "psql", "-XAt", "-v",
                 "ON_ERROR_STOP=1", "-U", "plinth", "-d", "plinth"],
                cwd=ROOT, env=self.env, stdin=subprocess.PIPE,
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                text=True,
            )
            lock.stdin.write("BEGIN;\nLOCK TABLE ext_shell.user_preferences "
                             "IN SHARE MODE;\n")
            lock.stdin.flush()
            self._wait_sql(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE datname=current_database() AND state='idle in transaction' "
                "AND query LIKE 'LOCK TABLE ext_shell.user_preferences%'",
                "1", stage="held-lock",
            )
            browser.stdin.write("begin\n")
            browser.stdin.flush()
            self._read_marker(browser, "issued", 10)
            self._wait_sql(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE datname=current_database() AND wait_event_type='Lock' "
                "AND query LIKE 'DELETE FROM ext_shell.user_preferences%'",
                "1", stage="blocked-browser-write",
            )
            watcher = self.start_container_exit_watch(*old_runtime)
            sent = time.monotonic()
            if signame == "SIGINT":
                self.kubectl(
                    "exec", "-n", self.namespace, pod["metadata"]["name"],
                    "--", "sh", "-euc",
                    "test \"$(readlink /proc/1/exe)\" = /usr/local/bin/plinth; "
                    "kill -INT 1", timeout=15,
                )
            else:
                self.kubectl(
                    "delete", "pod/" + pod["metadata"]["name"],
                    "-n", self.namespace, "--wait=false", timeout=15,
                )
            self._wait_shutdown_started(*old_runtime)
            # The signal was delivered to the deployed PID 1 while the browser
            # request was admitted and blocked on PostgreSQL. Let it commit.
            lock.stdin.write("ROLLBACK;\n\\q\n")
            lock.stdin.flush()
            lock.stdin.close()
            lock.stdin = None
            _, lock_errors = lock.communicate(timeout=15)
            require(lock.returncode == 0,
                    "database lock release failed: " + lock_errors[:200])
            lock = None
            old_status = self.finish_container_exit_watch(watcher)
            watcher = None
            require(time.monotonic() - sent < 65,
                    f"{signame} exceeded the supervisor shutdown bound")
            new = self._wait_restarted(old_runtime[1])
            require((new["metadata"]["uid"] != pod["metadata"]["uid"])
                    == (signame == "SIGTERM"),
                    f"{signame} did not follow the expected Pod supervisor path")
            self.verify_ordered_transition(old_status, new)
            self.wait_for_https()
            self._wait_sql(
                "SELECT count(*) FROM ext_shell.user_preferences "
                f"WHERE user_id='{user}'::uuid AND key='recovery_blocked'",
                "0", stage="committed-preference-deletion", timeout=20,
            )
            self._wait_sql(
                "SELECT (coalesce(max(seq),0) > " + before_seq + ")::int "
                "FROM plinth.events WHERE channel="
                "'plinth:data:ext_shell.user_preferences'",
                "1", stage="committed-event", timeout=20,
            )
            final_seq = self.sql(
                "SELECT coalesce(max(seq),0) FROM plinth.events "
                "WHERE channel='plinth:data:ext_shell.user_preferences'"
            )
            browser.stdin.write("replay " + before_seq + "\n")
            browser.stdin.flush()
            ready, _, _ = select.select([browser.stdout], [], [], 20)
            require(ready, "browser did not complete post-restart replay")
            replay_line = browser.stdout.readline().strip()
            require(replay_line.startswith("replay "),
                    f"browser replay failed: {replay_line!r}")
            require(json.loads(replay_line[7:]) == [int(final_seq)],
                    "browser replay did not deliver the one durable write")
            self.verify_login()
        finally:
            primary_failure = sys.exc_info()[0] is not None
            if lock is not None:
                try:
                    lock.stdin.write("ROLLBACK;\n\\q\n")
                    lock.stdin.flush()
                    lock.stdin.close()
                    lock.stdin = None
                    lock.communicate(timeout=5)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    lock.kill()
                    lock.communicate(timeout=5)
            if browser.poll() is None:
                try:
                    browser.stdin.write("stop\n")
                    browser.stdin.flush()
                    _, browser_errors = browser.communicate(timeout=10)
                except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                    browser.kill()
                    _, browser_errors = browser.communicate(timeout=5)
            else:
                _, browser_errors = browser.communicate(timeout=5)
            if browser.returncode != 0:
                diagnostic = self.root / f"browser-{signame}.stderr"
                descriptor = os.open(diagnostic, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                                     0o600)
                with os.fdopen(descriptor, "w", encoding="utf-8") as error_file:
                    error_file.write(browser_errors)
                if not primary_failure:
                    raise AssertionError(
                        f"deployed browser failed during {signame}; "
                        "stderr retained privately until cleanup")

    def cleanup(self):
        if self.cleanup_complete:
            return
        errors = []
        try:
            super().cleanup()
        except RuntimeError as error:
            errors.append(str(error))
        previous = self.run(
            ["docker", "image", "inspect", self.previous_local_tag],
            check=False, timeout=30,
        )
        if previous.returncode == 0:
            removed = self.run(
                ["docker", "image", "rm", self.previous_local_tag],
                check=False, timeout=60,
            )
            if removed.returncode != 0:
                errors.append("previous image tag cleanup failed")
        remaining = self.run(
            ["docker", "image", "inspect", self.previous_local_tag],
            check=False, timeout=30,
        )
        if remaining.returncode == 0:
            errors.append("previous image tag remains")
        if errors:
            raise RuntimeError("; ".join(errors))

    def execute(self):
        self.create_infrastructure()
        self.publish_previous()
        self.create_namespace_dependencies()
        self.install_chart()
        self.bootstrap_admin()
        self.remove_bootstrap_authority()
        self.enable_public_route()
        self.seed_installation()
        self.verify_retained_user()
        expected = self.snapshot()
        self.verify_files()
        artifacts = self.backup()
        self.delete_current_namespace()

        self.create_restore_namespace(self.restore_namespace)
        self.restore(artifacts)
        self.install_restored(self.previous_digest, "restored-previous",
                              self.args.previous_chart)
        self.verify_snapshot(expected)
        self.verify_retained_user()
        self.upgrade_current(expected)
        for signame in ("SIGINT", "SIGTERM"):
            self.verify_active_signal(signame)
        self.sql("DELETE FROM ext_shell.user_preferences "
                 "WHERE key='recovery_blocked'")
        self.verify_snapshot(expected)
        self.stop_plinth()
        self.delete_current_namespace()

        # A digest-only rollback cannot undo schema or package state. Restore
        # the matched pre-upgrade database and claim set before old code starts.
        self.create_restore_namespace(self.rollback_namespace)
        self.restore(artifacts)
        self.install_restored(self.previous_digest, "matched-rollback",
                              self.args.previous_chart)
        self.verify_snapshot(expected)
        self.verify_retained_user()
        self.stop_plinth()
        self.delete_current_namespace()
        print("k3d recovery: matched backup/restore, revision upgrade and "
              "rollback, active-work SIGINT/SIGTERM passed; "
              f"previous={self.previous_revision} current={self.current_revision}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True)
    parser.add_argument("--previous-image", required=True)
    parser.add_argument("--previous-chart", required=True, type=Path)
    parser.add_argument("--kubernetes", choices=("min", "max"), default="max")
    parser.add_argument("--helm", default="helm")
    parser.add_argument("--kubectl", default="kubectl")
    parser.add_argument("--k3d", default="k3d")
    args = parser.parse_args()
    args.previous_chart = args.previous_chart.resolve(strict=True)
    require((args.previous_chart / "Chart.yaml").is_file(),
            "previous source chart is missing Chart.yaml")
    require(CHART.is_dir(), "current source chart is missing")
    for executable in ("docker", "curl", "git", "node", "openssl",
                       args.helm, args.kubectl, args.k3d):
        require(shutil.which(executable),
                f"required executable is unavailable: {executable}")
    harness = RecoveryHarness(args)
    try:
        harness.execute()
    except BaseException:
        harness.diagnostics()
        raise
    finally:
        harness.cleanup()


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, RuntimeError, TimeoutError) as error:
        print(f"k3d recovery failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
