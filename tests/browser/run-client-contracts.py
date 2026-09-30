#!/usr/bin/env python3
"""One real browser/database journey across the same kernel's bounded restart."""

import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile

from database_cleanup import drop_database
from kernel_runtime import add_runtime_arguments, runtime_from_args
from process_cleanup import start_browser, stop_browser


def conflicting_frontend(repo, root):
    """Only change the known-valid fixture's mount, never tracked fixture bytes."""
    source = repo / "tests/fixtures/install_lifecycle/valid-install-frontend"
    stage = root / "conflicting-frontend"
    shutil.copytree(source, stage)
    manifest_path = stage / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    assert manifest["name"] == "notesfe" and manifest["frontend"]["mount"] == "/notes"
    manifest["frontend"]["mount"] = "/app"
    manifest_path.write_text(json.dumps(manifest))
    archive = root / "conflicting-frontend.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                package.write(path, path.relative_to(stage))
    return archive


def stop_kernel(child):
    """Successful phase transitions require a graceful, bounded zero exit."""
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
        child.wait(timeout=55)
    if child.returncode != 0:
        raise RuntimeError(f"client-contract kernel shutdown failed: {child.returncode}")


def start_kernel(runtime, config, root, env, children):
    log = root / f"kernel-{len(children)}.log"
    with log.open("w") as output:
        child = runtime.start(["serve", "--config", runtime.path(root, config)],
                              root=root, env=env, output=output)
    children.append(child)
    deadline = time.monotonic() + 30
    while child.poll() is None and time.monotonic() < deadline:
        try:
            with urllib.request.urlopen(env["PLINTH_BASE_URL"] + "/healthz", timeout=1) as response:
                if response.status == 200:
                    if runtime.is_container:
                        child.assert_plinth_is_pid1()
                    return child
        except (urllib.error.URLError, TimeoutError):
            pass
        time.sleep(0.05)
    # Logs may contain operational details. Keep them private in the owned
    # directory; never print their contents on a startup/browser failure.
    raise RuntimeError(f"client-contract kernel startup failed or exceeded 30s: {child.poll()}")


