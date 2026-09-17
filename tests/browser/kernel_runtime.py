"""Native and exact-container launch adapters for production browser tests."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import threading
import uuid


CONTAINER_ROOT = Path("/var/lib/plinth")
CONTAINER_MIGRATIONS = Path("/usr/local/share/plinth/migrations")
SERVER_ENV_KEYS = (
    "PLINTH_PG_HOST",
    "PLINTH_PG_PORT",
    "PLINTH_PG_USER",
    "PLINTH_PG_PASSWORD",
    "PLINTH_PG_DATABASE",
    "PLINTH_PG_POOL_SIZE",
    "PLINTH_DEV_MODE",
)
SERVER_ROOT_ENTRIES = ("config.json", "bundled", "data", "logs")


def add_runtime_arguments(parser):
    runtime = parser.add_mutually_exclusive_group(required=True)
    runtime.add_argument("--binary", type=Path,
                         help="native Plinth binary (existing behavior)")
    runtime.add_argument("--image",
                         help="exact local OCI image reference to run as PID 1")


def runtime_from_args(args):
    if args.binary is not None:
        return NativeRuntime(args.binary.resolve(strict=True))
    return ContainerRuntime(args.image)


class NativeRuntime:
    is_container = False

    def __init__(self, binary):
        self.binary = Path(binary)

    def prepare_root(self, _root):
        pass

    def cleanup_root(self, _root):
        pass

    def refresh_root(self, _root, _child):
        pass

    def path(self, _root, host_path):
        return str(host_path)

    def migrations_dir(self, repo):
        return str(repo / "migrations")

    def default_bundle_path(self):
        return self.binary.parent / "share/plinth/bundled"

    def test_build_dir(self, _repo, _root):
        return self.binary.parent

    def start(self, arguments, *, root, env, output):
        return subprocess.Popen(
            [str(self.binary), *arguments], cwd=root, env=env,
            stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)

    def run(self, arguments, *, root, env, **kwargs):
        return subprocess.run(
            [str(self.binary), *arguments], cwd=root, env=env, **kwargs)


class ContainerProcess:
    """Popen-compatible owner that signals the container's actual PID 1."""

    def __init__(self, child, name, cleanup):
        self.child = child
        self.name = name
        self.cleanup = cleanup
        self.cleaned = False
        self._cleanup_lock = threading.Lock()

    @property
    def returncode(self):
        result = self.child.returncode
        if result is not None:
            self._cleanup_once()
        return result

    def poll(self):
        result = self.child.poll()
        if result is not None:
            self._cleanup_once()
        return result

    def wait(self, timeout=None):
        result = self.child.wait(timeout=timeout)
        self._cleanup_once()
        return result

    def _cleanup_once(self):
        with self._cleanup_lock:
            if not self.cleaned:
                try:
                    self.cleanup()
                except ContainerCollectionError:
                    # State copy-back failed, but the owned container and its
                    # volumes are gone. Surface that failure once without
                    # retrying against an already removed resource.
                    self.cleaned = True
                    raise
                else:
                    self.cleaned = True

    def _signal(self, requested, *, check):
        signame = signal.Signals(requested).name
        result = subprocess.run(
            ["docker", "kill", f"--signal={signame}", self.name],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if check and result.returncode != 0 and self.child.poll() is None:
            raise RuntimeError(f"could not deliver {signame} to container {self.name}")

    def send_signal(self, requested):
        if self.poll() is None:
            self._signal(requested, check=True)

    def kill(self):
        # The Docker client can exit while an attached container remains. Try
        # the exact task-owned name even when the client already terminated.
        self._signal(signal.SIGKILL, check=False)
        if self.child.poll() is None:
            self.child.kill()

    def assert_plinth_is_pid1(self):
        subprocess.run(
            ["docker", "exec", self.name, "/bin/sh", "-euc",
             'test "$(readlink /proc/1/exe)" = /usr/local/bin/plinth'],
            check=True, timeout=10, stdout=subprocess.DEVNULL)


class ContainerCollectionError(RuntimeError):
    """State collection failed after the owned container was removed."""


class ContainerRuntime:
    is_container = True

    def __init__(self, image):
        if (not image or image.startswith("-") or "\0" in image or
                any(character.isspace() for character in image)):
            raise ValueError("--image must be one non-empty OCI image reference")
        self.image = image
        subprocess.run(
            ["docker", "image", "inspect", image], check=True, timeout=20,
            stdout=subprocess.DEVNULL)

    def prepare_root(self, root):
        # TemporaryDirectory defaults to 0700. The production image deliberately
        # runs as uid 10001. Docker copies client-side staging into the stopped
        # container as root, so make only this task-owned tree world-writable.
        root.chmod(0o777)
        for relative in ("data", "logs"):
            path = root / relative
            path.mkdir()
            path.chmod(0o777)

    def cleanup_root(self, _root):
        # Each invocation owns and removes its exact container plus the image's
        # anonymous data/log volumes. No runtime-level resource remains.
        pass

    def refresh_root(self, root, child):
        self._collect(child.name, root)

    def path(self, root, host_path):
        relative = Path(host_path).resolve().relative_to(Path(root).resolve())
        return str(CONTAINER_ROOT / relative)

    def migrations_dir(self, _repo):
        return str(CONTAINER_MIGRATIONS)

    def default_bundle_path(self):
        return None

    def test_build_dir(self, repo, root):
        fixture_root = root / "image-browser-build" / "fixtures"
        fixture_root.mkdir(parents=True)
        fixtures = (
            (repo / "tests/fixtures/install_lifecycle/valid-install",
             fixture_root / "valid-install.zip"),
            (repo / "tests/fixtures/lifecycle_transitions/upgrade-v2",
             fixture_root / "upgrade-v2.zip"),
        )
        import zipfile
        for source, archive in fixtures:
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
                for path in sorted(source.rglob("*")):
                    if path.is_file():
                        package.write(path, path.relative_to(source))
        return fixture_root.parent

    @staticmethod
    def _name():
        return "plinth-runtime-test-" + uuid.uuid4().hex

    @staticmethod
    def _make_tree_writable(root):
        root = Path(root)
        for path in (Path(root), *Path(root).rglob("*")):
            try:
                current = path.lstat()
                if stat.S_ISLNK(current.st_mode):
                    continue
                permissions = 0o777 if stat.S_ISDIR(current.st_mode) else 0o666
                os.chmod(path, current.st_mode | permissions,
                         follow_symlinks=False)
            except FileNotFoundError:
                # Browser cleanup can race removal of its own temporary files.
                continue
            except NotImplementedError:
                # A path was replaced with a symlink after lstat(). Refuse to
                # follow it rather than changing anything outside the owned tree.
                continue

    def _stage_container(self, name, root):
        root = Path(root).resolve()
        for relative in SERVER_ROOT_ENTRIES:
            source = root / relative
            if source.is_symlink():
                raise RuntimeError(
                    f"refusing to stage symbolic-link server path: {source}")
            if not source.exists():
                continue
            self._make_tree_writable(source)
            subprocess.run(
                ["docker", "cp", str(source),
                 name + ":" + str(CONTAINER_ROOT) + "/"],
                check=True, timeout=30, stdout=subprocess.DEVNULL)

    @staticmethod
    def _remove_owned_path(path):
        path = Path(path)
        try:
            mode = path.lstat().st_mode
        except FileNotFoundError:
            return
        if stat.S_ISDIR(mode) and not stat.S_ISLNK(mode):
            shutil.rmtree(path)
        else:
            path.unlink()

    def _collect(self, name, root):
        # A stopped container retains the image-declared anonymous data/log
        # volumes. The same API also supports a read-only snapshot while the
        # server is still running for assertions on its persisted filesystem.
        for relative in ("data", "logs"):
            destination = Path(root) / relative
            self._remove_owned_path(destination)
            subprocess.run(
                ["docker", "cp",
                 name + ":" + str(CONTAINER_ROOT / relative),
                 str(Path(root).resolve()) + "/"],
                check=True, timeout=30, stdout=subprocess.DEVNULL)
            self._make_tree_writable(destination)

    def _collect_and_remove(self, name, root):
        # Delete only this exact task-owned container and its anonymous volumes
        # after copying the persisted state back to the client workspace.
        collection_error = None
        try:
            self._collect(name, root)
        except BaseException as error:
            collection_error = error
        try:
            self._remove_container(name)
        except BaseException as error:
            if collection_error is not None:
                collection_error.add_note(f"container removal failed: {error}")
                raise collection_error
            raise
        if collection_error is not None:
            raise ContainerCollectionError(
                f"container {name} was removed after state collection failed"
            ) from collection_error

    @staticmethod
    def _remove_container(name):
        result = subprocess.run(
            ["docker", "rm", "--force", "--volumes", name],
            timeout=20, text=True, capture_output=True)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise RuntimeError(
                f"could not remove task-owned container {name}: {detail}")

    def _create(self, name, arguments, env):
        command = ["docker", "create", "--name", name, "--network", "host"]
        for key in SERVER_ENV_KEYS:
            if key in env:
                command.extend(("--env", key))
        command.extend((self.image, *arguments))
        subprocess.run(command, env=env, check=True, timeout=30,
                       stdout=subprocess.DEVNULL)

    def start(self, arguments, *, root, env, output):
        name = self._name()
        try:
            self._create(name, arguments, env)
            self._stage_container(name, root)
            child = subprocess.Popen(
                ["docker", "start", "--attach", name], cwd=root, env=env,
                stdin=subprocess.DEVNULL, stdout=output,
                stderr=subprocess.STDOUT)
        except BaseException as error:
            try:
                self._remove_container(name)
            except BaseException as cleanup_error:
                error.add_note(f"container cleanup failed: {cleanup_error}")
            raise
        return ContainerProcess(
            child, name, lambda: self._collect_and_remove(name, root))

    def run(self, arguments, *, root, env, **kwargs):
        name = self._name()
        created = False
        try:
            self._create(name, arguments, env)
            created = True
            self._stage_container(name, root)
            return subprocess.run(
                ["docker", "start", "--attach", name], cwd=root, env=env,
                **kwargs)
        finally:
            active_error = sys.exception()
            try:
                if created:
                    self._collect_and_remove(name, root)
                else:
                    # docker create can time out after the daemon has committed
                    # the object. The UUID name is task-owned, so always remove.
                    self._remove_container(name)
            except BaseException as cleanup_error:
                if active_error is None:
                    raise
                active_error.add_note(f"container cleanup failed: {cleanup_error}")
