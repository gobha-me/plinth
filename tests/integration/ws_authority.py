#!/usr/bin/env python3
"""Production socket authority renewal with only disposable fixture identities."""
import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import socket
import selectors
import signal
import subprocess
import tempfile
import struct
import time
import uuid

from auth_http import Kernel, running_kernel


class Peer:
    def __init__(self, kernel, token):
        self.socket = socket.create_connection(("127.0.0.1", kernel.port), timeout=5)
        try:
            self.initialize(kernel, token)
        except BaseException:
            self.socket.close()
            raise

    def initialize(self, kernel, token):
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        self.socket.sendall((f"GET /ws/events HTTP/1.1\r\nHost: 127.0.0.1:{kernel.port}\r\n"
                             "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        self.deadline = time.monotonic() + 5
        response = b""
        while not response.endswith(b"\r\n\r\n"):
            assert len(response) < 16384
            response += self.exact(1)
        assert response.startswith(b"HTTP/1.1 101 ")
        self.send({"type": "auth", "token": token})
        assert self.receive()["type"] == "connected"

    def close(self):
        self.socket.close()

    def exact(self, length):
        data = b""
        while len(data) < length:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("WebSocket response deadline exceeded")
            self.socket.settimeout(remaining)
            part = self.socket.recv(length - len(data))
            if not part:
                raise EOFError("WebSocket closed")
            data += part
        return data

    def send(self, value):
        data = json.dumps(value).encode()
        assert len(data) < 65536
        header = bytes([0x81, 0x80 | len(data)]) if len(data) < 126 else bytes([0x81, 0xfe]) + struct.pack("!H", len(data))
        mask = b"test"
        self.socket.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def receive(self, timeout=5):
        self.deadline = time.monotonic() + timeout
        opcode, length = self.exact(2)
        assert length < 128
        if length == 126:
            length = struct.unpack("!H", self.exact(2))[0]
        assert length != 127 and length < 65536
        body = self.exact(length)
        if opcode == 0x88:
            return {"type": "close"}
        assert opcode == 0x81
        return json.loads(body)

    def next_message(self, timeout=5):
        deadline = time.monotonic() + timeout
        while True:
            frame = self.receive(deadline - time.monotonic())
            if frame["type"] != "ping":
                return frame
            self.send({"type": "pong", "timestamp": frame["timestamp"]})

    def require_closed(self):
        # Any result/event/replay here would be authorized output after the
        # server explicitly invalidated the connection's authority.
        try:
            frame = self.next_message()
        except (EOFError, ConnectionResetError):
            return
        assert frame["type"] == "close", frame

    def require_reauthentication(self):
        # Renewals run every second, with a two-second lease. A scheduler
        # allowance is only for observing the close, never for authorization.
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            frame = self.receive(deadline - time.monotonic())
            if frame["type"] == "ping":
                self.send({"type": "pong", "timestamp": frame["timestamp"]})
                continue
            assert frame["type"] == "error" and frame["error"] == "auth_failed", frame
            return
        raise AssertionError("connection retained authority after database change")


def seed(kernel, pat=False, admin=True):
    user = str(uuid.uuid4())
    credential = str(uuid.uuid4())
    raw = "fake-authority-" + uuid.uuid4().hex
    digest = hashlib.sha256(raw.encode()).hexdigest()
    kernel.sql(f"INSERT INTO plinth.users(id,username,password_hash) VALUES ('{user}', '{user}', 'unused')")
    if admin:
        kernel.sql(f"INSERT INTO plinth.group_members(group_id,user_id) SELECT id,'{user}' FROM plinth.groups WHERE name='admin'")
    if pat:
        kernel.sql(f"INSERT INTO plinth.pats(id,user_id,name,token_hash,token_prefix) VALUES ('{credential}','{user}','fixture','{digest}','fake')")
    else:
        kernel.sql(f"INSERT INTO plinth.sessions(id,user_id,token_hash) VALUES ('{credential}','{user}','{digest}')")
    return user, credential, ("plinth_" if pat else "") + raw



CHANNEL = "plinth:ext:fixture:notice"


def publish_marker(kernel):
    marker = uuid.uuid4().hex
    payload = json.dumps({"layer": "extension", "channel": CHANNEL, "marker": marker})
    # All values are generated by this fixture; dollar quoting also preserves
    # JSON punctuation without interpolating any credential or external input.
    kernel.sql("SELECT pg_notify('plinth:realtime', $fixture$" + payload + "$fixture$)")
    return marker


def require_live_event(peer, marker):
    deadline = time.monotonic() + 5
    while True:
        frame = peer.next_message(deadline - time.monotonic())
        assert frame["type"] == "event" and frame["channel"] == CHANNEL, frame
        if frame["payload"]["marker"] == marker:
            return


def send_admin_call(peer):
    call_id = uuid.uuid4().hex
    peer.send({"type": "call", "id": call_id, "signature": "lh0:1:chain", "args": [1]})
    return call_id


def require_admin_call(peer):
    call_id = send_admin_call(peer)
    frame = peer.next_message()
    assert frame["type"] == "call_result" and frame["id"] == call_id, frame
    assert frame["value"]["depth"] == 1, frame


def require_invalidated_work_denied(kernel, peer):
    # Deliberately attempt new work on the old transport after the explicit
    # auth_failed response; neither fresh calls, subscriptions/replay, nor
    # separately published events may produce authorized output.
    try:
        send_admin_call(peer)
        peer.send({"type": "subscribe", "channels": [CHANNEL], "since_seq": 0})
    except (BrokenPipeError, ConnectionResetError):
        pass
    publish_marker(kernel)
    peer.require_closed()


class TableBarrier:
    """Hold a real table lock until the caller explicitly releases it."""
    def __init__(self, kernel, table):
        assert table in {"sessions", "events"}
        self.kernel = kernel
        self.table = table
        self.child = None

    def __enter__(self):
        self.child = subprocess.Popen(["psql", "-XqAt", "-v", "ON_ERROR_STOP=1"],
                                      env=self.kernel.pg_env, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            self.child.stdin.write(f"BEGIN; LOCK TABLE plinth.{self.table} IN ACCESS EXCLUSIVE MODE; "
                                   "SELECT 'barrier-ready';\n")
            self.child.stdin.flush()
            with selectors.DefaultSelector() as ready:
                ready.register(self.child.stdout, selectors.EVENT_READ)
                assert ready.select(timeout=5), "table lock did not become ready"
                assert self.child.stdout.readline().strip() == "barrier-ready"
        except BaseException:
            self.close()
            raise
        return self

    def close(self):
        if self.child is None:
            return
        child, self.child = self.child, None
        try:
            child.communicate("ROLLBACK;\n", timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.communicate(timeout=5)
            raise AssertionError("table lock failed to release") from None
        assert child.returncode == 0, "table lock process failed"

    def __exit__(self, *_):
        self.close()


def wait_for_query_lock(kernel, prefix):
    deadline = time.monotonic() + 5
    while True:
        waiting = kernel.sql("SELECT count(*) FROM pg_stat_activity WHERE "
                             "datname=current_database() AND state='active' AND "
                             "wait_event_type='Lock' AND query LIKE '" + prefix + "%'")
        if int(waiting) > 0:
            return
        assert time.monotonic() < deadline, "expected query never reached its table lock"
        time.sleep(0.01)  # Poll an observed PostgreSQL wait, not an elapsed barrier.


def replay_revocation(kernel):
    _, credential, token = seed(kernel)
    marker = uuid.uuid4().hex
    payload = json.dumps({"layer": "extension", "channel": CHANNEL, "marker": marker})
    kernel.sql("INSERT INTO plinth.events(channel,payload) VALUES ('" + CHANNEL +
               "', $fixture$" + payload + "$fixture$::jsonb)")
    with contextlib.closing(Peer(kernel, token)) as peer:
        peer.send({"type": "subscribe", "channels": [CHANNEL], "since_seq": 0})
        assert peer.next_message()["type"] == "subscribed"
        seen = False
        while True:
            frame = peer.next_message()
            if frame["type"] == "replay_done":
                break
            assert frame["type"] == "replay", frame
            seen |= frame["envelope"].get("marker") == marker
        assert seen, "production replay did not deliver its seeded positive control"
        # Reuse the same authenticated connection but stall replay's first
        # database statement. Revocation must stop queued replay delivery.
        with TableBarrier(kernel, "events"):
            peer.send({"type": "subscribe", "channels": [CHANNEL], "since_seq": 0})
            assert peer.next_message()["type"] == "subscribed"
            wait_for_query_lock(kernel, "SELECT COALESCE(MIN(seq)")
            kernel.sql(f"UPDATE plinth.sessions SET revoked_at=NOW() WHERE id='{credential}'")
            peer.require_reauthentication()
        peer.require_closed()
    print("replay positive control and revocation while replay query is blocked passed", flush=True)


def natural_expiry(kernel):
    for pat in (False, True):
        _, credential, token = seed(kernel, pat)
        table = "pats" if pat else "sessions"
        kernel.sql(f"UPDATE plinth.{table} SET expires_at=NOW()+interval '2500 milliseconds' "
                   f"WHERE id='{credential}'")
        with contextlib.closing(Peer(kernel, token)) as peer:
            require_admin_call(peer)
            # No database mutation after authentication: the initially stored
            # expiry itself must bound authority even if renewal succeeds.
            peer.require_reauthentication()
            peer.require_closed()
    print("preexisting session and PAT expiry close their active sockets", flush=True)


def multiple_nodes(kernel, binary, env):
    with tempfile.TemporaryDirectory(prefix="plinth-authority-second-node-") as temporary:
        second = Kernel(binary, Path(temporary), kernel.database, env, False)
        config = json.loads(second.config.read_text())
        # Both nodes share one installation, as well as the same authority DB.
        config["packages"] = json.loads(kernel.config.read_text())["packages"]
        config["node_id"] = "authority-second-fixture"
        second.config.write_text(json.dumps(config))
        try:
            second.start()
            _, credential, token = seed(kernel)
            with contextlib.closing(Peer(kernel, token)) as first_peer, \
                    contextlib.closing(Peer(second, token)) as second_peer:
                require_admin_call(first_peer)
                require_admin_call(second_peer)
                kernel.sql(f"UPDATE plinth.sessions SET revoked_at=NOW() WHERE id='{credential}'")
                first_peer.require_reauthentication()
                second_peer.require_reauthentication()
                first_peer.require_closed()
                second_peer.require_closed()
        except BaseException:
            log = second.root / "kernel.log"
            if log.exists():
                print(log.read_text(errors="replace"), flush=True)
            raise
        finally:
            second.stop()
    print("two production nodes independently observe shared credential revocation", flush=True)


def blocked_renewal_shutdown(binary, env):
    for action in (None, signal.SIGINT, signal.SIGTERM):
        with running_kernel(binary, env, False) as kernel:
            _, _, token = seed(kernel)
            with contextlib.closing(Peer(kernel, token)) as peer:
                require_admin_call(peer)
                with TableBarrier(kernel, "sessions"):
                    wait_for_query_lock(kernel, "SELECT u.username,")
                    if action is None:
                        peer.require_reauthentication()
                        peer.require_closed()
                        assert kernel.request("GET", "/healthz")[0] == 200
                        print("blocked renewal times out and closes the socket", flush=True)
                        continue
                    # Keep the external lock held throughout shutdown. Releasing
                    # it before waiting would hide an unbounded refresh lifetime.
                    started = time.monotonic()
                    kernel.child.send_signal(action)
                    try:
                        code = kernel.child.wait(timeout=12)
                    except subprocess.TimeoutExpired:
                        kernel.child.kill()
                        kernel.child.wait(timeout=5)
                        raise AssertionError("blocked authority renewal prevented bounded shutdown") from None
                    assert code == 0, f"blocked-renewal shutdown returned {code}"
                    assert time.monotonic() - started < 12
        print(f"blocked authority renewal: {action.name if action else 'timeout closes socket'} passed", flush=True)


def run(binary, env):
    with running_kernel(binary, env, False) as kernel:
        for scenario in ("session-revoke", "pat-revoke", "session-expire", "pat-expire", "disabled", "admin-remove", "grant-remove"):
            pat = scenario.startswith("pat")
            user, credential, token = seed(kernel, pat, scenario != "grant-remove")
            group = str(uuid.uuid4())
            if scenario == "grant-remove":
                kernel.sql(f"INSERT INTO plinth.groups(id,name) VALUES ('{group}','{group}'); "
                           "INSERT INTO plinth.rbac_rules(rule,namespace,description,extension_name) "
                           "VALUES ('fixture.realtime.subscribe.notice','fixture','fixture','fixture') ON CONFLICT DO NOTHING; "
                           f"INSERT INTO plinth.group_rules(group_id,rule_id) SELECT '{group}',id FROM plinth.rbac_rules WHERE rule='fixture.realtime.subscribe.notice'; "
                           f"INSERT INTO plinth.group_members(group_id,user_id) VALUES ('{group}','{user}')")
            with contextlib.closing(Peer(kernel, token)) as peer:
                peer.send({"type": "subscribe", "channels": ["plinth:ext:fixture:notice"]})
                assert peer.next_message()["channels"] == [CHANNEL]
                require_live_event(peer, publish_marker(kernel))
                if scenario != "grant-remove":
                    require_admin_call(peer)
                table = "pats" if pat else "sessions"
                if scenario.endswith("revoke"):
                    kernel.sql(f"UPDATE plinth.{table} SET revoked_at=NOW() WHERE id='{credential}'")
                elif scenario.endswith("expire"):
                    kernel.sql(f"UPDATE plinth.{table} SET expires_at=NOW() WHERE id='{credential}'")
                elif scenario == "disabled":
                    kernel.sql(f"UPDATE plinth.users SET disabled_at=NOW() WHERE id='{user}'")
                elif scenario == "admin-remove":
                    kernel.sql(f"DELETE FROM plinth.group_members WHERE user_id='{user}'")
                else:
                    kernel.sql(f"DELETE FROM plinth.group_rules WHERE group_id='{group}'")
                peer.require_reauthentication()
                require_invalidated_work_denied(kernel, peer)
                assert kernel.request("GET", "/healthz")[0] == 200
            print(scenario + ": socket authority revoked", flush=True)
        # The lease must continue renewing for an unchanged connection.
        _, _, token = seed(kernel)
        with contextlib.closing(Peer(kernel, token)) as peer:
            until = time.monotonic() + 5
            while time.monotonic() < until:
                peer.send({"type": "subscribe", "channels": []})
                assert peer.next_message()["type"] == "subscribed"
                time.sleep(0.25)
        print("unchanged authority renews across multiple leases", flush=True)
        replay_revocation(kernel)
        natural_expiry(kernel)
        multiple_nodes(kernel, binary, env)
    blocked_renewal_shutdown(binary, env)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get("PLINTH_PG_HOST"):
        return 77
    env = os.environ.copy()
    for suffix in ("HOST", "PORT", "USER", "PASSWORD", "DATABASE"):
        env["PG" + suffix] = os.environ["PLINTH_PG_" + suffix]
    run(args.binary.resolve(), env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
