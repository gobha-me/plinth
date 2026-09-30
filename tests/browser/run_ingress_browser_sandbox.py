"""Bound the CI sandbox prerequisite and reap only its owned descendants."""

from pathlib import Path
import signal
import subprocess
import sys
import tempfile

try:
    from process_cleanup import run_browser
except BaseException:
    print("ingress browser sandbox: supervisor failed", file=sys.stderr)
    raise SystemExit(1) from None


ROOT = Path(__file__).resolve().parents[2]
READY = "ingress browser sandbox: ready"
CLEANED = "ingress browser sandbox: cleaned"
PROFILE_REQUIRED = "ingress browser sandbox: namespace profile required"


def run(arguments):
    previous_handlers = {}
    interruption_started = False
    try:
        if arguments not in ([], ["--cleanup"]):
            raise ValueError("unsupported sandbox prerequisite arguments")

        def interrupted(_signum, _frame):
            # Unwind run_browser so its isolated supervisor still signals and
            # reaps detached Chromium descendants on workflow cancellation.
            nonlocal interruption_started
            if not interruption_started:
                interruption_started = True
                raise KeyboardInterrupt()

        try:
            for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
                previous_handlers[signum] = signal.signal(signum, interrupted)
            # Anonymous private capture also contains interpreter/Node import
            # and subreaper failures that happen before their safe CLI catches.
            with tempfile.TemporaryFile() as output:
                run_browser(
                    ["node", str(ROOT / ".github/scripts/prepare-ingress-browser.mjs"), *arguments],
                    timeout=90 if arguments else 180, cwd=ROOT,
                    stdout=output, stderr=subprocess.STDOUT,
                )
                if output.tell() > 65536:
                    raise ValueError("unexpected prerequisite output size")
                output.seek(0)
                lines = output.read(65537).decode("utf-8").splitlines()
            allowed = ([CLEANED],) if arguments else ([READY], [PROFILE_REQUIRED, READY])
            if lines not in allowed:
                raise ValueError("unexpected prerequisite output")
        finally:
            for signum, previous in previous_handlers.items():
                signal.signal(signum, previous)
        for line in lines:
            print(line)  # Every line is an exact fixed allowlist member.
    except BaseException:
        # Child diagnostics are finite too. Never print command arguments,
        # inherited environment, exception text or a supervisor traceback.
        print("ingress browser sandbox: supervisor failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(run(sys.argv[1:]))
