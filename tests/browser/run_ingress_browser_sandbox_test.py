import contextlib
import io
import runpy
import signal
import subprocess
import unittest
from unittest import mock

import run_ingress_browser_sandbox as supervisor


class SandboxSupervisorTest(unittest.TestCase):
    def test_both_modes_have_finite_deadline_and_owned_subreaper(self):
        for arguments, timeout in (([], 180), (["--cleanup"], 90)):
            def succeed(*_args, **kwargs):
                marker = supervisor.CLEANED if arguments else supervisor.READY
                kwargs["stdout"].write((marker + "\n").encode())

            with self.subTest(arguments=arguments), \
                    mock.patch.object(supervisor, "run_browser", side_effect=succeed) as run:
                with contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertEqual(supervisor.run(arguments), 0)
                self.assertEqual(output.getvalue(),
                                 (supervisor.CLEANED if arguments else supervisor.READY) + "\n")
                run.assert_called_once_with(
                    ["node", str(supervisor.ROOT / ".github/scripts/prepare-ingress-browser.mjs"),
                     *arguments], timeout=timeout, cwd=supervisor.ROOT,
                    stdout=mock.ANY, stderr=subprocess.STDOUT,
                )
                self.assertTrue(run.call_args.kwargs["stdout"].closed)

    def test_invalid_arguments_never_launch_or_print_their_values(self):
        for arguments in (["private-marker"], ["--cleanup", "https://private-marker/"]):
            with self.subTest(arguments=arguments), \
                    mock.patch.object(supervisor, "run_browser") as run:
                output, errors = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    self.assertEqual(supervisor.run(arguments), 1)
                run.assert_not_called()
                self.assertEqual(output.getvalue(), "")
                self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")

    def test_child_bootstrap_output_never_escapes_even_after_zero_exit(self):
        for text in ("Traceback: private-marker /private/path\n", "\xff", "x" * 65537,
                     supervisor.READY + "\nprivate-marker\n", supervisor.READY + "\n" + supervisor.READY,
                     supervisor.CLEANED + "\n", ""):
            def produce(*_args, **kwargs):
                kwargs["stdout"].write(text.encode("latin-1"))

            with self.subTest(size=len(text)), \
                    mock.patch.object(supervisor, "run_browser", side_effect=produce):
                output, errors = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    self.assertEqual(supervisor.run([]), 1)
                self.assertEqual(output.getvalue(), "")
                self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")

    def test_signal_interruptions_unwind_owned_run_and_restore_handlers(self):
        for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            original = signal.getsignal(signum)

            def interrupt(*_args, **_kwargs):
                signal.getsignal(signum)(signum, None)

            with self.subTest(signal=signum), \
                    mock.patch.object(supervisor, "run_browser", side_effect=interrupt):
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(supervisor.run([]), 1)
                self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")
                self.assertIs(signal.getsignal(signum), original)

    def test_timeout_process_cleanup_and_interruption_fail_without_raw_data(self):
        failures = (subprocess.TimeoutExpired(["private-marker"], 180),
                    subprocess.CalledProcessError(1, ["https://private-marker/"]),
                    RuntimeError("private-marker /private/path"), KeyboardInterrupt())
        for failure in failures:
            with self.subTest(failure=type(failure).__name__), \
                    mock.patch.object(supervisor, "run_browser", side_effect=failure):
                output, errors = io.StringIO(), io.StringIO()
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    self.assertEqual(supervisor.run([]), 1)
                self.assertEqual(output.getvalue(), "")
                self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")

    def test_repeated_signals_do_not_interrupt_owned_cleanup(self):
        originals = {s: signal.getsignal(s) for s in
                     (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)}
        cleanup_completed = []

        def interrupt(*_args, **_kwargs):
            try:
                signal.getsignal(signal.SIGTERM)(signal.SIGTERM, None)
            finally:
                for signum in originals:
                    signal.getsignal(signum)(signum, None)
                cleanup_completed.append(True)

        with mock.patch.object(supervisor, "run_browser", side_effect=interrupt):
            with contextlib.redirect_stderr(io.StringIO()) as errors:
                self.assertEqual(supervisor.run([]), 1)
        self.assertEqual(cleanup_completed, [True])
        self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")
        for signum, original in originals.items():
            self.assertIs(signal.getsignal(signum), original)

    def test_dependency_import_failure_is_finite(self):
        import builtins

        original_import = builtins.__import__

        def fail_import(name, *args, **kwargs):
            if name == "process_cleanup":
                raise ImportError("private-marker /private/path")
            return original_import(name, *args, **kwargs)

        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(builtins, "__import__", side_effect=fail_import), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            with self.assertRaises(SystemExit) as failure:
                runpy.run_path(str(supervisor.ROOT / "tests/browser/run_ingress_browser_sandbox.py"))
        self.assertEqual(failure.exception.code, 1)
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(errors.getvalue(), "ingress browser sandbox: supervisor failed\n")


if __name__ == "__main__":
    unittest.main()
