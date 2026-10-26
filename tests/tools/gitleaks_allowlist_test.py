#!/usr/bin/env python3
"""Fail closed if the pinned scanner's exact fixture allowlists become broad."""

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
MODULE_FIXTURE = Path("tests/browser/groups-admin-production.mjs")
MODULE_OTHER_PATH = Path("tests/browser/not_allowed_test.mjs")
CONTROL_NAMES = (
    "exact_fixture_allowed", "same_value_other_path", "other_value_same_path",
    "admin_defaults_exact_fixture_allowed", "admin_defaults_same_value_other_path",
    "admin_defaults_other_value_same_path", "admin_module_exact_fixture_allowed",
    "admin_module_same_value_other_path", "admin_module_other_value_same_path",
)
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


def module_fixture_inputs():
    source = (ROOT / MODULE_FIXTURE).read_text(encoding="utf-8")
    blocks = re.findall(
        r"\bconst\s+OLD_MODULES\s*=\s*Object\.freeze\(\{(.*?)\}\);",
        source, re.DOTALL,
    )
    if len(blocks) != 1:
        raise RegressionFailure("known module provenance fixture is missing or ambiguous")
    values = re.findall(r"^\s*api:\s*'([0-9a-f]{64})',?\s*$", blocks[0], re.MULTILINE)
    if len(values) != 1 or source.count(values[0]) != 1:
        raise RegressionFailure("known module provenance fixture changed shape")
    fingerprint = values[0]
    # Keep the real baseline module hash intact and derive one different,
    # equally shaped negative value without embedding another hash literal.
    changed = ("0" if fingerprint[0] != "0" else "1") + fingerprint[1:]
    return source, source.replace(fingerprint, changed, 1)


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
            original, changed = module_fixture_inputs()
            controls.extend([
                check_control(scanner, root, "admin_module_exact_fixture_allowed",
                              MODULE_FIXTURE, original, 0),
                check_control(scanner, root, "admin_module_same_value_other_path",
                              MODULE_OTHER_PATH, original, 1),
                check_control(scanner, root, "admin_module_other_value_same_path",
                              MODULE_FIXTURE, changed, 1),
            ])
        if tuple(control["control"] for control in controls) != CONTROL_NAMES:
            raise RegressionFailure("unexpected exact fixture control inventory")
        print(json.dumps({"gitleaksAllowlistControls": 9, "passed": 9, "controls": controls}, sort_keys=True))
    except (OSError, UnicodeError, RegressionFailure) as error:
        # No scanner stdout/stderr, match text or fingerprint is printed.
        print("gitleaks allowlist regression failed: " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
