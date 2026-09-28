#!/usr/bin/env python3
"""Require one real, passing Catch2 case in focused sanitizer/image gates."""

import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def verify_report(report: bytes, case: str) -> None:
    """Validate the pinned Catch2 XML v3 report, never its noisy console output."""
    try:
        root = ET.fromstring(report)
    except ET.ParseError as error:
        raise ValueError("Catch2 report is not complete XML") from error
    if root.tag != "Catch2TestRun" or root.get("xml-format-version") != "3":
        raise ValueError("Catch2 report has an unsupported root or format")

    def one(parent: ET.Element, tag: str) -> ET.Element:
        matches = parent.findall(tag)
        if len(matches) != 1:
            raise ValueError(f"Catch2 report requires exactly one {tag}")
        return matches[0]

    def count(element: ET.Element, name: str) -> int:
        value = element.get(name, "")
        if re.fullmatch(r"[0-9]+", value) is None:
            raise ValueError(f"Catch2 report has an invalid {name} count")
        return int(value)

    test = one(root, "TestCase")
    if test.get("name") != case or len(list(root.iter("TestCase"))) != 1:
        raise ValueError("Catch2 report did not run exactly the requested case")
    result = one(test, "OverallResult")
    if result.get("success") != "true" or count(result, "skips") != 0:
        raise ValueError("Catch2 case failed or skipped")
    assertions = one(root, "OverallResults")
    cases = one(root, "OverallResultsCases")
    if count(assertions, "successes") <= 0 or count(cases, "successes") != 1:
        raise ValueError("Catch2 case did not report one passing case with assertions")
    for totals in (assertions, cases):
        if any(
            count(totals, field) != 0
            for field in ("failures", "expectedFailures", "skips")
        ):
            raise ValueError("Catch2 report contains failures or skips")
    for totals in test.iter("OverallResults"):
        # Section summaries use a boolean 'skipped', unlike run-level counts.
        count(totals, "successes")
        if (
            count(totals, "failures") != 0
            or count(totals, "expectedFailures") != 0
            or totals.get("skipped") != "false"
        ):
            raise ValueError("Catch2 section failed or skipped")
    for record in root.iter():
        if record.tag in ("Skip", "Failure", "Exception", "FatalErrorCondition"):
            raise ValueError(f"Catch2 report contains {record.tag}")
        if record.tag == "Expression" and record.get("success") != "true":
            raise ValueError("Catch2 report contains an unsuccessful assertion")


def forward_report(path: Path) -> bytes | None:
    try:
        report = path.read_bytes()
    except OSError as error:
        print(f"Catch2 report is unavailable: {error}", file=sys.stderr)
        return None
    # Keep assertion diagnostics in the calling CI log before temporary cleanup.
    sys.stdout.write(report.decode("utf-8", errors="replace"))
    return report


def run_case(binary: str, case: str, timeout: int) -> int:
    # An absent report inside a fresh owned directory cannot reuse a stale pass.
    with tempfile.TemporaryDirectory(prefix="plinth-catch-case-") as temporary:
        return run_with_report(binary, case, timeout, Path(temporary) / "report.xml")


def run_with_report(binary: str, case: str, timeout: int, report_path: Path) -> int:
    process = subprocess.Popen(
        [binary, case, "--reporter", "xml", "--out", str(report_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        # Catch2's lifecycle cases spawn the production binary in the same
        # process group. Kill the group so an outer CTest timeout cannot leave
        # a server behind while later cases reset the shared database.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            forward_report(report_path)
            print("Catch2 case output pipe remained open after group kill", file=sys.stderr)
            return 1
        sys.stdout.write(stdout)
        sys.stderr.write(stderr)
        forward_report(report_path)
        print(f"Catch2 case timed out after {timeout}s", file=sys.stderr)
        return 1

    sys.stdout.write(stdout)
    sys.stderr.write(stderr)
    report = forward_report(report_path)

    # Production-coordinator teardown runs after Catch writes the XML summary.
    # Its nonzero exit must fail even if all assertions have already passed.
    if process.returncode != 0:
        print(f"Catch2 case exited with status {process.returncode}", file=sys.stderr)
        return 1
    if report is None:
        return 1
    try:
        verify_report(report, case)
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--timeout", required=True, type=int)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return run_case(args.binary, args.case, args.timeout)


if __name__ == "__main__":
    sys.exit(main())
