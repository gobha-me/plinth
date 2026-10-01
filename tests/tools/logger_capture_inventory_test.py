#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail closed if logger captures or pubsub/cap/database corpora lose isolation."""

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
CAP_TAG = "[isolated-cap]"
CAP_CASE = "QuickJS capability calls have a bounded deterministic admission and batch corpus"
CAP_GATE = "plinth_tests_cap_corpus"
DB_ADMISSION_TAG = "[isolated-db-admission]"
DB_ADMISSION_CASE = "QuickJS ordinary database calls have a bounded deterministic admission and ownership corpus"
DB_ADMISSION_GATE = "plinth_tests_db_admission_corpus"
GROUPED_JS_BASELINE = "[js] ~[batch-transaction]"
GROUPED_JS_SELECTOR = GROUPED_JS_BASELINE + " ~[isolated-logger] ~[isolated-pubsub] ~[isolated-cap] ~[isolated-db-admission]"
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
    cap: dict[str, str],
    db_admission: dict[str, str],
) -> None:
    if grouped_selector != GROUPED_JS_SELECTOR:
        raise AssertionError("grouped JS selector must preserve its broad baseline and exact exclusions")
    if set(isolated) != EXPECTED_NAMES:
        raise AssertionError(
            f"isolated capture inventory changed: missing={sorted(EXPECTED_NAMES - isolated.keys())} "
            f"extra={sorted(isolated.keys() - EXPECTED_NAMES)}"
        )
    if any(ISOLATION_TAG not in tags for tags in isolated.values()):
        raise AssertionError("capture fixture lacks its isolation tag")
    if any(PUBSUB_TAG in tags or CAP_TAG in tags or DB_ADMISSION_TAG in tags for tags in isolated.values()):
        raise AssertionError("logger fixtures must not enter another isolation group")
    if set(pubsub) != {PUBSUB_CASE} or PUBSUB_TAG not in pubsub[PUBSUB_CASE]:
        raise AssertionError("pubsub isolation requires exactly its named tagged corpus")
    if any(tag in pubsub[PUBSUB_CASE] for tag in (ISOLATION_TAG, CAP_TAG, DB_ADMISSION_TAG)):
        raise AssertionError("pubsub corpus must not enter another isolation group")
    if set(cap) != {CAP_CASE} or CAP_TAG not in cap[CAP_CASE]:
        raise AssertionError("cap isolation requires exactly its named tagged corpus")
    if any(tag in cap[CAP_CASE] for tag in (ISOLATION_TAG, PUBSUB_TAG, DB_ADMISSION_TAG)):
        raise AssertionError("cap corpus must not enter another isolation group")
    if set(db_admission) != {DB_ADMISSION_CASE} or DB_ADMISSION_TAG not in db_admission[DB_ADMISSION_CASE]:
        raise AssertionError("database admission isolation requires exactly its named tagged corpus")
    if any(tag in db_admission[DB_ADMISSION_CASE] for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG)):
        raise AssertionError("database admission corpus must not enter another isolation group")
    if EXPECTED_NAMES.intersection(grouped) or any(
        name in grouped for name in (PUBSUB_CASE, CAP_CASE, DB_ADMISSION_CASE)
    ) or any(
        any(tag in tags for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG))
        for tags in grouped.values()
    ):
        raise AssertionError("grouped JS selector contains an isolated fixture")
    if not EXPECTED_NAMES.issubset(baseline):
        raise AssertionError("baseline JS discovery lost a logger capture fixture")
    if any(baseline[name] != isolated[name] for name in EXPECTED_NAMES):
        raise AssertionError("baseline JS logger tags disagree with isolated discovery")
    baseline_pubsub = {
        name: tags for name, tags in baseline.items() if PUBSUB_TAG in tags
    }
    if baseline_pubsub != pubsub:
        raise AssertionError("baseline JS discovery lost or changed the pubsub corpus")
    baseline_cap = {
        name: tags for name, tags in baseline.items() if CAP_TAG in tags
    }
    if baseline_cap != cap:
        raise AssertionError("baseline JS discovery lost or changed the cap corpus")
    baseline_db_admission = {
        name: tags for name, tags in baseline.items() if DB_ADMISSION_TAG in tags
    }
    if baseline_db_admission != db_admission:
        raise AssertionError("baseline JS discovery lost or changed the database admission corpus")
    exclusions = EXPECTED_NAMES | {PUBSUB_CASE, CAP_CASE, DB_ADMISSION_CASE}
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
    for gate_name, case_name in (
        (PUBSUB_GATE, PUBSUB_CASE), (CAP_GATE, CAP_CASE),
        (DB_ADMISSION_GATE, DB_ADMISSION_CASE),
    ):
        validate_strict_gate(gates, gate_name, case_name, binary)
    for gate in gates:
        for gate_name, case_name, isolation_tag in (
            (CAP_GATE, CAP_CASE, CAP_TAG),
            (DB_ADMISSION_GATE, DB_ADMISSION_CASE, DB_ADMISSION_TAG),
        ):
            if gate.get("name") != gate_name and any(
                argument in (case_name, isolation_tag) for argument in gate.get("command", [])
            ):
                raise AssertionError("isolated corpus must not have an extra discovery or execution gate")


