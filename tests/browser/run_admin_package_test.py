"""Small ownership tests for the admin production-browser supervisor."""

import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock


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


if __name__ == "__main__":
    unittest.main(verbosity=2)
