"""Small ownership tests for the admin production-browser supervisor."""

import importlib.util
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

import admin_artifacts


SCRIPT = Path(__file__).with_name("run-admin-package.py")
spec = importlib.util.spec_from_file_location("run_admin_package", SCRIPT)
supervisor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(supervisor)


class AdminSupervisorTest(unittest.TestCase):
    def test_early_kernel_exit_is_owned_before_readiness_failure(self):
        child = mock.Mock()
        child.poll.return_value = 7
        runtime = mock.Mock(is_container=False)
        runtime.start.return_value = child
        runtime.path.return_value = "/owned/config.json"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            children = []
            with self.assertRaisesRegex(RuntimeError, "startup failed"):
                supervisor.start_kernel(runtime, root / "config.json", root,
                    {"PLINTH_BASE_URL": "http://127.0.0.1:1"}, children)
            self.assertEqual(children, [child])

    def test_private_diagnostics_redact_tokens_and_keep_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "kernel.log").write_text("fake-token fake-password")
            location = supervisor.preserve_failure(root,
                {"PLINTH_BOOTSTRAP_TOKEN": "fake-token",
                 "PLINTH_PG_PASSWORD": "fake-password"},
                "browser fake-token", "failure fake-password")
            self.addCleanup(lambda: shutil.rmtree(location))
            self.assertEqual(location.stat().st_mode & 0o777, 0o700)
            for path in location.iterdir():
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                self.assertNotIn("fake-token", path.read_text())
                self.assertNotIn("fake-password", path.read_text())


