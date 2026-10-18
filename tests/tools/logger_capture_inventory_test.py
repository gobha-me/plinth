#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail closed if logger captures or named QuickJS corpora lose isolation."""

from __future__ import annotations

import argparse
import copy
import fnmatch
import json
import os
from pathlib import Path
import re
import shutil
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
DB_BATCH_TAG = "[isolated-db-batch]"
DB_BATCH_CASE = "QuickJS db.batch has a bounded deterministic admission and orchestration corpus"
DB_BATCH_GATE = "plinth_tests_db_batch_corpus"
SYNC_CONVERSION_TAG = "[isolated-sync-conversion]"
SYNC_CONVERSION_CASE = "QuickJS synchronous evaluation has bounded deterministic conversion corpora"
SYNC_CONVERSION_GATE = "plinth_tests_sync_conversion_corpus"
SHARED_CONVERSION_GATE = "plinth_tests_conversion_corpus"
SHARED_CONVERSION_SELECTOR = "[js][conversion][corpus] ~[isolated-sync-conversion]"
INVENTORY_GATE = "plinth_tests_logger_capture_inventory"
AUDIT_DRAIN_TAG = "[isolated-audit-drain]"
AUDIT_DRAIN_CASE = "audit drain retains blocked writes before Drogon shutdown and retries"
AUDIT_DRAIN_GATE = "plinth_tests_audit_drain"
GROUPED_JS_BASELINE = "[js] ~[batch-transaction]"
GROUPED_JS_SELECTOR = GROUPED_JS_BASELINE + " ~[isolated-logger] ~[isolated-pubsub] ~[isolated-cap] ~[isolated-db-admission] ~[isolated-db-batch] ~[isolated-audit-drain] ~[isolated-sync-conversion]"
EXPECTED_GATES = {
    "plinth_tests_logger_capture_isolated": ISOLATION_TAG,
    "plinth_tests_log_regression": "[js][stdlib][log][regression]",
    "plinth_tests_log_corpus": "[js][stdlib][log][corpus]",
    "plinth_tests_log_oracle": "[js][stdlib][log][oracle]",
    SHARED_CONVERSION_GATE: SHARED_CONVERSION_SELECTOR,
}
SYNC_CONVERSION_TAGS = frozenset({"[js]", "[conversion]", "[corpus]", SYNC_CONVERSION_TAG})


def selects_sync_conversion(selector: str) -> bool:
    """Conservatively match the simple Catch selectors used by direct gates."""
    if not selector.isascii():
        # Catch2 normalizes bytes with ASCII tolower, unlike Unicode casefold.
        # Unsupported non-ASCII grammar cannot prove a safe tag/name nonmatch.
        return True
    for alternative in selector.split(","):
        # Catch2 compares both tags and names without ASCII case sensitivity.
        alternative = alternative.strip().casefold()
        if '"' in alternative or "\\" in alternative or alternative.startswith("exclude:"):
            # Catch2 removes quotes in its separate quoted-name grammar. Do
            # not let unsupported quotes, escapes, or exclusion prefixes prove
            # that execution is safe.
            return True
        terms = re.findall(r"(~?)\[([^\[\]]+)\]", alternative)
        remainder = re.sub(r"~?\[[^\[\]]+\]", "", alternative).strip()
        if terms:
            if remainder or any(tag.startswith("@") for _, tag in terms):
                # A nonmatching tag proves safety only when the entire
                # alternative is plain conjunctive tags. Mixed grammar and
                # registered aliases require Catch2's parser, so fail closed.
                return True
            if not all((f"[{tag}]" not in SYNC_CONVERSION_TAGS) if excluded
                       else (f"[{tag}]" in SYNC_CONVERSION_TAGS)
                       for excluded, tag in terms):
                continue
            return True
        if not alternative or any(char in remainder for char in "[]\\"):
            return True
        if alternative.startswith("~"):
            # Catch2 supports a narrower wildcard grammar than fnmatch. An
            # overmatching raw-name exclusion would incorrectly prove safety;
            # only the plain tag exclusions above can prove a nonmatch.
            return True
        if fnmatch.fnmatchcase(SYNC_CONVERSION_CASE.casefold(), alternative):
            return True
    return False


