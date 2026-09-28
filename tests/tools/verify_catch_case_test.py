#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hermetic checks for the strict, file-backed Catch2 XML verifier."""

import contextlib
import copy
import ctypes
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET


SPEC = importlib.util.spec_from_file_location(
    "verify_catch_case", Path(__file__).with_name("verify_catch_case.py")
)
VERIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFIER)
CASE = "WebSocket fixture waits for both owned listening sockets"
# Independent fixture from the pinned Catch2 XML-v3 reporter schema.
REPORT = f"""<?xml version="1.0" encoding="UTF-8"?>
<Catch2TestRun xml-format-version="3" catch2-version="3.7.1">
  <TestCase name="{CASE}">
    <OverallResult success="true" skips="0"/>
  </TestCase>
  <OverallResults successes="45" failures="0" expectedFailures="0" skips="0"/>
  <OverallResultsCases successes="1" failures="0" expectedFailures="0" skips="0"/>
</Catch2TestRun>
""".encode()
BROKEN_CONSOLE = (
    "All tests passed (45 assertions in 1 test case"
    "[synthetic logger] new connection\n"
    "[synthetic logger] skipped means a log word, not a Catch SKIP\n"
)


def document():
    return ET.fromstring(REPORT)


class ReportTest(unittest.TestCase):
    def reject(self, root):
        with self.assertRaises(ValueError):
            VERIFIER.verify_report(ET.tostring(root), CASE)

    def test_positive_and_large_count_with_nested_sections(self):
        VERIFIER.verify_report(REPORT, CASE)
        root = document()
        root.find("OverallResults").set("successes", "1000000000000")
        section = ET.SubElement(root.find("TestCase"), "Section", name="outer")
        section = ET.SubElement(section, "Section", name="inner")
        ET.SubElement(section, "Expression", success="true", type="REQUIRE")
        ET.SubElement(section, "OverallResults", successes="1", failures="0",
                      expectedFailures="0", skipped="false")
        VERIFIER.verify_report(ET.tostring(root), CASE)

    def test_empty_malformed_truncated_and_foreign_schema(self):
        for report in (b"", b"not XML", REPORT[:-24], b"<MatchingTests/>",
                       REPORT.replace(b'xml-format-version="3"', b'xml-format-version="2"'),
                       REPORT.replace(b'xml-format-version="3"', b"")):
            with self.subTest(report=report[:80]), self.assertRaises(ValueError):
                VERIFIER.verify_report(report, CASE)

    def test_exact_case_identity_and_cardinality(self):
        root = document()
        root.find("TestCase").set("name", "different case")
        self.reject(root)
        root = document()
        root.find("TestCase").attrib.pop("name")
        self.reject(root)
        root = document()
        root.append(copy.deepcopy(root.find("TestCase")))
        self.reject(root)
        root = document()
        root.find("TestCase").append(copy.deepcopy(root.find("TestCase")))
        self.reject(root)

    def test_missing_and_duplicate_mandatory_records(self):
        for tag in ("TestCase", "OverallResults", "OverallResultsCases", "OverallResult"):
            for duplicate in (False, True):
                with self.subTest(tag=tag, duplicate=duplicate):
                    root = document()
                    parent = root.find("TestCase") if tag == "OverallResult" else root
                    element = parent.find(tag)
                    if duplicate:
                        parent.append(copy.deepcopy(element))
                    else:
                        parent.remove(element)
                    self.reject(root)

    def test_all_required_counts_are_strict_unsigned_decimal(self):
        records = (("OverallResults", ("successes", "failures", "expectedFailures", "skips")),
                   ("OverallResultsCases", ("successes", "failures", "expectedFailures", "skips")),
                   ("TestCase/OverallResult", ("skips",)))
        for path, fields in records:
            for field in fields:
                for value in (None, "", "-1", "+1", "1.0", "1,000", " 0", "zero", "\u0660"):
                    with self.subTest(path=path, field=field, value=value):
                        root = document()
                        element = root.find(path)
                        if value is None:
                            element.attrib.pop(field)
                        else:
                            element.set(field, value)
                        self.reject(root)

    def test_zero_assertions_wrong_case_total_and_nonzero_failure_totals(self):
        mutations = [("OverallResults", "successes", "0"),
                     ("OverallResultsCases", "successes", "0"),
                     ("OverallResultsCases", "successes", "2")]
        mutations += [(path, field, "1") for path in ("OverallResults", "OverallResultsCases")
                      for field in ("failures", "expectedFailures", "skips")]
        mutations.append(("TestCase/OverallResult", "skips", "1"))
        for path, field, value in mutations:
            with self.subTest(path=path, field=field):
                root = document()
                root.find(path).set(field, value)
                self.reject(root)

    def test_case_success_flag_must_be_explicitly_true(self):
        for value in (None, "false", "TRUE", "1"):
            with self.subTest(value=value):
                root = document()
                result = root.find("TestCase/OverallResult")
                if value is None:
                    result.attrib.pop("success")
                else:
                    result.set("success", value)
                self.reject(root)

    def test_section_skips_failures_and_invalid_counts(self):
        for field, value in (("skipped", None), ("skipped", "true"), ("skipped", "0"),
                             ("failures", "1"), ("expectedFailures", "1"),
                             ("successes", "-1"), ("failures", None),
                             ("expectedFailures", None), ("successes", None)):
            with self.subTest(field=field, value=value):
                root = document()
                section = ET.SubElement(root.find("TestCase"), "Section", name="nested")
                totals = ET.SubElement(section, "OverallResults", successes="1", failures="0",
                                       expectedFailures="0", skipped="false")
                if value is None:
                    totals.attrib.pop(field)
                else:
                    totals.set(field, value)
                self.reject(root)

    def test_explicit_failure_records_and_unsuccessful_expressions(self):
        for tag in ("Skip", "Failure", "Exception", "FatalErrorCondition"):
            with self.subTest(tag=tag):
                root = document()
                ET.SubElement(root.find("TestCase"), tag).text = "synthetic failure"
                self.reject(root)
        for value in (None, "false"):
            with self.subTest(expression_success=value):
                root = document()
                expression = ET.SubElement(root.find("TestCase"), "Expression", type="REQUIRE")
                if value is not None:
                    expression.set("success", value)
                self.reject(root)


