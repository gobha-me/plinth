#!/usr/bin/env python3
"""Exercise the real root/service policy boundary; no same-owner substitute."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess
import tempfile


@contextmanager
def executable_fixture(binary: Path):
    # Policy data needs protected ancestors; executable scratch does not. In
    # particular, /run may permit policy reads while denying executable launch.
    # TMPDIR lets the caller select a writable, executable filesystem without
    # changing the protected-policy path or relaxing the source/build tree.
    with tempfile.TemporaryDirectory(prefix="plinth-policy-exec-") as temporary:
        root = Path(temporary)
        executable = root / "policy-tests"
        shutil.copyfile(binary, executable)
        executable.chmod(0o755)
        verifier = root / "verify.py"
        shutil.copyfile(Path(__file__).resolve().with_name("verify_catch_case.py"),
                        verifier)
        verifier.chmod(0o644)
        if not os.access(executable, os.X_OK):
            raise PermissionError(
                f"Cannot execute {executable}; set TMPDIR to an executable filesystem")
        # A root provisioner drops to the service identity below. Only its
        # owned scratch directory needs traversal by that identity.
        root.chmod(0o755 if os.geteuid() == 0 else 0o700)
        yield root


def run(binary: Path) -> None:
    with executable_fixture(binary) as execution_root:
        run_policy_fixture(execution_root)


def run_policy_fixture(execution_root: Path) -> None:
    # Root commands are individual argv-only filesystem operations against this
    # unique test directory. Never sudo the caller's test executable or shell.
    privileged = [] if os.geteuid() == 0 else ["sudo", "-n"]
    service_uid = os.geteuid() if os.geteuid() != 0 else 65534
    service_gid = os.getegid() if os.geteuid() != 0 else 65534
    root = Path("/run") / f"plinth-policy-test-{os.getpid()}-{secrets.token_hex(8)}"
    files: list[Path] = []
    directories: list[Path] = []

    def command(*args: str) -> None:
        subprocess.run([*privileged, *args], check=True, timeout=15)

    def directory(path: Path, mode: str = "0755") -> None:
        command("mkdir", "-m", mode, "--", str(path))
        directories.append(path)

    try:
        directory(root)
        executable = execution_root / "policy-tests"
        verifier = execution_root / "verify.py"
        with tempfile.TemporaryDirectory(prefix="plinth-policy-input-") as staging:
            source = Path(staging) / "policy.json"
            valid = json.dumps({
                "format_version": 1, "policy_revision": "1",
                "review": {
                    "admin_user_id": "00000000-0000-0000-0000-000000000000",
                    "reviewed_at": "2026-10-16T00:00:00Z", "review_ref": "fixture",
                    "evidence_sha256": "a" * 64,
                }, "entries": [],
            })

            def install(name: str, contents: str, mode: str = "0644") -> Path:
                source.write_text(contents, encoding="utf-8")
                target = root / name
                command("install", "-o", "0", "-g", "0", "-m", mode,
                        "--", str(source), str(target))
                files.append(target)
                return target

            install("valid.json", valid)
            # Independent schema-codec preimage, as in restore_codec_vectors.py;
            # the kernel recomputes its own digest from the C++ schema fixture.
            schema = {"version": 1, "fields": {
                "record_id": {"required": True, "max_bytes": 128,
                              "meaning": "Stable fixture record identifier"},
                "query": {"required": False, "max_bytes": 512,
                          "meaning": "Live-only search text"},
            }, "persist_keys": ["record_id"]}
            encoded = json.dumps(schema, sort_keys=True, ensure_ascii=False,
                                 separators=(",", ":")).encode("utf-8")
            schema_hash = hashlib.sha256(
                b"plinth.restore.schema\0" + struct.pack(">I", 1) + encoded).hexdigest()
            projection = json.loads(valid)
            entry = {"application_id": "notes", "panel_id": "preview",
                     "capability": "notes:1:read",
                     "projection_contract": {"id": "record-v1", "version": 1},
                     "artifact_digest": "a" * 64, "schema_digest": schema_hash,
                     "fields": [{"key": "record_id",
                                 "meaning": "Stable fixture record identifier"}]}
            projection["entries"] = [entry]
            install("projection.json", json.dumps(projection))
            entry["fields"] = []
            install("missing-required.json", json.dumps(projection))
            entry["fields"] = [{"key": "record_id", "meaning": "Unreviewed meaning"}]
            install("wrong-meaning.json", json.dumps(projection))
            install("duplicate.json", valid[:-1] + ',"policy_revision":"1"}')
            install("invalid.json", "{}")
            install("oversize.json", " " * (1024 * 1024 + 1))
            owned = install("same-owner.json", valid, "0444")
            command("chown", f"{service_uid}:{service_gid}", "--", str(owned))
            install("writable-file.json", valid, "0666")
            directory(root / "writable-parent", "0777")
            install("writable-parent/policy.json", valid)
            original = install("hardlink-source.json", valid)
            hardlink = root / "hardlink.json"
            command("ln", "--", str(original), str(hardlink))
            files.append(hardlink)
            link = root / "link.json"
            command("ln", "-s", "--", "valid.json", str(link))
            files.append(link)
            fifo = root / "fifo.json"
            command("mkfifo", "-m", "0644", "--", str(fifo))
            files.append(fifo)
            directory(root / "directory.json")

            environment = os.environ.copy()
            environment["PLINTH_TEST_PROTECTED_POLICY_ROOT"] = str(root)
            drop = ["setpriv", "--no-new-privs"]
            if os.geteuid() == 0:
                drop += [f"--reuid={service_uid}", f"--regid={service_gid}",
                         "--clear-groups"]
            case = "protected restore policy: real protected deployment boundary"
            subprocess.run([*drop, "python3", str(verifier), "--binary", str(executable),
                            "--case", case, "--timeout", "45"],
                           env=environment, cwd=execution_root, check=True, timeout=55)
    finally:
        # Only known exact files; no recursive removal, wildcard or broad target.
        for path in reversed(files):
            command("unlink", "--", str(path))
        for path in reversed(directories):
            command("rmdir", "--", str(path))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    run(args.binary.resolve(strict=True))