def commands_sync_conversion(command: list, binary: Path) -> bool:
    """Inspect actual binary/verifier execution, never arbitrary string mentions."""
    if not isinstance(command, list) or not command or not all(isinstance(arg, str) for arg in command):
        return False
    if Path(command[0]).resolve() == binary.resolve():
        selectors = []
        index = 1
        durations_seen = False
        while index < len(command):
            argument = command[index]
            if argument == "--durations":
                # Pinned Catch2 changes only showDurations for this exact
                # reporting pair. It does not select tests or waive failures.
                if durations_seen or index + 1 >= len(command) or command[index + 1] not in ("yes", "no"):
                    return True
                durations_seen = True
                index += 2
                continue
            if argument.startswith("-"):
                # Every other short/long option may alter selection. Keep
                # unknown, combined and malformed spellings fail closed.
                return True
            selectors.append(argument)
            index += 1
        if not selectors:
            # Reporting-only commands still default to running every test.
            return True
        for selector in selectors:
            folded = selector.casefold()
            if (SYNC_CONVERSION_TAG.casefold() in folded
                    or SYNC_CONVERSION_CASE.casefold() in folded):
                # Exclusion-bearing commands need a fully validated identity,
                # just like the shared and metadata gates checked above.
                return True
            if selects_sync_conversion(selector):
                return True
    elif (len(command) > 1 and Path(command[1]).resolve() ==
          Path(__file__).with_name("verify_catch_case.py").resolve()):
        return any(SYNC_CONVERSION_CASE.casefold() in arg.casefold()
                   or SYNC_CONVERSION_TAG.casefold() in arg.casefold()
                   for arg in command[2:])
    return False


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
    db_batch: dict[str, str],
    sync_conversion: dict[str, str],
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
    if any(any(tag in tags for tag in (PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG))
           for tags in isolated.values()):
        raise AssertionError("logger fixtures must not enter another isolation group")
    if set(pubsub) != {PUBSUB_CASE} or PUBSUB_TAG not in pubsub[PUBSUB_CASE]:
        raise AssertionError("pubsub isolation requires exactly its named tagged corpus")
    if any(tag in pubsub[PUBSUB_CASE] for tag in (ISOLATION_TAG, CAP_TAG, DB_ADMISSION_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG)):
        raise AssertionError("pubsub corpus must not enter another isolation group")
    if set(cap) != {CAP_CASE} or CAP_TAG not in cap[CAP_CASE]:
        raise AssertionError("cap isolation requires exactly its named tagged corpus")
    if any(tag in cap[CAP_CASE] for tag in (ISOLATION_TAG, PUBSUB_TAG, DB_ADMISSION_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG)):
        raise AssertionError("cap corpus must not enter another isolation group")
    if set(db_admission) != {DB_ADMISSION_CASE} or DB_ADMISSION_TAG not in db_admission[DB_ADMISSION_CASE]:
        raise AssertionError("database admission isolation requires exactly its named tagged corpus")
    if any(tag in db_admission[DB_ADMISSION_CASE] for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG)):
        raise AssertionError("database admission corpus must not enter another isolation group")
    if set(db_batch) != {DB_BATCH_CASE} or db_batch[DB_BATCH_CASE].count(DB_BATCH_TAG) != 1:
        raise AssertionError("database batch isolation requires exactly its named tagged corpus")
    if any(tag in db_batch[DB_BATCH_CASE] for tag in
           (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG, AUDIT_DRAIN_TAG, SYNC_CONVERSION_TAG)):
        raise AssertionError("database batch corpus must not enter another isolation group")
    if set(sync_conversion) != {SYNC_CONVERSION_CASE}:
        raise AssertionError("synchronous conversion requires exactly its independently named corpus")
    sync_tags = sync_conversion[SYNC_CONVERSION_CASE]
    parsed_sync_tags = re.findall(r"\[[^\[\]]+\]", sync_tags)
    if (set(parsed_sync_tags) != SYNC_CONVERSION_TAGS or len(parsed_sync_tags) != 4
            or "".join(parsed_sync_tags) != sync_tags):
        raise AssertionError("synchronous conversion requires its exact semantic and isolation tags")
    if EXPECTED_NAMES.intersection(grouped) or any(
        name in grouped for name in (PUBSUB_CASE, CAP_CASE, DB_ADMISSION_CASE, DB_BATCH_CASE, SYNC_CONVERSION_CASE)
    ) or any(
        any(tag in tags for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG))
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
    baseline_db_batch = {
        name: tags for name, tags in baseline.items() if DB_BATCH_TAG in tags
    }
    if baseline_db_batch != db_batch:
        raise AssertionError("baseline JS discovery lost or changed the database batch corpus")
    baseline_sync_conversion = {
        name: tags for name, tags in baseline.items() if SYNC_CONVERSION_TAG in tags
    }
    if baseline_sync_conversion != sync_conversion:
        raise AssertionError("baseline JS discovery lost or changed the synchronous conversion corpus")
    audit_drain = {name: tags for name, tags in baseline.items() if AUDIT_DRAIN_TAG in tags}
    if set(audit_drain) != {AUDIT_DRAIN_CASE}:
        raise AssertionError("audit drain requires exactly its named isolated fixture")
    if any(tag in audit_drain[AUDIT_DRAIN_CASE] for tag in
           (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG, DB_BATCH_TAG, SYNC_CONVERSION_TAG)):
        raise AssertionError("audit drain must not enter another isolation group")
    exclusions = EXPECTED_NAMES | {PUBSUB_CASE, CAP_CASE, DB_ADMISSION_CASE, DB_BATCH_CASE, AUDIT_DRAIN_CASE, SYNC_CONVERSION_CASE}
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
            not isinstance(command, list)
            or len(command) != 2
            or not all(isinstance(argument, str) for argument in command)
            or Path(command[0]).resolve() != binary.resolve()
            or command[1:] != [selector]
        ):
            raise AssertionError(f"named logger gate must occur once with its selector: {name}")
    for gate_name, case_name in (
        (PUBSUB_GATE, PUBSUB_CASE), (CAP_GATE, CAP_CASE),
        (DB_ADMISSION_GATE, DB_ADMISSION_CASE),
        (DB_BATCH_GATE, DB_BATCH_CASE),
        (SYNC_CONVERSION_GATE, SYNC_CONVERSION_CASE),
    ):
        validate_strict_gate(gates, gate_name, case_name, binary)
    validate_strict_gate(gates, AUDIT_DRAIN_GATE, AUDIT_DRAIN_CASE, binary, 30, 40)
    metadata = [gate for gate in gates if gate.get("name") == INVENTORY_GATE]
    if len(metadata) != 1:
        raise AssertionError("inventory metadata gate must occur exactly once")
    validate_metadata_gate(metadata[0], binary, grouped_selector)
    shared_conversion = next(gate for gate in gates if gate.get("name") == SHARED_CONVERSION_GATE)
    shared_deadlines = [prop.get("value") for prop in shared_conversion.get("properties", [])
                        if prop.get("name") == "TIMEOUT"]
    if (len(shared_deadlines) != 1 or type(shared_deadlines[0]) not in (int, float)
            or shared_deadlines[0] != 90):
        raise AssertionError("shared conversion must preserve its existing outer deadline")
    sync_gate = next(gate for gate in gates if gate.get("name") == SYNC_CONVERSION_GATE)
    for gate in (shared_conversion, sync_gate):
        working_directories = [prop.get("value") for prop in gate.get("properties", [])
                               if prop.get("name") == "WORKING_DIRECTORY"]
        if working_directories != [str(binary.resolve().parent)]:
            raise AssertionError("conversion gates must retain their exact working directory")
    for gate in gates:
        for gate_name, case_name, isolation_tag in (
            (CAP_GATE, CAP_CASE, CAP_TAG),
            (DB_ADMISSION_GATE, DB_ADMISSION_CASE, DB_ADMISSION_TAG),
            (DB_BATCH_GATE, DB_BATCH_CASE, DB_BATCH_TAG),
        ):
            if gate.get("name") not in (gate_name, "plinth_tests_js", INVENTORY_GATE) and any(
                isinstance(argument, str) and (case_name in argument or isolation_tag in argument)
                for argument in gate.get("command", [])
            ):
                raise AssertionError("isolated corpus must not have an extra discovery or execution gate")
        if gate.get("name") not in (SYNC_CONVERSION_GATE, SHARED_CONVERSION_GATE,
                                    "plinth_tests_js", INVENTORY_GATE) and commands_sync_conversion(
            gate.get("command", []), binary
        ):
            raise AssertionError("synchronous conversion must not have an extra discovery or execution gate")


