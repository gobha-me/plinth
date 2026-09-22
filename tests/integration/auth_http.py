#!/usr/bin/env python3
"""Production HTTP authentication regressions in an owned disposable database."""

import argparse
import base64
import contextlib
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
from http.cookies import SimpleCookie
import ipaddress
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import tempfile
import time
import uuid


class Kernel:
    def __init__(self, binary, root, database, pg_env, registration="disabled",
                 bootstrap_token="fixture-bootstrap-authority-at-least-32-bytes",
                 configured_https_origin=False):
        self.database = database
        self.pg_env = pg_env | {"PGDATABASE": database}
        self.binary = binary
        self.root = root
        self.child = None
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        repo = Path(__file__).resolve().parents[2]
        self.config = root / "config.json"
        registration_config = ({"mode": "open" if registration else "disabled"}
                               if isinstance(registration, bool)
                               else ({"mode": registration} if isinstance(registration, str)
                                     else dict(registration)))
        self.config_data = {
            "listen_host": "127.0.0.1", "listen_port": self.port,
            "dev_mode": False, "registration": registration_config,
            "database": {"pool_size": 8},
            "shell": {"enabled": False},
            "migrations_dir": str(repo / "migrations"),
            "packages": {"data_dir": str(root / "data"),
                         "staging_dir": str(root / "staging")}}
        if configured_https_origin:
            self.config_data["browser_origin"] = f"https://127.0.0.1:{self.port}"
        self.config.write_text(json.dumps(self.config_data))
        self.bootstrap_token = bootstrap_token

    def set_registration(self, registration):
        self.config_data["registration"] = (
            {"mode": registration} if isinstance(registration, str)
            else dict(registration))
        self.config.write_text(json.dumps(self.config_data))

    def sql(self, statement):
        return sql(self.pg_env, statement)

    def request(self, method, path, body=None, token=None, cookie=None,
                extra_headers=None):
        headers = dict(extra_headers or {})
        if body is not None:
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = "Bearer " + token
        if cookie is not None:
            headers["Cookie"] = cookie
        with contextlib.closing(http.client.HTTPConnection(
                "127.0.0.1", self.port, timeout=10)) as connection:
            connection.request(method, path, None if body is None else json.dumps(body), headers)
            response = connection.getresponse()
            payload = response.read()
            return response.status, (json.loads(payload) if payload else None), response.getheaders()

    def start(self):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("PLINTH_")}
        env.update({"PLINTH_PG_" + suffix: self.pg_env["PG" + suffix]
                    for suffix in ("HOST", "PORT", "USER", "PASSWORD", "DATABASE")})
        env.update(PLINTH_DEV_MODE="false", PLINTH_PG_POOL_SIZE="8")
        if self.bootstrap_token is not None:
            env["PLINTH_BOOTSTRAP_TOKEN"] = self.bootstrap_token
        with (self.root / "kernel.log").open("wb") as output:
            self.child = subprocess.Popen(
                [str(self.binary), "serve", "--config", str(self.config)],
                env=env, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            if self.child.poll() is not None:
                raise AssertionError("production kernel exited during startup")
            try:
                if self.request("GET", "/healthz")[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.025)  # Readiness polling, not concurrency coordination.
        raise AssertionError("production kernel did not become ready")

    def stop(self):
        if self.child is None:
            return
        if self.child.poll() is None:
            self.child.terminate()
        try:
            code = self.child.wait(timeout=55)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.wait(timeout=5)
            raise AssertionError("production kernel exceeded its shutdown bound") from None
        if code != 0:
            raise AssertionError(f"production kernel exited with status {code}")


def sql(env, statement):
    return subprocess.run(["psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-c", statement],
                          env=env, check=True, timeout=15, text=True,
                          capture_output=True).stdout.strip()


@contextlib.contextmanager
def running_kernel(binary, pg_env, registration="disabled",
                   bootstrap_token="fixture-bootstrap-authority-at-least-32-bytes",
                   configured_https_origin=False):
    database = "plinth_auth_" + uuid.uuid4().hex
    sql(pg_env, f'CREATE DATABASE "{database}"')
    try:
        with tempfile.TemporaryDirectory(prefix="plinth-auth-http-") as temporary:
            kernel = Kernel(binary, Path(temporary), database, pg_env,
                            registration, bootstrap_token, configured_https_origin)
            try:
                kernel.start()
                yield kernel
            except BaseException:
                log = kernel.root / "kernel.log"
                if log.exists():
                    print(log.read_text(errors="replace"), flush=True)
                raise
            finally:
                kernel.stop()
    finally:
        # Capture only logins with dependencies on this uniquely owned database.
        # Legacy aliases are shared names and are deliberately left alone.
        roles = sql(pg_env,
            "SELECT DISTINCT r.rolname FROM pg_roles r JOIN pg_shdepend d "
            "ON d.refclassid='pg_authid'::regclass AND d.refobjid=r.oid "
            "JOIN pg_database db ON db.oid=d.dbid "
            f"WHERE db.datname='{database}' AND r.rolname ~ '^px_[0-9a-f]{{60}}$'").splitlines()
        sql(pg_env, f'DROP DATABASE "{database}" WITH (FORCE)')
        for role in roles:
            sql(pg_env, 'DROP ROLE "' + role.replace('"', '""') + '"')


def websocket_auth(kernel, token):
    """Authenticate a fresh real WebSocket using the browser cookie path."""
    with socket.create_connection(("127.0.0.1", kernel.port), timeout=5) as peer:
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        host = f"127.0.0.1:{kernel.port}"
        peer.sendall((f"GET /ws/events HTTP/1.1\r\nHost: {host}\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                      f"Origin: http://{host}\r\nCookie: plinth_session={token}\r\n\r\n"
                      ).encode("ascii"))
        deadline = time.monotonic() + 8

        def receive(length):
            result = bytearray()
            while len(result) < length:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError("WebSocket authentication deadline exceeded")
                peer.settimeout(remaining)
                data = peer.recv(length - len(result))
                if not data:
                    raise AssertionError("WebSocket closed before authentication response")
                result.extend(data)
            return bytes(result)

        headers = bytearray()
        while not headers.endswith(b"\r\n\r\n"):
            assert len(headers) < 16384, "unbounded upgrade response"
            headers.extend(receive(1))
        assert headers.startswith(b"HTTP/1.1 101 "), "WebSocket upgrade failed"
        opcode, length = receive(2)
        assert opcode == 0x81 and length < 128, "expected unmasked final text frame"
        if length == 126:
            length = struct.unpack("!H", receive(2))[0]
        else:
            assert length != 127, "unbounded WebSocket frame"
        return json.loads(receive(length))


def response_cookies(headers):
    cookies = SimpleCookie()
    for name, value in headers:
        if name.lower() == "set-cookie":
            cookies.load(value)
    return cookies


def response_header(headers, expected_name):
    for name, value in headers:
        if name.lower() == expected_name.lower():
            return value
    return ""


def bootstrap_user(kernel, credentials, token=None, extra_headers=None):
    body = dict(credentials)
    body["bootstrap_token"] = kernel.bootstrap_token if token is None else token
    return kernel.request("POST", "/api/auth/bootstrap", body,
                          extra_headers=extra_headers)


def login_auth(kernel, credentials):
    status, body, headers = kernel.request("POST", "/api/auth/login", credentials)
    assert status == 200, f"login returned {status}: {body}"
    cookies = response_cookies(headers)
    session = cookies["plinth_session"].value
    csrf = cookies["plinth_csrf"].value
    cookie = f"plinth_session={session}; plinth_csrf={csrf}"
    return session, cookie, csrf


def login_session(kernel, credentials):
    return login_auth(kernel, credentials)[0]


def assert_non_object_json_rejected(kernel, method, path, token=None):
    for body in ([], "not-an-object"):
        status, response, _ = kernel.request(method, path, body, token=token)
        assert status == 400 and response["error"] == "invalid_request", \
            f"{method} {path} accepted top-level {type(body).__name__}: {status} {response}"


def csrf_contract(binary, pg_env):
    with running_kernel(binary, pg_env, "open") as kernel:
        origin = f"http://127.0.0.1:{kernel.port}"
        credentials = {
            "username": "csrf-primary",
            "password": "fake-password-for-csrf-test",
        }

        status, _, _ = bootstrap_user(kernel, credentials)
        assert status == 201, f"native bootstrap returned {status}"

        status, body, _ = kernel.request(
            "POST", "/api/auth/login", credentials,
            extra_headers={"Origin": "https://cross-origin.invalid"})
        assert status == 403 and body == {
            "error": "csrf_failed", "message": "Request validation failed"
        }, f"cross-origin login did not fail closed: {status}"

        status, _, headers = kernel.request(
            "POST", "/api/auth/login", credentials,
            extra_headers={"Origin": origin})
        assert status == 200, f"same-origin login returned {status}"
        cookies = response_cookies(headers)
        assert response_header(headers, "Cache-Control") == "no-store"
        assert response_header(headers, "Vary") == \
            "Origin, Cookie, Authorization"
        assert "plinth_session" in cookies and "plinth_csrf" in cookies
        session = cookies["plinth_session"].value
        csrf = cookies["plinth_csrf"].value
        assert len(csrf) == 43 and cookies["plinth_csrf"]["httponly"] == "", \
            "CSRF cookie did not use the public 43-character token contract"
        cookie = f"plinth_session={session}; plinth_csrf={csrf}"

        status, _, headers = kernel.request(
            "GET", "/api/auth/session", cookie=cookie)
        assert status == 200, f"cookie session bootstrap returned {status}"
        refreshed = response_cookies(headers)
        assert response_header(headers, "Cache-Control") == "no-store"
        assert response_header(headers, "Vary") == "Cookie, Authorization"
        assert refreshed["plinth_csrf"].value == csrf, \
            "session bootstrap did not reassert the bound CSRF cookie"

        def create_pat(bound_cookie, supplied_csrf=None, supplied_origin=origin):
            request_headers = {}
            if supplied_csrf is not None:
                request_headers["X-Plinth-CSRF"] = supplied_csrf
            if supplied_origin is not None:
                request_headers["Origin"] = supplied_origin
            return kernel.request("POST", "/api/auth/pats", {"name": "csrf-fixture"},
                                  cookie=bound_cookie,
                                  extra_headers=request_headers)

        for label, candidate, candidate_origin in (
                ("missing", None, origin),
                ("malformed", "not-a-token", origin),
                ("wrong-origin", csrf, "https://cross-origin.invalid"),
                ("missing-origin", csrf, None)):
            status, body, rejected_headers = create_pat(
                cookie, candidate, candidate_origin)
            assert status == 403 and body == {
                "error": "csrf_failed", "message": "Request validation failed"
            }, f"{label} CSRF request did not fail closed: {status}"
            assert response_header(rejected_headers, "Cache-Control") == "no-store"

        status, pat, pat_headers = create_pat(cookie, csrf)
        assert status == 201, f"valid cookie mutation returned {status}"
        assert response_header(pat_headers, "Cache-Control") == "no-store"

        other_credentials = {
            "username": "csrf-secondary",
            "password": "fake-password-for-csrf-test",
        }
        status, body, _ = kernel.request(
            "POST", "/api/auth/register", other_credentials)
        assert status == 202 and body == {"status": "processed"}
        status, _, other_headers = kernel.request(
            "POST", "/api/auth/login", other_credentials)
        assert status == 200
        other_cookies = response_cookies(other_headers)
        other_cookie = (f"plinth_session={other_cookies['plinth_session'].value}; "
                        f"plinth_csrf={other_cookies['plinth_csrf'].value}")
        status, body, _ = create_pat(other_cookie, csrf)
        assert status == 403 and body["error"] == "csrf_failed", \
            "a CSRF token was accepted across sessions"

        # Explicit bearer credentials are not ambient browser authority.
        status, bearer_pat, _ = kernel.request(
            "POST", "/api/auth/pats", {"name": "bearer-session"}, token=session)
        assert status == 201, f"bearer session mutation returned {status}"
        status, _, _ = kernel.request(
            "DELETE", f"/api/auth/pats/{bearer_pat['id']}", token=pat["token"])
        assert status == 200, f"PAT bearer mutation returned {status}"

        status, _, logout_headers = kernel.request(
            "POST", "/api/auth/logout", cookie=cookie,
            extra_headers={"Origin": origin, "X-Plinth-CSRF": csrf})
        assert status == 200, f"valid logout returned {status}"
        cleared = response_cookies(logout_headers)
        assert response_header(logout_headers, "Cache-Control") == "no-store"
        assert cleared["plinth_session"]["max-age"] == "0"
        assert cleared["plinth_csrf"]["max-age"] == "0"

        status, _, rotated_headers = kernel.request(
            "POST", "/api/auth/login", credentials)
        assert status == 200, f"native relogin returned {status}"
        rotated = response_cookies(rotated_headers)
        assert rotated["plinth_session"].value != session
        assert rotated["plinth_csrf"].value != csrf

    # TLS termination is represented only by the configured public origin;
    # forwarded headers never become authority.
    with running_kernel(binary, pg_env, "open",
                        configured_https_origin=True) as kernel:
        public_origin = f"https://127.0.0.1:{kernel.port}"
        credentials = {
            "username": "csrf-proxy",
            "password": "fake-password-for-csrf-proxy-test",
        }
        status, _, _ = bootstrap_user(
            kernel, credentials,
            extra_headers={"Origin": public_origin})
        assert status == 201, f"configured HTTPS origin returned {status}"
        status, body, _ = kernel.request(
            "POST", "/api/auth/login", credentials,
            extra_headers={"Origin": f"http://127.0.0.1:{kernel.port}",
                           "X-Forwarded-Proto": "https"})
        assert status == 403 and body["error"] == "csrf_failed", \
            "forwarded scheme improperly established browser authority"
    print("CSRF cookie, origin, bearer, rotation, logout, and proxy checks passed",
          flush=True)


def disabled_session(binary, pg_env):
    with running_kernel(binary, pg_env, "disabled") as kernel:
        credentials = {"username": "disabled-fixture", "password": "fake-password-for-auth-test"}
        status, user, _ = bootstrap_user(kernel, credentials)
        assert status == 201, f"bootstrap returned {status}"
        user_id = str(uuid.UUID(user["id"]))
        status, _, headers = kernel.request("POST", "/api/auth/login", credentials)
        assert status == 200, f"login returned {status}"
        cookies = SimpleCookie()
        for name, value in headers:
            if name.lower() == "set-cookie":
                cookies.load(value)
        token = cookies["plinth_session"].value
        status, pat, _ = kernel.request("POST", "/api/auth/pats", {"name": "fixture"}, token)
        assert status == 201, f"PAT creation returned {status}"
        for candidate in (token, pat["token"]):
            assert kernel.request("GET", "/api/auth/sessions", token=candidate)[0] == 200
        assert websocket_auth(kernel, token)["type"] == "connected"

        kernel.sql(f"UPDATE plinth.users SET disabled_at=NOW() WHERE id='{user_id}'::uuid")
        for candidate in (token, pat["token"]):
            status, body, _ = kernel.request("GET", "/api/auth/sessions", token=candidate)
            assert status == 401 and body["error"] == "not_authenticated", \
                f"disabled credential accepted or wrong rejection: {status}"
        status, body, _ = kernel.request("POST", "/api/auth/login", credentials)
        assert status == 401 and body["error"] == "invalid_credentials"
        denied = websocket_auth(kernel, token)
        assert denied["type"] == "error" and denied["error"] == "auth_failed"

        # Disabling is authoritative without deleting/revoking the stored token.
        kernel.sql(f"UPDATE plinth.users SET disabled_at=NULL WHERE id='{user_id}'::uuid")
        for candidate in (token, pat["token"]):
            assert kernel.request("GET", "/api/auth/sessions", token=candidate)[0] == 200
        assert websocket_auth(kernel, token)["type"] == "connected"

        # Existing password hashes remain login-compatible even when the
        # submitted candidate exceeds the creation-time password ceiling.
        status, body, _ = kernel.request(
            "POST", "/api/auth/login",
            {"username": credentials["username"], "password": "x" * 1025})
        assert status == 401 and body["error"] == "invalid_credentials", \
            f"long login password was rejected as request shape: {status} {body}"

        # Exhaust the remaining source window and require a privacy-safe audit.
        audit_subject = "private-rate-limit-subject"
        rate_limited = None
        for _ in range(6):
            result = kernel.request(
                "POST", "/api/auth/login",
                {"username": audit_subject, "password": "fake-wrong-password"})
            if result[0] == 429:
                rate_limited = result
                break
            assert result[0] == 401 and result[1]["error"] == "invalid_credentials"
        assert rate_limited is not None, "login source window did not close"
        status, body, headers = rate_limited
        assert body["error"] == "rate_limited"
        assert response_header(headers, "Retry-After")

        deadline = time.monotonic() + 5
        audit_detail = ""
        while time.monotonic() < deadline:
            audit_detail = kernel.sql(
                "SELECT detail::text FROM plinth.audit_log "
                "WHERE action='user.login_failed' "
                "AND detail->>'reason'='rate_limited' "
                "ORDER BY timestamp DESC LIMIT 1")
            if audit_detail:
                break
            time.sleep(0.01)
        assert audit_detail, "rate-limited login audit was not persisted"
        detail = json.loads(audit_detail)
        assert detail == {
            "reason": "rate_limited",
            "subject_hash": hashlib.sha256(audit_subject.encode()).hexdigest(),
        }
        assert audit_subject not in audit_detail

    # The configured source ceiling also governs login. This is the kernel's
    # shared proxy-hop bound in the supported Traefik topology, so it must not
    # silently remain at the direct-deployment default of five.
    with running_kernel(binary, pg_env,
                        {"mode": "disabled", "source_attempts": 7}) as kernel:
        credentials = {
            "username": "proxy-shared-admin",
            "password": "fake-proxy-shared-password",
        }
        assert bootstrap_user(kernel, credentials)[0] == 201
        for index in range(6):
            status, body, _ = kernel.request(
                "POST", "/api/auth/login",
                {"username": f"proxy-subject-{index}",
                 "password": "fake-wrong-password"})
            assert status == 401 and body["error"] == "invalid_credentials"
        assert kernel.request("POST", "/api/auth/login", credentials)[0] == 200
        status, body, headers = kernel.request(
            "POST", "/api/auth/login",
            {"username": "proxy-subject-final",
             "password": "fake-wrong-password"})
        assert status == 429 and body["error"] == "rate_limited"
        assert response_header(headers, "Retry-After")
    print("disabled-account HTTP/session/PAT/login and fresh WebSocket checks passed", flush=True)


class InsertBarrier:
    """Keep SELECT available while every concurrent user INSERT must wait."""
    def __init__(self, kernel):
        self.kernel = kernel
        self.child = None

    def __enter__(self):
        self.child = subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1"],
                                      env=self.kernel.pg_env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.child.stdin.write("BEGIN; LOCK TABLE plinth.users IN SHARE MODE; "
                                   "SELECT 'insert-barrier-ready';\n")
            self.child.stdin.flush()
            with selectors.DefaultSelector() as ready:
                ready.register(self.child.stdout, selectors.EVENT_READ)
                assert ready.select(timeout=5), "database barrier did not become ready"
                assert self.child.stdout.readline().strip() == "insert-barrier-ready"
            return self
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.child is None:
            return
        try:
            self.child.communicate("ROLLBACK;\n", timeout=5)
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.communicate(timeout=5)
            raise AssertionError("database barrier did not release") from None
        assert self.child.returncode == 0, "database barrier failed"

    def __exit__(self, *_):
        self.close()


class RecoveryUpdateBarrier:
    """Hold recovery after its user-row lock but before credential revocation."""
    LOCK_KEY = 370037

    def __init__(self, kernel, user_id):
        self.kernel = kernel
        self.user_id = str(uuid.UUID(user_id))
        self.child = None

    def __enter__(self):
        self.kernel.sql(
            "CREATE FUNCTION public.auth_recovery_update_barrier() "
            "RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM "
            f"pg_advisory_xact_lock({self.LOCK_KEY}); RETURN NEW; END $$")
        self.kernel.sql(
            "CREATE TRIGGER auth_recovery_update_barrier BEFORE UPDATE OF "
            "password_hash ON plinth.users FOR EACH ROW WHEN "
            f"(OLD.id = '{self.user_id}'::uuid) EXECUTE FUNCTION "
            "public.auth_recovery_update_barrier()")
        self.child = subprocess.Popen(
            ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1"],
            env=self.kernel.pg_env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.child.stdin.write(
                "SELECT 'recovery-barrier-ready' FROM (SELECT "
                f"pg_advisory_lock({self.LOCK_KEY})) AS locked;\n")
            self.child.stdin.flush()
            with selectors.DefaultSelector() as ready:
                ready.register(self.child.stdout, selectors.EVENT_READ)
                assert ready.select(timeout=5), "recovery barrier did not become ready"
                assert self.child.stdout.readline().strip() == \
                    "recovery-barrier-ready"
            return self
        except BaseException:
            self.close()
            raise

    def wait_for_recovery(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            waiting = int(self.kernel.sql(
                "SELECT count(*) FROM pg_stat_activity WHERE "
                "datname=current_database() AND wait_event_type='Lock' AND "
                "query LIKE 'WITH target AS (UPDATE plinth.users SET%'") or "0")
            if waiting == 1:
                return
            time.sleep(0.01)
        raise AssertionError("recovery did not reach the database barrier")

    def wait_for_issuers(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            waiting = int(self.kernel.sql(
                "SELECT count(*) FROM pg_stat_activity WHERE "
                "datname=current_database() AND wait_event_type='Lock' AND "
                "query LIKE 'SELECT id FROM plinth.users WHERE id=%FOR UPDATE%'"
            ) or "0")
            if waiting == 2:
                return
            time.sleep(0.01)
        raise AssertionError("credential issuers did not wait behind recovery")

    def close(self):
        if self.child is not None:
            try:
                self.child.communicate(
                    f"SELECT pg_advisory_unlock({self.LOCK_KEY});\n", timeout=5)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.communicate(timeout=5)
                raise AssertionError("recovery barrier did not release") from None
            assert self.child.returncode == 0, "recovery barrier failed"
            self.child = None
        self.kernel.sql(
            "DROP TRIGGER IF EXISTS auth_recovery_update_barrier ON plinth.users; "
            "DROP FUNCTION IF EXISTS public.auth_recovery_update_barrier()")

    def __exit__(self, *_):
        self.close()


class CredentialInsertBarrier:
    """Hold credential insertion after the issuer owns the user's row lock."""
    TARGETS = {
        "sessions": (370038, "session"),
        "pats": (370039, "pat"),
    }

    def __init__(self, kernel, table, user_id):
        if table not in self.TARGETS:
            raise ValueError(f"unsupported credential table: {table}")
        self.kernel = kernel
        self.table = table
        self.user_id = str(uuid.UUID(user_id))
        self.lock_key, self.label = self.TARGETS[table]
        self.function = f"auth_{self.label}_insert_barrier"
        self.trigger = self.function
        self.child = None

    def __enter__(self):
        try:
            self.kernel.sql(
                f"CREATE FUNCTION public.{self.function}() "
                "RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM "
                f"pg_advisory_xact_lock({self.lock_key}); RETURN NEW; END $$")
            self.kernel.sql(
                f"CREATE TRIGGER {self.trigger} BEFORE INSERT ON "
                f"plinth.{self.table} FOR EACH ROW WHEN "
                f"(NEW.user_id = '{self.user_id}'::uuid) EXECUTE FUNCTION "
                f"public.{self.function}()")
            self.child = subprocess.Popen(
                ["psql", "-XqAt", "-v", "ON_ERROR_STOP=1"],
                env=self.kernel.pg_env, stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.child.stdin.write(
                f"SELECT '{self.label}-insert-barrier-ready' FROM (SELECT "
                f"pg_advisory_lock({self.lock_key})) AS locked;\n")
            self.child.stdin.flush()
            with selectors.DefaultSelector() as ready:
                ready.register(self.child.stdout, selectors.EVENT_READ)
                assert ready.select(timeout=5), \
                    f"{self.label} insert barrier did not become ready"
                assert self.child.stdout.readline().strip() == \
                    f"{self.label}-insert-barrier-ready"
            return self
        except BaseException:
            self.close()
            raise

    def wait_for_insert(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            waiting = int(self.kernel.sql(
                "SELECT count(*) FROM pg_stat_activity WHERE "
                "datname=current_database() AND wait_event_type='Lock' AND "
                f"query LIKE 'INSERT INTO plinth.{self.table} %'") or "0")
            if waiting == 1:
                return
            time.sleep(0.01)
        raise AssertionError(
            f"{self.label} issuance did not reach the database barrier")

    def wait_for_recovery(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            waiting = int(self.kernel.sql(
                "SELECT count(*) FROM pg_stat_activity WHERE "
                "datname=current_database() AND wait_event_type='Lock' AND "
                "query LIKE 'SELECT id FROM plinth.users WHERE username=%FOR UPDATE%'"
            ) or "0")
            if waiting == 1:
                return
            time.sleep(0.01)
        raise AssertionError("recovery did not wait behind credential issuance")

    def close(self):
        try:
            if self.child is not None:
                self.child.communicate(
                    f"SELECT pg_advisory_unlock({self.lock_key});\n", timeout=5)
                assert self.child.returncode == 0, \
                    f"{self.label} insert barrier failed"
        except subprocess.TimeoutExpired:
            self.child.kill()
            self.child.communicate(timeout=5)
            raise AssertionError(
                f"{self.label} insert barrier did not release") from None
        finally:
            self.child = None
            self.kernel.sql(
                f"DROP TRIGGER IF EXISTS {self.trigger} ON plinth.{self.table}; "
                f"DROP FUNCTION IF EXISTS public.{self.function}()")

    def __exit__(self, *_):
        self.close()


def assert_processed(result):
    status, body, _ = result
    assert status == 202 and body == {"status": "processed"}, \
        f"registration did not use its generic response: {status} {body}"


def registration_modes(binary, pg_env):
    with running_kernel(binary, pg_env, "disabled") as kernel:
        assert kernel.request("GET", "/api/auth/registration")[:2] == \
            (200, {"mode": "disabled"})
        status, body, _ = kernel.request(
            "POST", "/api/auth/register",
            {"username": "disabled-user", "password": "fake-disabled-password"})
        assert status == 403 and body["error"] == "registration_unavailable"

    # Ordinary registration can never create or bootstrap the first account.
    with running_kernel(binary, pg_env, "open") as kernel:
        first = {"username": "not-an-admin", "password": "fake-open-password"}
        assert_processed(kernel.request("POST", "/api/auth/register", first))
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "0"

    with running_kernel(
            binary, pg_env,
            {"mode": "open", "max_accounts": 2,
             "source_attempts": 100, "subject_attempts": 100}) as kernel:
        admin = {"username": "open-admin", "password": "fake-admin-password"}
        assert bootstrap_user(kernel, admin)[0] == 201
        admin_session = login_session(kernel, admin)
        for path in ("/api/auth/register", "/api/auth/bootstrap",
                     "/api/auth/login"):
            assert_non_object_json_rejected(kernel, "POST", path)
        for path in ("/api/auth/recovery", "/api/auth/invites"):
            assert_non_object_json_rejected(
                kernel, "POST", path, token=admin_session)
        member = {"username": "open-member", "password": "fake-member-password"}
        assert_processed(kernel.request("POST", "/api/auth/register", member))
        deadline = time.monotonic() + 5
        registration_audit = ""
        while time.monotonic() < deadline:
            registration_audit = kernel.sql(
                "SELECT ip_address::text||'|'||(detail->>'mode') "
                "FROM plinth.audit_log WHERE action='user.registered' "
                "AND user_id=(SELECT id FROM plinth.users "
                "WHERE username='open-member') ORDER BY timestamp DESC LIMIT 1")
            if registration_audit:
                break
            time.sleep(0.01)
        audit_ip, audit_mode = registration_audit.rsplit("|", 1)
        assert ipaddress.ip_interface(audit_ip).ip.is_loopback and audit_mode == "open", \
            f"ordinary registration audit lost peer/mode: {registration_audit!r}"
        member_session = login_session(kernel, member)
        status, pat, _ = kernel.request(
            "POST", "/api/auth/pats", {"name": "registration-policy"},
            token=member_session)
        assert status == 201

        # Collision and account-cap rejection are indistinguishable from success.
        assert_processed(kernel.request("POST", "/api/auth/register", member))
        assert_processed(kernel.request(
            "POST", "/api/auth/register",
            {"username": "over-cap", "password": "fake-over-cap-password"}))
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "2"
        for forbidden_field in ("email", "real_name"):
            invalid = dict(member)
            invalid[forbidden_field] = "not-collected"
            status, body, _ = kernel.request("POST", "/api/auth/register", invalid)
            assert status == 400 and body["error"] == "invalid_request"

        # Turning registration off does not invalidate existing credentials.
        kernel.set_registration({"mode": "disabled", "source_attempts": 100,
                                 "subject_attempts": 100})
        kernel.stop()
        kernel.start()
        assert kernel.request("GET", "/api/auth/registration")[:2] == \
            (200, {"mode": "disabled"})
        assert kernel.request("GET", "/api/auth/sessions", token=member_session)[0] == 200
        assert kernel.request("GET", "/api/auth/sessions", token=pat["token"])[0] == 200
        assert kernel.request("POST", "/api/auth/login", member)[0] == 200

        # Recovery owns the user row before revoking credentials. Login and
        # PAT issuance that were admitted under the old authority must wait,
        # revalidate after recovery commits, and fail without inserting a row.
        member_id = kernel.sql(
            "SELECT id FROM plinth.users WHERE username='open-member'")
        raced_recovery = {
            "username": member["username"],
            "new_password": "fake-race-recovered-password",
        }
        with ThreadPoolExecutor(max_workers=3) as workers:
            with RecoveryUpdateBarrier(kernel, member_id) as barrier:
                recovery_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/recovery",
                    raced_recovery, admin_session)
                barrier.wait_for_recovery()
                login_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/login", member)
                pat_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/pats",
                    {"name": "raced-pat"}, member_session)
                barrier.wait_for_issuers()
            recovery_result = recovery_pending.result(timeout=12)
            login_result = login_pending.result(timeout=12)
            pat_result = pat_pending.result(timeout=12)
        assert recovery_result[:2] == (200, {"status": "recovered"})
        assert login_result[0] == 401 and \
            login_result[1]["error"] == "invalid_credentials"
        assert pat_result[0] == 401 and \
            pat_result[1]["error"] == "not_authenticated"
        assert kernel.sql(
            f"SELECT count(*) FROM plinth.sessions WHERE user_id='{member_id}'::uuid "
            "AND revoked_at IS NULL") == "0"
        assert kernel.sql(
            f"SELECT count(*) FROM plinth.pats WHERE user_id='{member_id}'::uuid "
            "AND revoked_at IS NULL") == "0"
        raced_credentials = {
            "username": member["username"],
            "password": raced_recovery["new_password"],
        }
        raced_session = login_session(kernel, raced_credentials)
        assert kernel.request(
            "GET", "/api/auth/sessions", token=raced_session)[0] == 200

        # The inverse ordering is also linearizable: a login that already owns
        # the user row commits first, then recovery acquires it and revokes the
        # exact newly-issued session before reporting success.
        login_first_recovery = {
            "username": member["username"],
            "new_password": "fake-login-first-recovered-password",
        }
        with ThreadPoolExecutor(max_workers=2) as workers:
            with CredentialInsertBarrier(kernel, "sessions", member_id) as barrier:
                login_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/login", raced_credentials)
                barrier.wait_for_insert()
                recovery_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/recovery",
                    login_first_recovery, admin_session)
                barrier.wait_for_recovery()
            login_result = login_pending.result(timeout=12)
            recovery_result = recovery_pending.result(timeout=12)
        assert login_result[0] == 200, \
            f"issuer-first login returned {login_result[0]}: {login_result[1]}"
        assert recovery_result[:2] == (200, {"status": "recovered"})
        issued_session_id = str(uuid.UUID(login_result[1]["session"]["id"]))
        issued_session = response_cookies(login_result[2])["plinth_session"].value
        assert kernel.sql(
            "SELECT revoked_at IS NOT NULL FROM plinth.sessions WHERE id="
            f"'{issued_session_id}'::uuid") == "t"
        assert kernel.request(
            "GET", "/api/auth/sessions", token=issued_session)[0] == 401

        login_first_credentials = {
            "username": member["username"],
            "password": login_first_recovery["new_password"],
        }
        pat_race_session = login_session(kernel, login_first_credentials)

        # PAT issuance follows the same inverse ordering independently: its
        # insert commits while holding the user row, and waiting recovery then
        # revokes the exact returned PAT and its authenticating session.
        pat_first_recovery = {
            "username": member["username"],
            "new_password": "fake-pat-first-recovered-password",
        }
        with ThreadPoolExecutor(max_workers=2) as workers:
            with CredentialInsertBarrier(kernel, "pats", member_id) as barrier:
                pat_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/pats",
                    {"name": "issuer-first-pat"}, pat_race_session)
                barrier.wait_for_insert()
                recovery_pending = workers.submit(
                    kernel.request, "POST", "/api/auth/recovery",
                    pat_first_recovery, admin_session)
                barrier.wait_for_recovery()
            pat_result = pat_pending.result(timeout=12)
            recovery_result = recovery_pending.result(timeout=12)
        assert pat_result[0] == 201, \
            f"issuer-first PAT returned {pat_result[0]}: {pat_result[1]}"
        assert recovery_result[:2] == (200, {"status": "recovered"})
        issued_pat_id = str(uuid.UUID(pat_result[1]["id"]))
        assert kernel.sql(
            "SELECT revoked_at IS NOT NULL FROM plinth.pats WHERE id="
            f"'{issued_pat_id}'::uuid") == "t"
        assert kernel.request(
            "GET", "/api/auth/sessions", token=pat_result[1]["token"])[0] == 401
        assert kernel.request(
            "GET", "/api/auth/sessions", token=pat_race_session)[0] == 401

        # Recovery revokes credentials, replaces the hash, and preserves disablement.
        kernel.sql("UPDATE plinth.users SET disabled_at=NOW() "
                   "WHERE username='open-member'")
        recovered = {"username": member["username"],
                     "new_password": "fake-recovered-password"}
        status, body, _ = kernel.request(
            "POST", "/api/auth/recovery", recovered, token=admin_session)
        assert status == 200 and body == {"status": "recovered"}
        assert kernel.request("GET", "/api/auth/sessions", token=member_session)[0] == 401
        assert kernel.request("GET", "/api/auth/sessions", token=raced_session)[0] == 401
        assert kernel.request("GET", "/api/auth/sessions", token=pat["token"])[0] == 401
        status, body, _ = kernel.request(
            "POST", "/api/auth/login",
            {"username": member["username"], "password": recovered["new_password"]})
        assert status == 401 and body["error"] == "invalid_credentials"
        assert kernel.sql("SELECT disabled_at IS NOT NULL FROM plinth.users "
                          "WHERE username='open-member'") == "t"
        deadline = time.monotonic() + 5
        recovery_detail = ""
        while time.monotonic() < deadline:
            recovery_detail = kernel.sql(
                "SELECT detail::text FROM plinth.audit_log "
                "WHERE action='auth.account.password_reset' "
                "ORDER BY timestamp DESC LIMIT 1")
            if recovery_detail:
                break
            time.sleep(0.01)
        assert json.loads(recovery_detail) == {
            "credentials_revoked": True,
            "target_user_id": member_id,
        }

    with running_kernel(binary, pg_env,
                        {"mode": "invite", "max_accounts": 3,
                         "source_attempts": 20}) as kernel:
        assert kernel.request("GET", "/api/auth/registration")[:2] == \
            (200, {"mode": "invite"})
        admin = {"username": "invite-admin", "password": "fake-admin-password"}
        assert bootstrap_user(kernel, admin)[0] == 201
        admin_session, admin_cookie, admin_csrf = login_auth(kernel, admin)
        assert kernel.request("POST", "/api/auth/invites", {}, token=None)[0] in (401, 403)
        status, invitation, _ = kernel.request(
            "POST", "/api/auth/invites", {}, token=admin_session)
        assert status == 201 and len(invitation["token"]) == 43
        digest = hashlib.sha256(invitation["token"].encode()).hexdigest()
        assert kernel.sql("SELECT token_hash FROM plinth.registration_invites WHERE id='" +
                          invitation["id"] + "'::uuid") == digest
        invitee = {"username": "invited-user", "password": "fake-invite-password",
                   "invite_token": invitation["token"]}
        assert_processed(kernel.request("POST", "/api/auth/register", invitee))
        invitee_credentials = {key: invitee[key] for key in ("username", "password")}
        invitee_session = login_session(kernel, invitee_credentials)
        assert_processed(kernel.request(
            "POST", "/api/auth/register",
            {"username": "invite-reuse", "password": "fake-invite-password",
             "invite_token": invitation["token"]}))
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE username='invite-reuse'") == "0"
        status, listing, _ = kernel.request(
            "GET", "/api/auth/invites", token=admin_session)
        assert status == 200 and listing["invites"]
        assert all("token" not in item and "token_hash" not in item
                   for item in listing["invites"])
        used = next(item for item in listing["invites"]
                    if item["id"] == invitation["id"])
        assert used["used_at"] is not None

        # A valid non-admin bearer must fail RBAC for every operator action.
        status, guarded, _ = kernel.request(
            "POST", "/api/auth/invites", {}, token=admin_session)
        assert status == 201
        admin_hash = kernel.sql(
            "SELECT password_hash FROM plinth.users WHERE username='invite-admin'")
        invite_count = kernel.sql("SELECT count(*) FROM plinth.registration_invites")
        non_admin_requests = (
            ("POST", "/api/auth/invites", {}),
            ("GET", "/api/auth/invites", None),
            ("DELETE", f"/api/auth/invites/{guarded['id']}", None),
            ("POST", "/api/auth/recovery",
             {"username": "invite-admin", "new_password": "unauthorized-password"}),
        )
        for method, path, request_body in non_admin_requests:
            status, body, _ = kernel.request(
                method, path, request_body, token=invitee_session)
            assert status == 403 and body["error"] == "permission_denied", \
                f"non-admin {method} {path} returned {status}: {body}"
        assert kernel.sql("SELECT count(*) FROM plinth.registration_invites") == invite_count
        assert kernel.sql("SELECT revoked_at IS NULL FROM plinth.registration_invites WHERE id='" +
                          guarded["id"] + "'::uuid") == "t"
        assert kernel.sql(
            "SELECT password_hash FROM plinth.users WHERE username='invite-admin'") == admin_hash

        # Cookie authority must pass CSRF before any operator mutation reaches RBAC.
        origin = f"http://127.0.0.1:{kernel.port}"
        protected_mutations = (
            ("POST", "/api/auth/invites", {}),
            ("DELETE", f"/api/auth/invites/{guarded['id']}", None),
            ("POST", "/api/auth/recovery",
             {"username": "invite-admin", "new_password": "csrf-bypass-password"}),
        )
        for method, path, request_body in protected_mutations:
            for csrf_header in (None, "wrong-csrf-token"):
                headers = {"Origin": origin}
                if csrf_header is not None:
                    headers["X-Plinth-CSRF"] = csrf_header
                status, body, _ = kernel.request(
                    method, path, request_body, cookie=admin_cookie,
                    extra_headers=headers)
                assert status == 403 and body["error"] == "csrf_failed", \
                    f"cookie {method} {path} bypassed CSRF: {status} {body}"
        assert kernel.sql("SELECT count(*) FROM plinth.registration_invites") == invite_count
        assert kernel.sql("SELECT revoked_at IS NULL FROM plinth.registration_invites WHERE id='" +
                          guarded["id"] + "'::uuid") == "t"
        assert kernel.sql(
            "SELECT password_hash FROM plinth.users WHERE username='invite-admin'") == admin_hash

        status, revoked, _ = kernel.request(
            "POST", "/api/auth/invites", {"ttl_seconds": 60}, token=admin_session)
        assert status == 201
        status, body, _ = kernel.request(
            "DELETE", f"/api/auth/invites/{revoked['id']}", token=admin_session)
        assert status == 200 and body == {"status": "revoked"}
        assert_processed(kernel.request(
            "POST", "/api/auth/register",
            {"username": "revoked-invite", "password": "fake-invite-password",
             "invite_token": revoked["token"]}))
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE username='revoked-invite'") == "0"

        # Reusing one invite concurrently can commit exactly one account/use.
        racing_users = [
            {"username": f"racing-invite-{index}",
             "password": "fake-racing-password",
             "invite_token": guarded["token"]}
            for index in range(2)
        ]
        with ThreadPoolExecutor(max_workers=2) as workers:
            pending = [workers.submit(
                kernel.request, "POST", "/api/auth/register", candidate)
                for candidate in racing_users]
            results = [item.result(timeout=15) for item in pending]
        for result in results:
            assert_processed(result)
        assert kernel.sql(
            "SELECT count(*) FROM plinth.users WHERE username IN "
            "('racing-invite-0','racing-invite-1')") == "1"
        assert kernel.sql(
            "SELECT count(*) FROM plinth.registration_invites i "
            "JOIN plinth.users u ON u.id=i.used_by_user_id "
            f"WHERE i.id='{guarded['id']}'::uuid AND i.used_at IS NOT NULL AND "
            "u.username IN ('racing-invite-0','racing-invite-1')") == "1"

        # Invite mode honors the same total-account ceiling without consuming
        # an otherwise valid invite or revealing the reason to the caller.
        status, capped, _ = kernel.request(
            "POST", "/api/auth/invites", {}, token=admin_session)
        assert status == 201
        assert_processed(kernel.request(
            "POST", "/api/auth/register",
            {"username": "invite-over-cap", "password": "fake-cap-password",
             "invite_token": capped["token"]}))
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE username='invite-over-cap'") == "0"
        assert kernel.sql("SELECT used_at IS NULL FROM plinth.registration_invites WHERE id='" +
                          capped["id"] + "'::uuid") == "t"

    with running_kernel(binary, pg_env,
                        {"mode": "open", "source_attempts": 2,
                         "subject_attempts": 2, "global_attempts": 100,
                         "window_seconds": 60}) as kernel:
        admin = {"username": "rate-admin", "password": "fake-rate-password"}
        assert bootstrap_user(kernel, admin)[0] == 201
        assert_processed(kernel.request("POST", "/api/auth/register", admin))
        assert_processed(kernel.request("POST", "/api/auth/register", admin))
        status, body, headers = kernel.request("POST", "/api/auth/register", admin)
        assert status == 429 and body["error"] == "rate_limited"
        assert response_header(headers, "Retry-After")
    print("disabled, open, invite, recovery, privacy, cap, and rate controls passed",
          flush=True)


def bootstrap_security(binary, pg_env):
    with running_kernel(binary, pg_env, "disabled", bootstrap_token=None) as kernel:
        status, body, _ = kernel.request(
            "POST", "/api/auth/bootstrap",
            {"bootstrap_token": "unconfigured", "username": "denied-admin",
             "password": "fake-denied-password"})
        assert status == 403 and body["error"] == "bootstrap_denied"

    with running_kernel(binary, pg_env, "disabled") as kernel:
        credentials = {"username": "denied-admin", "password": "fake-denied-password"}
        for request_body in (
                credentials,
                {**credentials, "bootstrap_token": "wrong-bootstrap-authority"}):
            status, body, _ = kernel.request(
                "POST", "/api/auth/bootstrap", request_body)
            assert status == 403 and body["error"] == "bootstrap_denied"

    # Bootstrap admission precedes secret comparison. Prove the independent
    # source and global windows by exhausting each with denied requests, then
    # showing that even a valid secret cannot reach authorization.
    rate_cases = (
        ("source", {"mode": "disabled", "source_attempts": 2,
                    "global_attempts": 100}),
        ("global", {"mode": "disabled", "source_attempts": 100,
                    "global_attempts": 2}),
    )
    for label, registration in rate_cases:
        with running_kernel(binary, pg_env, registration) as kernel:
            credentials = {"username": f"{label}-limited-admin",
                           "password": "fake-bootstrap-password"}
            denied = {**credentials, "bootstrap_token": "x" * 32}
            for _ in range(2):
                status, body, _ = kernel.request(
                    "POST", "/api/auth/bootstrap", denied)
                assert status == 403 and body["error"] == "bootstrap_denied"
            status, body, headers = bootstrap_user(kernel, credentials)
            assert status == 429 and body["error"] == "rate_limited", \
                f"bootstrap {label} limiter ran after authorization: {status} {body}"
            assert response_header(headers, "Retry-After")
            assert kernel.sql(
                "SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "0"

    with running_kernel(binary, pg_env, "disabled") as kernel:
        count = 2
        with ThreadPoolExecutor(max_workers=count) as workers:
            with InsertBarrier(kernel):
                pending = [workers.submit(
                    bootstrap_user, kernel,
                    {"username": f"bootstrap-{index}",
                     "password": "fake-concurrent-password"})
                    for index in range(count)]
                deadline = time.monotonic() + 8
                while True:
                    waiting = int(kernel.sql(
                        "SELECT count(*) FROM pg_stat_activity WHERE "
                        "datname=current_database() AND state='active' AND "
                        "wait_event_type='Lock' AND (query LIKE 'INSERT INTO plinth.users%' "
                        "OR query LIKE 'SELECT pg_advisory_xact_lock%')"))
                    if waiting == count:
                        break
                    assert not any(item.done() for item in pending), \
                        "bootstrap completed before the database barrier released"
                    assert time.monotonic() < deadline, \
                        f"only {waiting}/{count} bootstraps reached the database barrier"
                    time.sleep(0.01)
            results = [item.result(timeout=12) for item in pending]
        assert sorted(result[0] for result in results) == [201, 409]
        assert [result[1]["error"] for result in results if result[0] == 409] == \
            ["bootstrap_closed"]
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "1"
        assert kernel.sql(
            "SELECT count(*) FROM plinth.group_members gm JOIN plinth.groups g "
            "ON g.id=gm.group_id WHERE g.name='admin'") == "1"

    # Neither a statement failure nor a failed COMMIT may publish a user/201.
    for deferred in (False, True):
        with running_kernel(binary, pg_env, "disabled") as kernel:
            kernel.sql("CREATE FUNCTION public.reject_fixture_membership() RETURNS trigger "
                       "LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'fixture membership failure'; "
                       "END $$")
            if deferred:
                kernel.sql("CREATE CONSTRAINT TRIGGER reject_fixture_membership "
                           "AFTER INSERT ON plinth.group_members DEFERRABLE INITIALLY DEFERRED "
                           "FOR EACH ROW EXECUTE FUNCTION public.reject_fixture_membership()")
            else:
                kernel.sql("CREATE TRIGGER reject_fixture_membership BEFORE INSERT "
                           "ON plinth.group_members FOR EACH ROW "
                           "EXECUTE FUNCTION public.reject_fixture_membership()")
            credentials = {"username": "recoverable-bootstrap",
                           "password": "fake-recovery-password"}
            status, body, _ = bootstrap_user(kernel, credentials)
            assert status == 500 and body["error"] == "internal_error", \
                f"failed bootstrap unexpectedly returned {status}"
            assert credentials["username"] not in (
                kernel.root / "kernel.log").read_text(errors="replace"), \
                "failed bootstrap leaked its submitted username to the kernel log"
            kernel.sql("BEGIN; SET LOCAL lock_timeout='5s'; "
                       "LOCK TABLE plinth.users IN SHARE MODE; COMMIT")
            assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "0"
            assert kernel.sql("SELECT count(*) FROM plinth.group_members") == "0"
            kernel.sql("DROP TRIGGER reject_fixture_membership ON plinth.group_members; "
                       "DROP FUNCTION public.reject_fixture_membership()")
            status, user, _ = bootstrap_user(kernel, credentials)
            assert status == 201, f"bootstrap retry returned {status}"
            user_id = str(uuid.UUID(user["id"]))
            assert kernel.sql("SELECT count(*) FROM plinth.group_members gm JOIN plinth.groups g "
                              "ON g.id=gm.group_id WHERE g.name='admin' AND "
                              f"gm.user_id='{user_id}'::uuid") == "1"
    print("bootstrap authorization, concurrency, close, and rollback checks passed",
          flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scenario", choices=["disabled-session", "registration-modes",
                                               "bootstrap-security", "csrf"], required=True)
    args = parser.parse_args()
    if not os.environ.get("PLINTH_PG_HOST"):
        print("PostgreSQL fixture is not configured")
        return 77
    pg_env = os.environ.copy()
    for suffix in ("HOST", "PORT", "USER", "PASSWORD", "DATABASE"):
        pg_env["PG" + suffix] = os.environ["PLINTH_PG_" + suffix]
    binary = args.binary.resolve()
    if args.scenario == "disabled-session":
        disabled_session(binary, pg_env)
    elif args.scenario == "csrf":
        csrf_contract(binary, pg_env)
    elif args.scenario == "registration-modes":
        registration_modes(binary, pg_env)
    else:
        bootstrap_security(binary, pg_env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