@unittest.skipUnless(os.name == "posix", "process-group contract requires POSIX")
class ProcessTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="plinth-verifier-selftest-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def fake_binary(self, mode):
        binary = self.directory / f"fake-{mode}"
        probe = self.directory / f"probe-{mode}.jsonl"
        # Generated executable and reports are synthetic, task-owned artifacts.
        source = f"""#!{sys.executable}
import json, os, resource, signal, sys
from pathlib import Path
mode = {mode!r}
case = {CASE!r}
report_bytes = {REPORT!r}
probe = Path({str(probe)!r})
if sys.argv[1] != case or sys.argv[2:5] != ['--reporter', 'xml', '--out']:
    sys.exit(90)
report = Path(sys.argv[5])
if not report.is_absolute() or not report.parent.is_dir() or report.exists():
    sys.exit(91)
if mode != 'missing':
    report.write_bytes(b'<Catch2TestRun' if mode == 'malformed' else report_bytes)
print({BROKEN_CONSOLE!r}, end='', flush=True)
print('synthetic stderr retained', file=sys.stderr, flush=True)
child = None
if mode == 'timeout':
    child = os.fork()
    if child == 0:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        print('synthetic descendant holds pipe', flush=True)
        while True:
            signal.pause()
with probe.open('a', encoding='utf-8') as output:
    output.write(json.dumps({{'report': str(report), 'pid': os.getpid(),
                             'pgid': os.getpgrp(), 'child': child}}) + '\\n')
if mode == 'nonzero':
    sys.exit(7)
if mode == 'crash':
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.kill(os.getpid(), signal.SIGABRT)
sys.exit(0)
"""
        binary.write_text(source, encoding="utf-8")
        binary.chmod(0o700)
        return binary, probe

    def run_fake(self, mode, timeout=3):
        binary, probe = self.fake_binary(mode)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = VERIFIER.run_case(str(binary), CASE, timeout)
        records = [json.loads(line) for line in probe.read_text(encoding="utf-8").splitlines()]
        for record in records:
            self.assertFalse(Path(record["report"]).parent.exists(), "owned directory not cleaned")
        return result, stdout.getvalue(), stderr.getvalue(), records

    def test_fresh_xml_ignores_broken_console_and_preserves_logs(self):
        result, stdout, stderr, records = self.run_fake("positive")
        self.assertEqual(result, 0)
        self.assertIn(BROKEN_CONSOLE, stdout)
        self.assertIn("synthetic stderr retained", stderr)
        self.assertEqual(len(records), 1)
        self.assertNotEqual(records[0]["pgid"], os.getpgrp())
        self.assertEqual(records[0]["pgid"], records[0]["pid"])

    def test_repeated_calls_own_distinct_fresh_reports(self):
        binary, probe = self.fake_binary("positive")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            for _ in range(2):
                self.assertEqual(VERIFIER.run_case(str(binary), CASE, 3), 0)
        records = [json.loads(line) for line in probe.read_text(encoding="utf-8").splitlines()]
        self.assertEqual(len(records), 2)
        self.assertNotEqual(records[0]["report"], records[1]["report"])
        for record in records:
            self.assertFalse(Path(record["report"]).parent.exists())

    def test_valid_report_cannot_hide_nonzero_or_crash_exit(self):
        for mode in ("nonzero", "crash"):
            with self.subTest(mode=mode):
                result, _, stderr, _ = self.run_fake(mode)
                self.assertEqual(result, 1)
                self.assertIn("exited with status", stderr)

    def test_missing_or_malformed_report_cannot_use_console_or_stale_pass(self):
        stale = self.directory / "report.xml"
        stale.write_bytes(REPORT)
        for mode in ("missing", "malformed"):
            with self.subTest(mode=mode):
                result, stdout, stderr, records = self.run_fake(mode)
                self.assertEqual(result, 1)
                self.assertIn(BROKEN_CONSOLE, stdout)
                self.assertNotEqual(Path(records[0]["report"]), stale)
                self.assertIn("report", stderr)
        self.assertEqual(stale.read_bytes(), REPORT)

    @unittest.skipUnless(sys.platform.startswith("linux"), "bounded orphan reap requires Linux")
    def test_timeout_kills_whole_group_with_pipe_holding_descendant(self):
        # Adopt the synthetic orphan so this test, rather than PID 1, reaps it.
        libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        self.assertEqual(libc.prctl(37, ctypes.byref(previous), 0, 0, 0), 0)
        self.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        child = None
        reaped = False
        probe = self.directory / "probe-timeout.jsonl"
        try:
            started = time.monotonic()
            result, stdout, stderr, records = self.run_fake("timeout", timeout=1)
            self.assertEqual(len(records), 1)
            child = records[0]["child"]
            self.assertIsInstance(child, int)
            self.assertGreater(child, 1)
            self.assertEqual(result, 1)
            self.assertLess(time.monotonic() - started, 4)
            self.assertIn("timed out after 1s", stderr)
            self.assertIn("synthetic descendant holds pipe", stdout)
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                waited, status = os.waitpid(child, os.WNOHANG)
                if waited == child:
                    reaped = True
                    self.assertTrue(os.WIFSIGNALED(status))
                    self.assertEqual(os.WTERMSIG(status), signal.SIGKILL)
                    break
                time.sleep(0.005)  # bounded reap observation, never a source readiness delay
            self.assertTrue(reaped, "helper did not kill its pipe-holding descendant")
        finally:
            # Even a failing control cannot leave its synthetic process behind.
            try:
                if child is None and probe.exists():
                    child = json.loads(probe.read_text(encoding="utf-8").splitlines()[-1])["child"]
                if child is not None and not reaped:
                    self.assertIsInstance(child, int)
                    self.assertGreater(child, 1)
                    # Check adoption before signalling: never target a PID that
                    # was already reaped and could now belong to someone else.
                    waited, _ = os.waitpid(child, os.WNOHANG)
                    reaped = waited == child
                    if not reaped:
                        try:
                            os.kill(child, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    deadline = time.monotonic() + 1
                    while not reaped and time.monotonic() < deadline:
                        waited, _ = os.waitpid(child, os.WNOHANG)
                        reaped = waited == child
                        if not reaped:
                            time.sleep(0.005)
                    self.assertTrue(reaped, "synthetic descendant cleanup exceeded its bound")
            finally:
                self.assertEqual(libc.prctl(36, previous.value, 0, 0, 0), 0)


if __name__ == "__main__":
    unittest.main()
