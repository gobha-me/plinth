#!/usr/bin/env python3
"""Verify the bundled-shell packer is deterministic and complete."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile


FIXED_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
FIXED_MODE = stat.S_IFREG | 0o644


def run_packer(packer: Path, source: Path, output: Path) -> None:
    subprocess.run(
        [
            sys.executable,
            str(packer),
            "--source",
            str(source),
            "--output",
            str(output),
        ],
        check=True,
    )


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="plinth-shell-package-") as root_name:
        root = Path(root_name)
        source = root / "shell"
        shutil.copytree(args.source, source)
        first = root / "first.zip"
        second = root / "second.zip"
        run_packer(args.packer, source, first)

        expected: dict[str, bytes] = {}
        for path in source.rglob("*"):
            if path.is_file():
                expected[path.relative_to(source).as_posix()] = path.read_bytes()
                os.utime(path, (1_700_000_000, 1_700_000_000))
                path.chmod(0o600)
        run_packer(args.packer, source, second)

        if digest(first) != digest(second):
            raise AssertionError("shell ZIP changed after source metadata changed")
        if stat.S_IMODE(first.stat().st_mode) != 0o644:
            raise AssertionError("shell ZIP output mode is not 0644")

        with zipfile.ZipFile(first) as archive:
            infos = archive.infolist()
            names = [info.filename for info in infos]
            if names != sorted(expected, key=lambda name: name.encode("utf-8")):
                raise AssertionError("shell ZIP paths are incomplete or unsorted")
            for info in infos:
                if info.date_time != FIXED_TIMESTAMP:
                    raise AssertionError(f"non-deterministic timestamp: {info.filename}")
                if info.compress_type != zipfile.ZIP_STORED:
                    raise AssertionError(f"unexpected compression: {info.filename}")
                if info.external_attr >> 16 != FIXED_MODE:
                    raise AssertionError(f"unexpected mode: {info.filename}")
                if archive.read(info) != expected[info.filename]:
                    raise AssertionError(f"content mismatch: {info.filename}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
