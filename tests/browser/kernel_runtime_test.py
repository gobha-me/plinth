"""Unit tests for native/container production-test launch ownership."""

from argparse import Namespace
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest import mock

from kernel_runtime import ContainerProcess, ContainerRuntime, NativeRuntime, runtime_from_args


class KernelRuntimeTest(unittest.TestCase):
    def test_runtime_selection_preserves_native_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "plinth"
            binary.touch()
            runtime = runtime_from_args(Namespace(binary=binary, image=None))
        self.assertIsInstance(runtime, NativeRuntime)
        self.assertEqual(runtime.binary, binary)

    @mock.patch("kernel_runtime.subprocess.run")
    def test_container_launch_stages_without_daemon_host_paths(self, run):
        run.return_value = subprocess.CompletedProcess([], 0)
        runtime = ContainerRuntime("plinth:test")
        child = mock.Mock(returncode=None)
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch("kernel_runtime.subprocess.Popen", return_value=child) as popen:
            root = Path(temporary)
            runtime.prepare_root(root)
            (root / "config.json").write_text("{}")
            private = root / "browser-profile"
            private.mkdir()
            (private / "cookies.sqlite").write_text("must-not-be-staged")
            runtime.start(
                ["serve", "--config", "/var/lib/plinth/config.json"], root=root,
                env={"PATH": "/bin", "PLINTH_PG_PASSWORD": "not-on-command-line",
                     "PLINTH_PG_DATABASE": "runtime_test", "UNRELATED": "ignored"},
                output=subprocess.DEVNULL)
        create = next(
            call.args[0] for call in run.call_args_list
            if call.args and call.args[0][:2] == ["docker", "create"])
        name = create[3]
        self.assertEqual(create[:6], [
            "docker", "create", "--name", name, "--network", "host"])
        self.assertIn("PLINTH_PG_PASSWORD", create)
        self.assertIn("PLINTH_PG_DATABASE", create)
        self.assertNotIn("not-on-command-line", create)
        self.assertNotIn("runtime_test", create)
        self.assertNotIn("UNRELATED", create)
        self.assertFalse(any(str(root.resolve()) in item for item in create))
        self.assertEqual(create[-4:], ["plinth:test", "serve", "--config",
                                      "/var/lib/plinth/config.json"])
        copies = [call.args[0] for call in run.call_args_list
                  if call.args and call.args[0][:2] == ["docker", "cp"]]
        self.assertEqual({Path(command[2]).name for command in copies},
                         {"config.json", "data", "logs"})
        self.assertTrue(all(command[-1] == name + ":/var/lib/plinth/"
                            for command in copies))
        self.assertFalse(any("browser-profile" in item
                             for command in copies for item in command))
        self.assertEqual(popen.call_args.args[0],
                         ["docker", "start", "--attach", name])

    @mock.patch("kernel_runtime.subprocess.run")
    def test_container_signal_targets_named_pid1(self, run):
        run.return_value = subprocess.CompletedProcess([], 0)
        child = mock.Mock()
        child.poll.return_value = None
        process = ContainerProcess(child, "plinth-runtime-test-owned", lambda: None)
        process.send_signal(signal.SIGTERM)
        run.assert_called_once_with(
            ["docker", "kill", "--signal=SIGTERM", "plinth-runtime-test-owned"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def test_container_wait_restores_host_tree_once(self):
        child = mock.Mock()
        child.wait.return_value = 0
        cleanup = mock.Mock()
        process = ContainerProcess(child, "plinth-runtime-test-owned", cleanup)
        self.assertEqual(process.wait(timeout=3), 0)
        self.assertEqual(process.wait(timeout=3), 0)
        cleanup.assert_called_once_with()

    def test_container_poll_cleans_terminal_client_once(self):
        child = mock.Mock(returncode=0)
        child.poll.return_value = 0
        cleanup = mock.Mock()
        process = ContainerProcess(child, "plinth-runtime-test-owned", cleanup)
        self.assertEqual(process.poll(), 0)
        self.assertEqual(process.poll(), 0)
        self.assertEqual(process.returncode, 0)
        cleanup.assert_called_once_with()

    def test_container_cleanup_failure_is_reported_and_retryable(self):
        child = mock.Mock(returncode=0)
        child.poll.return_value = 0
        cleanup = mock.Mock(side_effect=[RuntimeError("remove failed"), None])
        process = ContainerProcess(child, "plinth-runtime-test-owned", cleanup)
        with self.assertRaisesRegex(RuntimeError, "remove failed"):
            process.poll()
        self.assertEqual(process.poll(), 0)
        self.assertEqual(cleanup.call_count, 2)

    @mock.patch.object(ContainerRuntime, "_remove_container")
    @mock.patch.object(ContainerRuntime, "_collect")
    def test_collection_failure_after_removal_is_reported_only_once(
            self, collect, remove):
        collect.side_effect = RuntimeError("copy failed")
        runtime = object.__new__(ContainerRuntime)
        child = mock.Mock(returncode=0)
        child.poll.return_value = 0
        process = ContainerProcess(
            child, "plinth-runtime-test-owned",
            lambda: runtime._collect_and_remove(
                "plinth-runtime-test-owned", Path("/owned")),
        )
        with self.assertRaisesRegex(RuntimeError, "state collection failed"):
            process.poll()
        self.assertEqual(process.poll(), 0)
        collect.assert_called_once_with("plinth-runtime-test-owned", Path("/owned"))
        remove.assert_called_once_with("plinth-runtime-test-owned")

    def test_permission_normalization_never_follows_symlinks(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "data"
            root.mkdir()
            outside = Path(temporary) / "outside"
            outside.write_text("owned elsewhere")
            outside.chmod(0o600)
            (root / "escape").symlink_to(outside)
            runtime._make_tree_writable(root)
            self.assertEqual(outside.stat().st_mode & 0o777, 0o600)
            self.assertTrue((root / "escape").is_symlink())

    def test_remove_owned_path_unlinks_top_level_symlink(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            outside = Path(temporary) / "outside"
            outside.mkdir()
            retained = outside / "retained"
            retained.write_text("keep")
            link = Path(temporary) / "data"
            link.symlink_to(outside, target_is_directory=True)
            runtime._remove_owned_path(link)
            self.assertFalse(link.is_symlink())
            self.assertEqual(retained.read_text(), "keep")

    @mock.patch("kernel_runtime.subprocess.run")
    def test_staging_rejects_allowlisted_top_level_symlink(self, run):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            outside = root.parent / (root.name + "-outside")
            outside.write_text("do not copy")
            try:
                (root / "config.json").symlink_to(outside)
                with self.assertRaisesRegex(RuntimeError, "symbolic-link"):
                    runtime._stage_container("owned", root)
            finally:
                outside.unlink(missing_ok=True)
        run.assert_not_called()

    @mock.patch("kernel_runtime.subprocess.run")
    def test_start_removes_exact_container_when_popen_fails(self, run):
        run.return_value = subprocess.CompletedProcess([], 0)
        runtime = object.__new__(ContainerRuntime)
        runtime.image = "plinth:test"
        with tempfile.TemporaryDirectory() as temporary, \
             mock.patch("kernel_runtime.subprocess.Popen",
                        side_effect=OSError("client failed")):
            root = Path(temporary)
            runtime.prepare_root(root)
            with self.assertRaisesRegex(OSError, "client failed"):
                runtime.start(["--version"], root=root, env={},
                              output=subprocess.DEVNULL)
        removals = [call.args[0] for call in run.call_args_list
                    if call.args and call.args[0][:2] == ["docker", "rm"]]
        self.assertEqual(len(removals), 1)
        self.assertEqual(removals[0][:4],
                         ["docker", "rm", "--force", "--volumes"])

    @mock.patch("kernel_runtime.subprocess.run")
    def test_run_attempts_cleanup_when_create_times_out(self, run):
        runtime = object.__new__(ContainerRuntime)
        runtime.image = "plinth:test"
        run.side_effect = [subprocess.TimeoutExpired("docker create", 30),
                           subprocess.CompletedProcess([], 0)]
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(subprocess.TimeoutExpired):
                runtime.run(["--version"], root=Path(temporary), env={})
        self.assertEqual(run.call_args_list[-1].args[0][:4],
                         ["docker", "rm", "--force", "--volumes"])

    @mock.patch("kernel_runtime.subprocess.run")
    def test_nonzero_container_removal_is_not_silenced(self, run):
        run.return_value = subprocess.CompletedProcess(
            [], 1, stdout="", stderr="daemon unavailable")
        with self.assertRaisesRegex(RuntimeError, "daemon unavailable"):
            ContainerRuntime._remove_container("plinth-runtime-test-owned")

    def test_container_paths_must_remain_under_owned_root(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.assertEqual(runtime.path(root, root / "data"),
                             "/var/lib/plinth/data")
            self.assertEqual(runtime.path(root, root / "data" / "staging"),
                             "/var/lib/plinth/data/staging")
            with self.assertRaises(ValueError):
                runtime.path(root, root.parent / "outside")


if __name__ == "__main__":
    unittest.main()
