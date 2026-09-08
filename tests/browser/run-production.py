#!/usr/bin/env python3
"""Run browser smoke against a real kernel and an owned disposable database."""

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid

from process_cleanup import run_browser


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    repo = Path(__file__).resolve().parents[2]
    database = "plinth_browser_" + uuid.uuid4().hex
    pg_env = os.environ.copy()
    for suffix, default in (("HOST", "127.0.0.1"), ("PORT", "5432"),
                            ("USER", "plinth"), ("PASSWORD", "plinth"),
                            ("DATABASE", "plinth_test")):
        pg_env["PG" + suffix] = os.environ.get("PLINTH_PG_" + suffix, default)
    pg_env["PGCONNECT_TIMEOUT"] = "5"

    def sql(statement):
        subprocess.run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-c", statement],
                       env=pg_env, check=True, timeout=15, stdout=subprocess.DEVNULL)

    sql(f'CREATE DATABASE "{database}"')
    try:
        with tempfile.TemporaryDirectory(prefix="plinth-browser-server-") as temporary:
            root = Path(temporary)
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            config = {
                "database": {"pool_size": 4},
                "migrations_dir": str(repo / "migrations"),
                "listen_host": "127.0.0.1", "listen_port": port,
                "dev_mode": False, "registration_enabled": False,
                "packages": {"data_dir": str(root / "data"),
                             "staging_dir": str(root / "staging")},
                "shell": {"bundle_path": str(binary.parent / "share/plinth/bundled")},
            }
            config_path = root / "config.json"
            config_path.write_text(json.dumps(config))
            child_env = os.environ.copy()
            for suffix in ("HOST", "PORT", "USER", "PASSWORD"):
                child_env["PLINTH_PG_" + suffix] = pg_env["PG" + suffix]
            child_env["PLINTH_PG_DATABASE"] = database
            child_env["PLINTH_PG_POOL_SIZE"] = "4"
            child_env["PLINTH_DEV_MODE"] = "false"
            child_env["PLINTH_MIGRATIONS_DIR"] = str(repo / "migrations")
            child_env["PLINTH_BASE_URL"] = f"http://127.0.0.1:{port}"
            output_path = root / "kernel.log"
            with output_path.open("w") as output:
                child = subprocess.Popen(
                    [str(binary), "serve", "--config", str(config_path)],
                    cwd=root, env=child_env, stdin=subprocess.DEVNULL,
                    stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 30
                while True:
                    if child.poll() is not None:
                        raise RuntimeError(f"kernel exited during startup: {child.returncode}")
                    try:
                        with urllib.request.urlopen(child_env["PLINTH_BASE_URL"] + "/healthz",
                                                    timeout=1) as response:
                            if response.status == 200:
                                break
                    except (urllib.error.URLError, TimeoutError):
                        pass
                    if time.monotonic() >= deadline:
                        raise TimeoutError("kernel startup exceeded 30 seconds")
                    time.sleep(0.05)
                run_browser(["npm", "test", "--prefix", str(repo / "tests/browser")],
                            env=child_env, timeout=180)
                child.send_signal(signal.SIGTERM)
                if child.wait(timeout=55) != 0:
                    raise RuntimeError(f"kernel shutdown failed: {child.returncode}")
            except BaseException:
                print(output_path.read_text(), flush=True)
                raise
            finally:
                if child.poll() is None:
                    child.send_signal(signal.SIGTERM)
                    try:
                        child.wait(timeout=55)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=5)
    finally:
        sql(f'DROP DATABASE "{database}" WITH (FORCE)')


if __name__ == "__main__":
    main()
