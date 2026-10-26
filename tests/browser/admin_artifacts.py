"""Build the fixed historical admin source fixture; never fetch or publish it."""

from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
import zipfile


class BaselineError(RuntimeError):
    """The exact historical source cannot be established locally."""


@dataclass(frozen=True)
class _SourceFile:
    path: str
    mode: str
    blob: str
    size: int
    sha256: str


@dataclass(frozen=True)
class _TreeSpec:
    commit: str
    tree: str
    files: tuple[_SourceFile, ...]
    package_name: str = "admin"
    package_version: str = "0.1.0"


# Public M198 source, not a published package or an existing deployment receipt.
# Production callers cannot replace this pin through arguments or environment.
_M198 = _TreeSpec(
    "3301b7ee01c2098847d47f3e51d5ff14d28442f3",
    "2399eadb48c5d1bc6f41f4bbc77b425ec0d9dc26",
    (
        _SourceFile("client/admin/README.md", "100644", "d7caf526e75e2d449e091c4054b4649c04681fc1", 854,
                    "dde8835bdfc989e76c47bcb7c3751fdd62b6ee32a78e2b0cee65d76f5980d5ee"),
        _SourceFile("client/admin/capabilities.json", "100644", "433ae63482ba3728a719809cc7a4b7afd65a7a18", 39,
                    "051cbb966141ed6f1e76da19dca4b5a46fd9399ecb5eaa266e149ad04ca13cae"),
        _SourceFile("client/admin/client/packages/api.js", "100644", "75f2f6d2bdcb8dbdae58fad9fa05dfcd8ebebbe4", 7012,
                    "809e189dcc1df92b8ea873db241be51d55db92de5e5a0cfc7843770936ab9242"),
        _SourceFile("client/admin/client/packages/controller.js", "100644", "150ba00a75c622d138f2bd91ee7d3919a47f1433", 16015,
                    "59fc07c37a1d964d12f8b0a7801ee184b32f0bb534cbe1431712016c79ba128d"),
        _SourceFile("client/admin/client/panels/packages.js", "100644", "74c2156dd353fb9db28b27b3a5bc542a4160634a", 11387,
                    "8dd9b670591527f453ca8c8ce8414464680d85f5a5ae6630acd0b643e7ac234a"),
        _SourceFile("client/admin/manifest.json", "100644", "4b367af146fdade7549e771873386132bec9e5d5", 243,
                    "270dd38d9dd25ac2452931b7c522b8f0c5c1984fb60a80d129d1ba053a0104b9"),
        _SourceFile("client/admin/panels.json", "100644", "4fa839363e2abbf36498fab045790795382d22a6", 188,
                    "2b68fbde0925e3cc25f7b0430e544cd1a3bf27740840f440843e7bbf75ea09cd"),
        _SourceFile("client/admin/rbac.json", "100644", "f38180da4160bef2b50d4d9535b140f24ae10bee", 205,
                    "740c826e1abe726135060f2e73b1a0e94fe57992dcf14b2d2a73df7d5c3afe3c"),
        _SourceFile("client/admin/server/main.js", "100644", "04a5b38e1cf8ba68fdc4d4c18653ece44149e498", 108,
                    "4eb15bcfd0d5c8a59b512a24051c0ce44f133a099ae2aafbce070375c405e534"),
    ),
)


@dataclass(frozen=True)
class AdminBaseline:
    archive: Path
    archive_sha256: str
    source_commit: str
    source_tree: str
    package_name: str
    package_version: str
    source_files: tuple[_SourceFile, ...]

    def receipt(self):
        genuine = (self.source_commit, self.source_tree, self.source_files,
                   self.package_name, self.package_version) == (
                       _M198.commit, _M198.tree, _M198.files, _M198.package_name, _M198.package_version)
        return {
            "source_commit": self.source_commit, "source_tree": self.source_tree,
            "package_name": self.package_name, "package_version": self.package_version,
            "archive_sha256": self.archive_sha256,
            "source_files": [asdict(entry) for entry in self.source_files],
            "scope": ("fixed M198 source-built admin fixture; not published-artifact proof" if genuine else
                      "synthetic private-core fixture; not M198 or native upgrade proof"),
        }


def _git(repository, *arguments, absent_ok=False, maximum=65536):
    # Object reads must not lazy-fetch from a partial clone. Reject promisor
    # configuration below as well, including on Git versions without this env.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment.update(GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0", GIT_OPTIONAL_LOCKS="0")
    command = ["git", "--no-replace-objects", "-c", "protocol.allow=never",
               "-c", f"safe.directory={repository}", "-C", str(repository), *arguments]
    try:
        result = subprocess.run(command, env=environment, timeout=15, capture_output=True)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise BaselineError("historical source Git read unavailable or timed out") from error
    if result.returncode != 0 and not (absent_ok and result.returncode == 1):
        # Do not forward repository/config-shaped diagnostics to public logs.
        raise BaselineError(f"historical source Git read failed (exit {result.returncode})")
    if len(result.stdout) > maximum:
        raise BaselineError("historical source Git output exceeded its expected bound")
    return result.stdout


