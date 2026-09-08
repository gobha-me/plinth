#!/usr/bin/env python3
"""Retained-install operator upgrade and persistent-profile regression (real PG/kernel)."""

import argparse
from contextlib import ExitStack
import importlib.util
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

from process_cleanup import start_browser, stop_browser
from cache_transition import CacheTransition

spec = importlib.util.spec_from_file_location("production", Path(__file__).with_name("run-production.py"))
production = importlib.util.module_from_spec(spec)
spec.loader.exec_module(production)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    repo = Path(__file__).resolve().parents[2]
    pg_env = os.environ.copy()
    for suffix, default in (("HOST", "127.0.0.1"), ("PORT", "5432"),
                            ("USER", "plinth"), ("PASSWORD", "plinth"),
                            ("DATABASE", "plinth_test")):
        pg_env["PG" + suffix] = os.environ.get("PLINTH_PG_" + suffix, default)
    pg_env["PGCONNECT_TIMEOUT"] = "5"
    database = "plinth_upgrade_" + uuid.uuid4().hex

    def sql(statement, admin=False):
        result = subprocess.run(["psql", "-XAt", "-v", "ON_ERROR_STOP=1", "-c", statement],
            env=pg_env if admin else pg_env | {"PGDATABASE": database},
            check=True, timeout=20, text=True, capture_output=True)
        return result.stdout.strip()

    sql(f'CREATE DATABASE "{database}"', admin=True)
    try:
        with tempfile.TemporaryDirectory(prefix="plinth-bundled-upgrade-") as temporary, ExitStack() as owned:
            root = Path(temporary)
            bundles = root / "bundled"
            bundles.mkdir()
            old = production.package_cache_probe(repo, root, "901.0.1", legacy_document=True)
            new = production.package_cache_probe(repo, root, "901.0.2")
            shutil.copyfile(old, bundles / "shell.zip")
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            config = root / "config.json"
            config.write_text(json.dumps({"database": {"pool_size": 4},
                "dev_mode": False, "registration_enabled": False,
                "listen_host": "127.0.0.1", "listen_port": port,
                "migrations_dir": str(repo / "migrations"),
                "packages": {"data_dir": str(root / "data"), "staging_dir": str(root / "staging")},
                "shell": {"bundle_path": str(bundles)}}))
            env = os.environ | {"PLINTH_PG_" + suffix: pg_env["PG" + suffix]
                                for suffix in ("HOST", "PORT", "USER", "PASSWORD")}
            env.update(PLINTH_PG_DATABASE=database, PLINTH_PG_POOL_SIZE="4", PLINTH_DEV_MODE="false",
                PLINTH_MIGRATIONS_DIR=str(repo / "migrations"),
                PLINTH_BASE_URL=f"http://127.0.0.1:{port}",
                PLINTH_BROWSER_PROFILE_DIR=str(root / "browser-profile"))
            browser_tmp = root / "browser-tmp"
            browser_tmp.mkdir()
            env["TMPDIR"] = str(browser_tmp)
            transition = owned.enter_context(CacheTransition(port))
            env["PLINTH_BASE_URL"] = transition.origin
            children = []
            browser = None

            def launch(upgrade=False, fails="", cancel_commit=None):
                output_path = root / f"kernel-{len(children)}.log"
                with output_path.open("w") as output:
                    child = subprocess.Popen([str(binary), "serve", "--config", str(config)] +
                        (["--upgrade-bundled-shell"] if upgrade else []), cwd=root, env=env,
                        stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
                children.append(child)
                try:
                    if cancel_commit is not None:
                        deadline = time.monotonic() + 15
                        while child.poll() is None and time.monotonic() < deadline:
                            # The deferred trigger runs only inside the final
                            # COMMIT, after the candidate pointer was swapped.
                            if sql("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() "
                                   "AND query='COMMIT' AND state='active' AND wait_event='PgSleep'") == "1":
                                break
                            time.sleep(0.05)
                        else:
                            raise RuntimeError("upgrade never reached the blocked COMMIT")
                        child.send_signal(cancel_commit)
                        child.wait(timeout=15)
                        assert child.returncode == 2, "COMMIT cancellation lost its recovery failure"
                        assert "database commit outcome is unknown" in output_path.read_text()
                        return None
                    if fails:
                        child.wait(timeout=45)
                        assert child.returncode != 0, "invalid upgrade unexpectedly succeeded"
                        assert fails in output_path.read_text(), "upgrade did not reach expected failure: " + fails
                        return None
                    deadline = time.monotonic() + 45
                    while child.poll() is None and time.monotonic() < deadline:
                        try:
                            with urllib.request.urlopen(env["PLINTH_BASE_URL"] + "/healthz", timeout=1) as response:
                                if response.status == 200:
                                    return child
                        except (urllib.error.URLError, TimeoutError):
                            pass
                        time.sleep(0.05)
                    raise RuntimeError(f"startup failed or timed out: {child.poll()}")
                except BaseException:
                    print(output_path.read_text(), flush=True)
                    raise

            def status():
                result = subprocess.run([str(binary), "shell", "status", "--config", str(config), "--json"],
                    cwd=root, env=env, check=True, timeout=20, text=True, capture_output=True)
                return json.loads(result.stdout)

            def snapshot():
                return {name: sql(query) for name, query in {
                    "preferences": "SELECT row_to_json(p) FROM ext_shell.user_preferences p ORDER BY user_id,key",
                    "rules": "SELECT id,rule,created_at FROM plinth.rbac_rules WHERE extension_name='shell' ORDER BY rule",
                    "grants": "SELECT g.* FROM plinth.group_rules g JOIN plinth.rbac_rules r ON r.id=g.rule_id WHERE r.extension_name='shell' ORDER BY group_id,rule_id",
                    "migrations": "SELECT migration_file,checksum,applied_at FROM plinth.migrations WHERE extension_name='shell' ORDER BY migration_file",
                    "active": "SELECT id,version,provenance,state FROM plinth.packages WHERE name='shell' AND state IN ('ACTIVE','ACTIVE_FLAGGED')",
                }.items()}

            def candidate(version, migrations=(), invalid=False):
                path = root / f"candidate-{version}.zip"
                with zipfile.ZipFile(new) as source, zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as target:
                    for name in source.namelist():
                        data = source.read(name)
                        if name == "manifest.json":
                            manifest = json.loads(data)
                            manifest["version"] = version
                            if invalid is True:
                                manifest["name"] = "not_shell"
                            data = json.dumps(manifest).encode()
                        if name == "capabilities.json" and invalid == "capabilities":
                            data = b"{"
                        target.writestr(name, data)
                    for name, contents in migrations:
                        target.writestr("migrations/" + name, contents)
                shutil.copyfile(path, bundles / "shell.zip")

            try:
                child = launch()
                sql("INSERT INTO plinth.users(id,username,password_hash) VALUES "
                    "('00000000-0000-4000-8000-000000000014','upgrade_fixture','not-a-valid-password-hash'); "
                    "INSERT INTO ext_shell.user_preferences(user_id,key,value) VALUES "
                    "('00000000-0000-4000-8000-000000000014','theme','\"dark\"'); "
                    "INSERT INTO plinth.groups(name) VALUES ('upgrade_fixture'); "
                    "INSERT INTO plinth.group_rules(group_id,rule_id) SELECT g.id,r.id FROM plinth.groups g "
                    "CROSS JOIN plinth.rbac_rules r WHERE g.name='upgrade_fixture' AND r.extension_name='shell';")
                production.stop_kernel(child)
                before = snapshot()
                assert status()["installed_version"] == "901.0.1"
                assert not status()["upgrade_available"]
                shutil.copyfile(new, bundles / "shell.zip")
                observed = status()
                assert observed["installed_version"] == "901.0.1" and observed["available_version"] == "901.0.2"
                assert observed["upgrade_available"] and snapshot() == before, "status mutated installation"
                # A normal restart with replaced bundle bytes must keep version one.
                child = launch()
                assert snapshot() == before
                browser = start_browser(["node", str(repo / "tests/browser/shell-upgrade.mjs")],
                    env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                with selectors.DefaultSelector() as selector:
                    selector.register(browser.stdout, selectors.EVENT_READ)
                    assert selector.select(timeout=45), "browser failed to cache installed frontend"
                    assert browser.stdout.readline().strip() == "ready_for_upgrade"
                production.stop_kernel(child)
                active = root / "data/extensions/shell/active"

                def unchanged():
                    assert snapshot() == before, "failed upgrade changed retained schema data or identities"
                    assert active.readlink() == Path("901.0.1")
                    assert sql("SELECT count(*) FROM information_schema.columns WHERE table_schema='ext_shell' "
                        "AND table_name='user_preferences' AND column_name='upgrade_probe'") == "0"

                candidate("901.0.3", invalid=True)
                launch(upgrade=True, fails="valid shell frontend manifest")
                unchanged()
                candidate("901.0.2", invalid="capabilities")
                launch(upgrade=True, fails="upgrade validation failed")
                unchanged()
                validation_id = sql("SELECT id FROM plinth.packages WHERE name='shell' AND version='901.0.2' AND state='INSTALL_FAILED'")
                assert validation_id
                first_migration = ("002_upgrade_probe.sql", "ALTER TABLE ext_shell.user_preferences ADD COLUMN upgrade_probe TEXT; "
                    "UPDATE ext_shell.user_preferences SET value='\"changed\"'::jsonb;")
                candidate("901.0.2", [first_migration, ("003_failure.sql", "SELECT 1/0;")])
                launch(upgrade=True, fails="division by zero")
                unchanged()
                failed_id = sql("SELECT id FROM plinth.packages WHERE name='shell' AND version='901.0.2' AND state='INSTALL_FAILED'")
                assert failed_id == validation_id
                failure_audits = sql("SELECT count(*) FROM plinth.audit_log WHERE action='packages.bundled_upgrade_failed'")
                # Transaction control cannot escape the encompassing transaction.
                candidate("901.0.5", [first_migration,
                    ("003_commit.sql", r"SELECT E'it\'s'; COMMIT; SELECT 'x';")])
                launch(upgrade=True, fails="cannot contain transaction control")
                unchanged()
                # Registration failure must also roll back already successful migrations.
                sql("CREATE FUNCTION plinth.reject_upgrade_registration() RETURNS trigger LANGUAGE plpgsql "
                    "AS $$ BEGIN IF NEW.extension_name='shell' THEN RAISE EXCEPTION 'fixture registration failure'; "
                    "END IF; RETURN NEW; END $$; "
                    "CREATE TRIGGER reject_upgrade_registration BEFORE INSERT ON plinth.capabilities "
                    "FOR EACH ROW EXECUTE FUNCTION plinth.reject_upgrade_registration();")
                candidate("901.0.7", [first_migration])
                launch(upgrade=True, fails="fixture registration failure")
                unchanged()
                sql("DROP TRIGGER reject_upgrade_registration ON plinth.capabilities; "
                    "DROP FUNCTION plinth.reject_upgrade_registration();")
                # Extraction failure occurs after successful migrations/registration.
                collision = root / "data/extensions/shell/901.0.6"
                collision.mkdir()
                (collision / "sentinel").write_text("retain unrelated files")
                candidate("901.0.6", [first_migration])
                launch(upgrade=True, fails="incoming version directory already exists")
                unchanged()
                assert (collision / "sentinel").read_text() == "retain unrelated files"
                # A signal while COMMIT is in flight must retain both versions
                # and remain a recovery failure, even after backend cancellation.
                sql("CREATE FUNCTION plinth.pause_bundled_commit() RETURNS trigger LANGUAGE plpgsql "
                    "AS $$ BEGIN IF NEW.name='shell' AND NEW.state='ACTIVE' "
                    "AND NEW.version IN ('901.0.8','901.0.9') THEN PERFORM pg_sleep(60); "
                    "END IF; RETURN NEW; END $$; CREATE CONSTRAINT TRIGGER pause_bundled_commit "
                    "AFTER UPDATE ON plinth.packages DEFERRABLE INITIALLY DEFERRED "
                    "FOR EACH ROW EXECUTE FUNCTION plinth.pause_bundled_commit();")
                for interrupted_version, shutdown_signal in (("901.0.8", signal.SIGINT),
                                                               ("901.0.9", signal.SIGTERM)):
                    candidate(interrupted_version, [first_migration])
                    launch(upgrade=True, cancel_commit=shutdown_signal)
                    unchanged()
                    retained = root / "data/extensions/shell" / interrupted_version
                    assert retained.is_dir(), "uncertain COMMIT discarded the candidate files"
                    assert sql("SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() "
                               "AND query='COMMIT' AND state='active'") == "0"
                    # Simulate operator recovery only after independently
                    # confirming the committed old database state and pointer.
                    sql("UPDATE plinth.packages SET state='INSTALL_FAILED' WHERE name='shell' "
                        f"AND version='{interrupted_version}' AND state='UPLOADING'")
                    shutil.rmtree(retained)
                sql("DROP TRIGGER pause_bundled_commit ON plinth.packages; "
                    "DROP FUNCTION plinth.pause_bundled_commit();")
                candidate("901.0.2", [("002_upgrade_probe.sql",
                    "ALTER TABLE ext_shell.user_preferences ADD COLUMN upgrade_probe TEXT;")])
                transition.legacy.clear()
                child = launch(upgrade=True)
                after = snapshot()
                # Added column has a null default; compare preserved values explicitly.
                assert sql("SELECT value::text FROM ext_shell.user_preferences WHERE key='theme'") == '"dark"'
                for key in ("rules", "grants"):
                    assert after[key] == before[key], f"{key} identities changed"
                assert after["migrations"].splitlines()[0] == before["migrations"], "original migration history changed"
                assert len(after["migrations"].splitlines()) == 2
                assert sql("SELECT count(*) FROM plinth.packages n JOIN plinth.packages o ON n.supersedes_id=o.id "
                    "WHERE n.version='901.0.2' AND n.state IN ('ACTIVE','ACTIVE_FLAGGED') AND n.provenance='bundled' "
                    "AND o.version='901.0.1' AND o.state='SUPERSEDED' AND o.retired_at IS NOT NULL") == "1"
                assert active.readlink() == Path("901.0.2")
                assert sql("SELECT id FROM plinth.packages WHERE name='shell' AND version='901.0.2'") == failed_id
                assert int(sql("SELECT count(*) FROM plinth.audit_log WHERE action='packages.bundled_upgrade_failed'")) >= int(failure_audits)
                browser.stdin.write("continue\n")
                browser.stdin.flush()
                stdout, _ = browser.communicate(timeout=60)
                print(stdout, end="", flush=True)
                assert browser.returncode == 0, "persistent profile upgrade failed"
                production.stop_kernel(child)
                assert status()["installed_version"] == "901.0.2" and not status()["upgrade_available"]
                child = launch()
                assert snapshot() == after, "ordinary restart changed upgraded installation"
                production.stop_kernel(child)
                print("PASS explicit bundled upgrade: retained data/grants/IDs/history, rollback controls, cached browser")
            finally:
                try:
                    if browser is not None:
                        stop_browser(browser)
                finally:
                    # Every kernel must be attempted even if another owned
                    # process reports a cleanup failure.
                    cleanup_error = None
                    for child in children:
                        try:
                            if child.poll() is None:
                                child.send_signal(signal.SIGTERM)
                                try:
                                    child.wait(timeout=55)
                                except subprocess.TimeoutExpired:
                                    child.kill()
                                    child.wait(timeout=5)
                        except BaseException as error:
                            cleanup_error = error
                    if cleanup_error is not None:
                        raise cleanup_error
    finally:
        sql(f'DROP DATABASE "{database}" WITH (FORCE)', admin=True)


if __name__ == "__main__":
    main()
