#!/usr/bin/env python3
"""Fail closed if the pinned scanner's fake cursor allowlist becomes broad."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path("tests/kernel/frontend/surface_protocol_test.cpp")
FIXTURES = (
    ("", FIXTURE),
    ("admin_defaults_", Path("tests/kernel/frontend/admin_defaults_contract_test.cpp")),
)
OTHER_PATH = Path("tests/kernel/frontend/not_allowed_test.cpp")
REPORT_MAX_BYTES = 1024 * 1024


class RegressionFailure(RuntimeError):
    pass


def fixture_inputs(fixture):
    source = (ROOT / fixture).read_text(encoding="utf-8")
    match = re.search(
        r'\bconstexpr\s+std::string_view\s+PAYLOAD\s*=\s*R"\((.*?)\)";',
        source, re.DOTALL,
    )
    if match is None:
        raise RegressionFailure("known cursor fixture payload is missing")
    try:
        credential = json.loads(match.group(1))["credential"]
    except (ValueError, KeyError, TypeError):
        raise RegressionFailure("known cursor fixture payload is malformed") from None
    if (not isinstance(credential, str)
            or re.fullmatch(r"[A-Za-z0-9_-]{43}", credential) is None
            or source.count(credential) != 1):
        raise RegressionFailure("known cursor fixture fingerprint changed shape")
    # Derive a distinct equally shaped fake value without another embedded
    # high-entropy literal. Never emit either fingerprint in diagnostics.
    changed = ("t" if credential[0] != "t" else "u") + credential[1:]
    return source, source.replace(credential, changed, 1)


def check_control(scanner, root, name, path, content, expected_count):
    context = root / name
    target = context / path
    target.parent.mkdir(parents=True)
    target.write_text(content, encoding="utf-8")
    target.chmod(0o600)
    report = root / (name + ".json")
    try:
        result = subprocess.run(
            [str(scanner), "dir", "--redact", "--config", str(ROOT / ".gitleaks.toml"),
             "--report-format", "json", "--report-path", str(report), "."],
            cwd=context, capture_output=True, timeout=30, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise RegressionFailure(name + ": scanner did not complete") from None
    expected_exit = 1 if expected_count else 0
    if result.returncode != expected_exit:
        raise RegressionFailure(name + ": unexpected scanner exit code " + str(result.returncode))
    try:
        if report.stat().st_size > REPORT_MAX_BYTES:
            raise RegressionFailure(name + ": scanner report exceeds bound")
        records = json.loads(report.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError):
        raise RegressionFailure(name + ": scanner report is unavailable or malformed") from None
    if not isinstance(records, list) or len(records) != expected_count:
        raise RegressionFailure(name + ": unexpected finding count")
    # Only rule/file/count are consumed; report matches and secret fields are
    # neither displayed nor used as an authority to ignore another finding.
    expected = {"rule": "generic-api-key", "file": path.as_posix()}
    for record in records:
        if not isinstance(record, dict) or {
            "rule": record.get("RuleID"), "file": record.get("File")
        } != expected:
            raise RegressionFailure(name + ": unexpected finding rule or file")
    return {"control": name, "findings": expected_count,
            "rule": expected["rule"] if expected_count else None,
            "file": expected["file"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scanner", required=True, type=Path,
                        help="already downloaded and checksum-verified pinned scanner")
    args = parser.parse_args()
    try:
        scanner = args.scanner.resolve(strict=True)
        if not scanner.is_file() or not os.access(scanner, os.X_OK):
            raise RegressionFailure("pinned scanner is not executable")
        with tempfile.TemporaryDirectory(prefix="plinth-gitleaks-allowlist-") as temporary:
            root = Path(temporary)
            controls = []
            for prefix, fixture in FIXTURES:
                original, changed = fixture_inputs(fixture)
                controls.extend([
                    check_control(scanner, root, prefix + "exact_fixture_allowed", fixture, original, 0),
                    check_control(scanner, root, prefix + "same_value_other_path", OTHER_PATH, original, 1),
                    check_control(scanner, root, prefix + "other_value_same_path", fixture, changed, 1),
                ])
        if len(controls) != 6:
            raise RegressionFailure("unexpected cursor fixture control inventory")
        print(json.dumps({"gitleaksAllowlistControls": 6, "passed": 6, "controls": controls}, sort_keys=True))
    except (OSError, UnicodeError, RegressionFailure) as error:
        # No scanner stdout/stderr, match text or fingerprint is printed.
        print("gitleaks allowlist regression failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
