"""Own and clean up a browser command together with all of its descendants."""

import os
import signal
import subprocess


def start_browser(command, **kwargs):
    # npm may spawn Node, which in turn spawns Chromium. A separate session
    # gives this invocation an owned process group instead of killing npm alone.
    return subprocess.Popen(command, start_new_session=True, **kwargs)


def stop_browser(child):
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except ProcessLookupError:
        # Reap an already exited direct child if its group has disappeared.
        child.wait(timeout=5)
        return
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        pass
    finally:
        # The direct child can exit before its Node/Chromium descendants. Kill
        # any group members still present even when wait() already returned.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.wait(timeout=5)


def run_browser(command, *, timeout, **kwargs):
    child = start_browser(command, **kwargs)
    try:
        result = child.wait(timeout=timeout)
        if result != 0:
            raise subprocess.CalledProcessError(result, command)
    finally:
        stop_browser(child)