def validate_metadata_gate(gate: dict, binary: Path, grouped_selector: str) -> None:
    """Validate this helper's metadata inspection before allowing its selector."""
    command = gate.get("command", [])
    if (
        not isinstance(command, list)
        or len(command) != 10
        or not all(isinstance(argument, str) and argument and
                   not any(char in argument for char in "\0\n\r") for argument in command)
        or Path(command[0]).resolve() != Path(sys.executable).resolve()
        or Path(command[1]).resolve() != Path(__file__).resolve()
        or command[2] != "--binary"
        or Path(command[3]).resolve() != binary.resolve()
        or command[4:7] != ["--grouped-selector", grouped_selector, "--ctest-command"]
        or not Path(command[7]).is_absolute()
        or Path(command[7]).name != "ctest"
        or not Path(command[7]).is_file()
        or not os.access(command[7], os.X_OK)
        or command[8] != "--build-dir"
        or not Path(command[9]).is_absolute()
        or Path(command[9]).resolve() != binary.resolve().parent
        or not Path(command[9]).is_dir()
    ):
        raise AssertionError("inventory metadata gate must use its exact inspection command")
    deadlines = [prop.get("value") for prop in gate.get("properties", [])
                 if prop.get("name") == "TIMEOUT"]
    working_directories = [prop.get("value") for prop in gate.get("properties", [])
                           if prop.get("name") == "WORKING_DIRECTORY"]
    if (len(deadlines) != 1 or type(deadlines[0]) not in (int, float) or deadlines[0] != 90
            or working_directories != [command[9]]):
        raise AssertionError("inventory metadata gate must retain its deadline and working directory")


def validate_strict_gate(gates: list[dict], gate_name: str, case_name: str, binary: Path,
                         child_timeout: int = 60, outer_timeout: int = 75) -> None:
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
        or command[4:] != ["--case", case_name, "--timeout", str(child_timeout)]
    ):
        raise AssertionError("named isolated gate must use its exact strict verifier command")
    deadlines = [
        prop.get("value") for prop in matches[0].get("properties", [])
        if prop.get("name") == "TIMEOUT"
    ]
    if len(deadlines) != 1 or type(deadlines[0]) not in (int, float) or deadlines[0] != outer_timeout:
        raise AssertionError("named isolated gate must have the exact outer deadline")