def await_restart_marker(browser):
    deadline = time.monotonic() + 120
    with selectors.DefaultSelector() as selector:
        selector.register(browser.stdout, selectors.EVENT_READ)
        while time.monotonic() < deadline:
            if selector.select(timeout=min(1, max(0, deadline - time.monotonic()))):
                line = browser.stdout.readline()
                if not line:
                    raise RuntimeError("client-contract browser exited before restart admission")
                if line.strip() != "ready_for_client_restart":
                    raise RuntimeError("unexpected client-contract restart protocol output")
                return
            if browser.poll() is not None:
                raise RuntimeError("client-contract browser failed before phase A admission")
    raise TimeoutError("client-contract browser phase A exceeded 120s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_runtime_arguments(parser)
    args = parser.parse_args()
    runtime = runtime_from_args(args)
    repo = Path(__file__).resolve().parents[2]
    database = "plinth_client_contracts_" + uuid.uuid4().hex
    pg_env = os.environ.copy()
    for suffix, default in (("HOST", "127.0.0.1"), ("PORT", "5432"),
                            ("USER", "plinth"), ("PASSWORD", "plinth"),
                            ("DATABASE", "plinth_test")):
        pg_env["PG" + suffix] = os.environ.get("PLINTH_PG_" + suffix, default)
    pg_env["PGCONNECT_TIMEOUT"] = "5"
    failures = []
    try:
        subprocess.run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-c", f'CREATE DATABASE "{database}"'],
                       env=pg_env, check=True, timeout=20, capture_output=True)
        with tempfile.TemporaryDirectory(prefix="plinth-client-contracts-") as temporary:
            root = Path(temporary)
            children, browser = [], None
            try:
                runtime.prepare_root(root)
                archive = conflicting_frontend(repo, root)
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                config_body = {
                    "database": {"pool_size": 4}, "dev_mode": False,
                    "registration": {"mode": "open"},
                    "listen_host": "127.0.0.1", "listen_port": port,
                    "migrations_dir": runtime.migrations_dir(repo),
                    "packages": {"data_dir": runtime.path(root, root / "data"),
                                 "staging_dir": runtime.path(root, root / "data/staging")},
                }
                bundle = runtime.default_bundle_path()
                if bundle is not None:
                    config_body["shell"] = {"enabled": True, "bundle_path": str(bundle)}
                config = root / "config.json"
                config.write_text(json.dumps(config_body))
                env = os.environ.copy()
                env.pop("PLINTH_REGISTRATION_ENABLED", None)
                env.update({"PLINTH_PG_" + suffix: pg_env["PG" + suffix]
                            for suffix in ("HOST", "PORT", "USER", "PASSWORD")})
                env.update(PLINTH_PG_DATABASE=database, PLINTH_PG_POOL_SIZE="4",
                           PLINTH_DEV_MODE="false", PLINTH_REGISTRATION_MODE="open",
                           PLINTH_MIGRATIONS_DIR=runtime.migrations_dir(repo),
                           PLINTH_BASE_URL=f"http://127.0.0.1:{port}",
                           PLINTH_BROWSER_PROFILE_DIR=str(root / "browser-profile"),
                           PLINTH_CLIENT_CONFLICT_ZIP=str(archive))
                browser_tmp = root / "browser-tmp"
                browser_tmp.mkdir()
                env["TMPDIR"] = str(browser_tmp)
                first = start_kernel(runtime, config, root, env, children)
                first_identity = first.name if runtime.is_container else first.pid
                browser = start_browser(["node", str(repo / "tests/browser/client-contracts-production.mjs")],
                                        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                await_restart_marker(browser)
                stop_kernel(first)
                assert first.poll() == 0, "phase A kernel must be stopped before replacement admission"
                second = start_kernel(runtime, config, root, env, children)
                second_identity = second.name if runtime.is_container else second.pid
                assert second_identity != first_identity, "restart must admit a new process/container"
                browser.stdin.write("continue\n")
                browser.stdin.flush()
                output, _ = browser.communicate(timeout=90)
                if browser.returncode != 0:
                    raise RuntimeError(f"client-contract browser phase B failed: {browser.returncode}")
                expected = "PASS joined real client contracts: register/login, concurrent UPSERT, UI mirror, CSS, mount rejection, same-database restart/hydration; browser cleanup PASS\n"
                if output != expected:
                    raise RuntimeError("client-contract browser did not report the exact completed journey")
                stop_kernel(second)
                print(output, end="", flush=True)
            except BaseException as error:
                failures.append(error)
            finally:
                # Each independent owned resource is attempted even when an
                # earlier cleanup fails. The supervisor owns detached Chromium.
                if browser is not None:
                    try:
                        stop_browser(browser)
                    except BaseException as error:
                        failures.append(error)
                for child in children:
                    try:
                        if child.poll() is None:
                            try:
                                stop_kernel(child)
                            except BaseException:
                                if child.poll() is None:
                                    child.kill()
                                    child.wait(timeout=5)
                                raise
                    except BaseException as error:
                        failures.append(error)
                try:
                    runtime.cleanup_root(root)
                except BaseException as error:
                    failures.append(error)
    except BaseException as error:
        failures.append(error)
    finally:
        try:
            # CREATE may have committed even if the client timed out. Query
            # only our generated UUID target before attempting exact cleanup.
            exists = subprocess.run(["psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-c",
                f"SELECT 1 FROM pg_database WHERE datname='{database}'"], env=pg_env,
                check=True, timeout=20, capture_output=True, text=True)
            if exists.stdout.strip() == "1":
                drop_database(database, pg_env)
        except BaseException as error:
            failures.append(error)
    if failures:
        raise BaseExceptionGroup("joined client contract/cleanup failures", failures)
    print("PASS joined client contracts kernel/process/database cleanup", flush=True)


if __name__ == "__main__":
    main()
