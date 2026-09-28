#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail closed if default-logger capture fixtures lose process isolation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET


EXPECTED_NAMES = frozenset(
    {
        "stdlib: log.* forwards to plinth::log at the right level",
        "stdlib: log bindings preserve bytes and reject unserializable ctx",
        "stdlib: log.* preserves caller-supplied ctx and does not inject kernel fields",
        "QuickJS log callbacks have a bounded deterministic corpus",
        "RuntimePool release on cancelled context routes to destroy",
    }
)
ISOLATION_TAG = "[isolated-logger]"
GROUPED_JS_BASELINE = "[js] ~[batch-transaction]"
EXPECTED_GATES = {
    "plinth_tests_logger_capture_isolated": ISOLATION_TAG,
    "plinth_tests_log_regression": "[js][stdlib][log][regression]",
    "plinth_tests_log_corpus": "[js][stdlib][log][corpus]",
    "plinth_tests_log_oracle": "[js][stdlib][log][oracle]",
}


def parse_cases(document: str) -> dict[str, str]:
    root = ET.fromstring(document)
    if root.tag != "MatchingTests":
        raise AssertionError("unexpected Catch2 discovery root")
    cases: dict[str, str] = {}
    for case in root.findall("TestCase"):
        name = case.findtext("Name")
        tags = case.findtext("Tags")
        if not name or tags is None or name in cases:
            raise AssertionError("missing or duplicate Catch2 fixture identity")
        cases[name] = tags
    return cases


def validate_inventory(
    isolated: dict[str, str],
    grouped: dict[str, str],
    gates: list[dict],
    grouped_selector: str,
    binary: Path,
    baseline: dict[str, str],
) -> None:
    if set(isolated) != EXPECTED_NAMES:
        raise AssertionError(
            f"isolated capture inventory changed: missing={sorted(EXPECTED_NAMES - isolated.keys())} "
            f"extra={sorted(isolated.keys() - EXPECTED_NAMES)}"
        )
    if any(ISOLATION_TAG not in tags for tags in isolated.values()):
        raise AssertionError("capture fixture lacks its isolation tag")
    if EXPECTED_NAMES.intersection(grouped) or any(
        ISOLATION_TAG in tags for tags in grouped.values()
    ):
        raise AssertionError("grouped JS selector contains a logger capture fixture")
    if not EXPECTED_NAMES.issubset(baseline):
        raise AssertionError("baseline JS discovery lost a logger capture fixture")
    expected_grouped = {
        name: tags for name, tags in baseline.items() if name not in EXPECTED_NAMES
    }
    if grouped != expected_grouped:
        raise AssertionError("grouped JS discovery must retain every ordinary JS fixture")
    expected_gates = {**EXPECTED_GATES, "plinth_tests_js": grouped_selector}
    for name, selector in expected_gates.items():
        matches = [gate for gate in gates if gate.get("name") == name]
        if len(matches) != 1:
            raise AssertionError(f"named logger gate must occur once: {name}")
        command = matches[0].get("command", [])
        if (
            len(command) != 2
            or Path(command[0]).resolve() != binary.resolve()
            or command[1:] != [selector]
        ):
            raise AssertionError(f"named logger gate must occur once with its selector: {name}")


def self_test() -> int:
    isolated = {name: f"[js]{ISOLATION_TAG}" for name in EXPECTED_NAMES}
    grouped_selector = "[js] ~[batch-transaction] ~[isolated-logger]"
    binary = Path("plinth_tests")
    grouped = {"ordinary JS fixture": "[js][ordinary]"}
    baseline = {**isolated, **grouped}
    gates = [
        {"name": name, "command": ["plinth_tests", selector]}
        for name, selector in {**EXPECTED_GATES, "plinth_tests_js": grouped_selector}.items()
    ]
    validate_inventory(isolated, grouped, gates, grouped_selector, binary, baseline)
    malformed = [
        ({}, grouped, gates),
        ({**isolated, "unexpected": ISOLATION_TAG}, grouped, gates),
        ({name: "[js]" for name in EXPECTED_NAMES}, grouped, gates),
        (isolated, baseline, gates),
        (isolated, grouped, []),
        (isolated, grouped, gates + [gates[0]]),
        (isolated, grouped, gates[:-1]),
        (isolated, grouped, gates + [gates[-1]]),
        (isolated, {}, gates),
        (
            isolated,
            grouped,
            gates[:-1] + [{"name": "plinth_tests_js", "command": ["plinth_tests", "[js]"]}],
        ),
        (
            isolated,
            grouped,
            gates[:-1]
            + [{"name": "plinth_tests_js", "command": ["true", grouped_selector]}],
        ),
    ]
    for candidate in malformed:
        try:
            validate_inventory(*candidate, grouped_selector, binary, baseline)
        except AssertionError:
            continue
        raise AssertionError("inventory negative control was not rejected")
    document = "<MatchingTests><TestCase><Name>fixture</Name><Tags>[js]</Tags></TestCase></MatchingTests>"
    if parse_cases(document) != {"fixture": "[js]"}:
        raise AssertionError("Catch2 XML discovery parsing failed")
    return len(malformed)


def run_json(command: list[str]) -> dict:
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=30)
    return json.loads(result.stdout)


def discover(binary: Path, selector: str) -> dict[str, str]:
    result = subprocess.run(
        [str(binary), selector, "--list-tests", "--reporter", "xml"],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return parse_cases(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--grouped-selector")
    parser.add_argument("--ctest-command")
    parser.add_argument("--build-dir", type=Path)
    args = parser.parse_args()
    negative_controls = self_test()
    if args.self_test:
        print(f"logger inventory self-test: {negative_controls} negative controls rejected")
        return 0
    if not all((args.binary, args.grouped_selector, args.ctest_command, args.build_dir)):
        parser.error("binary, grouped selector, CTest command, and build directory are required")
    isolated = discover(args.binary, ISOLATION_TAG)
    grouped = discover(args.binary, args.grouped_selector)
    baseline = discover(args.binary, GROUPED_JS_BASELINE)
    gates = run_json(
        [args.ctest_command, "--test-dir", str(args.build_dir), "--show-only=json-v1"]
    )["tests"]
    validate_inventory(isolated, grouped, gates, args.grouped_selector, args.binary, baseline)
    print(
        "logger capture inventory: exactly 5 isolated fixtures, 0 grouped, "
        f"{len(grouped)} ordinary JS fixtures retained, one of each named gate"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