class CurrentAdminArchiveTest(unittest.TestCase):
    """Current artifact binding uses synthetic bytes, never native-upgrade credit."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-admin-current-unit-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.contents = {"manifest.json": b'{"name":"admin","version":"0.1.1"}\n',
                         "capabilities.json": b'{"provides":[],"requires":[]}\n',
                         "rbac.json": b'{"rules":[]}\n', "panels.json": b'{"panels":[]}\n',
                         "server/main.js": b"export default function main() {}\n"}
        for name in (*supervisor.PACKAGE_MODULES.values(), *supervisor.GROUP_MODULES.values()):
            self.contents[name] = ("// synthetic " + name + "\n").encode()
        for name, data in self.contents.items():
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        self.archive = self.root / "admin-0.1.1.zip"

    def write_archive(self, contents=None, bad_mode=False):
        with zipfile.ZipFile(self.archive, "w") as packed:
            for name, data in sorted((contents or self.contents).items()):
                info = zipfile.ZipInfo(name)
                info.external_attr = (0o100755 if bad_mode else 0o100644) << 16
                packed.writestr(info, data)

    def test_current_archive_identity_binds_all_bytes_and_both_module_families(self):
        self.write_archive()
        identity = supervisor.current_admin_identity(self.source, self.archive)
        self.assertEqual(identity["version"], "0.1.1")
        self.assertEqual(identity["archiveSha256"], hashlib.sha256(self.archive.read_bytes()).hexdigest())
        for key, paths in (("moduleHashes", supervisor.PACKAGE_MODULES),
                           ("groupsModuleHashes", supervisor.GROUP_MODULES)):
            self.assertEqual(identity[key], {role: hashlib.sha256(self.contents[path]).hexdigest()
                                             for role, path in paths.items()})

    def test_current_archive_rejects_stale_bytes_extra_inventory_mode_and_old_version(self):
        stale = dict(self.contents)
        stale["client/groups/api.js"] = b"// stale current module\n"
        for contents, bad_mode in ((stale, False), ({**self.contents, "extra.js": b"extra"}, False),
                                   (self.contents, True)):
            with self.subTest(extra="extra.js" in contents, bad_mode=bad_mode):
                self.write_archive(contents, bad_mode)
                with self.assertRaises(ValueError):
                    supervisor.current_admin_identity(self.source, self.archive)
        (self.source / "manifest.json").write_text('{"name":"admin","version":"0.1.0"}')
        with self.assertRaisesRegex(ValueError, "admin0.1.1"):
            supervisor.current_admin_identity(self.source, self.archive)


class AdminBaselineSyntheticTest(unittest.TestCase):
    """S01–S04 use their own tiny Git database even in a .git-free image.

    The private core receives a synthetic immutable spec. Production pin
    constants and the fixed public entry are never replaced or invoked here.
    """

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-admin-synthetic-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        self.repository.mkdir()
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.git("init", "--quiet")
        self.contents = {
            "README.md": b"Synthetic Git unit fixture. Not M198 source.\n",
            "manifest.json": b'{"name":"admin","version":"0.1.0"}\n',
            "capabilities.json": b'{"provides":[],"requires":[]}\n',
            "panels.json": b'{"panels":[]}\n',
            "rbac.json": b'{"rules":[]}\n',
            "server/main.js": b"export default function main() {}\n",
            "client/panels/packages.js": b"export default function packages() {}\n",
            "client/packages/api.js": b"export const synthetic = true;\n",
            "client/packages/controller.js": b"export const syntheticController = true;\n",
        }
        for relative, data in self.contents.items():
            path = self.repository / "client/admin" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o644)
        self.git("add", "--", "client/admin")
        tree = self.git("write-tree").decode().strip()
        commit = self.git("-c", "user.name=Synthetic Fixture", "-c", "user.email=fixture@example.test",
                          "-c", "commit.gpgsign=false", "commit-tree", tree,
                          input=b"synthetic unit fixture, not M198\n").decode().strip()
        files = []
        for record in self.git("ls-tree", "-rz", "--full-tree", commit, "--", "client/admin").split(b"\0"):
            if not record:
                continue
            metadata, name = record.split(b"\t", 1)
            mode, kind, blob = metadata.decode().split()
            self.assertEqual(kind, "blob")
            path = name.decode()
            data = (self.repository / path).read_bytes()
            files.append(admin_artifacts._SourceFile(path, mode, blob, len(data), hashlib.sha256(data).hexdigest()))
        self.specification = admin_artifacts._TreeSpec(commit, tree, tuple(files))

    def git(self, *arguments, input=None):
        result = subprocess.run(["git", "-C", str(self.repository), *arguments],
                                env=self.environment, input=input, check=True, timeout=10, capture_output=True)
        return result.stdout

    def prepare(self, name, specification=None):
        return admin_artifacts._prepare_baseline(self.repository, self.root / name,
                                                specification or self.specification)

    def test_S01_synthetic_private_core_materializes_exact_bytes_without_outer_git(self):
        before = self.git("ls-files", "--stage", "-z")
        first = self.prepare("first")
        second = self.prepare("second")
        self.assertEqual(first.archive_sha256, second.archive_sha256)
        self.assertEqual(first.archive_sha256, hashlib.sha256(first.archive.read_bytes()).hexdigest())
        self.assertEqual(first.source_commit, self.specification.commit)
        self.assertNotEqual(first.source_commit, "3301b7ee01c2098847d47f3e51d5ff14d28442f3")
        self.assertEqual(first.receipt()["scope"], "synthetic private-core fixture; not M198 or native upgrade proof")
        self.assertEqual((first.package_name, first.package_version), ("admin", "0.1.0"))
        self.assertEqual(set(first.archive.parent.iterdir()), {first.archive})
        self.assertEqual(set(second.archive.parent.iterdir()), {second.archive})
        with zipfile.ZipFile(first.archive) as archive:
            self.assertEqual(archive.namelist(), sorted(self.contents))
            for name, data in self.contents.items():
                self.assertEqual(archive.read(name), data)
                self.assertEqual(archive.getinfo(name).external_attr >> 16, 0o100644)
        self.assertEqual(self.git("ls-files", "--stage", "-z"), before)
        for name, data in self.contents.items():
            self.assertEqual((self.repository / "client/admin" / name).read_bytes(), data)

    def test_S02_missing_synthetic_object_fails_without_fetch_fallback_or_output(self):
        wrong = replace(self.specification, commit="0" * 40)
        actual_run = subprocess.run
        with mock.patch.object(admin_artifacts.subprocess, "run", wraps=actual_run) as commands:
            with self.assertRaises(admin_artifacts.BaselineError):
                self.prepare("missing", wrong)
        self.assertFalse((self.root / "missing").exists())
        self.assertGreater(len(commands.call_args_list), 0)
        for call in commands.call_args_list:
            self.assertFalse({"fetch", "clone", "pull", "checkout", "reset"} & set(call.args[0]))
            self.assertEqual(call.kwargs["env"]["GIT_NO_LAZY_FETCH"], "1")

    def test_S03_wrong_synthetic_tree_inventory_mode_size_hash_or_identity_is_rejected(self):
        original = self.specification.files[0]
        altered = [replace(self.specification, tree="0" * 40),
                   replace(self.specification, files=self.specification.files[1:]),
                   replace(self.specification, package_version="0.1.1")]
        for entry in [replace(original, path="client/admin/../outside"),
                      replace(original, mode="100755"), replace(original, blob="0" * 40),
                      replace(original, size=original.size + 1), replace(original, sha256="0" * 64)]:
            altered.append(replace(self.specification, files=(entry, *self.specification.files[1:])))
        for index, specification in enumerate(altered):
            with self.subTest(index=index):
                with self.assertRaises(admin_artifacts.BaselineError):
                    self.prepare(f"wrong-{index}", specification)
                self.assertFalse((self.root / f"wrong-{index}").exists())
        existing = self.root / "existing"
        existing.mkdir()
        marker = existing / "owned-by-someone-else"
        marker.write_bytes(b"preserve")
        with self.assertRaises(admin_artifacts.BaselineError):
            self.prepare("existing")
        self.assertEqual(marker.read_bytes(), b"preserve")

    def test_S04_pack_failure_removes_owned_source_and_output_and_preserves_original_error(self):
        marker = RuntimeError("synthetic pack failure")
        staged = []

        def fail(source, output):
            staged.append(source)
            self.assertEqual((source / "manifest.json").read_bytes(), self.contents["manifest.json"])
            output.write_bytes(b"incomplete synthetic archive")
            raise marker

        with mock.patch.object(admin_artifacts, "_pack", side_effect=fail):
            with self.assertRaises(RuntimeError) as raised:
                self.prepare("pack-failed")
        self.assertIs(raised.exception, marker)
        self.assertEqual(len(staged), 1)
        self.assertFalse(staged[0].exists())
        self.assertFalse((self.root / "pack-failed").exists())
        for name, data in self.contents.items():
            self.assertEqual((self.repository / "client/admin" / name).read_bytes(), data)


# Source-preparation snippet; synthetic receipt controls, not native proof.
class AdminGroupsReceiptTest(unittest.TestCase):
    def setUp(self):
        from hashlib import sha256
        def hashed(label):
            return sha256(("synthetic owned fixture: " + label).encode()).hexdigest()
        def modules(label):
            return {role: hashed(label + role) for role in ("panel", "api", "controller")}
        self.baseline = {"commit": "3301b7ee01c2098847d47f3e51d5ff14d28442f3",
                         "tree": "2399eadb48c5d1bc6f41f4bbc77b425ec0d9dc26",
                         "version": "0.1.0", "archiveSha256": hashed("baseline archive"),
                         "moduleHashes": modules("baseline ")}
        self.current = {"version": "0.1.1", "archiveSha256": hashed("current archive"),
                        "moduleHashes": modules("current packages "),
                        "groupsModuleHashes": modules("current groups ")}
        legacy_id, baseline_id, upgraded_id, user_id, session_id, audit_id = [
            "aaaaaaaa-0000-4000-8000-" + str(number).zfill(12) for number in range(1, 7)]
        actor = {"userId": user_id, "username": "synthetic_owner", "sessionId": session_id,
                 "ipAddress": "127.0.0.1"}
        self.receipt = {
            "adminPackageNativeCases": 12, "expectedCases": 12, "actualAuth": True,
            "actualRbacCsrf": True, "packageProvenance": "user",
            "archiveSha256": self.current["archiveSha256"],
            "installedModuleSha256": self.current["moduleHashes"]["panel"],
            "installedModulesSha256": dict(self.current["moduleHashes"]),
            "cleanup": "PASS", "scope": "actual kernel/ZIP/installed panel/browser/SQL",
            "groupsProof": {
                "groupsAdminNativeCases": 8, "expectedCases": 8,
                "cases": list(supervisor.GROUPS_NATIVE_CASES), "actualAuth": True,
                "actualRbacCsrf": True, "cleanup": "PASS",
                "legacyProof": {"cases": list(supervisor.LEGACY_NATIVE_CASES), "count": 12,
                    "version": "0.1.1", "archiveSha256": self.current["archiveSha256"],
                    "moduleHashes": dict(self.current["moduleHashes"]), "packageId": legacy_id,
                    "actor": actor, "uninstallAudit": {"id": audit_id,
                        "action": "packages.uninstalled", "packageId": legacy_id,
                        "uninstalledByUserId": user_id},
                    "finalState": "uninstalled_at_N12"},
                "baseline": {**self.baseline, "name": "admin", "packageId": baseline_id},
                "upgraded": {"name": "admin", "version": "0.1.1",
                    "archiveSha256": self.current["archiveSha256"],
                    "groupsModuleHashes": dict(self.current["groupsModuleHashes"]),
                    "packageId": upgraded_id, "supersedesId": baseline_id},
            },
        }

    def rejected(self, path, value):
        from copy import deepcopy
        candidate = deepcopy(self.receipt)
        target = candidate
        for key in path[:-1]:
            target = target[key]
        target[path[-1]] = value
        with self.assertRaisesRegex(ValueError, "admin groups receipt"):
            supervisor.validate_groups_receipt(candidate, self.baseline, self.current)

    def test_R01_complete_synthetic_receipt_binds_independent_inputs(self):
        from copy import deepcopy
        before = deepcopy(self.receipt)
        self.assertIsNone(supervisor.validate_groups_receipt(
            self.receipt, self.baseline, self.current))
        self.assertEqual(self.receipt, before)

    def test_R02_missing_duplicate_reordered_groups_ledger_rejected(self):
        cases = list(supervisor.GROUPS_NATIVE_CASES)
        for invalid in ([], cases[:-1], cases[:-1] + cases[:1], list(reversed(cases))):
            with self.subTest(ledger=invalid):
                self.rejected(("groupsProof", "cases"), invalid)
        self.rejected(("groupsProof", "groupsAdminNativeCases"), 7)
        self.rejected(("groupsProof", "expectedCases"), 7)
        self.rejected(("groupsProof",), None)

    def test_R03_wrong_artifact_version_commit_or_lineage_rejected(self):
        changes = [
            (("archiveSha256",), "0" * 64),
            (("groupsProof", "legacyProof", "version"), "0.1.0"),
            (("groupsProof", "legacyProof", "archiveSha256"), self.baseline["archiveSha256"]),
            (("groupsProof", "baseline", "commit"), "0" * 40),
            (("groupsProof", "baseline", "tree"), "0" * 40),
            (("groupsProof", "baseline", "version"), "0.1.1"),
            (("groupsProof", "baseline", "name"), "shell"),
            (("groupsProof", "baseline", "archiveSha256"), self.current["archiveSha256"]),
            (("groupsProof", "upgraded", "version"), "0.1.0"),
            (("groupsProof", "upgraded", "archiveSha256"), self.baseline["archiveSha256"]),
            (("groupsProof", "upgraded", "supersedesId"),
             self.receipt["groupsProof"]["legacyProof"]["packageId"]),
            (("groupsProof", "upgraded", "packageId"),
             self.receipt["groupsProof"]["baseline"]["packageId"]),
            (("groupsProof", "upgraded", "packageId"),
             self.receipt["groupsProof"]["baseline"]["packageId"].upper()),
            (("groupsProof", "legacyProof", "packageId"),
             self.receipt["groupsProof"]["baseline"]["packageId"]),
        ]
        for path, value in changes:
            with self.subTest(path=path):
                self.rejected(path, value)
        from copy import deepcopy
        bad_host = deepcopy(self.baseline)
        bad_host["commit"] = "0" * 40
        with self.assertRaisesRegex(ValueError, "host source identity"):
            supervisor.validate_groups_receipt(self.receipt, bad_host, self.current)
        bad_host = deepcopy(self.baseline)
        bad_host["archiveSha256"] = self.current["archiveSha256"]
        with self.assertRaisesRegex(ValueError, "archives conflated"):
            supervisor.validate_groups_receipt(self.receipt, bad_host, self.current)

    def test_R04_module_hashes_must_match_independent_source_inputs(self):
        for prefix in (("installedModulesSha256",),
                       ("groupsProof", "legacyProof", "moduleHashes"),
                       ("groupsProof", "baseline", "moduleHashes"),
                       ("groupsProof", "upgraded", "groupsModuleHashes")):
            for role in ("panel", "api", "controller"):
                with self.subTest(prefix=prefix, role=role):
                    self.rejected((*prefix, role), "f" * 64)
        self.rejected(("installedModuleSha256",), "f" * 64)
        self.rejected(("groupsProof", "upgraded", "groupsModuleHashes"), {})

    def test_R05_cleanup_or_nonpass_controls_cannot_be_accepted(self):
        for prefix in ((), ("groupsProof",)):
            for key, value in (("cleanup", "FAIL"), ("actualAuth", False),
                               ("actualRbacCsrf", False)):
                with self.subTest(prefix=prefix, key=key):
                    self.rejected((*prefix, key), value)
        with self.assertRaisesRegex(ValueError, "outer proof missing"):
            supervisor.validate_groups_receipt(None, self.baseline, self.current)

    def test_R06_original_twelve_and_N12_actor_audit_remain_required(self):
        cases = list(supervisor.LEGACY_NATIVE_CASES)
        for invalid in ([], cases[:-1], cases[:-1] + cases[:1], list(reversed(cases))):
            with self.subTest(ledger=invalid):
                self.rejected(("groupsProof", "legacyProof", "cases"), invalid)
        for path, value in [
            (("adminPackageNativeCases",), 11), (("expectedCases",), 11),
            (("packageProvenance",), "bundled"), (("scope",), "mocked HTTP"),
            (("groupsProof", "legacyProof"), None),
            (("groupsProof", "legacyProof", "count"), 11),
            (("groupsProof", "legacyProof", "finalState"), "active"),
            (("groupsProof", "legacyProof", "uninstallAudit", "id"), "missing"),
            (("groupsProof", "legacyProof", "uninstallAudit", "uninstalledByUserId"),
             "00000000-0000-4000-8000-000000000099"),
            (("groupsProof", "legacyProof", "uninstallAudit", "packageId"),
             "00000000-0000-4000-8000-000000000099"),
        ]:
            with self.subTest(path=path):
                self.rejected(path, value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
