#!/usr/bin/env python3
"""Real filesystem and restricted-exec checks for policy executable staging."""

import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest

from tests.tools.protected_policy_fixture import executable_fixture


class ExecutableFixtureTest(unittest.TestCase):
    def test_private_source_is_copied_with_exact_modes_and_executes_restricted(self):
        with tempfile.TemporaryDirectory(prefix="plinth-policy-source-") as source:
            Path(source).chmod(0o700)
            binary = Path(source) / "cat"
            shutil.copyfile(shutil.which("cat"), binary)
            binary.chmod(0o4750)
            with executable_fixture(binary) as root:
                executable = root / "policy-tests"
                verifier = root / "verify.py"
                self.assertEqual(root.parent, Path(tempfile.gettempdir()))
                self.assertEqual(root.stat().st_uid, os.geteuid())
                self.assertEqual(stat.S_IMODE(root.stat().st_mode),
                                 0o755 if os.geteuid() == 0 else 0o700)
                self.assertEqual(stat.S_IMODE(executable.stat().st_mode), 0o755)
                self.assertEqual(stat.S_IMODE(verifier.stat().st_mode), 0o644)
                self.assertEqual(executable.read_bytes(), binary.read_bytes())
                self.assertEqual(verifier.read_bytes(),
                                 Path(__file__).with_name("verify_catch_case.py").read_bytes())
                drop = ["setpriv", "--no-new-privs"]
                if os.geteuid() == 0:
                    drop += ["--reuid=65534", "--regid=65534", "--clear-groups"]
                result = subprocess.run([*drop, str(executable), "/proc/self/status"],
                                        cwd=root, check=True, capture_output=True,
                                        text=True, timeout=10)
                self.assertIn("NoNewPrivs:\t1\n", result.stdout)
                self.assertIn("CapPrm:\t0000000000000000\n", result.stdout)
                self.assertIn("CapEff:\t0000000000000000\n", result.stdout)
                self.assertEqual(stat.S_IMODE(binary.stat().st_mode), 0o4750)
                self.assertEqual(stat.S_IMODE(Path(source).stat().st_mode), 0o700)
            self.assertFalse(root.exists())

    def test_staging_is_removed_when_policy_fixture_fails(self):
        with self.assertRaisesRegex(RuntimeError, "injected policy fixture failure"):
            with executable_fixture(Path(shutil.which("cat"))) as root:
                self.assertTrue((root / "policy-tests").is_file())
                raise RuntimeError("injected policy fixture failure")
        self.assertFalse(root.exists())


if __name__ == "__main__":
    unittest.main()