def validate_strict_gate(gates: list[dict], gate_name: str, case_name: str, binary: Path) -> None:
    matches = [gate for gate in gates if gate.get("name") == gate_name]
    if len(matches) != 1:
        raise AssertionError("named strict isolated gate must occur exactly once")
    command = matches[0].get("command", [])
    if (
        not isinstance(command, list)
        or len(command) != 8
        or not all(isinstance(argument, str) for argument in command)
        or Path(command[0]).resolve() != Path(sys.executable).resolve()
        or Path(command[1]).resolve() != Path(__file__).with_name("verify_catch_case.py").resolve()
        or command[2] != "--binary"
        or Path(command[3]).resolve() != binary.resolve()
        or command[4:] != ["--case", case_name, "--timeout", "60"]
    ):
        raise AssertionError("named isolated gate must use its exact strict verifier command")
    deadlines = [
        prop.get("value") for prop in matches[0].get("properties", [])
        if prop.get("name") == "TIMEOUT"
    ]
    if len(deadlines) != 1 or type(deadlines[0]) not in (int, float) or deadlines[0] != 75:
        raise AssertionError("named isolated gate must have the exact outer deadline")


def self_test() -> int:
    isolated = {name: f"[js]{ISOLATION_TAG}" for name in EXPECTED_NAMES}
    grouped_selector = GROUPED_JS_SELECTOR
    binary = Path("plinth_tests")
    grouped = {"ordinary JS fixture": "[js][ordinary]"}
    pubsub = {PUBSUB_CASE: f"[js][pubsub][corpus]{PUBSUB_TAG}"}
    cap = {CAP_CASE: f"[js][cap][corpus]{CAP_TAG}"}
    db_admission = {DB_ADMISSION_CASE: f"[js][db][corpus]{DB_ADMISSION_TAG}"}
    baseline = {**isolated, **grouped, **pubsub, **cap, **db_admission}
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
    cap_gate = copy.deepcopy(pubsub_gate)
    cap_gate["name"] = CAP_GATE
    cap_gate["command"][5] = CAP_CASE
    # Keep the pubsub gate second-last for its existing command controls.
    gates.insert(-2, cap_gate)
    db_admission_gate = copy.deepcopy(pubsub_gate)
    db_admission_gate["name"] = DB_ADMISSION_GATE
    db_admission_gate["command"][5] = DB_ADMISSION_CASE
    # Preserve the existing cap/pubsub/grouped gate positions for old controls.
    gates.insert(-3, db_admission_gate)
    validate_inventory(isolated, grouped, gates, grouped_selector, binary, baseline, pubsub, cap, db_admission)
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
            validate_inventory(*candidate, grouped_selector, binary, baseline, pubsub, cap, db_admission)
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
                               candidate_baseline, candidate_pubsub, cap, db_admission)
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
                               binary, baseline, pubsub, cap, db_admission)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub gate negative control was not rejected")
    for candidate_grouped in ({**grouped, **pubsub},
                              {**grouped, "unexpected isolated": PUBSUB_TAG}):
        try:
            validate_inventory(isolated, candidate_grouped, gates, grouped_selector,
                               binary, baseline, pubsub, cap, db_admission)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub grouped leakage was not rejected")

    # These ordinary fixtures share descriptive tags with the isolated corpora;
    # only the explicit, independently named eight-case exclusion set may leave.
    ordinary = {
        **grouped,
        "ordinary pubsub integration fixture": "[js][pubsub][integration]",
        "ordinary capability fixture": "[js][cap]",
    }
    ordinary_baseline = {**isolated, **ordinary, **pubsub, **cap, **db_admission}
    validate_inventory(isolated, ordinary, gates, grouped_selector, binary,
                       ordinary_baseline, pubsub, cap, db_admission)

    def reject_cap_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": ordinary_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission,
        }
        arguments.update(changes)
        try:
            validate_inventory(**arguments)
        except AssertionError:
            rejected += 1
            return
        raise AssertionError("cap inventory negative control was not rejected")

    for candidate_cap in (
        {},
        {**cap, "unexpected cap": CAP_TAG},
        {"renamed cap": cap[CAP_CASE]},
        {CAP_CASE: "[js][cap][corpus]"},
        {CAP_CASE: cap[CAP_CASE] + ISOLATION_TAG},
        {CAP_CASE: cap[CAP_CASE] + PUBSUB_TAG},
    ):
        reject_cap_control(cap=candidate_cap)
    logger_name = next(iter(EXPECTED_NAMES))
    for candidate_isolated in (
        {**isolated, logger_name: isolated[logger_name] + CAP_TAG},
        {**isolated, logger_name: isolated[logger_name] + PUBSUB_TAG},
    ):
        reject_cap_control(isolated=candidate_isolated)
    reject_cap_control(pubsub={PUBSUB_CASE: pubsub[PUBSUB_CASE] + CAP_TAG})
    for candidate_baseline in (
        {},
        {name: tags for name, tags in ordinary_baseline.items() if name != CAP_CASE},
        {**ordinary_baseline, CAP_CASE: "[js][cap][corpus]"},
        {**ordinary_baseline, CAP_CASE: cap[CAP_CASE] + "[changed]"},
        {**ordinary_baseline, "unexpected cap": CAP_TAG},
    ):
        reject_cap_control(baseline=candidate_baseline)
    for candidate_grouped in (
        {**ordinary, **cap},
        {**ordinary, CAP_CASE: "[js]"},
        {**ordinary, "unknown isolated cap": CAP_TAG},
        {name: tags for name, tags in ordinary.items() if name != "ordinary pubsub integration fixture"},
        {name: tags for name, tags in ordinary.items() if name != "ordinary capability fixture"},
        {**ordinary, "ordinary capability fixture": "[js][changed]"},
    ):
        reject_cap_control(grouped=candidate_grouped)
    for candidate_selector in (
        grouped_selector.replace(" ~[isolated-cap]", ""),
        grouped_selector.replace("[js] ~[batch-transaction]", "[js][cap]"),
        GROUPED_JS_BASELINE,
    ):
        reject_cap_control(grouped_selector=candidate_selector)
    reject_cap_control(gates=[gate for gate in gates if gate["name"] != CAP_GATE])
    reject_cap_control(gates=gates + [copy.deepcopy(cap_gate)])
    unknown_cap_gate = copy.deepcopy(cap_gate)
    unknown_cap_gate["name"] = "unexpected_cap_gate"
    reject_cap_control(gates=gates + [unknown_cap_gate])
    unknown_cap_gate["command"] = [str(binary), CAP_TAG]
    reject_cap_control(gates=gates + [unknown_cap_gate])
    cap_index = next(index for index, gate in enumerate(gates) if gate["name"] == CAP_GATE)
    for index, replacement in (
        (0, "true"), (1, "stale-verifier.py"), (2, "--wrong-binary"),
        (3, "stale-tests"), (4, "--wrong-case"), (5, PUBSUB_CASE),
        (6, "--wrong-timeout"), (7, "61"),
    ):
        mutated = copy.deepcopy(gates)
        mutated[cap_index]["command"][index] = replacement
        reject_cap_control(gates=mutated)
    for command in ([], "unavailable", cap_gate["command"] + ["--extra"]):
        mutated = copy.deepcopy(gates)
        mutated[cap_index]["command"] = command
        reject_cap_control(gates=mutated)
    for properties in (
        [], [{"name": "TIMEOUT", "value": 74}],
        [{"name": "TIMEOUT", "value": "75"}],
        [{"name": "TIMEOUT", "value": 75}] * 2,
        [{"name": "TIMEOUT", "value": True}],
    ):
        mutated = copy.deepcopy(gates)
        mutated[cap_index]["properties"] = properties
        reject_cap_control(gates=mutated)
    for failure in (
        FileNotFoundError("synthetic unavailable"),
        subprocess.CalledProcessError(7, ["synthetic"]),
        subprocess.TimeoutExpired(["synthetic"], 30),
    ):
        with mock.patch.object(subprocess, "run", side_effect=failure):
            try:
                discover(binary, CAP_TAG)
            except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                rejected += 1
                continue
        raise AssertionError("cap discovery failure did not fail closed")

    # A descriptive [db] tag is not permission to remove an ordinary fixture.
    # Keep the independent logger/pubsub/cap names, rather than subtracting
    # whatever an isolated discovery happens to return from the baseline.
    ordinary_db = {**ordinary, "ordinary database fixture": "[js][db]"}
    db_baseline = {**isolated, **ordinary_db, **pubsub, **cap, **db_admission}
    validate_inventory(isolated, ordinary_db, gates, grouped_selector, binary,
                       db_baseline, pubsub, cap, db_admission)

    def reject_db_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary_db, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": db_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission,
        }
        arguments.update(changes)
        try:
            validate_inventory(**arguments)
        except AssertionError:
            rejected += 1
            return
        raise AssertionError("database admission inventory negative control was not rejected")

    for candidate_db in (
        {},
        {**db_admission, "unexpected database admission": DB_ADMISSION_TAG},
        {"renamed database admission": db_admission[DB_ADMISSION_CASE]},
        {DB_ADMISSION_CASE: "[js][db][corpus]"},
        *({DB_ADMISSION_CASE: db_admission[DB_ADMISSION_CASE] + tag}
          for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG)),
    ):
        reject_db_control(db_admission=candidate_db)
    reject_db_control(isolated={**isolated, logger_name: isolated[logger_name] + DB_ADMISSION_TAG})
    reject_db_control(pubsub={PUBSUB_CASE: pubsub[PUBSUB_CASE] + DB_ADMISSION_TAG})
    reject_db_control(cap={CAP_CASE: cap[CAP_CASE] + DB_ADMISSION_TAG})
    for candidate_baseline in (
        {},
        {name: tags for name, tags in db_baseline.items() if name != DB_ADMISSION_CASE},
        {**db_baseline, DB_ADMISSION_CASE: "[js][db][corpus]"},
        {**db_baseline, DB_ADMISSION_CASE: db_admission[DB_ADMISSION_CASE] + "[changed]"},
        {**db_baseline, "unexpected database admission": DB_ADMISSION_TAG},
        {name: tags for name, tags in db_baseline.items() if name != logger_name},
        {name: tags for name, tags in db_baseline.items() if name != PUBSUB_CASE},
        {name: tags for name, tags in db_baseline.items() if name != CAP_CASE},
    ):
        reject_db_control(baseline=candidate_baseline)
    for candidate_grouped in (
        {**ordinary_db, **db_admission},
        {**ordinary_db, DB_ADMISSION_CASE: "[js]"},
        {**ordinary_db, "unknown isolated database": DB_ADMISSION_TAG},
        *({name: tags for name, tags in ordinary_db.items() if name != removed}
          for removed in ordinary_db),
        {**ordinary_db, "ordinary database fixture": "[js][changed]"},
        {**ordinary_db, "unexpected ordinary fixture": "[js][db]"},
    ):
        reject_db_control(grouped=candidate_grouped)
    for candidate_selector in (
        grouped_selector.replace(" ~[isolated-db-admission]", ""),
        grouped_selector.replace("[js] ~[batch-transaction]", "[js][db]"),
        GROUPED_JS_BASELINE,
        grouped_selector + " ~[db]",
    ):
        reject_db_control(grouped_selector=candidate_selector)
    reject_db_control(gates=[gate for gate in gates if gate["name"] != DB_ADMISSION_GATE])
    reject_db_control(gates=gates + [copy.deepcopy(db_admission_gate)])
    unknown_db_gate = copy.deepcopy(db_admission_gate)
    unknown_db_gate["name"] = "unexpected_database_admission_gate"
    reject_db_control(gates=gates + [unknown_db_gate])
    unknown_db_gate["command"] = [str(binary), DB_ADMISSION_TAG]
    reject_db_control(gates=gates + [unknown_db_gate])
    db_index = next(index for index, gate in enumerate(gates) if gate["name"] == DB_ADMISSION_GATE)
    for index, replacement in (
        (0, "true"), (1, "stale-verifier.py"), (2, "--wrong-binary"),
        (3, "stale-tests"), (4, "--wrong-case"), (5, CAP_CASE),
        (6, "--wrong-timeout"), (7, "61"),
    ):
        mutated = copy.deepcopy(gates)
        mutated[db_index]["command"][index] = replacement
        reject_db_control(gates=mutated)
    for command in ([], "unavailable", db_admission_gate["command"] + ["--extra"]):
        mutated = copy.deepcopy(gates)
        mutated[db_index]["command"] = command
        reject_db_control(gates=mutated)
    for properties in (
        [], [{"name": "TIMEOUT", "value": 74}],
        [{"name": "TIMEOUT", "value": "75"}],
        [{"name": "TIMEOUT", "value": 75}] * 2,
        [{"name": "TIMEOUT", "value": True}],
    ):
        mutated = copy.deepcopy(gates)
        mutated[db_index]["properties"] = properties
        reject_db_control(gates=mutated)
    # Renaming or swapping a gate may neither lose the expected gate nor
    # attach another isolated corpus to its command.
    for replacement in ("dangling_database_gate", CAP_GATE):
        mutated = copy.deepcopy(gates)
        mutated[db_index]["name"] = replacement
        reject_db_control(gates=mutated)
    for failure in (
        FileNotFoundError("synthetic unavailable"),
        subprocess.CalledProcessError(7, ["synthetic"]),
        subprocess.TimeoutExpired(["synthetic"], 30),
    ):
        with mock.patch.object(subprocess, "run", side_effect=failure):
            try:
                discover(binary, DB_ADMISSION_TAG)
            except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                rejected += 1
                continue
        raise AssertionError("database admission discovery failure did not fail closed")
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
    cap = discover(args.binary, CAP_TAG)
    db_admission = discover(args.binary, DB_ADMISSION_TAG)
    grouped = discover(args.binary, args.grouped_selector)
    baseline = discover(args.binary, GROUPED_JS_BASELINE)
    gates = run_json(
        [args.ctest_command, "--test-dir", str(args.build_dir), "--show-only=json-v1"]
    )["tests"]
    validate_inventory(isolated, grouped, gates, args.grouped_selector, args.binary,
                       baseline, pubsub, cap, db_admission)
    print(
        "logger/pubsub/cap/database inventory: exactly 5 logger fixtures, 1 pubsub corpus, 1 cap corpus "
        "and 1 database admission corpus isolated, 0 grouped, "
        f"{len(grouped)} ordinary JS fixtures retained, one of each named gate"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
