#!/usr/bin/env python3
"""Hermetic admission tests, not evidence of a live replacement or login."""

from __future__ import annotations

import ast
import contextlib
import inspect
import io
from pathlib import Path
import signal
import subprocess
import textwrap
import traceback
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import k3d_lifecycle_test as lifecycle
from k3d_lifecycle_test import Harness
from k3d_recovery_test import RecoveryHarness


SENTINEL = "no available server\n"
BODY_CANARY = "fake-body-private-canary"
COMMAND_CANARY = "fake-command-private-canary"
SESSION_CANARY = "fake-session-private-canary"
PASSWORD = "fake-password-for-issue36!"


class FakeClock:
    def __init__(self):
        self.now = 100.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        self.sleeps.append(duration)
        self.now += duration


def harness_fixture(kind=Harness):
    # Do not construct files, sockets, resources, or cleanup registrations.
    harness = kind.__new__(kind)
    harness.host = "fake.plinth.test"
    harness.https_port = 443
    harness.cookies = Path("/fake/" + SESSION_CANARY)
    harness.curl = Mock(return_value=(0, "200", "{}"))
    return harness


def login_calls(method):
    parsed = ast.parse(textwrap.dedent(inspect.getsource(method)))
    return [node for node in ast.walk(parsed) if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and node.func.attr == "verify_login"]


