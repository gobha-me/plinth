#!/usr/bin/env python3
"""Production HTTP authentication regressions in an owned disposable database."""

import argparse
import base64
import contextlib
from concurrent.futures import ThreadPoolExecutor
import http.client
from http.cookies import SimpleCookie
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
    def __init__(self, binary, root, database, pg_env, registration_enabled,
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
        config = {
            "listen_host": "127.0.0.1", "listen_port": self.port,
            "dev_mode": False, "registration_enabled": registration_enabled,
            "database": {"pool_size": 8},
            "shell": {"enabled": False},
            "migrations_dir": str(repo / "migrations"),
            "packages": {"data_dir": str(root / "data"),
                         "staging_dir": str(root / "staging")}}
        if configured_https_origin:
            config["browser_origin"] = f"https://127.0.0.1:{self.port}"
        self.config.write_text(json.dumps(config))

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
def running_kernel(binary, pg_env, registration_enabled,
                   configured_https_origin=False):
    database = "plinth_auth_" + uuid.uuid4().hex
    sql(pg_env, f'CREATE DATABASE "{database}"')
    try:
        with tempfile.TemporaryDirectory(prefix="plinth-auth-http-") as temporary:
            kernel = Kernel(binary, Path(temporary), database, pg_env,
                            registration_enabled, configured_https_origin)
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


def csrf_contract(binary, pg_env):
    with running_kernel(binary, pg_env, True) as kernel:
        origin = f"http://127.0.0.1:{kernel.port}"
        credentials = {
            "username": "csrf-primary",
            "password": "fake-password-for-csrf-test",
        }

        status, _, _ = kernel.request("POST", "/api/auth/register", credentials)
        assert status == 201, f"native registration returned {status}"

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
        assert kernel.request("POST", "/api/auth/register", other_credentials)[0] == 201
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
    with running_kernel(binary, pg_env, True,
                        configured_https_origin=True) as kernel:
        public_origin = f"https://127.0.0.1:{kernel.port}"
        credentials = {
            "username": "csrf-proxy",
            "password": "fake-password-for-csrf-proxy-test",
        }
        status, _, _ = kernel.request(
            "POST", "/api/auth/register", credentials,
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
    with running_kernel(binary, pg_env, False) as kernel:
        credentials = {"username": "disabled-fixture", "password": "fake-password-for-auth-test"}
        status, user, _ = kernel.request("POST", "/api/auth/register", credentials)
        assert status == 201, f"registration returned {status}"
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
        assert status == 403 and body["error"] == "account_disabled"
        denied = websocket_auth(kernel, token)
        assert denied["type"] == "error" and denied["error"] == "auth_failed"

        # Disabling is authoritative without deleting/revoking the stored token.
        kernel.sql(f"UPDATE plinth.users SET disabled_at=NULL WHERE id='{user_id}'::uuid")
        for candidate in (token, pat["token"]):
            assert kernel.request("GET", "/api/auth/sessions", token=candidate)[0] == 200
        assert websocket_auth(kernel, token)["type"] == "connected"
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


def bootstrap_concurrent(binary, pg_env, enabled):
    with running_kernel(binary, pg_env, enabled) as kernel:
        count = 6
        # Under the vulnerable implementation all INSERTs wait here, after
        # their independent decisions. With the repair, the first INSERT and
        # other registration-lock acquisitions wait. Observe the actual DB
        # waits before releasing; elapsed sleeps never stand in for readiness.
        with ThreadPoolExecutor(max_workers=count) as workers:
            with InsertBarrier(kernel):
                pending = [workers.submit(kernel.request, "POST", "/api/auth/register",
                                          {"username": f"bootstrap-{index}",
                                           "password": "fake-concurrent-password"})
                           for index in range(count)]
                deadline = time.monotonic() + 4
                while True:
                    waiting = int(kernel.sql(
                        "SELECT count(*) FROM pg_stat_activity WHERE "
                        "datname=current_database() AND state='active' AND "
                        "wait_event_type='Lock' AND (query LIKE 'INSERT INTO plinth.users%' "
                        "OR query LIKE 'SELECT pg_advisory_xact_lock%')"))
                    if waiting == count:
                        break
                    assert not any(item.done() for item in pending), \
                        "registration completed before the database barrier released"
                    assert time.monotonic() < deadline, \
                        f"only {waiting}/{count} registrations reached the database barrier"
                    time.sleep(0.01)  # Poll an explicit database-state condition.
            results = [item.result(timeout=12) for item in pending]
        statuses = sorted(result[0] for result in results)
        expected = [201] * count if enabled else [201] + [403] * (count - 1)
        administrators = kernel.sql(
            "SELECT count(*) FROM plinth.group_members gm JOIN plinth.groups g "
            "ON g.id=gm.group_id WHERE g.name='admin'")
        assert statuses == expected, \
            f"concurrent registration statuses: {statuses}; administrators={administrators}"
        assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == \
            str(count if enabled else 1)
        assert administrators == "1", \
            "concurrent bootstrap did not create exactly one administrator"
        for status, body, _ in results:
            if status == 403:
                assert body["error"] == "registration_disabled"
        # The committed winner is usable, not just a membership without a user.
        winner = kernel.sql("SELECT u.username FROM plinth.users u JOIN plinth.group_members gm "
                            "ON gm.user_id=u.id JOIN plinth.groups g ON g.id=gm.group_id "
                            "WHERE g.name='admin'")
        assert kernel.request("POST", "/api/auth/login",
                              {"username": winner, "password": "fake-concurrent-password"})[0] == 200
    print(f"six concurrent registrations, registration_enabled={enabled}: one administrator", flush=True)


def bootstrap_rollback(binary, pg_env):
    # Cover both the membership statement and the final COMMIT. Neither may
    # publish a user/201 before the administrator membership is durable.
    for deferred in (False, True):
        with running_kernel(binary, pg_env, False) as kernel:
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
            credentials = {"username": "recoverable-bootstrap", "password": "fake-recovery-password"}
            status, body, _ = kernel.request("POST", "/api/auth/register", credentials)
            assert status == 500 and body["error"] == "internal_error", \
                f"failed bootstrap unexpectedly returned {status}"
            # An error may initiate rollback asynchronously; wait for the
            # transaction lock on users to be released before observing state.
            kernel.sql("BEGIN; SET LOCAL lock_timeout='5s'; "
                       "LOCK TABLE plinth.users IN SHARE MODE; COMMIT")
            assert kernel.sql("SELECT count(*) FROM plinth.users WHERE NOT is_test_user") == "0", \
                "failed bootstrap left an unprivileged first user"
            assert kernel.sql("SELECT count(*) FROM plinth.group_members") == "0"
            kernel.sql("DROP TRIGGER reject_fixture_membership ON plinth.group_members; "
                       "DROP FUNCTION public.reject_fixture_membership()")
            status, user, _ = kernel.request("POST", "/api/auth/register", credentials)
            assert status == 201, f"bootstrap retry returned {status}"
            user_id = str(uuid.UUID(user["id"]))
            assert kernel.sql("SELECT count(*) FROM plinth.group_members gm JOIN plinth.groups g "
                              "ON g.id=gm.group_id WHERE g.name='admin' AND "
                              f"gm.user_id='{user_id}'::uuid") == "1"
    print("membership and COMMIT failures roll back bootstrap and allow a usable retry", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scenario", choices=["disabled-session", "bootstrap-disabled",
                                               "bootstrap-enabled", "bootstrap-rollback",
                                               "csrf"], required=True)
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
    elif args.scenario == "bootstrap-rollback":
        bootstrap_rollback(binary, pg_env)
    else:
        bootstrap_concurrent(binary, pg_env, args.scenario == "bootstrap-enabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