def _pack(source, output):
    packer = Path(__file__).resolve().parents[2] / "tools/pack_admin.py"
    result = subprocess.run([sys.executable, str(packer), "--source", str(source),
                             "--output", str(output)], timeout=30, capture_output=True)
    if result.returncode != 0:
        raise BaselineError(f"historical admin packer failed (exit {result.returncode})")


def _prepare_baseline(repository, output_directory, specification):
    """Private core: synthetic immutable specifications are for isolated units."""
    repository = Path(repository).resolve(strict=True)
    requested = Path(output_directory)
    if requested.exists() or requested.is_symlink():
        raise BaselineError("historical artifact output must be a new directory")
    output = requested.parent.resolve(strict=True) / requested.name
    if output.is_relative_to(repository):
        raise BaselineError("historical artifact output must be outside its source repository")
    if _git(repository, "config", "--get-regexp",
            r"^(extensions\.partialclone|remote\..*\.promisor)$", absent_ok=True):
        raise BaselineError("historical source requires complete local Git objects, not a partial clone")
    if _git(repository, "rev-parse", "--verify", specification.commit + "^{commit}").strip().decode() != specification.commit:
        raise BaselineError("historical source commit differs from its pin")
    if _git(repository, "rev-parse", "--verify", specification.commit + "^{tree}").strip().decode() != specification.tree:
        raise BaselineError("historical source tree differs from its pin")
    expected = {}
    for entry in specification.files:
        path = PurePosixPath(entry.path)
        if (path.parts[:2] != ("client", "admin") or len(path.parts) < 3 or
                any(part in ("", ".", "..") for part in path.parts) or
                entry.path != path.as_posix() or entry.mode != "100644" or entry.path in expected):
            raise BaselineError("historical source files must be unique regular100644 admin paths")
        expected[entry.path] = entry
    raw = _git(repository, "ls-tree", "-rz", "--full-tree", specification.commit, "--", "client/admin")
    actual = {}
    for item in raw.split(b"\0"):
        if not item:
            continue
        metadata, name = item.split(b"\t", 1)
        mode, kind, blob = metadata.decode("ascii").split()
        path = name.decode("utf-8")
        if path in actual or kind != "blob":
            raise BaselineError("historical source tree has an unexpected entry")
        actual[path] = (mode, blob)
    if actual != {path: (entry.mode, entry.blob) for path, entry in expected.items()}:
        raise BaselineError("historical source file inventory differs from its pin")
    contents = {}
    for path, entry in expected.items():
        size = _git(repository, "cat-file", "-s", entry.blob, maximum=32).strip()
        if size != str(entry.size).encode("ascii"):
            raise BaselineError("historical source blob size differs from its pin")
        data = _git(repository, "cat-file", "blob", entry.blob, maximum=entry.size)
        if len(data) != entry.size or hashlib.sha256(data).hexdigest() != entry.sha256:
            raise BaselineError("historical source blob bytes differ from their pin")
        contents[PurePosixPath(path).relative_to("client/admin").as_posix()] = data
    manifest = json.loads(contents["manifest.json"])
    if (manifest.get("name"), manifest.get("version")) != (specification.package_name, specification.package_version):
        raise BaselineError("historical package identity differs from its pin")
    output.mkdir(mode=0o700)
    try:
        archive = output / f"{specification.package_name}-{specification.package_version}.zip"
        with tempfile.TemporaryDirectory(prefix=".admin-source-", dir=output) as temporary:
            source = Path(temporary)
            for relative, data in contents.items():
                path = source / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
                path.chmod(0o644)
            _pack(source, archive)
            with zipfile.ZipFile(archive) as packed:
                names = sorted(contents, key=lambda name: name.encode("utf-8"))
                if packed.namelist() != names:
                    raise BaselineError("historical archive entries differ from verified source")
                for name in names:
                    info = packed.getinfo(name)
                    if ((info.external_attr >> 16) != 0o100644 or info.compress_type != zipfile.ZIP_STORED or
                            packed.read(name) != contents[name]):
                        raise BaselineError("historical archive bytes or file modes differ from verified source")
        return AdminBaseline(archive, hashlib.sha256(archive.read_bytes()).hexdigest(),
                             specification.commit, specification.tree, specification.package_name,
                             specification.package_version, specification.files)
    except BaseException as error:
        try:
            shutil.rmtree(output)
        except BaseException as cleanup_error:
            error.add_note(f"owned historical output cleanup failed: {type(cleanup_error).__name__}")
        raise


def prepare_admin_baseline(repository, output_directory):
    """Return fixed M198admin0.1.0 in a new caller-owned output directory.

    Temporary source staging is joined/removed before return. The successful
    archive remains owned by the caller's outer temporary root until the
    browser has joined; failure removes only this newly created output.
    This entry is never called on import or by the synthetic core unit tests.
    """
    return _prepare_baseline(repository, output_directory, _M198)
