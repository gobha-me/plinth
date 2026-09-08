"""Own browser descendants through an isolated Linux child-subreaper process.

Playwright starts Chromium in a separate session. A process-group kill alone
cannot own that tree. This supervisor runs only the browser command, adopts
its orphaned descendants, and signals/reaps its own direct children until
none remain. It never scans or signals unrelated system processes.
"""

import ctypes
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


# The supervisor's cleanup fits inside the caller's bounded 15-second wait.
_GRACE_SECONDS = 3.0
_KILL_SECONDS = 5.0


def start_browser(command, **kwargs):
    if sys.platform != "linux":
        raise RuntimeError("production browser cleanup requires Linux child-subreaper support")
    return subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "--", *command],
        start_new_session=True, **kwargs)


def stop_browser(child):
    if child.poll() is not None:
        return  # A normally exited supervisor has already reaped its descendants.
    child.send_signal(signal.SIGTERM)
    # Signal the supervisor, not npm's group: it also owns detached Chromium
    # children and keeps running until they have terminated and been reaped.
    child.wait(timeout=15)


def run_browser(command, *, timeout, **kwargs):
    child = start_browser(command, **kwargs)
    try:
        result = child.wait(timeout=timeout)
        if result != 0:
            raise subprocess.CalledProcessError(result, command)
    finally:
        stop_browser(child)


def _direct_children():
    path = Path(f"/proc/self/task/{os.getpid()}/children")
    return [int(pid) for pid in path.read_text().split()]


def _reap_exited():
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def _clean_descendants():
    graceful_deadline = time.monotonic() + _GRACE_SECONDS
    kill_deadline = graceful_deadline + _KILL_SECONDS
    signalled = set()
    while True:
        children = _direct_children()
        if not children:
            return
        force = time.monotonic() >= graceful_deadline
        # No children are reaped between this snapshot and these signals.
        # Exited direct children remain zombies, reserving their PIDs; therefore
        # a PID cannot be recycled to an unrelated process before os.kill().
        for pid in children:
            if force or pid not in signalled:
                try:
                    os.kill(pid, signal.SIGKILL if force else signal.SIGTERM)
                except ProcessLookupError:
                    pass
                signalled.add(pid)
        _reap_exited()
        if time.monotonic() >= kill_deadline and _direct_children():
            raise RuntimeError("browser descendants did not terminate within the cleanup bound")
        time.sleep(0.02)


def _supervise(command):
    # PR_SET_CHILD_SUBREAPER: orphaned descendants reparent here rather than
    # escaping to init. The parent launcher and its kernel process are outside
    # this supervisor's tree and cannot be adopted or signalled here.
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    _direct_children()  # Fail before spawning if procfs child enumeration is unavailable.
    stopped_by = 0

    def stop(signum, _frame):
        nonlocal stopped_by
        stopped_by = signum

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, stop)
    child = None
    try:
        child = subprocess.Popen(command)
        while child.poll() is None and not stopped_by:
            time.sleep(0.02)
        result = 128 + stopped_by if stopped_by else child.returncode
    finally:
        _clean_descendants()
    return result if result >= 0 else 128 - result


if __name__ == "__main__":
    if sys.platform != "linux" or sys.argv[1:2] != ["--"] or len(sys.argv) < 3:
        raise SystemExit("usage (Linux): process_cleanup.py -- command [arguments...]")
    raise SystemExit(_supervise(sys.argv[2:]))
