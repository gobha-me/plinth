"""Unit tests for native/container production-test launch ownership."""

from argparse import Namespace
import importlib.util
import io
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

from kernel_runtime import ContainerProcess, ContainerRuntime, NativeRuntime, runtime_from_args


class KernelRuntimeTest(unittest.TestCase):
    def test_image_test_build_stages_sdk_fixture_without_shipping_it(self):
        runtime = object.__new__(ContainerRuntime)
        repo = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = runtime.test_build_dir(repo, root)
            self.assertEqual(build, root / "image-browser-build")
            expected = {
                "valid-install.zip": "notes",
                "upgrade-v2.zip": "notes",
                "upgrade-v2-broken-migration.zip": "notes",
                "upgrade-v1-slow.zip": "slow",
                "upgrade-v2-slow.zip": "slow",
                "extensions/sdk-demo.zip": "sdkdemo",
            }
            import json
            for relative, name in expected.items():
                with zipfile.ZipFile(build / "fixtures" / relative) as archive:
                    manifest = json.loads(archive.read("manifest.json"))
                    self.assertEqual(manifest["name"], name)
                    self.assertFalse(any(path.startswith("/") or ".." in Path(path).parts
                                         for path in archive.namelist()))
                    if name == "sdkdemo":
                        self.assertIn("client/panels/demo.js", archive.namelist())
                        self.assertEqual(archive.read("client/panels/demo.js"),
                                         (repo / "tests/extensions/sdk-demo/client/panels/demo.js").read_bytes())
            with zipfile.ZipFile(build / "packages/admin-0.1.1.zip") as archive:
                manifest = json.loads(archive.read("manifest.json"))
                self.assertEqual((manifest["name"], manifest["version"]), ("admin", "0.1.1"))
                self.assertIn("client/panels/packages.js", archive.namelist())
                for name in ("client/panels/groups.js", "client/groups/api.js", "client/groups/controller.js"):
                    self.assertEqual(archive.read(name), (repo / "client/admin" / name).read_bytes())
            self.assertFalse((root / "bundled").exists())

    def test_runtime_selection_preserves_native_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "plinth"
            binary.touch()
            runtime = runtime_from_args(Namespace(binary=binary, image=None))
        self.assertIsInstance(runtime, NativeRuntime)
        self.assertEqual(runtime.binary, binary)

    @mock.patch("kernel_runtime.subprocess.run")
    def test_container_launch_stages_without_daemon_host_paths(self, run):
        staged = []

        def execute(command, **kwargs):
            if command[:2] == ["docker", "cp"]:
                with tarfile.open(fileobj=io.BytesIO(kwargs["stdin"].read())) as archive:
                    staged.extend(archive.getmembers())
            return subprocess.CompletedProcess([], 0)

        run.side_effect = execute
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
        self.assertEqual(len(copies), 1)
        self.assertEqual(copies[0][:3], ["docker", "cp", "-"])
        self.assertNotIn("--archive", copies[0])
        self.assertEqual({member.name for member in staged},
                         {"config.json", "data", "logs"})
        self.assertTrue(all((member.uid, member.gid) == (10001, 10001)
                            for member in staged))
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

    def test_archive_preserves_modes_link_text_and_excludes_private_paths(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runtime.prepare_root(root)
            (root / "data").chmod(0o750)
            source = root / "data/source"
            source.write_bytes(b"owned\0bytes")
            source.chmod(0o640)
            outside = root / "outside"
            outside.write_text("owned elsewhere")
            outside.chmod(0o600)
            (root / "data/escape").symlink_to(outside)
            (root / "data/active").symlink_to("source")
            (root / "browser-profile").mkdir()
            (root / "browser-profile/cookies.sqlite").write_text("not server state")
            output = io.BytesIO()
            runtime._write_stage_archive(root, output)
            self.assertNotIn(b"owned elsewhere", output.getvalue())
            with tarfile.open(fileobj=io.BytesIO(output.getvalue())) as archive:
                members = {member.name: member for member in archive.getmembers()}
                self.assertEqual(set(members), {"data", "data/source", "data/escape",
                                               "data/active", "logs"})
                self.assertEqual(members["data"].mode, 0o750)
                self.assertEqual(members["data/source"].mode, 0o640)
                self.assertEqual(archive.extractfile(members["data/source"]).read(), b"owned\0bytes")
                self.assertEqual(members["data/escape"].linkname, str(outside))
                self.assertEqual(members["data/active"].linkname, "source")
                self.assertTrue(all((member.uid, member.gid) == (10001, 10001)
                                    for member in members.values()))
                self.assertTrue(all(member.uname == member.gname == "plinth"
                                    for member in members.values()))
            self.assertEqual(outside.stat().st_mode & 0o777, 0o600)
            self.assertTrue((root / "data/escape").is_symlink())
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o640)
            self.assertEqual(stat.S_IMODE(root.stat().st_mode), 0o700)

    def test_retained_collection_and_restage_preserve_real_artifact_digest(self):
        # Real filesystem/codec regression; mocked Docker transport is NOT an
        # OCI roundtrip proof. Parent runs the actual image acceptance journey.
        spec = importlib.util.spec_from_file_location(
            "installed_float_digest", Path(__file__).with_name("run-installed-floats.py"))
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root, container = Path(temporary) / "host", Path(temporary) / "container"
            root.mkdir()
            runtime.prepare_root(root)
            generation = root / "data/extensions/shell/0.6.8"
            server = generation / "server"
            server.mkdir(parents=True)
            server.chmod(0o750)
            handler = server / "main.js"
            handler.write_bytes(b"export const retained = true;\n")
            handler.chmod(0o640)
            (generation.parent / "active").symlink_to("0.6.8")
            before = harness.artifact_digest(generation)
            # Simulate Docker's client-owned copy-back, preserving permissions
            # and link text. No ownership or mode normalization is performed.
            shutil.copytree(root / "data", container / "data", symlinks=True)
            shutil.copytree(root / "logs", container / "logs", symlinks=True)

            def collect(command, **kwargs):
                self.assertEqual(command[:2], ["docker", "cp"])
                self.assertNotIn("--archive", command)
                relative = command[2].rsplit("/", 1)[1]
                shutil.copytree(container / relative, root / relative, symlinks=True)
                self.assertEqual(kwargs["timeout"], 30)
                return subprocess.CompletedProcess(command, 0)

            with mock.patch("kernel_runtime.subprocess.run", side_effect=collect):
                runtime._collect("owned", root)
            self.assertEqual(harness.artifact_digest(generation), before)
            self.assertEqual(handler.stat().st_uid, os.getuid())
            self.assertEqual(stat.S_IMODE(handler.stat().st_mode), 0o640)
            self.assertEqual(stat.S_IMODE(server.stat().st_mode), 0o750)
            self.assertEqual((generation.parent / "active").readlink(), Path("0.6.8"))

            def stage(command, **kwargs):
                self.assertEqual(command, ["docker", "cp", "-", "owned:/var/lib/plinth/"])
                self.assertNotIn("--archive", command)
                self.assertEqual(kwargs["timeout"], 30)
                with tarfile.open(fileobj=kwargs["stdin"]) as archive:
                    member = archive.getmember("data/extensions/shell/0.6.8/server/main.js")
                    self.assertEqual((member.mode, member.uid, member.gid), (0o640, 10001, 10001))
                    self.assertEqual(archive.extractfile(member).read(), handler.read_bytes())
                    self.assertEqual(archive.getmember("data/extensions/shell/active").linkname, "0.6.8")
                    restaged = Path(temporary) / "restaged"
                    members = archive.getmembers()
                    for item in members:
                        self.assertFalse(Path(item.name).is_absolute())
                        self.assertNotIn("..", Path(item.name).parts)
                        self.assertEqual((item.uid, item.gid), (10001, 10001))
                        destination = restaged / item.name
                        if item.isdir():
                            destination.mkdir(parents=True, exist_ok=True)
                        elif item.issym():
                            self.assertEqual(item.linkname, "0.6.8")
                            destination.symlink_to(item.linkname)
                        else:
                            self.assertTrue(item.isfile())
                            destination.write_bytes(archive.extractfile(item).read())
                    for item in reversed(members):
                        if not item.issym():
                            (restaged / item.name).chmod(item.mode)
                    self.assertEqual(harness.artifact_digest(
                        restaged / "data/extensions/shell/0.6.8"), before)
                return subprocess.CompletedProcess(command, 0)

            with mock.patch("kernel_runtime.subprocess.run", side_effect=stage):
                runtime._stage_container("owned", root)
            self.assertEqual(harness.artifact_digest(generation), before)

    def test_staging_refuses_oversized_stat_before_opening_regular_file(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data").mkdir()
            with (root / "data/large").open("wb") as source:
                source.truncate(256 * 1024 * 1024 + 1)
            original = os.open

            def open_owned(path, flags, **kwargs):
                self.assertNotEqual(path, "large", "oversized regular file must not be opened")
                return original(path, flags, **kwargs)

            with mock.patch("kernel_runtime.os.open", side_effect=open_owned):
                with self.assertRaisesRegex(RuntimeError, "content exceeds transport bound"):
                    runtime._write_stage_archive(root, io.BytesIO())

    def test_staging_archive_output_and_inventory_bounds_fail_closed(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data").mkdir()
            (root / "data/file").write_bytes(b"x" * 1024)
            with mock.patch("kernel_runtime.STAGE_ARCHIVE_MAX_BYTES", 1024):
                output = io.BytesIO()
                with self.assertRaisesRegex(RuntimeError, "transport bound"):
                    runtime._write_stage_archive(root, output)
                self.assertLessEqual(len(output.getvalue()), 1024)
            with mock.patch("kernel_runtime.STAGE_MAX_ENTRIES", 1):
                with self.assertRaisesRegex(RuntimeError, "inventory exceeds transport bound"):
                    runtime._write_stage_archive(root, io.BytesIO())
            with mock.patch("kernel_runtime.STAGE_MAX_DEPTH", 1):
                with self.assertRaisesRegex(RuntimeError, "inventory exceeds transport bound"):
                    runtime._write_stage_archive(root, io.BytesIO())
            with mock.patch("kernel_runtime.STAGE_MAX_PATH_BYTES", 3):
                with self.assertRaisesRegex(RuntimeError, "inventory exceeds transport bound"):
                    runtime._write_stage_archive(root, io.BytesIO())
            with mock.patch("kernel_runtime.STAGE_ARCHIVE_TIMEOUT_SECONDS", 0):
                with self.assertRaisesRegex(RuntimeError, "deadline expired"):
                    runtime._write_stage_archive(root, io.BytesIO())

    def test_staging_checks_regular_identity_after_read(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data").mkdir()
            source = root / "data/file"
            source.write_bytes(b"before")
            original = tarfile.TarFile.addfile

            def changed(archive, member, fileobj=None):
                original(archive, member, fileobj)
                if member.name == "data/file":
                    source.write_bytes(b"different size")

            with mock.patch.object(tarfile.TarFile, "addfile", changed):
                with self.assertRaisesRegex(RuntimeError, "file changed during capture"):
                    runtime._write_stage_archive(root, io.BytesIO())

    def test_staging_rejects_special_file_without_reading_it(self):
        runtime = object.__new__(ContainerRuntime)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "data").mkdir()
            os.mkfifo(root / "data/pipe")
            with self.assertRaisesRegex(RuntimeError, "unsupported file type"):
                runtime._write_stage_archive(root, io.BytesIO())

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
