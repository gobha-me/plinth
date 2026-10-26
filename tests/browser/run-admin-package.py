#!/usr/bin/env python3
"""Run the separately installed admin package in one owned native/image kernel."""

import argparse
import hashlib
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
import zipfile

from admin_artifacts import prepare_admin_baseline
from database_cleanup import drop_database
from kernel_runtime import add_runtime_arguments, runtime_from_args
from process_cleanup import start_browser, stop_browser

LEGACY_NATIVE_CASES = (
    'N01 first boot shell-only and exact local ZIP/source bytes',
    'N02 real bootstrap/register/login and no first-boot admin discovery',
    'N03 real authenticated dry run/install, provenance, ownership and audit',
    'N04 five real actors, independent visibility/API grants, RBAC and CSRF',
    'N05 actual panel list/detail/history and ZIP validate/install',
    'N06 failed migration retains old authority; successful upgrade and observed versions',
    'N07 panel 200 disable/enable and real operation actors',
    'N08 old activation pending fetch and JSON cannot overwrite current panel',
    'N09 real in-flight drain failure retains old and admitted call completes',
    'N10 lost committed reply remains unknown despite matching row by other actor',
    'N11 confirmed 204 normal uninstall, no live notes generation, audit actor',
    'N12 admin normal uninstall and actor',
)
GROUPS_NATIVE_CASES = (
    'G01 genuine source baseline and uninterrupted admin upgrade',
    'G02 independent groups visibility and API authority',
    'G03 native groups UI lifecycle and exact stored state',
    'G04 built-in protections and owned spare admin membership',
    'G05 explicit UUID membership and authoritative counts',
    'G06 real concurrent uniqueness and audit winners',
    'G07 real authentication CSRF and live-authority denials',
    'G08 committed lost reply stays unknown and retires ownership',
)

PACKAGE_MODULES = {"panel": "client/panels/packages.js",
                   "api": "client/packages/api.js",
                   "controller": "client/packages/controller.js"}
GROUP_MODULES = {"panel": "client/panels/groups.js",
                 "api": "client/groups/api.js",
                 "controller": "client/groups/controller.js"}