class AuthReadinessTest(unittest.TestCase):
    def setUp(self):
        self.harness = harness_fixture()
        self.clock = FakeClock()
        monotonic = patch.object(lifecycle.time, "monotonic", self.clock.monotonic)
        sleep = patch.object(lifecycle.time, "sleep", self.clock.sleep)
        monotonic.start()
        sleep.start()
        self.addCleanup(monotonic.stop)
        self.addCleanup(sleep.stop)

    def verify(self, phase="restart"):
        return self.harness.verify_login(replacement_phase=phase)

    def assert_fixed_failure(self, outcome, phase="restart", *, status="503"):
        with self.assertRaisesRegex(RuntimeError, "outcome=" + outcome) as raised:
            self.verify(phase)
        status_suffix = " http_status=" + status if outcome == "response" else ""
        self.assertEqual(str(raised.exception),
                         f"persisted account login: phase={phase or 'single-shot'} outcome={outcome}"
                         + status_suffix)

    def test_immediate_login_success_is_one_request_without_sleep(self):
        self.verify()
        self.harness.curl.assert_called_once_with(
            "/api/auth/login", data=lifecycle.json.dumps({
                "username": "issue36-admin", "password": PASSWORD,
            }), origin=self.harness.origin, cookie_jar=self.harness.cookies,
            timeout=30,
        )
        self.assertEqual(self.clock.sleeps, [])

    def test_both_fixed_phases_retry_only_exact_sentinel(self):
        for phase in ("restart", "sequential-rollout"):
            with self.subTest(phase=phase):
                self.harness.curl.reset_mock(side_effect=True)
                self.clock.now = 100.0
                self.clock.sleeps.clear()
                self.harness.curl.side_effect = [
                    (0, "503", SENTINEL), (0, "200", "{}"),
                ]
                self.verify(phase)
                self.assertEqual(self.harness.curl.call_count, 2)
                self.assertEqual(self.clock.sleeps, [12])
                self.assertEqual([c.kwargs["timeout"] for c in
                                  self.harness.curl.call_args_list], [30, 18])

    def test_repeated_sentinel_expires_without_extra_request(self):
        self.harness.curl.return_value = (0, "503", SENTINEL)
        self.assert_fixed_failure("deadline")
        self.assertEqual(self.harness.curl.call_count, 3)
        self.assertEqual(self.clock.now, 130)
        self.assertEqual(self.clock.sleeps, [12, 12, 6])
        self.assertEqual(self.harness.curl.call_args.kwargs["timeout"], 6)

    def test_backoff_matches_unmodified_auth_rate_defaults_and_fixture(self):
        defaults = lifecycle.yaml.safe_load(
            (lifecycle.CHART / "values.yaml").read_text(encoding="utf-8")
        )["traefik"]["limits"]
        self.assertEqual({key: defaults[key] for key in
                          ("authRateAverage", "authRateBurst", "authRatePeriodSeconds")}, {
                              "authRateAverage": 5, "authRateBurst": 5,
                              "authRatePeriodSeconds": 60,
                          })
        self.assertEqual(lifecycle.AUTH_ROUTE_RETRY_BACKOFF_SECONDS,
                         defaults["authRatePeriodSeconds"] / defaults["authRateAverage"])
        middleware = (lifecycle.CHART / "templates" / "traefik-middlewares.yaml").read_text(
            encoding="utf-8")
        for key, default in (("authRateAverage", 5), ("authRateBurst", 5),
                             ("authRatePeriodSeconds", 60)):
            self.assertIn(f".Values.traefik.limits.{key} | default {default}", middleware)
        self.harness.internal_repository = "fake-registry/plinth"
        self.harness.candidate_digest = "sha256:" + "a" * 64
        self.harness.namespace = "fake-namespace"
        values = self.harness.values(registration=False, nonce="replacement",
                                     traefik_enabled=True)
        self.assertTrue({"authRateAverage", "authRateBurst", "authRatePeriodSeconds"}.isdisjoint(
            values["traefik"]["limits"]))

    def test_depleted_token_bucket_does_not_self_induce_429_on_retry(self):
        # Four prior auth requests leave one token. Each attempted POST,
        # including a pre-forward 503, consumes one; refill is five per minute.
        tokens = 1.0
        last = self.clock.now
        observed = []
        def curl(*args, **kwargs):
            nonlocal tokens, last
            tokens = min(5, tokens + (self.clock.now - last) * 5 / 60)
            last = self.clock.now
            if tokens < 1:
                status = "429"
            else:
                tokens -= 1
                status = "503" if len(observed) < 2 else "200"
            observed.append((self.clock.now - 100, status))
            return 0, status, SENTINEL if status == "503" else "{}"
        self.harness.curl.side_effect = curl
        self.verify()
        self.assertEqual(observed, [(0, "503"), (12, "503"), (24, "200")])
        self.assertEqual(self.clock.sleeps, [12, 12])
        self.assertEqual(tokens, 0)

    def test_completed_success_at_or_after_deadline_is_failure(self):
        for elapsed in (30, 31):
            with self.subTest(elapsed=elapsed):
                self.clock.now = 100
                self.harness.curl.reset_mock()
                def curl(*args, **kwargs):
                    self.clock.now += elapsed
                    return 0, "200", "{}"
                self.harness.curl.side_effect = curl
                self.assert_fixed_failure("deadline")
                self.harness.curl.assert_called_once()
                self.assertEqual(self.clock.sleeps, [])

    def test_backoff_is_clipped_at_expiry(self):
        def curl(*args, **kwargs):
            self.clock.now += 29.75
            return 0, "503", SENTINEL
        self.harness.curl.side_effect = curl
        self.assert_fixed_failure("deadline")
        self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [0.25])
        self.assertEqual(self.clock.now, 130)

    def test_elapsed_request_clips_next_request_timeout(self):
        results = iter(((0, "503", SENTINEL), (0, "200", "{}")))
        def curl(*args, **kwargs):
            self.clock.now += 4
            return next(results)
        self.harness.curl.side_effect = curl
        self.verify()
        self.assertEqual([c.kwargs["timeout"] for c in
                          self.harness.curl.call_args_list], [30, 14])

    def test_expiry_before_first_request_does_not_issue_post(self):
        with patch.object(lifecycle.time, "monotonic", side_effect=[100, 130]):
            self.assert_fixed_failure("deadline")
        self.harness.curl.assert_not_called()

    def test_nonmatching_503_bodies_fail_without_retry(self):
        for body in ("no available server", "no available server\r\n",
                     "no available server\nextra", " no available server\n",
                     "no available server \n", "No available server\n",
                     "no available server\n\n", '{"error":"no available server"}',
                     "<html>no available server</html>", BODY_CANARY,
                     "no available server\ufffd\n"):
            with self.subTest(body=body):
                self.harness.curl.reset_mock()
                self.harness.curl.return_value = (0, "503", body)
                self.assert_fixed_failure("response")
                self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_arbitrary_status_is_redacted_to_fixed_invalid_label(self):
        for status in (BODY_CANARY + PASSWORD + SESSION_CANARY, "429\n", " 429",
                       "503 extra", "２００", 200, b"429", None, {}):
            with self.subTest(kind=type(status).__name__):
                self.harness.curl.reset_mock()
                self.harness.curl.return_value = (0, status, BODY_CANARY)
                self.assert_fixed_failure("response", status="invalid")
                self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_other_http_statuses_fail_without_retry(self):
        for status in ("000", "201", "301", "400", "401", "403", "429", "500", "502", "504"):
            with self.subTest(status=status):
                self.harness.curl.reset_mock()
                self.harness.curl.return_value = (0, status, SENTINEL)
                self.assert_fixed_failure("response", status=status)
                self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_nonzero_exit_never_retries_even_with_matching_response(self):
        for status in ("200", "503"):
            with self.subTest(status=status):
                self.harness.curl.reset_mock()
                self.harness.curl.return_value = (28, status, SENTINEL)
                self.assert_fixed_failure("transport-exit")
                self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_default_login_remains_single_shot(self):
        self.harness.verify_login()
        self.harness.curl.assert_called_once()
        self.harness.curl.reset_mock()
        self.harness.curl.return_value = (0, "503", SENTINEL)
        self.assert_fixed_failure("response", None)
        self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_unknown_phase_fails_closed_without_echoing_input(self):
        for phase in ("", "recovery", BODY_CANARY, 1):
            with self.subTest(phase=phase):
                with self.assertRaisesRegex(RuntimeError, "invalid replacement phase") as raised:
                    self.verify(phase)
                self.assertNotIn(BODY_CANARY, str(raised.exception))
        self.harness.curl.assert_not_called()

    def test_recovery_inheritance_remains_single_shot_at_all_five_sites(self):
        self.harness = harness_fixture(RecoveryHarness)
        self.harness.curl.return_value = (0, "503", SENTINEL)
        self.assert_fixed_failure("response", None)
        self.harness.curl.assert_called_once()
        self.assertIs(RecoveryHarness.verify_login, Harness.verify_login)
        nodes = login_calls(RecoveryHarness)
        self.assertEqual(len(nodes), 5)
        self.assertTrue(all(not node.args and not node.keywords for node in nodes))
        self.assertEqual(self.clock.sleeps, [])

    def test_only_two_controlled_lifecycle_sites_opt_in(self):
        nodes = login_calls(Harness)
        self.assertEqual(len(nodes), 2)
        for method, expected in ((Harness.verify_restart, "restart"),
                                 (Harness.verify_sequential_rollout, "sequential-rollout")):
            nodes = login_calls(method)
            self.assertEqual(len(nodes), 1)
            self.assertEqual(nodes[0].args, [])
            self.assertEqual([(kw.arg, ast.literal_eval(kw.value))
                              for kw in nodes[0].keywords], [("replacement_phase", expected)])

    def test_transport_exceptions_are_immediate_fixed_diagnostics(self):
        for phase in (None, "restart", "sequential-rollout"):
            for exception, outcome in (
                (subprocess.TimeoutExpired([COMMAND_CANARY, PASSWORD], 30,
                                           output=BODY_CANARY, stderr=SESSION_CANARY), "timeout"),
                (OSError(BODY_CANARY + PASSWORD + SESSION_CANARY), "transport-error"),
            ):
                with self.subTest(phase=phase, outcome=outcome):
                    self.harness.curl.reset_mock()
                    self.harness.curl.side_effect = exception
                    self.assert_fixed_failure(outcome, phase)
                    self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])

    def test_rendered_traceback_and_cli_capture_omit_canaries(self):
        source = ast.parse(Path(lifecycle.__file__).read_text(encoding="utf-8"))
        # Execute the actual CLI exception boundary, not a copied diagnostic.
        boundary = compile(ast.Module(body=[source.body[-1]], type_ignores=[]),
                           lifecycle.__file__, "exec")
        for phase in (None, "restart", "sequential-rollout"):
            for failure in (
                subprocess.TimeoutExpired([COMMAND_CANARY, PASSWORD], 30,
                                          output=BODY_CANARY, stderr=SESSION_CANARY),
                OSError(COMMAND_CANARY + PASSWORD + BODY_CANARY + SESSION_CANARY),
                (0, "503", BODY_CANARY + PASSWORD + SESSION_CANARY),
                (0, COMMAND_CANARY + PASSWORD + BODY_CANARY + SESSION_CANARY, ""),
                (22, "200", BODY_CANARY + PASSWORD + SESSION_CANARY),
            ):
                with self.subTest(phase=phase, kind=type(failure).__name__):
                    self.harness.curl.side_effect = failure if isinstance(failure, Exception) else None
                    self.harness.curl.return_value = failure
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                        try:
                            exec(boundary, {
                                "__name__": "__main__", "main": lambda: self.verify(phase),
                                "sys": lifecycle.sys, "yaml": lifecycle.yaml,
                            })
                        except SystemExit as error:
                            self.assertEqual(error.code, 1)
                            rendered = traceback.format_exc()
                        else:
                            self.fail("diagnostic fixture must fail")
                    self.assertIn("k3d lifecycle: persisted account login:", output.getvalue())
                    for canary in (BODY_CANARY, COMMAND_CANARY, SESSION_CANARY, PASSWORD):
                        self.assertNotIn(canary, rendered)
                        self.assertNotIn(canary, output.getvalue())

    def test_keyboard_interrupt_propagates_without_retry(self):
        for phase in (None, "restart", "sequential-rollout"):
            with self.subTest(phase=phase):
                self.harness.curl.reset_mock()
                self.harness.curl.side_effect = KeyboardInterrupt("fake interruption")
                with self.assertRaises(KeyboardInterrupt):
                    self.verify(phase)
                self.harness.curl.assert_called_once()
        self.assertEqual(self.clock.sleeps, [])


