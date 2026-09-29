"""Fail-closed regression cases for the real-Node client unit discovery gate."""

import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("contract_units", Path(__file__).with_name("run-contract-units.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def tap(count=2, **overrides):
    counters = {"tests": count, "suites": 0, "pass": count, "fail": 0,
                "cancelled": 0, "skipped": 0, "todo": 0} | overrides
    lines = ["TAP version 13", "# harmless Node warning"]
    for index in range(1, count + 1):
        lines.extend((f"# Subtest: fake case {index}", f"ok {index} - fake case {index}",
                      "  ---", "  duration_ms: 0.1", "  type: 'test'", "  ..."))
    lines.append(f"1..{count}")
    lines.extend(f"# {key} {value}" for key, value in counters.items())
    lines.append("# duration_ms 1.25")
    return "\n".join(lines) + "\n"


class ContractUnitDiscoveryTest(unittest.TestCase):
    def reject(self, output, pattern, returncode=0):
        with self.assertRaisesRegex(gate.GateError, "fake.test.mjs:.*" + pattern):
            gate.validate_tap("fake.test.mjs", 2, output, returncode)

    def test_complete_named_nonzero_tap_passes(self):
        self.assertEqual(gate.validate_tap("fake.test.mjs", 2, tap(), 0), 2)

    def test_zero_discovery_is_not_success(self):
        self.reject(tap(0), "counts differ")

    def test_wrong_nonzero_file_count_is_not_success(self):
        self.reject(tap(3), "counts differ")

    def test_skip_count_is_rejected(self):
        self.reject(tap(skipped=1), "counts differ")

    def test_cancel_count_is_rejected(self):
        self.reject(tap(cancelled=1), "counts differ")

    def test_todo_count_is_rejected(self):
        self.reject(tap(todo=1), "counts differ")

    def test_failure_count_is_rejected(self):
        self.reject(tap(fail=1), "counts differ")

    def test_wrong_pass_count_is_rejected(self):
        self.reject(tap(**{"pass": 1}), "counts differ")

    def test_suites_cannot_masquerade_as_flat_cases(self):
        self.reject(tap(suites=1), "counts differ")

    def test_passing_tap_with_failed_exit_is_rejected(self):
        self.reject(tap(), "Node exited 1", returncode=1)

    def test_signalled_or_unknown_exit_is_rejected(self):
        for code in (-15, None):
            with self.subTest(code=code):
                self.reject(tap(), "Node exited", returncode=code)

    def test_failed_named_case_is_rejected_even_with_forged_summary(self):
        self.reject(tap().replace("ok 2 -", "not ok 2 -"), "case 2 failed")

    def test_skip_todo_directives_are_rejected_even_with_forged_counts(self):
        for directive in ("SKIP", "TODO"):
            with self.subTest(directive=directive):
                self.reject(tap().replace("ok 2 - fake case 2", "ok 2 - fake case 2 # " + directive), "skip/TODO")

    def test_unknown_or_non_tap_output_is_rejected(self):
        for output in ("", "all passed\n", tap().replace("TAP version 13", "TAP version 14"),
                       tap() + "unrecognized record\n", tap() + "# unknown_counter 1\n"):
            with self.subTest(output=output[:40]):
                self.reject(output, "header|unknown TAP")

    def test_summary_counters_are_required_once_and_integer(self):
        for output in (tap().replace("# skipped 0\n", ""), tap() + "# skipped 0\n",
                       tap().replace("# pass 2", "# pass 2.0")):
            with self.subTest(output=output[-80:]):
                self.reject(output, "missing|duplicate|malformed")

    def test_plan_and_discovered_case_number_are_checked(self):
        self.reject(tap().replace("1..2", "1..1"), "plan/named discovery")
        self.reject(tap() + "1..2\n", "duplicate plan")
        self.reject(tap().replace("ok 2 -", "ok 3 -"), "not consecutive")

    def test_missing_duplicate_or_unmatched_case_names_are_rejected(self):
        for output in (tap().replace("# Subtest: fake case 2\n", ""),
                       tap().replace("fake case 2", "fake case 1"),
                       tap().replace("# Subtest: fake case 2", "# Subtest: wrong name"),
                       tap() + "# Subtest: trailing case\n"):
            with self.subTest(output=output[-100:]):
                self.reject(output, "name|subtest")

    def test_unfinished_diagnostic_is_rejected(self):
        self.reject(tap().replace("  ...\n", "", 1), "diagnostic")

    def test_expected_count_itself_must_be_positive(self):
        with self.assertRaisesRegex(gate.GateError, "must be positive"):
            gate.validate_tap("fake.test.mjs", 0, tap(0), 0)

    @mock.patch.object(gate.subprocess, "run")
    def test_runner_invokes_real_exact_file_without_substituting_implementation(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, tap(), "fake warning\n")
        with mock.patch("builtins.print"):
            gate.run_file(Path("/owned/tests"), "fake.test.mjs", 2)
        run.assert_called_once_with(["node", "--experimental-vm-modules", "--test", "fake.test.mjs"],
                                    cwd=Path("/owned/tests"), timeout=60, text=True, capture_output=True)

    @mock.patch.object(gate.subprocess, "run")
    def test_runner_preserves_original_failure_diagnostics(self, run):
        run.return_value = subprocess.CompletedProcess([], 1, "fake assertion output\n", "fake stderr\n")
        with self.assertRaises(gate.GateError) as raised:
            gate.run_file(Path("/owned/tests"), "fake.test.mjs", 2)
        self.assertEqual(raised.exception.diagnostics, "fake assertion output\nfake stderr\n")

    @mock.patch.object(gate.subprocess, "run")
    def test_timeout_preserves_partial_diagnostics_and_fails(self, run):
        run.side_effect = subprocess.TimeoutExpired("node", 60, output=b"fake partial\n", stderr=b"fake timeout\n")
        with self.assertRaisesRegex(gate.GateError, "fake.test.mjs:.*exceeded 60") as raised:
            gate.run_file(Path("/owned/tests"), "fake.test.mjs", 2)
        self.assertEqual(raised.exception.diagnostics, "fake partial\nfake timeout\n")

    @mock.patch.object(gate.subprocess, "run")
    def test_missing_node_is_named_failure_not_skip(self, run):
        run.side_effect = FileNotFoundError("fake missing Node")
        with self.assertRaisesRegex(gate.GateError, "fake.test.mjs:.*could not execute"):
            gate.run_file(Path("/owned/tests"), "fake.test.mjs", 2)

    def test_fixed_real_file_counts_are_preserved(self):
        self.assertEqual(gate.GROUPS, {
            "transport": (("sdk-transport.test.mjs", 35), ("smart-data-controller.test.mjs", 30)),
            "launcher": (("launcher-model.test.mjs", 2), ("launcher-owner.test.mjs", 51)),
        })


if __name__ == "__main__":
    unittest.main()
