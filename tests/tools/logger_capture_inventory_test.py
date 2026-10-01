#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail closed if logger captures or the pubsub corpus lose isolation."""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys
from unittest import mock
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
PUBSUB_TAG = "[isolated-pubsub]"
PUBSUB_CASE = "QuickJS pubsub has a bounded deterministic admission and callback corpus"
PUBSUB_GATE = "plinth_tests_pubsub_corpus"
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
    pubsub: dict[str, str],
) -> None:
    if set(isolated) != EXPECTED_NAMES:
        raise AssertionError(
            f"isolated capture inventory changed: missing={sorted(EXPECTED_NAMES - isolated.keys())} "
            f"extra={sorted(isolated.keys() - EXPECTED_NAMES)}"
        )
    if any(ISOLATION_TAG not in tags for tags in isolated.values()):
        raise AssertionError("capture fixture lacks its isolation tag")
    if set(pubsub) != {PUBSUB_CASE} or PUBSUB_TAG not in pubsub[PUBSUB_CASE]:
        raise AssertionError("pubsub isolation requires exactly its named tagged corpus")
    if ISOLATION_TAG in pubsub[PUBSUB_CASE]:
        raise AssertionError("pubsub corpus must not enter the logger capture group")
    if EXPECTED_NAMES.intersection(grouped) or PUBSUB_CASE in grouped or any(
        ISOLATION_TAG in tags or PUBSUB_TAG in tags for tags in grouped.values()
    ):
        raise AssertionError("grouped JS selector contains a logger capture fixture")
    if not EXPECTED_NAMES.issubset(baseline):
        raise AssertionError("baseline JS discovery lost a logger capture fixture")
    if any(baseline[name] != isolated[name] for name in EXPECTED_NAMES):
        raise AssertionError("baseline JS logger tags disagree with isolated discovery")
    baseline_pubsub = {
        name: tags for name, tags in baseline.items() if PUBSUB_TAG in tags
    }
    if baseline_pubsub != pubsub:
        raise AssertionError("baseline JS discovery lost or changed the pubsub corpus")
    exclusions = EXPECTED_NAMES | {PUBSUB_CASE}
    expected_grouped = {
        name: tags for name, tags in baseline.items() if name not in exclusions
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
    matches = [gate for gate in gates if gate.get("name") == PUBSUB_GATE]
    if len(matches) != 1:
        raise AssertionError("named strict pubsub gate must occur exactly once")
    command = matches[0].get("command", [])
    if (
        not isinstance(command, list)
        or len(command) != 8
        or not all(isinstance(argument, str) for argument in command)
        or Path(command[0]).resolve() != Path(sys.executable).resolve()
        or Path(command[1]).resolve() != Path(__file__).with_name("verify_catch_case.py").resolve()
        or command[2] != "--binary"
        or Path(command[3]).resolve() != binary.resolve()
        or command[4:] != ["--case", PUBSUB_CASE, "--timeout", "60"]
    ):
        raise AssertionError("named pubsub gate must use its exact strict verifier command")
    deadlines = [
        prop.get("value") for prop in matches[0].get("properties", [])
        if prop.get("name") == "TIMEOUT"
    ]
    if len(deadlines) != 1 or type(deadlines[0]) not in (int, float) or deadlines[0] != 75:
        raise AssertionError("named pubsub gate must have the exact outer deadline")


def self_test() -> int:
    isolated = {name: f"[js]{ISOLATION_TAG}" for name in EXPECTED_NAMES}
    grouped_selector = "[js] ~[batch-transaction] ~[isolated-logger] ~[isolated-pubsub]"
    binary = Path("plinth_tests")
    grouped = {"ordinary JS fixture": "[js][ordinary]"}
    pubsub = {PUBSUB_CASE: f"[js][pubsub][corpus]{PUBSUB_TAG}"}
    baseline = {**isolated, **grouped, **pubsub}
    gates = [
        {"name": name, "command": ["plinth_tests", selector]}
        for name, selector in {**EXPECTED_GATES, "plinth_tests_js": grouped_selector}.items()
    ]
    pubsub_gate = {
        "name": PUBSUB_GATE,
        "command": [sys.executable, str(Path(__file__).with_name("verify_catch_case.py")),
                    "--binary", str(binary), "--case", PUBSUB_CASE, "--timeout", "60"],
        "properties": [{"name": "TIMEOUT", "value": 75.0}],
    }
    # Keep the grouped JS gate last so the original logger controls retain their meaning.
    gates.insert(-1, pubsub_gate)
    validate_inventory(isolated, grouped, gates, grouped_selector, binary, baseline, pubsub)
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
            validate_inventory(*candidate, grouped_selector, binary, baseline, pubsub)
        except AssertionError:
            continue
        raise AssertionError("inventory negative control was not rejected")
    extended = [
        (baseline, {}),
        (baseline, {**pubsub, "unexpected pubsub": PUBSUB_TAG}),
        (baseline, {PUBSUB_CASE: "[js][corpus]"}),
        (baseline, {PUBSUB_CASE: pubsub[PUBSUB_CASE] + ISOLATION_TAG}),
        ({name: tags for name, tags in baseline.items() if name != PUBSUB_CASE}, pubsub),
        ({**baseline, PUBSUB_CASE: "[js][corpus]"}, pubsub),
        ({**baseline, "unexpected pubsub": PUBSUB_TAG}, pubsub),
        ({**baseline, next(iter(EXPECTED_NAMES)): "[js]"}, pubsub),
    ]
    rejected = len(malformed)
    for candidate_baseline, candidate_pubsub in extended:
        try:
            validate_inventory(isolated, grouped, gates, grouped_selector, binary,
                               candidate_baseline, candidate_pubsub)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub inventory negative control was not rejected")
    bad_gates = [
        [gate for gate in gates if gate["name"] != PUBSUB_GATE],
        gates + [copy.deepcopy(pubsub_gate)],
    ]
    for index, replacement in (
        (0, "true"), (1, "stale-verifier.py"), (2, "--wrong-binary"),
        (3, "stale-tests"), (4, "--wrong-case"), (5, "stale case"),
        (6, "--wrong-timeout"), (7, "61"),
    ):
        mutated = copy.deepcopy(gates)
        mutated[-2]["command"][index] = replacement
        bad_gates.append(mutated)
    for properties in ([], [{"name": "TIMEOUT", "value": 74}],
                       [{"name": "TIMEOUT", "value": "75"}],
                       [{"name": "TIMEOUT", "value": 75}] * 2):
        mutated = copy.deepcopy(gates)
        mutated[-2]["properties"] = properties
        bad_gates.append(mutated)
    for candidate_gates in bad_gates:
        try:
            validate_inventory(isolated, grouped, candidate_gates, grouped_selector,
                               binary, baseline, pubsub)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub gate negative control was not rejected")
    for candidate_grouped in ({**grouped, **pubsub},
                              {**grouped, "unexpected isolated": PUBSUB_TAG}):
        try:
            validate_inventory(isolated, candidate_grouped, gates, grouped_selector,
                               binary, baseline, pubsub)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub grouped leakage was not rejected")
    document = "<MatchingTests><TestCase><Name>fixture</Name><Tags>[js]</Tags></TestCase></MatchingTests>"
    if parse_cases(document) != {"fixture": "[js]"}:
        raise AssertionError("Catch2 XML discovery parsing failed")
    # Both discovery paths must propagate unavailable/nonzero subprocess failure.
    for operation in (lambda: discover(binary, PUBSUB_TAG),
                      lambda: run_json(["synthetic-ctest"])):
        for failure in (FileNotFoundError("synthetic unavailable"),
                        subprocess.CalledProcessError(7, ["synthetic"])):
            with mock.patch.object(subprocess, "run", side_effect=failure):
                try:
                    operation()
                except (FileNotFoundError, subprocess.CalledProcessError):
                    rejected += 1
                    continue
            raise AssertionError("unavailable or nonzero discovery did not fail closed")
    return rejected


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
    pubsub = discover(args.binary, PUBSUB_TAG)
    grouped = discover(args.binary, args.grouped_selector)
    baseline = discover(args.binary, GROUPED_JS_BASELINE)
    gates = run_json(
        [args.ctest_command, "--test-dir", str(args.build_dir), "--show-only=json-v1"]
    )["tests"]
    validate_inventory(isolated, grouped, gates, args.grouped_selector, args.binary,
                       baseline, pubsub)
    print(
        "logger/pubsub inventory: exactly 5 logger fixtures and 1 pubsub corpus isolated, 0 grouped, "
        f"{len(grouped)} ordinary JS fixtures retained, one of each named gate"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