class CurlBodyReadinessTest(unittest.TestCase):
    def test_actual_curl_preserves_bytes_and_default_timeout(self):
        harness = harness_fixture()
        harness.root = Path("/fake/root")
        harness.certificate = Path("/fake/tls.crt")
        harness.host = "fake.plinth.test"
        harness.https_port = 443
        harness.run = Mock(return_value=SimpleNamespace(returncode=0, stdout="503"))
        for raw in (b"no available server\n", b"no available server\r\n",
                    b"no available server\xff\n"):
            with self.subTest(raw=raw), patch.object(Path, "exists", return_value=True), \
                    patch.object(Path, "read_bytes", return_value=raw):
                result = Harness.curl(harness, "/api/auth/login", data="fake-credentials")
                self.assertEqual(result, (0, "503", raw.decode("utf-8", errors="replace")))
                self.assertEqual(result[2] == SENTINEL, raw == b"no available server\n")
                self.assertEqual(harness.run.call_args.kwargs, {"timeout": 30, "check": False})
        with patch.object(Path, "exists", return_value=False):
            self.assertEqual(Harness.curl(harness, "/healthz", timeout=0.25), (0, "503", ""))
            self.assertEqual(harness.run.call_args.kwargs["timeout"], 0.25)

    def test_actual_crlf_body_is_not_admitted_as_readiness(self):
        harness = harness_fixture()
        del harness.curl
        harness.root = Path("/fake/root")
        harness.certificate = Path("/fake/tls.crt")
        harness.host = "fake.plinth.test"
        harness.https_port = 443
        harness.run = Mock(return_value=SimpleNamespace(returncode=0, stdout="503"))
        with patch.object(Path, "exists", return_value=True), \
                patch.object(Path, "read_bytes", return_value=b"no available server\r\n"), \
                patch.object(lifecycle.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "outcome=response"):
                harness.verify_login(replacement_phase="restart")
        harness.run.assert_called_once()
        sleep.assert_not_called()


