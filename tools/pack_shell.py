#!/usr/bin/env python3
"""Create Plinth's bundled shell ZIP with byte-stable metadata and ordering."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import tempfile
import zipfile


ROOT_FILES = (
    "manifest.json",
    "capabilities.json",
    "rbac.json",
    "panels.json",
    "config.json",
)
ROOT_DIRECTORIES = ("client", "server", "migrations")
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
REGULAR_FILE_MODE = stat.S_IFREG | 0o644


def shell_files(source: Path) -> list[tuple[Path, str]]:
    entries: list[tuple[Path, str]] = []
    for name in ROOT_FILES:
        path = source / name
        if not path.is_file() or path.is_symlink():
            raise ValueError(f"required regular file is missing: {name}")
        entries.append((path, name))

    for name in ROOT_DIRECTORIES:
        root = source / name
        if not root.is_dir() or root.is_symlink():
            raise ValueError(f"required directory is missing: {name}")
        for path in root.rglob("*"):
            relative = path.relative_to(source).as_posix()
            if path.is_symlink():
                raise ValueError(f"symbolic links are not allowed: {relative}")
            if path.is_file():
                entries.append((path, relative))
            elif not path.is_dir():
                raise ValueError(f"unsupported filesystem entry: {relative}")

    entries.sort(key=lambda entry: entry[1].encode("utf-8"))
    archive_names = [name for _, name in entries]
    if len(archive_names) != len(set(archive_names)):
        raise ValueError("duplicate archive path")
    return entries


def pack(source: Path, output: Path) -> None:
    source = source.resolve(strict=True)
    output = output.resolve(strict=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    entries = shell_files(source)

    temporary_fd, temporary_name = tempfile.mkstemp(
        dir=output.parent, prefix=f".{output.name}.", suffix=".tmp"
    )
    os.close(temporary_fd)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as archive:
            for path, name in entries:
                info = zipfile.ZipInfo(name, ZIP_TIMESTAMP)
                info.create_system = 3
                info.compress_type = zipfile.ZIP_STORED
                info.external_attr = REGULAR_FILE_MODE << 16
                archive.writestr(info, path.read_bytes())
        temporary.chmod(0o644)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    pack(args.source, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