def current_admin_identity(source, archive):
    """Bind the current development ZIP to every independently read source byte."""
    source, archive = Path(source), Path(archive)
    entries = {}
    for path in source.rglob("*"):
        if path.is_symlink() or any(part.startswith(".") for part in path.relative_to(source).parts):
            raise ValueError("current admin source contains a symbolic or hidden entry")
        if path.is_file():
            entries[path.relative_to(source).as_posix()] = path.read_bytes()
        elif not path.is_dir():
            raise ValueError("current admin source contains an unsupported entry")
    manifest = json.loads(entries["manifest.json"])
    if (manifest.get("name"), manifest.get("version")) != ("admin", "0.1.1"):
        raise ValueError("current admin source must be admin0.1.1")
    with zipfile.ZipFile(archive) as packed:
        if packed.namelist() != sorted(entries, key=lambda name: name.encode("utf-8")):
            raise ValueError("current admin ZIP inventory differs from source")
        for name, expected in entries.items():
            info = packed.getinfo(name)
            if (packed.read(name) != expected or info.compress_type != zipfile.ZIP_STORED
                    or info.external_attr >> 16 != 0o100644):
                raise ValueError("current admin ZIP bytes or file mode differ from source")
    def modules(paths):
        return {name: hashlib.sha256(entries[path]).hexdigest() for name, path in paths.items()}
    return {"version": "0.1.1", "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "moduleHashes": modules(PACKAGE_MODULES), "groupsModuleHashes": modules(GROUP_MODULES)}


def prepare_artifact_inputs(repo, build_dir, root):
    """Production baseline pin is fixed; caller owns all outputs until browser join."""
    baseline = prepare_admin_baseline(repo, root / "historical-admin")
    with zipfile.ZipFile(baseline.archive) as packed:
        baseline_modules = {name: hashlib.sha256(packed.read(path)).hexdigest()
                            for name, path in PACKAGE_MODULES.items()}
    baseline_identity = {"commit": baseline.source_commit, "tree": baseline.source_tree,
                         "version": baseline.package_version,
                         "archiveSha256": baseline.archive_sha256, "moduleHashes": baseline_modules}
    current_archive = build_dir / "packages/admin-0.1.1.zip"
    current_identity = current_admin_identity(repo / "client/admin", current_archive)
    identities = {"baseline": baseline_identity, "current": current_identity}
    identity_path = root / "admin-artifact-identities.json"
    identity_path.write_text(json.dumps(identities), encoding="utf-8")
    return baseline.archive, identity_path, identities


def validate_groups_receipt(receipt, baseline_identity, current_identity):
    """Bind a browser result to independently retained host artifact inputs.

    The caller computes expected hashes from owned archives/reviewed source;
    a second hash merely supplied by the browser is not independent proof.
    This function performs no native operation or artifact construction.
    """
    def require(condition, message):
        if not condition:
            raise ValueError("admin groups receipt: " + message)

    def identifier(value):
        return isinstance(value, str) and re.fullmatch(
            r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
            value) is not None

    def digest(value):
        return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None

    def modules(value):
        return (isinstance(value, dict) and set(value) == {"panel", "api", "controller"}
                and all(digest(item) for item in value.values()))

    require(isinstance(receipt, dict), "outer proof missing")
    require(isinstance(baseline_identity, dict) and isinstance(current_identity, dict),
            "independent host expectations missing")
    require(baseline_identity.get("commit") == "3301b7ee01c2098847d47f3e51d5ff14d28442f3"
            and baseline_identity.get("tree") == "2399eadb48c5d1bc6f41f4bbc77b425ec0d9dc26"
            and baseline_identity.get("version") == "0.1.0"
            and current_identity.get("version") == "0.1.1", "host source identity mismatch")
    require(digest(baseline_identity.get("archiveSha256"))
            and digest(current_identity.get("archiveSha256"))
            and modules(baseline_identity.get("moduleHashes"))
            and modules(current_identity.get("moduleHashes"))
            and modules(current_identity.get("groupsModuleHashes")), "host hashes missing")
    require(baseline_identity["archiveSha256"] != current_identity["archiveSha256"],
            "baseline and current archives conflated")
    require(receipt.get("adminPackageNativeCases") == 12 and receipt.get("expectedCases") == 12
            and receipt.get("actualAuth") is True and receipt.get("actualRbacCsrf") is True
            and receipt.get("packageProvenance") == "user" and receipt.get("cleanup") == "PASS"
            and receipt.get("scope") == "actual kernel/ZIP/installed panel/browser/SQL",
            "original native proof incomplete")
    require(receipt.get("archiveSha256") == current_identity["archiveSha256"]
            and receipt.get("installedModulesSha256") == current_identity["moduleHashes"]
            and receipt.get("installedModuleSha256") == current_identity["moduleHashes"]["panel"],
            "original proof not bound to current source/archive")
    proof = receipt.get("groupsProof")
    require(isinstance(proof, dict), "groups proof missing")
    require(proof.get("groupsAdminNativeCases") == 8 and proof.get("expectedCases") == 8
            and proof.get("cases") == list(GROUPS_NATIVE_CASES)
            and proof.get("actualAuth") is True and proof.get("actualRbacCsrf") is True
            and proof.get("cleanup") == "PASS", "groups ledger or cleanup incomplete")
    legacy = proof.get("legacyProof")
    require(isinstance(legacy, dict), "original interval snapshot missing")
    require(legacy.get("cases") == list(LEGACY_NATIVE_CASES) and legacy.get("count") == 12
            and legacy.get("version") == current_identity["version"]
            and legacy.get("archiveSha256") == current_identity["archiveSha256"]
            and legacy.get("moduleHashes") == current_identity["moduleHashes"]
            and legacy.get("finalState") == "uninstalled_at_N12"
            and identifier(legacy.get("packageId")), "original interval snapshot invalid")
    actor, audit = legacy.get("actor"), legacy.get("uninstallAudit")
    require(isinstance(actor, dict) and isinstance(audit, dict), "N12 actor/audit missing")
    require(identifier(actor.get("userId")) and identifier(actor.get("sessionId"))
            and isinstance(actor.get("username"), str) and bool(actor["username"])
            and isinstance(actor.get("ipAddress"), str) and bool(actor["ipAddress"]),
            "N12 actor incomplete")
    require(identifier(audit.get("id")) and audit.get("action") == "packages.uninstalled"
            and audit.get("packageId") == legacy["packageId"]
            and audit.get("uninstalledByUserId") == actor["userId"],
            "N12 uninstall detail attribution mismatch")
    baseline, upgraded = proof.get("baseline"), proof.get("upgraded")
    require(isinstance(baseline, dict) and isinstance(upgraded, dict), "upgrade identities missing")
    require(baseline.get("name") == "admin" and upgraded.get("name") == "admin"
            and all(baseline.get(key) == value for key, value in baseline_identity.items())
            and identifier(baseline.get("packageId")), "baseline identity mismatch")
    require(upgraded.get("version") == current_identity["version"]
            and upgraded.get("archiveSha256") == current_identity["archiveSha256"]
            and upgraded.get("groupsModuleHashes") == current_identity["groupsModuleHashes"]
            and identifier(upgraded.get("packageId"))
            and upgraded.get("supersedesId") == baseline["packageId"]
            and len({legacy["packageId"], baseline["packageId"], upgraded["packageId"]}) == 3,
            "upgrade lineage or served source mismatch")


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
                build_dir = Path(runtime.test_build_dir(repo, root))
                baseline_archive, identity_path, identities = prepare_artifact_inputs(repo, build_dir, root)
                env.update(PLINTH_PG_DATABASE=database, PLINTH_PG_POOL_SIZE="4",
                           PLINTH_DEV_MODE="false", PLINTH_REGISTRATION_MODE="open",
                           PLINTH_BOOTSTRAP_TOKEN=token, PLINTH_TEST_BOOTSTRAP_TOKEN=token,
                           PLINTH_MIGRATIONS_DIR=runtime.migrations_dir(repo),
                           PLINTH_BASE_URL=f"http://127.0.0.1:{port}",
                           PLINTH_BROWSER_PROFILE_DIR=str(root / "browser-profile"),
                           PLINTH_TEST_BUILD_DIR=str(build_dir),
                           PLINTH_ADMIN_BASELINE_ARCHIVE=str(baseline_archive),
                           PLINTH_ADMIN_ARTIFACT_IDENTITIES=str(identity_path))
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
                validate_groups_receipt(receipt, identities["baseline"], identities["current"])
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
