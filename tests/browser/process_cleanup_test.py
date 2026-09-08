"""Hermetic ownership regressions, including the installed Playwright browser."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from process_cleanup import run_browser, start_browser, stop_browser


class BrowserCleanupTest(unittest.TestCase):
    def assert_reaped(self, pid):
        self.assertFalse(Path(f"/proc/{pid}").exists(), f"owned child {pid} was not reaped")

    def exercise_descendant(self, *, detached, command_exits):
        with tempfile.TemporaryDirectory(prefix="plinth-cleanup-test-") as temporary:
            ready = Path(temporary) / "ready"
            identities = Path(temporary) / "pids.json"
            descendant = (
                "import os, signal, time; from pathlib import Path; "
                "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                f"Path({str(ready)!r}).write_text('ready'); time.sleep(60)"
            )
            command = (
                "import json, os, subprocess, sys, time\nfrom pathlib import Path\n"
                f"child = subprocess.Popen([sys.executable, '-c', {descendant!r}], "
                f"start_new_session={detached!r})\n"
                f"while not Path({str(ready)!r}).exists(): time.sleep(0.01)\n"
                f"Path({str(identities)!r}).write_text(json.dumps("
                "{'command': os.getpid(), 'descendant': child.pid}))\n"
                + ("" if command_exits else "time.sleep(60)\n")
            )
            if command_exits:
                run_browser([sys.executable, "-c", command], timeout=10)
            else:
                child = start_browser([sys.executable, "-c", command])
                try:
                    deadline = time.monotonic() + 10
                    while not identities.exists():
                        self.assertIsNone(child.poll(), "descendant probe failed before readiness")
                        self.assertLess(time.monotonic(), deadline, "descendant probe did not become ready")
                        time.sleep(0.02)
                    with self.assertRaises(subprocess.TimeoutExpired):
                        child.wait(timeout=0.1)
                finally:
                    stop_browser(child)
            for pid in json.loads(identities.read_text()).values():
                self.assert_reaped(pid)

    def test_command_failure_propagates_after_cleanup(self):
        with self.assertRaises(subprocess.CalledProcessError) as caught:
            run_browser([sys.executable, "-c", "raise SystemExit(7)"], timeout=10)
        self.assertEqual(caught.exception.returncode, 7)

    def test_unrelated_launcher_child_is_untouched(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            run_browser([sys.executable, "-c", "pass"], timeout=10)
            self.assertIsNone(unrelated.poll(), "supervisor touched a process outside its tree")
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=5)

    def test_timeout_reaps_same_group_descendant(self):
        self.exercise_descendant(detached=False, command_exits=False)

    def test_timeout_reaps_detached_descendant(self):
        self.exercise_descendant(detached=True, command_exits=False)

    def test_normal_exit_reaps_abandoned_detached_descendant(self):
        self.exercise_descendant(detached=True, command_exits=True)

    def test_locked_playwright_timeout_reaps_detached_browser(self):
        with tempfile.TemporaryDirectory(prefix="plinth-playwright-cleanup-test-") as temporary:
            identities = Path(temporary) / "pids.json"
            probe = Path(__file__).with_name("cleanup-playwright-probe.mjs")
            child = start_browser(["node", str(probe), str(identities)],
                                  env=os.environ | {"TMPDIR": temporary})
            try:
                deadline = time.monotonic() + 20
                while not identities.exists():
                    self.assertIsNone(child.poll(), "Playwright probe failed before readiness")
                    self.assertLess(time.monotonic(), deadline, "Playwright probe did not become ready")
                    time.sleep(0.02)
                browser = json.loads(identities.read_text())["browser"]
                self.assertNotEqual(os.getpgid(browser), child.pid,
                                    "probe must exercise Playwright's detached process group")
                with self.assertRaises(subprocess.TimeoutExpired):
                    child.wait(timeout=0.1)
            finally:
                stop_browser(child)
            self.assert_reaped(browser)
            self.assert_reaped(child.pid)


if __name__ == "__main__":
    unittest.main()
