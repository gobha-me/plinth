#!/usr/bin/env python3
"""Pack the ordinary admin extension; never install or bundle it implicitly."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import tempfile
import zipfile

from pack_shell import REGULAR_FILE_MODE, ZIP_TIMESTAMP


REQUIRED_FILES = ("manifest.json", "capabilities.json", "rbac.json", "panels.json")
OPTIONAL_FILES = ("README.md",)
DIRECTORIES = ("client", "server")


def admin_files(source: Path) -> list[tuple[Path, str]]:
    """Collect a closed source surface, not a substitute for kernel validation."""
    allowed = set(REQUIRED_FILES + OPTIONAL_FILES + DIRECTORIES)
    if any(path.name not in allowed for path in source.iterdir()):
        raise ValueError("unexpected admin package root entry")
    entries = []
    for name in REQUIRED_FILES + OPTIONAL_FILES:
        path = source / name
        if name in OPTIONAL_FILES and not path.exists() and not path.is_symlink():
            continue
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"required regular file is missing: {name}")
        entries.append((path, name))
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    version = manifest.get("version") if isinstance(manifest, dict) else None
    if not isinstance(manifest, dict) or manifest.get("name") != "admin":
        raise ValueError("canonical admin package identity required")
    if not isinstance(version, str) or not re.fullmatch(
        r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][A-Za-z0-9][A-Za-z0-9.+-]*)?", version
    ):
        raise ValueError("safe versioned admin artifact identity required")
    for name in DIRECTORIES:
        root = source / name
        if root.is_symlink() or not root.is_dir():
            raise ValueError(f"required directory is missing: {name}")
        for path in root.rglob("*"):
            relative = path.relative_to(source)
            if path.is_symlink() or any(part.startswith(".") for part in relative.parts):
                raise ValueError("symbolic links and hidden package entries are forbidden")
            if path.is_file():
                entries.append((path, relative.as_posix()))
            elif not path.is_dir():
                raise ValueError("unsupported filesystem entry")
    entries.sort(key=lambda entry: entry[1].encode("utf-8"))
    names = [name for _, name in entries]
    if len(names) != len(set(names)):
        raise ValueError("duplicate archive path")
    if "server/main.js" not in names or "client/panels/packages.js" not in names:
        raise ValueError("admin server and package-panel entry points required")
    return entries


def pack(source: Path, output: Path) -> None:
    if source.is_symlink() or not source.is_dir():
        raise ValueError("regular admin source directory required")
    if output.is_symlink() or output.is_dir():
        raise ValueError("regular archive output path required")
    source = source.resolve(strict=True)
    output = output.resolve(strict=False)
    if output.is_relative_to(source):
        raise ValueError("archive output must be outside the package source")
    entries = admin_files(source)
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        dir=output.parent, prefix=f".{output.name}.", suffix=".tmp"
    )
    os.close(descriptor)
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
    arguments = parser.parse_args()
    pack(arguments.source, arguments.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
