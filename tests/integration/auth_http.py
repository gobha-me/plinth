#!/usr/bin/env python3
"""Production HTTP authentication regressions in an owned disposable database."""

import argparse
import base64
import contextlib
import http.client
from http.cookies import SimpleCookie
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import uuid


class Kernel:
    def __init__(self, binary, root, database, pg_env, registration_enabled):
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
        self.config.write_text(json.dumps({
            "listen_host": "127.0.0.1", "listen_port": self.port,
            "dev_mode": False, "registration_enabled": registration_enabled,
            "database": {"pool_size": 8},
            "shell": {"enabled": False},
            "migrations_dir": str(repo / "migrations"),
            "packages": {"data_dir": str(root / "data"),
                         "staging_dir": str(root / "staging")}}))

    def sql(self, statement):
        return sql(self.pg_env, statement)

    def request(self, method, path, body=None, token=None):
        headers = {}
        if body is not None:
            headers["Content-Type"] = "application/json"
        if token is not None:
            headers["Authorization"] = "Bearer " + token
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
def running_kernel(binary, pg_env, registration_enabled):
    database = "plinth_auth_" + uuid.uuid4().hex
    sql(pg_env, f'CREATE DATABASE "{database}"')
    try:
        with tempfile.TemporaryDirectory(prefix="plinth-auth-http-") as temporary:
            kernel = Kernel(binary, Path(temporary), database, pg_env, registration_enabled)
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
        sql(pg_env, f'DROP DATABASE "{database}" WITH (FORCE)')


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--scenario", choices=["disabled-session"], required=True)
    args = parser.parse_args()
    if not os.environ.get("PLINTH_PG_HOST"):
        print("PostgreSQL fixture is not configured")
        return 77
    pg_env = os.environ.copy()
    for suffix in ("HOST", "PORT", "USER", "PASSWORD", "DATABASE"):
        pg_env["PG" + suffix] = os.environ["PLINTH_PG_" + suffix]
    disabled_session(args.binary.resolve(), pg_env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
