#!/usr/bin/env python3
"""Check actual deployment harness admission with mocked external commands.

These are manifest and control-flow regressions, not live PostgreSQL evidence.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, Mock, call, mock_open, patch

from k3d_lifecycle_test import Harness
from k3d_recovery_test import RecoveryHarness


EXPECTED_POSTGRES_IMAGE = (
    "docker.io/pgvector/pgvector:pg16@"
    "sha256:ccc6e83d6e35e931dc7c5def2022729d5a6c370318d099181995567ff1fb4d6b"
)
EXPECTED_RESTORE_ADMIN = "plinth_restore_test_admin"


def harness_fixture(kind):
    # Avoid constructor-created files, sockets, credentials and cleanup hooks.
    harness = kind.__new__(kind)
    harness.args = SimpleNamespace(kubectl="mock-kubectl")
    harness.namespace = "mock-source-namespace"
    harness.restore_namespace = "mock-restored-namespace"
    harness.namespace_created = False
    harness.chart_installed = False
    harness.release = "mock-release"
    harness.host = "mock.plinth.test"
    harness.certificate = Path("/mock/tls.crt")
    harness.private_key = Path("/mock/tls.key")
    harness.bootstrap_secret_name = "mock-bootstrap"
    harness.bootstrap_token = "fake-readiness-test-token"
    harness.run = Mock()
    harness.kubectl = Mock()
    harness.apply_json = Mock()
    return harness


def postgres_container(harness):
    deployments = []
    for applied in harness.apply_json.call_args_list:
        document = applied.args[0]
        documents = document["items"] if document["kind"] == "List" else [document]
        deployments.extend(item for item in documents
                           if item["kind"] == "Deployment"
                           and item["metadata"]["name"] == "postgres")
    if len(deployments) != 1:
        raise AssertionError("expected exactly one actual PostgreSQL deployment")
    return deployments[0]["spec"]["template"]["spec"]["containers"][0]


class PostgresReadinessTest(unittest.TestCase):
    def assert_readiness(self, harness, role, namespace):
        container = postgres_container(harness)
        self.assertEqual(container["image"], EXPECTED_POSTGRES_IMAGE)
        self.assertEqual(container["readinessProbe"], {
            "exec": {"command": [
                "pg_isready", "-h", "127.0.0.1", "-p", "5432",
                "-U", role, "-d", role,
            ]},
            "periodSeconds": 2,
            "timeoutSeconds": 2,
            "failureThreshold": 30,
        })
        environment = {entry["name"]: entry for entry in container["env"]}
        self.assertEqual(environment["POSTGRES_USER"]["value"], role)
        self.assertEqual(environment["POSTGRES_DB"]["value"], role)
        self.assertEqual(harness.kubectl.call_args, call(
            "rollout", "status", "deployment/postgres", "-n", namespace,
            "--timeout=180s", timeout=200,
        ))

    def test_lifecycle_probe_waits_for_loopback_tcp_with_original_bounds(self):
        harness = harness_fixture(Harness)
        Harness.create_namespace_dependencies(harness)
        self.assert_readiness(harness, "plinth", "mock-source-namespace")

    def test_restore_probe_keeps_admin_identity_and_returns_after_rollout(self):
        harness = harness_fixture(RecoveryHarness)
        events = []

        def kubectl(*arguments, **kwargs):
            if arguments[:2] == ("rollout", "status"):
                # Deployment application must precede the blocking readiness call.
                self.assertEqual(postgres_container(harness)["name"], "postgres")
                events.append("rollout-succeeded")

        harness.kubectl.side_effect = kubectl
        result = RecoveryHarness.create_restore_namespace(
            harness, "mock-restored-namespace")
        events.append("namespace-returned")
        self.assertIsNone(result)
        self.assertEqual(events, ["rollout-succeeded", "namespace-returned"])
        self.assert_readiness(harness, EXPECTED_RESTORE_ADMIN,
                              "mock-restored-namespace")

    def test_lifecycle_rollout_failure_propagates_before_chart_admission(self):
        harness = harness_fixture(Harness)
        harness.create_infrastructure = Mock()
        harness.install_chart = Mock()
        failure = RuntimeError("mock PostgreSQL rollout failed")

        def kubectl(*arguments, **kwargs):
            if arguments[:2] == ("rollout", "status"):
                raise failure

        harness.kubectl.side_effect = kubectl
        with self.assertRaises(RuntimeError) as raised:
            Harness.execute(harness)
        self.assertIs(raised.exception, failure)
        self.assert_readiness(harness, "plinth", "mock-source-namespace")
        harness.install_chart.assert_not_called()

    def test_restore_rollout_failure_propagates_before_restore_admission(self):
        harness = harness_fixture(RecoveryHarness)
        for name in (
            "create_infrastructure", "publish_previous",
            "create_namespace_dependencies", "install_chart", "bootstrap_admin",
            "remove_bootstrap_authority", "enable_public_route",
            "seed_installation", "verify_retained_user", "snapshot",
            "verify_files", "backup", "delete_current_namespace",
            "restore", "install_restored",
        ):
            setattr(harness, name, Mock())
        failure = subprocess.TimeoutExpired(["mock-kubectl", "rollout"], 200)

        def kubectl(*arguments, **kwargs):
            if arguments[:2] == ("rollout", "status"):
                raise failure

        harness.kubectl.side_effect = kubectl
        with self.assertRaises(subprocess.TimeoutExpired) as raised:
            RecoveryHarness.execute(harness)
        self.assertIs(raised.exception, failure)
        self.assert_readiness(harness, EXPECTED_RESTORE_ADMIN,
                              "mock-restored-namespace")
        harness.restore.assert_not_called()
        harness.install_restored.assert_not_called()

    def test_restore_rejects_an_existing_namespace_or_chart_before_commands(self):
        for active in ("namespace_created", "chart_installed"):
            with self.subTest(active=active):
                harness = harness_fixture(RecoveryHarness)
                setattr(harness, active, True)
                with self.assertRaisesRegex(AssertionError, "old namespace is still active"):
                    RecoveryHarness.create_restore_namespace(
                        harness, "mock-restored-namespace")
                harness.kubectl.assert_not_called()
                harness.apply_json.assert_not_called()

    def restore_fixture(self):
        harness = harness_fixture(RecoveryHarness)
        harness.namespace = "mock-restored-namespace"
        harness.postgres_pod = Mock(return_value="mock-postgres-pod")
        harness._binary_command = Mock()
        harness.sql = Mock(return_value="plinth")
        harness.helper = Mock(return_value="mock-volume-helper")
        harness.remove_helper = Mock()
        payload = b"fake-backup-artifact-" * 8
        artifacts = {"sha256": {}}
        for name in ("globals", "database", "data", "logs"):
            path = MagicMock(spec=Path)
            path.is_file.return_value = True
            path.stat.return_value = SimpleNamespace(st_size=len(payload))
            path.read_bytes.return_value = payload
            artifacts[name] = path
            artifacts["sha256"][name] = hashlib.sha256(payload).hexdigest()
        return harness, artifacts

    def test_restore_globals_and_database_keep_fail_closed_command_flags(self):
        harness, artifacts = self.restore_fixture()
        RecoveryHarness.restore(harness, artifacts)
        self.assertEqual(harness._binary_command.call_args_list[:2], [
            call([
                "mock-kubectl", "exec", "-i", "-n", "mock-restored-namespace",
                "mock-postgres-pod", "--", "psql", "-X", "-v",
                "ON_ERROR_STOP=1", "-U", EXPECTED_RESTORE_ADMIN,
                "-d", EXPECTED_RESTORE_ADMIN,
            ], stage="restore-globals", input_path=artifacts["globals"]),
            call([
                "mock-kubectl", "exec", "-i", "-n", "mock-restored-namespace",
                "mock-postgres-pod", "--", "pg_restore", "-U",
                EXPECTED_RESTORE_ADMIN, "-d", EXPECTED_RESTORE_ADMIN,
                "--create", "--exit-on-error",
            ], stage="restore-database", input_path=artifacts["database"]),
        ])
        harness.remove_helper.assert_called_once_with("mock-volume-helper")

    def test_failed_globals_restore_prevents_database_and_claim_restore(self):
        harness, artifacts = self.restore_fixture()
        failure = AssertionError("mock private globals transfer failure")
        harness._binary_command.side_effect = failure
        with self.assertRaises(AssertionError) as raised:
            RecoveryHarness.restore(harness, artifacts)
        self.assertIs(raised.exception, failure)
        self.assertEqual(harness._binary_command.call_count, 1)
        self.assertEqual(harness._binary_command.call_args.kwargs["stage"],
                         "restore-globals")
        harness.sql.assert_not_called()
        harness.helper.assert_not_called()

    def test_binary_transfer_failure_keeps_stderr_private_and_closes_input(self):
        harness = harness_fixture(RecoveryHarness)
        harness.root = Path("/mock/private-readiness-test")
        harness.env = {"KUBECONFIG": "/mock/kubeconfig"}
        detail = b"fake-private-transfer-detail"
        input_file = mock_open(read_data=b"fake-backup")
        diagnostic_file = mock_open()
        with patch("builtins.open", input_file), \
                patch("k3d_recovery_test.os.open", return_value=37) as private_open, \
                patch("k3d_recovery_test.os.fdopen", diagnostic_file), \
                patch("k3d_recovery_test.subprocess.run", return_value=
                      SimpleNamespace(returncode=3, stderr=detail)) as run:
            with self.assertRaisesRegex(AssertionError,
                                        "stderr retained privately until cleanup") as raised:
                RecoveryHarness._binary_command(
                    harness, ["mock-kubectl", "exec", "psql"],
                    stage="restore-globals", input_path=Path("/mock/globals.sql"))
        self.assertNotIn(detail.decode(), str(raised.exception))
        self.assertEqual(private_open.call_args.args[0],
                         harness.root / "transfer-restore-globals.stderr")
        self.assertEqual(private_open.call_args.args[2], 0o600)
        diagnostic_file().write.assert_called_once_with(detail)
        input_file().close.assert_called_once_with()
        self.assertEqual(run.call_args.kwargs["timeout"], 120)
        self.assertIs(run.call_args.kwargs["stdout"], subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
