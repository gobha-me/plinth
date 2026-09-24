#!/usr/bin/env python3
"""Require one real, passing Catch2 case in the focused TSan CI lane."""

import argparse
import os
import re
import signal
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--case", required=True)
    parser.add_argument("--timeout", required=True, type=int)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    process = subprocess.Popen(
        [args.binary, args.case],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        errors="replace",
        start_new_session=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
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
            print("TSan case output pipe remained open after group kill", file=sys.stderr)
            return 1
        sys.stdout.write(stdout)
        sys.stderr.write(stderr)
        print(f"TSan case timed out after {args.timeout}s", file=sys.stderr)
        return 1

    sys.stdout.write(stdout)
    sys.stderr.write(stderr)

    output = stdout + stderr
    if process.returncode != 0:
        print(f"TSan case exited with status {process.returncode}", file=sys.stderr)
        return 1
    if re.search(r"\bskipped\b|No test cases matched", output, re.IGNORECASE):
        print("TSan case skipped or did not match", file=sys.stderr)
        return 1
    if not re.search(
        r"All tests passed \([1-9][0-9,]* assertions? in 1 test case\)", output
    ):
        print("TSan case did not report one passing case with assertions", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
