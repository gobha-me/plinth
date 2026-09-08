#!/usr/bin/env python3
"""Run browser smoke against a real kernel and an owned disposable database."""

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


def package_cache_probe(repo, root, version):
    """Package the real shell with observable versions throughout its graph."""
    stage = root / ("package-" + version)
    shutil.copytree(repo / "client/shell", stage)
    manifest = json.loads((stage / "manifest.json").read_text())
    manifest["version"] = version
    (stage / "manifest.json").write_text(json.dumps(manifest))
    client = stage / "client"
    for script in client.rglob("*.js"):
        marker = json.dumps([script.relative_to(client).as_posix(), version])
        with script.open("a") as output:
            output.write("\n;(globalThis.__plinthCacheVersions ??= []).push(" + marker + ");\n")
    document = client / "index.html"
    document.write_text(document.read_text().replace(
        "</head>", f'<meta name="plinth-cache-version" content="{version}">\n</head>'))
    with (client / "css/tokens.css").open("a") as output:
        output.write(f"\n:root {{ --plinth-cache-version: {version}; }}\n")
    archive = root / ("shell-" + version + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
        for file in stage.rglob("*"):
            if file.is_file():
                package.write(file, file.relative_to(stage))
    return archive


def stop_kernel(child):
    if child.poll() is None:
        child.send_signal(signal.SIGTERM)
        child.wait(timeout=55)
    if child.returncode != 0:
        raise RuntimeError(f"kernel shutdown failed: {child.returncode}")


def start_kernel(binary, config_path, root, child_env, output_path):
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
                        return child
            except (urllib.error.URLError, TimeoutError):
                pass
            if time.monotonic() >= deadline:
                raise TimeoutError("kernel startup exceeded 30 seconds")
            time.sleep(0.05)
    except BaseException:
        print(output_path.read_text(), flush=True)
        if child.poll() is None:
            child.kill()
            child.wait(timeout=5)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--upgrade-cache", action="store_true",
                        help="replace two packaged frontend versions with one cached browser profile")
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
            bundle_path = binary.parent / "share/plinth/bundled"
            replacement = None
            if args.upgrade_cache:
                bundle_path = root / "bundled"
                bundle_path.mkdir()
                shutil.copyfile(package_cache_probe(repo, root, "901.0.1"), bundle_path / "shell.zip")
                replacement = package_cache_probe(repo, root, "901.0.2")
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
                "shell": {"bundle_path": str(bundle_path)},
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
            browser_tmp = root / "browser-tmp"
            browser_tmp.mkdir()
            child_env["TMPDIR"] = str(browser_tmp)
            output_path = root / "kernel.log"
            child = start_kernel(binary, config_path, root, child_env, output_path)
            browser = None
            try:
                if args.upgrade_cache:
                    browser = subprocess.Popen(
                        ["node", str(repo / "tests/browser/shell-upgrade.mjs")],
                        env=child_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        text=True)
                    with selectors.DefaultSelector() as selector:
                        selector.register(browser.stdout, selectors.EVENT_READ)
                        if not selector.select(timeout=45):
                            raise TimeoutError("browser did not cache the first frontend")
                        if browser.stdout.readline().strip() != "ready_for_upgrade":
                            raise RuntimeError("browser failed before frontend replacement")
                    stop_kernel(child)
                    # Simulate the completed supported replacement while stopped.
                    # Install/upgrade operator behavior has its own regression;
                    # this case isolates mount caching across two package snapshots.
                    extension = root / "data/extensions/shell"
                    version_root = extension / "901.0.2"
                    with zipfile.ZipFile(replacement) as package:
                        package.extractall(version_root)
                    active = extension / "active"
                    if not active.is_symlink():
                        raise RuntimeError("expected the installer's active-version symlink")
                    active.unlink()
                    active.symlink_to("901.0.2", target_is_directory=True)
                    install_env = pg_env | {"PGDATABASE": database}
                    subprocess.run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-c",
                        "DO $$ BEGIN UPDATE plinth.packages SET version = '901.0.2' "
                        "WHERE name = 'shell' AND version = '901.0.1'; "
                        "IF NOT FOUND THEN RAISE EXCEPTION 'missing first frontend'; END IF; END $$;"],
                        env=install_env, check=True, timeout=15, stdout=subprocess.DEVNULL)
                    output_path = root / "restarted.log"
                    child = start_kernel(binary, config_path, root, child_env, output_path)
                    browser.stdin.write("continue\n")
                    browser.stdin.flush()
                    stdout, _ = browser.communicate(timeout=60)
                    print(stdout, end="", flush=True)
                    if browser.returncode != 0:
                        raise RuntimeError(f"cached-browser upgrade failed: {browser.returncode}")
                else:
                    subprocess.run(["npm", "test", "--prefix", str(repo / "tests/browser")],
                                   env=child_env, check=True, timeout=180)
                stop_kernel(child)
            except BaseException:
                print(output_path.read_text(), flush=True)
                raise
            finally:
                if browser is not None and browser.poll() is None:
                    browser.terminate()
                    try:
                        browser.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        browser.kill()
                        browser.wait(timeout=5)
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
