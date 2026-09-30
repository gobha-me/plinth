#!/usr/bin/env python3
"""Hermetic archive admission/determinism tests; no implicit runtime install."""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
import zipfile


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
spec = importlib.util.spec_from_file_location("pack_admin", REPO / "tools/pack_admin.py")
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


class AdminPackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-admin-package-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "admin"
        self.source.mkdir()
        for name in packer.REQUIRED_FILES:
            (self.source / name).write_text("{}", encoding="utf-8")
        self.manifest()
        for name in ("server/main.js", "client/panels/packages.js", "client/packages/api.js"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("export const fixture = true;\n", encoding="utf-8")
        self.output = self.root / "archives/admin-0.1.0.zip"

    def manifest(self, **changes):
        value = {"name": "admin", "version": "0.1.0", **changes}
        (self.source / "manifest.json").write_text(json.dumps(value), encoding="utf-8")

    def rejected(self):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"previous owned archive")
        with self.assertRaises((ValueError, json.JSONDecodeError)):
            packer.pack(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous owned archive")
        self.assertEqual([p for p in self.output.parent.iterdir() if p.suffix == ".tmp"], [])

    def test_mid_write_failure_preserves_previous_archive_and_cleans_temp(self):
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self.output.write_bytes(b"previous owned archive")
        read_bytes = Path.read_bytes

        def fail_on_entry(path):
            if path == self.source / "server/main.js":
                raise OSError("injected fixture read failure")
            return read_bytes(path)

        with mock.patch.object(Path, "read_bytes", fail_on_entry):
            with self.assertRaisesRegex(OSError, "injected fixture"):
                packer.pack(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous owned archive")
        self.assertEqual([p for p in self.output.parent.iterdir() if p.suffix == ".tmp"], [])

    def test_exact_sorted_graph_and_fixed_metadata(self):
        packer.pack(self.source, self.output)
        expected = {path.relative_to(self.source).as_posix(): path.read_bytes()
                    for path in self.source.rglob("*") if path.is_file()}
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.namelist(), sorted(expected, key=lambda name: name.encode("utf-8")))
            for info in archive.infolist():
                self.assertEqual(info.date_time, packer.ZIP_TIMESTAMP)
                self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
                self.assertEqual(info.external_attr >> 16, stat.S_IFREG | 0o644)
                self.assertEqual(archive.read(info), expected[info.filename])
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o644)

    def test_source_metadata_does_not_change_bytes(self):
        packer.pack(self.source, self.output)
        original = self.output.read_bytes()
        for path in self.source.rglob("*"):
            if path.is_file():
                os.utime(path, (1_700_000_000, 1_700_000_000))
                path.chmod(0o600)
        packer.pack(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), original)

    def test_optional_readme_is_exactly_included(self):
        (self.source / "README.md").write_text("Ordinary API-installed package.\n", encoding="utf-8")
        packer.pack(self.source, self.output)
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.read("README.md"), b"Ordinary API-installed package.\n")

    def test_missing_each_required_file_preserves_output(self):
        for name in packer.REQUIRED_FILES:
            with self.subTest(name=name):
                path = self.source / name
                original = path.read_bytes()
                path.unlink()
                self.rejected()
                path.write_bytes(original)

    def test_missing_entrypoint_preserves_output(self):
        (self.source / "server/main.js").unlink()
        self.rejected()

    def test_wrong_identity_and_unsafe_version_are_rejected(self):
        for changes in ({"name": "shell"}, {"version": "../artifact"}, {"version": {}}, {"version": "not-a-version"}):
            with self.subTest(changes=changes):
                self.manifest(**changes)
                self.rejected()

    def test_malformed_manifest_preserves_output(self):
        (self.source / "manifest.json").write_text("{", encoding="utf-8")
        self.rejected()

    def test_unknown_root_entry_cannot_be_shipped(self):
        (self.source / "AGENTS.md").write_text("unowned handoff fixture", encoding="utf-8")
        self.rejected()

    def test_hidden_client_entry_cannot_be_shipped(self):
        (self.source / "client/.env").write_text("fake fixture", encoding="utf-8")
        self.rejected()

    def test_nested_symlink_cannot_be_shipped(self):
        (self.source / "client/link.js").symlink_to(self.source / "server/main.js")
        self.rejected()

    def test_required_root_symlink_cannot_be_shipped(self):
        path = self.source / "rbac.json"
        path.unlink()
        path.symlink_to(self.source / "capabilities.json")
        self.rejected()

    def test_output_inside_source_is_rejected_before_writing(self):
        output = self.source / "client/archive.zip"
        with self.assertRaises(ValueError):
            packer.pack(self.source, output)
        self.assertFalse(output.exists())

    def test_source_and_output_symlinks_are_rejected(self):
        source_link = self.root / "source-link"
        source_link.symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(ValueError):
            packer.pack(source_link, self.output)
        protected = self.root / "fake-protected-output"
        protected.write_bytes(b"preserve")
        self.output.parent.mkdir(parents=True)
        self.output.symlink_to(protected)
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.output)
        self.assertEqual(protected.read_bytes(), b"preserve")


if __name__ == "__main__":
    unittest.main(verbosity=2)
