"""Native and exact-container launch adapters for production browser tests."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
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
    "PLINTH_BOOTSTRAP_TOKEN",
)
SERVER_ROOT_ENTRIES = ("config.json", "bundled", "data", "logs")
# Finite test-transport bounds, not package-installation eligibility limits.
STAGE_ARCHIVE_MAX_BYTES = 256 * 1024 * 1024
STAGE_MAX_ENTRIES = 32768
STAGE_MAX_PATH_BYTES = 4096
STAGE_MAX_DEPTH = 128
STAGE_ARCHIVE_TIMEOUT_SECONDS = 30
IMAGE_UID = IMAGE_GID = 10001


class _BoundedTarWriter:
    def __init__(self, output):
        self.output = output
        self.deadline = time.monotonic() + STAGE_ARCHIVE_TIMEOUT_SECONDS

    def check_deadline(self):
        # Cooperative checks do not promise interruption of a blocking syscall.
        if time.monotonic() >= self.deadline:
            raise RuntimeError("container staging archive deadline expired")

    def tell(self):
        return self.output.tell()

    def write(self, data):
        self.check_deadline()
        if len(data) > STAGE_ARCHIVE_MAX_BYTES - self.tell():
            raise RuntimeError("container staging archive exceeds transport bound")
        return self.output.write(data)

    def flush(self):
        self.output.flush()


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
        # No host bind mount is used. Staging assigns the image's numeric owner
        # without changing permission bits that belong to artifact receipts.
        for relative in ("data", "logs"):
            path = root / relative
            path.mkdir()

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
            (repo / "tests/fixtures/lifecycle_transitions/upgrade-v2-broken-migration",
             fixture_root / "upgrade-v2-broken-migration.zip"),
            (repo / "tests/fixtures/lifecycle_transitions/upgrade-v1-slow",
             fixture_root / "upgrade-v1-slow.zip"),
            (repo / "tests/fixtures/lifecycle_transitions/upgrade-v2-slow",
             fixture_root / "upgrade-v2-slow.zip"),
            (repo / "tests/extensions/sdk-demo",
             fixture_root / "extensions/sdk-demo.zip"),
        )
        import zipfile
        for source, archive in fixtures:
            archive.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
                for path in sorted(source.rglob("*")):
                    if path.is_file():
                        package.write(path, path.relative_to(source))
        admin_archive = fixture_root.parent / "packages/admin-0.1.1.zip"
        subprocess.run(
            [sys.executable, str(repo / "tools/pack_admin.py"),
             "--source", str(repo / "client/admin"), "--output", str(admin_archive)],
            check=True, timeout=30, capture_output=True)
        return fixture_root.parent

    @staticmethod
    def _name():
        return "plinth-runtime-test-" + uuid.uuid4().hex

    @staticmethod
    def _write_stage_archive(root, output):
        count = 0
        writer = _BoundedTarWriter(output)

        def identity(info):
            return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
                    info.st_mtime_ns, info.st_ctime_ns)

        def add(archive, parent, name, relative, depth, *, top=False):
            nonlocal count
            writer.check_deadline()
            if (count >= STAGE_MAX_ENTRIES or depth > STAGE_MAX_DEPTH or
                    len(os.fsencode(relative)) > STAGE_MAX_PATH_BYTES or
                    relative.startswith("/") or
                    any(part in ("", ".", "..") for part in relative.split("/"))):
                raise RuntimeError("container staging inventory exceeds transport bound")
            info = os.stat(name, dir_fd=parent, follow_symlinks=False)
            if top and stat.S_ISLNK(info.st_mode):
                raise RuntimeError("refusing to stage symbolic-link server path")
            member = tarfile.TarInfo(relative)
            member.mode = stat.S_IMODE(info.st_mode)
            member.uid = IMAGE_UID
            member.gid = IMAGE_GID
            member.uname = member.gname = "plinth"
            member.mtime = int(info.st_mtime)
            count += 1
            if stat.S_ISLNK(info.st_mode):
                target = os.readlink(name, dir_fd=parent)
                if len(os.fsencode(target)) > STAGE_MAX_PATH_BYTES:
                    raise RuntimeError("container staging link exceeds transport bound")
                member.type = tarfile.SYMTYPE
                member.linkname = target
                archive.addfile(member)
                if identity(os.stat(name, dir_fd=parent, follow_symlinks=False)) != identity(info):
                    raise RuntimeError("container staging link changed during capture")
            elif stat.S_ISDIR(info.st_mode):
                descriptor = os.open(name, os.O_RDONLY | os.O_DIRECTORY |
                                     os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=parent)
                try:
                    if identity(os.fstat(descriptor)) != identity(info):
                        raise RuntimeError("container staging directory changed before capture")
                    member.type = tarfile.DIRTYPE
                    archive.addfile(member)
                    with os.scandir(descriptor) as entries:
                        for entry in entries:
                            add(archive, descriptor, entry.name,
                                relative + "/" + entry.name, depth + 1)
                    if identity(os.fstat(descriptor)) != identity(info):
                        raise RuntimeError("container staging directory changed during capture")
                finally:
                    os.close(descriptor)
            elif stat.S_ISREG(info.st_mode):
                if (info.st_size < 0 or
                        info.st_size > STAGE_ARCHIVE_MAX_BYTES - archive.fileobj.tell()):
                    raise RuntimeError("container staging content exceeds transport bound")
                descriptor = os.open(name, os.O_RDONLY | os.O_CLOEXEC |
                                     os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=parent)
                with os.fdopen(descriptor, "rb") as source:
                    if identity(os.fstat(source.fileno())) != identity(info):
                        raise RuntimeError("container staging file changed before capture")
                    member.type = tarfile.REGTYPE
                    member.size = info.st_size
                    archive.addfile(member, source)
                    if identity(os.fstat(source.fileno())) != identity(info):
                        raise RuntimeError("container staging file changed during capture")
            else:
                raise RuntimeError("container staging contains unsupported file type")

        descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY |
                             os.O_CLOEXEC | os.O_NOFOLLOW)
        try:
            with tarfile.open(fileobj=writer, mode="w",
                              format=tarfile.GNU_FORMAT) as archive:
                for relative in SERVER_ROOT_ENTRIES:
                    try:
                        os.stat(relative, dir_fd=descriptor, follow_symlinks=False)
                    except FileNotFoundError:
                        continue
                    add(archive, descriptor, relative, relative, 1, top=True)
        finally:
            os.close(descriptor)

    def _stage_container(self, name, root):
        # The stopped container receives original modes but its real image UID.
        # stdin is a finite owned disk file, not a materialized in-memory tree.
        # Do not pass --archive: stdin already carries numeric ownership. Engine
        # 27's CopyUIDGID path instead looks up Config.User as one account name
        # (including its :group suffix), rather than retaining these TAR IDs.
        with tempfile.TemporaryFile(mode="w+b") as archive:
            self._write_stage_archive(root, archive)
            archive.seek(0)
            subprocess.run(
                ["docker", "cp", "-",
                 name + ":" + str(CONTAINER_ROOT) + "/"],
                stdin=archive, check=True, timeout=30, stdout=subprocess.DEVNULL)

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