class MainSignalOwnershipTest(unittest.TestCase):
    def test_mocked_term_and_hup_mark_failed_diagnose_and_cleanup(self):
        for signum in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signum=signum):
                harness = Mock()
                harness.failed = False
                handlers = {}
                events = []
                def execute():
                    events.append("execute")
                    handlers[signum](signum, None)
                harness.execute.side_effect = execute
                harness.diagnostics.side_effect = lambda: events.append("diagnostics")
                harness.cleanup.side_effect = lambda: events.append("cleanup")
                with patch.object(lifecycle, "Harness", return_value=harness), \
                        patch.object(lifecycle.sys, "argv", ["mock-harness", "--image", "fake-image"]), \
                        patch.object(lifecycle.shutil, "which", return_value="/fake/tool"), \
                        patch.object(lifecycle.CHART.__class__, "is_dir", return_value=True), \
                        patch.object(lifecycle.atexit, "register") as register, \
                        patch.object(lifecycle.signal, "signal",
                                     side_effect=lambda sig, handler: handlers.__setitem__(sig, handler)) as signals:
                    with self.assertRaises(KeyboardInterrupt):
                        lifecycle.main()
                self.assertTrue(harness.failed)
                harness.diagnostics.assert_called_once_with()
                harness.cleanup.assert_called_once_with()
                register.assert_called_once_with(harness.cleanup)
                self.assertEqual([item.args[0] for item in signals.call_args_list],
                                 [signal.SIGTERM, signal.SIGHUP])
                self.assertEqual(events, ["execute", "diagnostics", "cleanup"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
