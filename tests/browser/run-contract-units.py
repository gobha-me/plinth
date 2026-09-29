#!/usr/bin/env python3
"""Execute the fixed client unit files and reject incomplete TAP discovery."""

import argparse
from pathlib import Path
import re
import subprocess
import sys


GROUPS = {
    "transport": (("sdk-transport.test.mjs", 35), ("smart-data-controller.test.mjs", 30)),
    "launcher": (("launcher-model.test.mjs", 2), ("launcher-owner.test.mjs", 51)),
}
COUNTERS = ("tests", "suites", "pass", "fail", "cancelled", "skipped", "todo")


class GateError(RuntimeError):
    def __init__(self, message, diagnostics=""):
        super().__init__(message)
        self.diagnostics = diagnostics


def validate_tap(name, expected, output, returncode):
    """Only the flat, named TAP13 contract emitted by these Node unit files."""
    def reject(reason):
        raise GateError(f"{name}: {reason}")

    if returncode != 0:
        reject(f"Node exited {returncode}; success requires exit 0")
    if expected <= 0:
        reject("expected case count must be positive")
    lines = output.splitlines()
    if not lines or lines[0] != "TAP version 13":
        reject("missing exact TAP version 13 header")
    counters, cases, names = {}, [], set()
    pending, plan, diagnostic = None, None, False
    for line in lines[1:]:
        if diagnostic:
            if line == "  ...":
                diagnostic = False
            elif not line.startswith("  "):
                reject("malformed TAP diagnostic block")
            continue
        if line == "  ---":
            if not cases:
                reject("TAP diagnostic without an executed case")
            diagnostic = True
            continue
        if not line:
            continue
        if line.startswith("# Subtest: "):
            if pending is not None or plan is not None:
                reject("unmatched or post-plan named subtest")
            pending = line.removeprefix("# Subtest: ")
            if not pending:
                reject("unnamed subtest")
            continue
        match = re.fullmatch(r"(not ok|ok) ([0-9]+) - (.+)", line)
        if match:
            status, number, title = match.groups()
            if re.search(r"#\s*(SKIP|TODO)\b", title, re.IGNORECASE):
                reject(f"case {number} carries a skip/TODO directive")
            if status != "ok":
                reject(f"case {number} failed: {title}")
            if plan is not None or int(number) != len(cases) + 1:
                reject("executed case numbers are not consecutive before the plan")
            if pending != title or title in names:
                reject("executed case must have a matching unique name")
            cases.append(title)
            names.add(title)
            pending = None
            continue
        match = re.fullmatch(r"1\.\.([0-9]+)", line)
        if match:
            if plan is not None or pending is not None:
                reject("duplicate plan or incomplete named case")
            plan = int(match.group(1))
            continue
        match = re.fullmatch(r"# ([a-z_]+) ([0-9]+)", line)
        if match:
            counter, value = match.groups()
            if counter not in COUNTERS:
                if counter == "duration_ms":
                    continue
                reject(f"unknown TAP counter {counter}")
            if counter in counters:
                reject(f"duplicate TAP counter {counter}")
            counters[counter] = int(value)
            continue
        if any(line.startswith("# " + counter + " ") for counter in COUNTERS):
            reject("malformed TAP summary counter")
        if line.startswith("#"):
            continue  # Node warnings, comments and fractional duration metadata.
        reject("unknown TAP output line")
    if diagnostic or pending is not None:
        reject("unfinished diagnostic or named case")
    if set(counters) != set(COUNTERS):
        reject("missing required TAP summary counters")
    wanted = {"tests": expected, "suites": 0, "pass": expected,
              "fail": 0, "cancelled": 0, "skipped": 0, "todo": 0}
    if counters != wanted:
        reject(f"TAP counts differ from required {expected} executed/passing and zero nonpasses: {counters}")
    if plan != expected or len(cases) != expected:
        reject(f"plan/named discovery differs from required {expected}: plan={plan}, cases={len(cases)}")
    return len(cases)


def text(value):
    return value.decode("utf-8", errors="replace") if isinstance(value, bytes) else value or ""


def run_file(directory, name, expected):
    command = ["node", "--experimental-vm-modules", "--test", name]
    try:
        result = subprocess.run(command, cwd=directory, timeout=60, text=True, capture_output=True)
    except subprocess.TimeoutExpired as error:
        raise GateError(f"{name}: Node test exceeded 60 seconds",
                        text(error.stdout) + text(error.stderr)) from error
    except OSError as error:
        raise GateError(f"{name}: could not execute real Node test: {error}") from error
    try:
        validate_tap(name, expected, result.stdout, result.returncode)
    except GateError as error:
        error.diagnostics = result.stdout + result.stderr
        raise
    print(f"PASS {name}: {expected}/{expected} named cases; fail/cancel/skip/todo=0", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("group", choices=(*GROUPS, "all"))
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    selected = GROUPS.values() if args.group == "all" else (GROUPS[args.group],)
    files = [item for group in selected for item in group]
    failed = False
    for name, expected in files:
        try:
            run_file(directory, name, expected)
        except GateError as error:
            failed = True
            print(f"FAIL contract unit discovery: {error}", file=sys.stderr, flush=True)
            if error.diagnostics:
                print(error.diagnostics, file=sys.stderr, end="" if error.diagnostics.endswith("\n") else "\n")
    if failed:
        return 1
    print(f"PASS {args.group} discovery gate: {sum(count for _, count in files)} executed cases across {len(files)} files", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
