#!/usr/bin/env python3
"""Run the separately installed admin package in one owned native/image kernel."""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid

from database_cleanup import drop_database
from kernel_runtime import add_runtime_arguments, runtime_from_args
from process_cleanup import start_browser, stop_browser


def preserve_failure(root, env, browser_output, browser_error):
    """Retain only redacted, task-owned diagnostics outside the source tree."""
    location = Path(tempfile.mkdtemp(prefix="plinth-admin-package-diagnostics-"))
    secrets_to_hide = tuple(value for value in
                            (env.get("PLINTH_BOOTSTRAP_TOKEN"), env.get("PLINTH_PG_PASSWORD"))
                            if value)
    def redact(value):
        for secret in secrets_to_hide:
            value = value.replace(secret, "[redacted]")
        return value
    for name, value in (("kernel.log", (root / "kernel.log").read_text(errors="replace")
                        if (root / "kernel.log").exists() else ""),
                        ("browser.stdout.log", browser_output),
                        ("browser.stderr.log", browser_error)):
        destination = location / name
        destination.write_text(redact(value), encoding="utf-8")
        destination.chmod(0o600)
    return location


def start_kernel(runtime, config, root, env, children):
    log = root / "kernel.log"
    with log.open("w") as output:
        child = runtime.start(["serve", "--config", runtime.path(root, config)],
                              root=root, env=env, output=output)
    children.append(child)  # Own it even if readiness fails after launch.
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
    raise RuntimeError("admin package kernel startup failed or exceeded 30s; private log retained")


def stop_kernel(child):
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
        child.wait(timeout=55)
    if child.returncode != 0:
        raise RuntimeError(f"admin package kernel shutdown failed: {child.returncode}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_runtime_arguments(parser)
    args = parser.parse_args()
    runtime = runtime_from_args(args)
    repo = Path(__file__).resolve().parents[2]
    database = "plinth_admin_package_" + uuid.uuid4().hex
    pg_env = os.environ.copy()
    for suffix, default in (("HOST", "127.0.0.1"), ("PORT", "5432"),
                            ("USER", "plinth"), ("PASSWORD", "plinth"),
                            ("DATABASE", "plinth_test")):
        pg_env["PG" + suffix] = os.environ.get("PLINTH_PG_" + suffix, default)
    pg_env["PGCONNECT_TIMEOUT"] = "5"
    failures = []
    try:
        subprocess.run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-c",
                        f'CREATE DATABASE "{database}"'], env=pg_env, check=True,
                       timeout=20, capture_output=True)
        with tempfile.TemporaryDirectory(prefix="plinth-admin-package-") as temporary:
            root = Path(temporary)
            children, browser = [], None
            browser_output, browser_error = "", ""
            env = {}
            try:
                runtime.prepare_root(root)
                with socket.socket() as probe:
                    probe.bind(("127.0.0.1", 0))
                    port = probe.getsockname()[1]
                config_body = {
                    "database": {"pool_size": 4}, "dev_mode": False,
                    "registration": {"mode": "open", "max_accounts": 10,
                                     "source_attempts": 20, "subject_attempts": 20,
                                     "global_attempts": 100, "window_seconds": 60},
                    "listen_host": "127.0.0.1", "listen_port": port,
                    "migrations_dir": runtime.migrations_dir(repo),
                    "packages": {"data_dir": runtime.path(root, root / "data"),
                                 "staging_dir": runtime.path(root, root / "data/staging"),
                                 "upgrade_drain_timeout_ms": 500},
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
                token = secrets.token_urlsafe(48)
                env.update(PLINTH_PG_DATABASE=database, PLINTH_PG_POOL_SIZE="4",
                           PLINTH_DEV_MODE="false", PLINTH_REGISTRATION_MODE="open",
                           PLINTH_BOOTSTRAP_TOKEN=token, PLINTH_TEST_BOOTSTRAP_TOKEN=token,
                           PLINTH_MIGRATIONS_DIR=runtime.migrations_dir(repo),
                           PLINTH_BASE_URL=f"http://127.0.0.1:{port}",
                           PLINTH_BROWSER_PROFILE_DIR=str(root / "browser-profile"),
                           PLINTH_TEST_BUILD_DIR=str(runtime.test_build_dir(repo, root)))
                browser_tmp = root / "browser-tmp"
                browser_tmp.mkdir()
                env["TMPDIR"] = str(browser_tmp)
                child = start_kernel(runtime, config, root, env, children)
                browser = start_browser(
                    ["node", str(repo / "tests/browser/admin-package-production.mjs")],
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                output, error_output = browser.communicate(timeout=240)
                browser_output, browser_error = output, error_output
                if browser.returncode != 0:
                    raise RuntimeError(
                        f"admin package browser failed: {browser.returncode}; "
                        "private diagnostic output withheld")
                lines = output.splitlines()
                if len(lines) != 1:
                    raise RuntimeError("admin package browser did not emit one exact receipt")
                receipt = json.loads(lines[0])
                if (receipt.get("adminPackageNativeCases") != 12 or
                    receipt.get("expectedCases") != 12 or
                    receipt.get("actualAuth") is not True or
                    receipt.get("actualRbacCsrf") is not True or
                    receipt.get("packageProvenance") != "user" or
                    not isinstance(receipt.get("archiveSha256"), str) or
                    re.fullmatch(r"[0-9a-f]{64}", receipt["archiveSha256"]) is None or
                    not isinstance(receipt.get("installedModuleSha256"), str) or
                    re.fullmatch(r"[0-9a-f]{64}", receipt["installedModuleSha256"]) is None or
                    not isinstance(receipt.get("installedModulesSha256"), dict) or
                    set(receipt["installedModulesSha256"]) != {"panel", "api", "controller"} or
                    any(not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None
                        for value in receipt["installedModulesSha256"].values()) or
                    receipt["installedModuleSha256"] != receipt["installedModulesSha256"]["panel"] or
                    receipt.get("cleanup") != "PASS" or
                    receipt.get("scope") != "actual kernel/ZIP/installed panel/browser/SQL"):
                    raise RuntimeError("admin package browser receipt missing required proof")
                stop_kernel(child)
                print(lines[0], flush=True)
            except BaseException as error:
                if isinstance(error, subprocess.TimeoutExpired):
                    browser_output = error.output or ""
                    browser_error = error.stderr or ""
                    if isinstance(browser_output, bytes):
                        browser_output = browser_output.decode("utf-8", errors="replace")
                    if isinstance(browser_error, bytes):
                        browser_error = browser_error.decode("utf-8", errors="replace")
                try:
                    location = preserve_failure(root, env, browser_output, browser_error)
                    error.add_note(f"private redacted admin package diagnostics: {location}")
                except BaseException as diagnostic_error:
                    error.add_note(f"private diagnostic preservation failed: {diagnostic_error}")
                failures.append(error)
            finally:
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
            exists = subprocess.run(["psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-c",
                f"SELECT 1 FROM pg_database WHERE datname='{database}'"], env=pg_env,
                check=True, timeout=20, capture_output=True, text=True)
            if exists.stdout.strip() == "1":
                drop_database(database, pg_env)
        except BaseException as error:
            failures.append(error)
    if failures:
        raise BaseExceptionGroup("joined admin package/cleanup failures", failures)
    print("PASS joined admin package kernel/process/database cleanup", flush=True)


if __name__ == "__main__":
    main()