def self_test() -> int:
    isolated = {name: f"[js]{ISOLATION_TAG}" for name in EXPECTED_NAMES}
    grouped_selector = GROUPED_JS_SELECTOR
    binary = Path("plinth_tests")
    grouped = {"ordinary JS fixture": "[js][ordinary]"}
    pubsub = {PUBSUB_CASE: f"[js][pubsub][corpus]{PUBSUB_TAG}"}
    cap = {CAP_CASE: f"[js][cap][corpus]{CAP_TAG}"}
    db_admission = {DB_ADMISSION_CASE: f"[js][db][corpus]{DB_ADMISSION_TAG}"}
    db_batch = {DB_BATCH_CASE: f"[js][db][corpus]{DB_BATCH_TAG}"}
    sync_conversion = {SYNC_CONVERSION_CASE: f"[js][conversion][corpus]{SYNC_CONVERSION_TAG}"}
    audit_drain = {AUDIT_DRAIN_CASE: "[js][audit]" + AUDIT_DRAIN_TAG}
    baseline = {**isolated, **grouped, **pubsub, **cap, **db_admission, **db_batch, **audit_drain, **sync_conversion}
    gates = [
        {"name": name, "command": ["plinth_tests", selector]}
        for name, selector in {**EXPECTED_GATES, "plinth_tests_js": grouped_selector}.items()
    ]
    next(gate for gate in gates if gate["name"] == SHARED_CONVERSION_GATE)["properties"] = [
        {"name": "TIMEOUT", "value": 90.0},
        {"name": "WORKING_DIRECTORY", "value": str(binary.resolve().parent)},
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
    audit_gate = copy.deepcopy(pubsub_gate)
    audit_gate["name"] = AUDIT_DRAIN_GATE
    audit_gate["command"][5] = AUDIT_DRAIN_CASE
    audit_gate["command"][7] = "30"
    audit_gate["properties"][0]["value"] = 40
    gates.insert(1, audit_gate)
    db_batch_gate = copy.deepcopy(pubsub_gate)
    db_batch_gate["name"] = DB_BATCH_GATE
    db_batch_gate["command"][5] = DB_BATCH_CASE
    # Add near the front: existing positional cap/pubsub/grouped controls stay intact.
    gates.insert(1, db_batch_gate)
    sync_gate = copy.deepcopy(pubsub_gate)
    sync_gate["name"] = SYNC_CONVERSION_GATE
    sync_gate["command"][5] = SYNC_CONVERSION_CASE
    sync_gate["properties"].append({"name": "WORKING_DIRECTORY", "value": str(binary.resolve().parent)})
    # All historical positional controls still target cap/pubsub/grouped gates.
    gates.insert(1, sync_gate)
    ctest = shutil.which("ctest")
    if ctest is None:
        raise AssertionError("inventory self-test requires the CTest metadata tool")
    metadata_gate = {
        "name": INVENTORY_GATE,
        "command": [sys.executable, str(Path(__file__).resolve()),
                    "--binary", str(binary.resolve()), "--grouped-selector", grouped_selector,
                    "--ctest-command", ctest, "--build-dir", str(binary.resolve().parent)],
        "properties": [{"name": "TIMEOUT", "value": 90.0},
                       {"name": "WORKING_DIRECTORY", "value": str(binary.resolve().parent)}],
    }
    gates.insert(1, metadata_gate)
    validate_inventory(isolated, grouped, gates, grouped_selector, binary, baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
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
            validate_inventory(*candidate, grouped_selector, binary, baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
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
                               candidate_baseline, candidate_pubsub, cap, db_admission, db_batch, sync_conversion)
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
                               binary, baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub gate negative control was not rejected")
    for candidate_grouped in ({**grouped, **pubsub},
                              {**grouped, "unexpected isolated": PUBSUB_TAG}):
        try:
            validate_inventory(isolated, candidate_grouped, gates, grouped_selector,
                               binary, baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
        except AssertionError:
            rejected += 1
            continue
        raise AssertionError("pubsub grouped leakage was not rejected")

    # These ordinary fixtures share descriptive tags with the isolated corpora;
    # only the explicit, independently named eleven-case exclusion set may leave.
    ordinary = {
        **grouped,
        "ordinary pubsub integration fixture": "[js][pubsub][integration]",
        "ordinary capability fixture": "[js][cap]",
    }
    ordinary_baseline = {**isolated, **ordinary, **pubsub, **cap, **db_admission, **db_batch, **audit_drain, **sync_conversion}
    validate_inventory(isolated, ordinary, gates, grouped_selector, binary,
                       ordinary_baseline, pubsub, cap, db_admission, db_batch, sync_conversion)

    def reject_cap_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": ordinary_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission, "db_batch": db_batch,
            "sync_conversion": sync_conversion,
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
    db_baseline = {**isolated, **ordinary_db, **pubsub, **cap, **db_admission, **db_batch, **audit_drain, **sync_conversion}
    validate_inventory(isolated, ordinary_db, gates, grouped_selector, binary,
                       db_baseline, pubsub, cap, db_admission, db_batch, sync_conversion)

    def reject_db_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary_db, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": db_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission, "db_batch": db_batch,
            "sync_conversion": sync_conversion,
        }
        arguments.update(changes)
        try:
            validate_inventory(**arguments)
        except AssertionError:
            rejected += 1
            return
        raise AssertionError("database admission inventory negative control was not rejected")

    reject_db_control(grouped={**ordinary_db, **audit_drain})
    reject_db_control(baseline={name: tags for name, tags in db_baseline.items()
                                if name != AUDIT_DRAIN_CASE})
    reject_db_control(baseline={**db_baseline, "unexpected audit drain": AUDIT_DRAIN_TAG})
    reject_db_control(grouped_selector=grouped_selector.replace(" ~[isolated-audit-drain]", ""))
    reject_db_control(gates=[gate for gate in gates if gate["name"] != AUDIT_DRAIN_GATE])
    reject_db_control(gates=gates + [copy.deepcopy(audit_gate)])
    audit_index = next(index for index, gate in enumerate(gates) if gate["name"] == AUDIT_DRAIN_GATE)
    for index, replacement in ((0, "true"), (3, "stale-tests"), (5, "stale case"), (7, "31")):
        mutated = copy.deepcopy(gates)
        mutated[audit_index]["command"][index] = replacement
        reject_db_control(gates=mutated)
    for properties in ([], [{"name": "TIMEOUT", "value": "40"}],
                       [{"name": "TIMEOUT", "value": 41}],
                       [{"name": "TIMEOUT", "value": 40}] * 2):
        mutated = copy.deepcopy(gates)
        mutated[audit_index]["properties"] = properties
        reject_db_control(gates=mutated)

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

    # Independently preserve ordinary database AND batch-tagged fixtures. The
    # new targeted discovery is required separately from this fixed baseline.
    ordinary_batch = {**ordinary_db, "ordinary batch fixture": "[js][db][batch]"}
    batch_baseline = {**db_baseline, **ordinary_batch}
    validate_inventory(isolated, ordinary_batch, gates, grouped_selector, binary,
                       batch_baseline, pubsub, cap, db_admission, db_batch, sync_conversion)

    def reject_batch_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary_batch, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": batch_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission, "db_batch": db_batch,
            "sync_conversion": sync_conversion,
        }
        arguments.update(changes)
        try:
            validate_inventory(**arguments)
        except AssertionError:
            rejected += 1
            return
        raise AssertionError("database batch inventory negative control was not rejected")

    for candidate_batch in (
        {},
        {**db_batch, "unexpected database batch": DB_BATCH_TAG},
        {"renamed database batch": db_batch[DB_BATCH_CASE]},
        {DB_BATCH_CASE: "[js][db][corpus]"},
        {DB_BATCH_CASE: db_batch[DB_BATCH_CASE] + DB_BATCH_TAG},
        *({DB_BATCH_CASE: db_batch[DB_BATCH_CASE] + tag}
          for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG, AUDIT_DRAIN_TAG)),
    ):
        reject_batch_control(db_batch=candidate_batch)
    reject_batch_control(isolated={**isolated, logger_name: isolated[logger_name] + DB_BATCH_TAG})
    reject_batch_control(pubsub={PUBSUB_CASE: pubsub[PUBSUB_CASE] + DB_BATCH_TAG})
    reject_batch_control(cap={CAP_CASE: cap[CAP_CASE] + DB_BATCH_TAG})
    reject_batch_control(db_admission={DB_ADMISSION_CASE: db_admission[DB_ADMISSION_CASE] + DB_BATCH_TAG})
    reject_batch_control(baseline={**batch_baseline, AUDIT_DRAIN_CASE: audit_drain[AUDIT_DRAIN_CASE] + DB_BATCH_TAG})
    for candidate_baseline in (
        {},
        {name: tags for name, tags in batch_baseline.items() if name != DB_BATCH_CASE},
        {**batch_baseline, DB_BATCH_CASE: "[js][db][corpus]"},
        {**batch_baseline, DB_BATCH_CASE: db_batch[DB_BATCH_CASE] + "[changed]"},
        {**batch_baseline, "unexpected database batch": DB_BATCH_TAG},
        {name: tags for name, tags in batch_baseline.items() if name != logger_name},
        {name: tags for name, tags in batch_baseline.items() if name != PUBSUB_CASE},
        {name: tags for name, tags in batch_baseline.items() if name != CAP_CASE},
        {name: tags for name, tags in batch_baseline.items() if name != DB_ADMISSION_CASE},
        {name: tags for name, tags in batch_baseline.items() if name != AUDIT_DRAIN_CASE},
    ):
        reject_batch_control(baseline=candidate_baseline)
    for candidate_grouped in (
        {**ordinary_batch, **db_batch},
        {**ordinary_batch, DB_BATCH_CASE: "[js]"},
        {**ordinary_batch, "unknown isolated database batch": DB_BATCH_TAG},
        *({name: tags for name, tags in ordinary_batch.items() if name != removed}
          for removed in ordinary_batch),
        {**ordinary_batch, "ordinary batch fixture": "[js][changed]"},
        {**ordinary_batch, "unexpected ordinary fixture": "[js][db][batch]"},
    ):
        reject_batch_control(grouped=candidate_grouped)
    for candidate_selector in (
        grouped_selector.replace(" ~[isolated-db-batch]", ""),
        grouped_selector.replace("[js] ~[batch-transaction]", "[js][db]"),
        GROUPED_JS_BASELINE,
        grouped_selector + " ~[db]",
        grouped_selector + " ~[batch]",
        grouped_selector + " ~[isolated-db-batch]",
        grouped_selector.replace(" ~[isolated-db-batch] ~[isolated-audit-drain]",
                                 " ~[isolated-audit-drain] ~[isolated-db-batch]"),
    ):
        reject_batch_control(grouped_selector=candidate_selector)
    reject_batch_control(gates=[gate for gate in gates if gate["name"] != DB_BATCH_GATE])
    reject_batch_control(gates=gates + [copy.deepcopy(db_batch_gate)])
    for command in (
        db_batch_gate["command"], [str(binary), DB_BATCH_TAG],
        [str(binary), "[js] " + DB_BATCH_TAG, "--list-tests"],
        [str(binary), DB_BATCH_CASE + "*"],
    ):
        reject_batch_control(gates=gates + [{"name": "unexpected_database_batch_gate", "command": command}])
    batch_index = next(index for index, gate in enumerate(gates) if gate["name"] == DB_BATCH_GATE)
    for index, replacement in (
        (0, "true"), (1, "stale-verifier.py"), (2, "--wrong-binary"),
        (3, "stale-tests"), (4, "--wrong-case"), (5, DB_ADMISSION_CASE),
        (6, "--wrong-timeout"), (7, "61"),
    ):
        mutated = copy.deepcopy(gates)
        mutated[batch_index]["command"][index] = replacement
        reject_batch_control(gates=mutated)
    for command in ([], "unavailable", db_batch_gate["command"] + ["--extra"],
                    db_batch_gate["command"][:-1], [None] * 8):
        mutated = copy.deepcopy(gates)
        mutated[batch_index]["command"] = command
        reject_batch_control(gates=mutated)
    for properties in (
        [], [{"name": "TIMEOUT", "value": 74}],
        [{"name": "TIMEOUT", "value": "75"}],
        [{"name": "TIMEOUT", "value": 75}] * 2,
        *([{"name": "TIMEOUT", "value": value}] for value in
          (True, False, None, float("nan"), float("inf"), -float("inf"))),
    ):
        mutated = copy.deepcopy(gates)
        mutated[batch_index]["properties"] = properties
        reject_batch_control(gates=mutated)
    for replacement in ("dangling_database_batch_gate", DB_ADMISSION_GATE):
        mutated = copy.deepcopy(gates)
        mutated[batch_index]["name"] = replacement
        reject_batch_control(gates=mutated)
    mutated = copy.deepcopy(gates)
    mutated[batch_index]["command"], mutated[db_index]["command"] = (
        mutated[db_index]["command"], mutated[batch_index]["command"]
    )
    reject_batch_control(gates=mutated)
    for failure in (
        FileNotFoundError("synthetic unavailable"),
        subprocess.CalledProcessError(7, ["synthetic"]),
        subprocess.TimeoutExpired(["synthetic"], 30),
    ):
        with mock.patch.object(subprocess, "run", side_effect=failure):
            try:
                discover(binary, DB_BATCH_TAG)
            except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                rejected += 1
                continue
        raise AssertionError("database batch discovery failure did not fail closed")

    # The actual CTest metadata inventory includes this helper's own command.
    # Its exclusion-bearing selector is safe only after validating the complete
    # inspection command; retaining the name alone must never create a bypass.
    metadata_index = next(index for index, gate in enumerate(gates)
                          if gate["name"] == INVENTORY_GATE)
    reject_batch_control(gates=[gate for gate in gates if gate["name"] != INVENTORY_GATE])
    reject_batch_control(gates=gates + [copy.deepcopy(metadata_gate)])
    for replacement in ("unvalidated_metadata_gate", DB_BATCH_GATE):
        mutated = copy.deepcopy(gates)
        mutated[metadata_index]["name"] = replacement
        reject_batch_control(gates=mutated)
    for index, replacement in (
        (0, "true"), (1, "stale-inventory.py"), (2, "--wrong-binary"),
        (3, "stale-tests"), (4, "--wrong-selector"),
        (5, grouped_selector + " ~[batch]"), (6, "--wrong-ctest"),
        (7, "ctest"), (7, "/unavailable/ctest"), (7, sys.executable),
        (8, "--wrong-build-dir"), (9, "."), (9, "/unavailable/build-dir"),
        (9, str(binary.resolve().parent.parent)), (7, ctest + "\n"),
    ):
        mutated = copy.deepcopy(gates)
        mutated[metadata_index]["command"][index] = replacement
        reject_batch_control(gates=mutated)
    for command in ([], "unavailable", [None] * 10,
                    metadata_gate["command"] + ["--extra"],
                    metadata_gate["command"][:-1],
                    [str(binary), DB_BATCH_TAG]):
        mutated = copy.deepcopy(gates)
        mutated[metadata_index]["command"] = command
        reject_batch_control(gates=mutated)
    for properties in (
        [], metadata_gate["properties"] * 2,
        [{"name": "TIMEOUT", "value": 90}],
        [{"name": "TIMEOUT", "value": 90},
         {"name": "WORKING_DIRECTORY", "value": "/wrong/build-dir"}],
        *([{"name": "TIMEOUT", "value": value}, metadata_gate["properties"][1]]
          for value in (89, "90", True, float("nan"), float("inf"))),
    ):
        mutated = copy.deepcopy(gates)
        mutated[metadata_index]["properties"] = properties
        reject_batch_control(gates=mutated)
    # An extra command with the exact valid metadata shape is still forbidden
    # when its independently validated identity is absent or already in use.
    unknown_metadata = copy.deepcopy(metadata_gate)
    unknown_metadata["name"] = "extra_metadata_inspection"
    reject_batch_control(gates=gates + [unknown_metadata])
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
    if rejected != 267:
        raise AssertionError("the original 267 isolation controls must remain present")

    # The new exclusion removes one independently named case, never an ordinary
    # conversion/corpus fixture or whatever a targeted discovery happens to list.
    ordinary_sync = {
        **ordinary_batch,
        "ordinary conversion fixture": "[js][conversion]",
        "ordinary conversion corpus fixture": "[js][conversion][corpus]",
    }
    sync_baseline = {**batch_baseline, **ordinary_sync}
    validate_inventory(isolated, ordinary_sync, gates, grouped_selector, binary,
                       sync_baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
    for command in (
        [str(binary), "[js][ordinary]"], [str(binary), "[conversion][unrelated]"],
        [str(binary), "[js] ~[conversion]"], [str(binary), "[unrelated],[js][ordinary]"],
        [str(binary), "ordinary conversion*"], [str(binary), "~[js]"],
        ["echo", SYNC_CONVERSION_CASE], ["metadata-tool", "--label", SYNC_CONVERSION_TAG],
    ):
        validate_inventory(isolated, ordinary_sync,
                           gates + [{"name": "ordinary_nonmatching_gate", "command": command}],
                           grouped_selector, binary, sync_baseline, pubsub, cap,
                           db_admission, db_batch, sync_conversion)

    def reject_sync_control(**changes) -> None:
        nonlocal rejected
        arguments = {
            "isolated": isolated, "grouped": ordinary_sync, "gates": gates,
            "grouped_selector": grouped_selector, "binary": binary,
            "baseline": sync_baseline, "pubsub": pubsub, "cap": cap,
            "db_admission": db_admission, "db_batch": db_batch,
            "sync_conversion": sync_conversion,
        }
        arguments.update(changes)
        try:
            validate_inventory(**arguments)
        except AssertionError:
            rejected += 1
            return
        raise AssertionError("synchronous conversion inventory negative control was not rejected")

    for candidate in (
        {}, {**sync_conversion, "unexpected synchronous conversion": SYNC_CONVERSION_TAG},
        {"renamed synchronous conversion": sync_conversion[SYNC_CONVERSION_CASE]},
        {SYNC_CONVERSION_CASE: "[js][conversion][corpus]"},
        {SYNC_CONVERSION_CASE: sync_conversion[SYNC_CONVERSION_CASE] + SYNC_CONVERSION_TAG},
        *({SYNC_CONVERSION_CASE: sync_conversion[SYNC_CONVERSION_CASE].replace(tag, "")}
          for tag in ("[js]", "[conversion]", "[corpus]")),
        *({SYNC_CONVERSION_CASE: sync_conversion[SYNC_CONVERSION_CASE] + tag}
          for tag in (ISOLATION_TAG, PUBSUB_TAG, CAP_TAG, DB_ADMISSION_TAG,
                      DB_BATCH_TAG, AUDIT_DRAIN_TAG, "[unknown]", "[batch-transaction]")),
        {SYNC_CONVERSION_CASE: sync_conversion[SYNC_CONVERSION_CASE] + "unparsed"},
    ):
        reject_sync_control(sync_conversion=candidate)
    reject_sync_control(isolated={**isolated, logger_name: isolated[logger_name] + SYNC_CONVERSION_TAG})
    reject_sync_control(pubsub={PUBSUB_CASE: pubsub[PUBSUB_CASE] + SYNC_CONVERSION_TAG})
    reject_sync_control(cap={CAP_CASE: cap[CAP_CASE] + SYNC_CONVERSION_TAG})
    reject_sync_control(db_admission={DB_ADMISSION_CASE: db_admission[DB_ADMISSION_CASE] + SYNC_CONVERSION_TAG})
    reject_sync_control(db_batch={DB_BATCH_CASE: db_batch[DB_BATCH_CASE] + SYNC_CONVERSION_TAG})
    reject_sync_control(baseline={**sync_baseline, AUDIT_DRAIN_CASE: audit_drain[AUDIT_DRAIN_CASE] + SYNC_CONVERSION_TAG})
    for candidate in (
        {},
        *({name: tags for name, tags in sync_baseline.items() if name != removed}
          for removed in (*EXPECTED_NAMES, PUBSUB_CASE, CAP_CASE, DB_ADMISSION_CASE,
                          DB_BATCH_CASE, AUDIT_DRAIN_CASE, SYNC_CONVERSION_CASE)),
        {**sync_baseline, SYNC_CONVERSION_CASE: "[js][conversion][corpus]"},
        {**sync_baseline, SYNC_CONVERSION_CASE: sync_conversion[SYNC_CONVERSION_CASE] + "[changed]"},
        {**sync_baseline, "unexpected synchronous conversion": SYNC_CONVERSION_TAG},
    ):
        reject_sync_control(baseline=candidate)
    for candidate in (
        {**ordinary_sync, **sync_conversion},
        {**ordinary_sync, SYNC_CONVERSION_CASE: "[js]"},
        {**ordinary_sync, "unknown isolated synchronous conversion": SYNC_CONVERSION_TAG},
        *({name: tags for name, tags in ordinary_sync.items() if name != removed}
          for removed in ordinary_sync),
        {**ordinary_sync, "ordinary conversion fixture": "[js][changed]"},
        {**ordinary_sync, "unexpected ordinary conversion": "[js][conversion]"},
    ):
        reject_sync_control(grouped=candidate)
    for selector in (
        grouped_selector.replace(" ~[isolated-sync-conversion]", ""),
        grouped_selector + " ~[conversion]", grouped_selector + " ~[corpus]",
        grouped_selector.replace("[js] ~[batch-transaction]", "[js][conversion]"),
        grouped_selector + " ~[isolated-sync-conversion]",
        grouped_selector.replace(" ~[isolated-audit-drain] ~[isolated-sync-conversion]",
                                 " ~[isolated-sync-conversion] ~[isolated-audit-drain]"),
    ):
        reject_sync_control(grouped_selector=selector)

    sync_index = next(index for index, gate in enumerate(gates) if gate["name"] == SYNC_CONVERSION_GATE)
    shared_index = next(index for index, gate in enumerate(gates) if gate["name"] == SHARED_CONVERSION_GATE)
    for gate_index, expected_name, replacements in (
        (sync_index, SYNC_CONVERSION_GATE,
         ((0, "true"), (1, "stale-verifier.py"), (2, "--wrong-binary"),
          (3, "stale-tests"), (4, "--wrong-case"), (5, DB_BATCH_CASE),
          (5, SYNC_CONVERSION_TAG), (5, SYNC_CONVERSION_CASE + "*"),
          (6, "--wrong-timeout"), (7, "61"))),
        (shared_index, SHARED_CONVERSION_GATE,
         ((0, "true"), (0, "stale-tests"), (1, "[js][conversion][corpus]"),
          (1, "[js][conversion][corpus] ~[corpus]"),
          (1, SHARED_CONVERSION_SELECTOR + " ~[conversion]"),
          (1, SYNC_CONVERSION_TAG), (1, SYNC_CONVERSION_CASE))),
    ):
        expected_gate = gates[gate_index]
        reject_sync_control(gates=[gate for gate in gates if gate["name"] != expected_name])
        reject_sync_control(gates=gates + [copy.deepcopy(expected_gate)])
        for replacement in ("unexpected_conversion_gate", DB_BATCH_GATE, INVENTORY_GATE):
            mutated = copy.deepcopy(gates)
            mutated[gate_index]["name"] = replacement
            reject_sync_control(gates=mutated)
        for index, replacement in replacements:
            mutated = copy.deepcopy(gates)
            mutated[gate_index]["command"][index] = replacement
            reject_sync_control(gates=mutated)
        for command in ([], "unavailable", [None] * len(expected_gate["command"]),
                        expected_gate["command"] + ["--extra"], expected_gate["command"][:-1]):
            mutated = copy.deepcopy(gates)
            mutated[gate_index]["command"] = command
            reject_sync_control(gates=mutated)
        deadline = 75 if expected_name == SYNC_CONVERSION_GATE else 90
        for properties in (
            [], expected_gate["properties"] * 2,
            [{"name": "TIMEOUT", "value": deadline}],
            [{"name": "TIMEOUT", "value": deadline},
             {"name": "WORKING_DIRECTORY", "value": "/wrong/build-dir"}],
            *([{"name": "TIMEOUT", "value": value}, expected_gate["properties"][1]]
              for value in (deadline - 1, str(deadline), True, None,
                            float("nan"), float("inf"))),
        ):
            mutated = copy.deepcopy(gates)
            mutated[gate_index]["properties"] = properties
            reject_sync_control(gates=mutated)
    mutated = copy.deepcopy(gates)
    mutated[sync_index]["command"], mutated[shared_index]["command"] = (
        mutated[shared_index]["command"], mutated[sync_index]["command"]
    )
    reject_sync_control(gates=mutated)
    for command in (
        sync_gate["command"], [str(binary), SYNC_CONVERSION_TAG],
        [str(binary), SYNC_CONVERSION_CASE + "*"],
        [str(binary), "[js][conversion][corpus]"], [str(binary), "[conversion]"],
        [str(binary), "[js] ~[other]"], [str(binary), "*"],
        [str(binary), "[unrelated],[conversion][corpus]"],
        [str(binary), "[js]*"], [str(binary), "[js"],
        [str(binary)], [str(binary), "--list-tests"],
        [str(binary), SHARED_CONVERSION_SELECTOR],
    ):
        reject_sync_control(gates=gates + [{"name": "unexpected_sync_conversion_gate", "command": command}])
    # Exemption names must carry the fully validated metadata/selector, never a
    # new execution command hidden behind an inventory or old shared gate name.
    for gate_index in (metadata_index, shared_index):
        mutated = copy.deepcopy(gates)
        mutated[gate_index]["command"] = [str(binary), SYNC_CONVERSION_TAG]
        reject_sync_control(gates=mutated)
    for failure in (FileNotFoundError("synthetic unavailable"),
                    subprocess.CalledProcessError(7, ["synthetic"]),
                    subprocess.TimeoutExpired(["synthetic"], 30)):
        with mock.patch.object(subprocess, "run", side_effect=failure):
            try:
                discover(binary, SYNC_CONVERSION_TAG)
            except (FileNotFoundError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
                rejected += 1
                continue
        raise AssertionError("synchronous conversion discovery failure did not fail closed")
    if rejected != 399:
        raise AssertionError("the original 267 and initial 132 synchronous controls must remain present")
    # Pinned Catch2 tag equality and name matching are case-insensitive. A
    # literal quote survives argv and Catch2 then strips it in quoted-name mode.
    for selector in (
        "[JS]", "[CONVERSION]", "[CORPUS]", "[ISOLATED-SYNC-CONVERSION]",
        "[Js][CoNvErSiOn][CoRpUs]", "[unrelated],[CONVERSION][CORPUS]",
        "[JS] ~[OTHER]", "[CoNvErSiOn]*",
        '"' + SYNC_CONVERSION_CASE.swapcase() + '"',
        '"' + SYNC_CONVERSION_CASE.lower() + '"',
        '"quickjs synchronous evaluation*"',
    ):
        reject_sync_control(gates=gates + [{
            "name": "unexpected_case_insensitive_sync_gate",
            "command": [str(binary), selector],
        }])
    upper_verifier = copy.deepcopy(sync_gate["command"])
    upper_verifier[5] = SYNC_CONVERSION_CASE.upper()
    reject_sync_control(gates=gates + [{
        "name": "unexpected_case_insensitive_sync_verifier", "command": upper_verifier,
    }])
    for command in (
        [str(binary), "[JS][ORDINARY]"], [str(binary), "[JS] ~[CONVERSION]"],
        [str(binary), "[UNRELATED],[JS][ORDINARY]"],
        ["echo", '"' + SYNC_CONVERSION_CASE.swapcase() + '"'],
        ["metadata-tool", "--label", SYNC_CONVERSION_TAG.upper()],
    ):
        validate_inventory(isolated, ordinary_sync,
                           gates + [{"name": "ordinary_case_insensitive_gate", "command": command}],
                           grouped_selector, binary, sync_baseline, pubsub, cap,
                           db_admission, db_batch, sync_conversion)
    if rejected != 411:
        raise AssertionError("all 411 existing isolation controls must remain present")
    for selector in (
        "exclude:[not-a-sync-tag]", "EXCLUDE:[not-a-sync-tag]",
        "exclude:ordinary conversion*", "exclude:not-the-sync-case",
        "[unrelated]mixed name", "ordinary name [unrelated]",
        "~~[unrelated]", r"[unrelated]\escaped", r"\[unrelated]",
        "[@sync-alias]", "[unrelated][@sync-alias]",
        "[unrelated],exclude:[not-a-sync-tag]",
    ):
        reject_sync_control(gates=gates + [{
            "name": "unexpected_unsupported_sync_selector",
            "command": [str(binary), selector],
        }])
    if rejected != 423:
        raise AssertionError("all 423 existing isolation controls must remain present")
    for selector in ("~[j\u017f]", "~[conver\u017fion]", "~[corpu\u017f]",
                     "~QuickJ\u017f synchronous evaluation*"):
        reject_sync_control(gates=gates + [{
            "name": "unexpected_unicode_sync_selector",
            "command": [str(binary), selector],
        }])
    for command in (["echo", "~[j\u017f]"],
                    ["metadata-tool", "--label", "~[conver\u017fion]"]):
        validate_inventory(isolated, ordinary_sync,
                           gates + [{"name": "ordinary_unicode_mention", "command": command}],
                           grouped_selector, binary, sync_baseline, pubsub, cap,
                           db_admission, db_batch, sync_conversion)
    if rejected != 427:
        raise AssertionError("all 427 existing isolation controls must remain present")
    for selector in ("~QuickJS*corpora",
                     "~?uickJS synchronous evaluation has bounded deterministic conversion corpora"):
        reject_sync_control(gates=gates + [{
            "name": "unexpected_raw_name_exclusion", "command": [str(binary), selector],
        }])
    if rejected != 429:
        raise AssertionError("all 429 existing isolation controls must remain present")
    pg_selector = "[integration] ~[ws] ~[js] ~[subprocess]"
    for arguments in (
        [pg_selector, "--durations", "yes"], [pg_selector, "--durations", "no"],
        ["--durations", "yes", pg_selector],
        ["[js][ordinary]", "--durations", "yes"],
        ["[js] ~[conversion]", "--durations", "no"],
        ["[unrelated]", "--durations", "yes", "ordinary conversion*"],
    ):
        validate_inventory(isolated, ordinary_sync,
                           gates + [{"name": "ordinary_duration_reporting_gate",
                                     "command": [str(binary), *arguments]}],
                           grouped_selector, binary, sync_baseline, pubsub, cap,
                           db_admission, db_batch, sync_conversion)
    for arguments in (
        ["--durations", "yes"], ["--durations", "no"],
        ["--durations"], [pg_selector, "--durations"],
        [pg_selector, "--durations", ""], [pg_selector, "--durations", "YES"],
        [pg_selector, "--durations", "maybe"],
        [pg_selector, "--durations", SYNC_CONVERSION_CASE],
        [pg_selector, "--durations=yes"], [pg_selector, "--durations", "yes", "--durations", "no"],
        ["-d", "yes"], ["-dyes"], ["-r", "xml"], ["-rxml"],
        [pg_selector, "-d", "yes"], [pg_selector, "-dyes"],
        [pg_selector, "-rxml", "--durations", "yes"],
        [pg_selector, "--unknown", "value"],
        [pg_selector, "--input-file", "wanted-tests.txt"],
        [pg_selector, "-f", "wanted-tests.txt"],
        [pg_selector, "--list-tests"], [pg_selector, "--", "--durations", "yes"],
        [pg_selector, "--durations", "yes", SYNC_CONVERSION_CASE],
        ["--durations", "no", SYNC_CONVERSION_TAG],
        ["[ordinary]", "--durations", "yes", "[js]"],
        [pg_selector, "--durations", "yes", "[CONVERSION][CORPUS]"],
        [pg_selector, "--durations", "yes", ""],
    ):
        reject_sync_control(gates=gates + [{
            "name": "unexpected_duration_selection_gate",
            "command": [str(binary), *arguments],
        }])
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
    db_batch = discover(args.binary, DB_BATCH_TAG)
    sync_conversion = discover(args.binary, SYNC_CONVERSION_TAG)
    grouped = discover(args.binary, args.grouped_selector)
    baseline = discover(args.binary, GROUPED_JS_BASELINE)
    gates = run_json(
        [args.ctest_command, "--test-dir", str(args.build_dir), "--show-only=json-v1"]
    )["tests"]
    validate_inventory(isolated, grouped, gates, args.grouped_selector, args.binary,
                       baseline, pubsub, cap, db_admission, db_batch, sync_conversion)
    print(
        "logger/pubsub/cap/database inventory: exactly 5 logger fixtures, 1 pubsub corpus, 1 cap corpus, "
        "1 database admission corpus, 1 database batch corpus, 1 audit drain and "
        "1 synchronous conversion corpus isolated, 0 grouped, "
        f"{len(grouped)} ordinary JS fixtures retained, one of each named gate"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
