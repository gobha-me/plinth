#!/usr/bin/env python3
"""Verify the exact production image contract, startup, and PID 1 shutdown."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tests/browser"))

from database_cleanup import drop_database  # noqa: E402
from kernel_runtime import SERVER_ENV_KEYS  # noqa: E402


IMAGE_BINARY = "/usr/local/bin/plinth"
IMAGE_ROOT = "/var/lib/plinth"
IMAGE_SHARE = "/usr/local/share/plinth"
IMAGE_DOC = "/usr/local/share/doc/plinth"
REQUIRED_FILES = (
    f"{IMAGE_SHARE}/bundled/shell.zip",
    f"{IMAGE_SHARE}/migrations/schema.sql",
    f"{IMAGE_SHARE}/migrations/extension_database.sql",
    f"{IMAGE_DOC}/LICENSE",
    f"{IMAGE_DOC}/THIRD_PARTY_NOTICES.md",
    f"{IMAGE_DOC}/sbom.cdx.json",
)
BUILD_TOOLS = (
    "cc", "c++", "gcc", "g++", "clang", "ld", "ar", "as", "objcopy",
    "cmake", "make", "ninja", "meson", "pkg-config", "git", "node", "npm",
    "python3", "psql",
)


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def inspect_image(image):
    result = run(["docker", "image", "inspect", image], timeout=30,
                 capture_output=True)
    return json.loads(result.stdout)[0]


def verify_config(metadata, expected_revision):
    expected_version = (REPO / "VERSION").read_text().strip()
    config = metadata["Config"]
    require(config.get("User") == "10001:10001",
            f"unexpected image user: {config.get('User')}")
    require(config.get("WorkingDir") == IMAGE_ROOT,
            f"unexpected image working directory: {config.get('WorkingDir')}")
    require(config.get("Entrypoint") == [IMAGE_BINARY],
            f"unexpected image entrypoint: {config.get('Entrypoint')}")
    require(config.get("Cmd") == ["serve", "--host", "0.0.0.0"],
            f"unexpected image command: {config.get('Cmd')}")
    require(config.get("StopSignal") == "SIGTERM",
            f"unexpected image stop signal: {config.get('StopSignal')}")
    require(config.get("ExposedPorts") == {"8080/tcp": {}},
            f"unexpected exposed ports: {config.get('ExposedPorts')}")
    require(config.get("Volumes") == {
        IMAGE_ROOT + "/data": {}, IMAGE_ROOT + "/logs": {},
    }, f"unexpected declared volumes: {config.get('Volumes')}")
    require("PLINTH_MIGRATIONS_DIR=" + IMAGE_SHARE + "/migrations" in
            (config.get("Env") or []), "image has no fixed migrations environment")

    labels = config.get("Labels") or {}
    require(labels.get("org.opencontainers.image.version") == expected_version,
            f"unexpected OCI version label: {labels}")
    require(labels.get("org.opencontainers.image.source") ==
            "https://github.com/gobha-me/plinth",
            f"unexpected OCI source label: {labels}")
    revision = labels.get("org.opencontainers.image.revision", "")
    require(re.fullmatch(r"[0-9a-f]{40}", revision),
            f"invalid OCI revision label: {revision!r}")
    if expected_revision:
        require(revision == expected_revision,
                f"OCI revision {revision!r} does not match {expected_revision!r}")

    sensitive = re.compile(r"(?:PASSWORD|TOKEN|SECRET|PRIVATE_KEY|CREDENTIAL)", re.I)
    leaked_env = [item.split("=", 1)[0] for item in (config.get("Env") or [])
                  if sensitive.search(item.split("=", 1)[0])]
    require(not leaked_env,
            "sensitive image environment keys: " + ", ".join(leaked_env))
    return expected_version


def verify_filesystem(image):
    tests = [
        'test "$(id -u)" = 10001',
        'test "$(id -g)" = 10001',
        f"test -x {IMAGE_BINARY}",
        *(f"test -f {path}" for path in REQUIRED_FILES),
        f"test ! -e {IMAGE_ROOT}/src",
        "test ! -e /src",
        "test ! -e /workspace",
        "test ! -e /opt/plinth/client",
        f"test -d {IMAGE_ROOT}/data && test -w {IMAGE_ROOT}/data",
        f"test -d {IMAGE_ROOT}/logs && test -w {IMAGE_ROOT}/logs",
        f"touch {IMAGE_ROOT}/data/.write-probe {IMAGE_ROOT}/logs/.write-probe",
        *(f"! command -v {tool} >/dev/null 2>&1" for tool in BUILD_TOOLS),
        f'test -z "$(find /usr/local {IMAGE_ROOT} \\( -name .git -o '
        "-name .env -o -name '*.pem' -o -name '*.key' \\) -print -quit)\"",
    ]
    run(["docker", "run", "--rm", "--entrypoint", "/bin/sh", image,
         "-euc", "; ".join(tests)], timeout=30)


def verify_assets(image, expected_version, expected_revision, expected_identity):
    name = "plinth-runtime-assets-" + uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="plinth-runtime-assets-") as temporary:
        archive = Path(temporary) / "shell.zip"
        sbom_path = Path(temporary) / "sbom.cdx.json"
        try:
            run(["docker", "create", "--name", name, image, "--version"],
                timeout=20, stdout=subprocess.DEVNULL)
            run(["docker", "cp", name + ":" + IMAGE_SHARE + "/bundled/shell.zip",
                 str(archive)], timeout=20, stdout=subprocess.DEVNULL)
            run(["docker", "cp", name + ":" + IMAGE_DOC + "/sbom.cdx.json",
                 str(sbom_path)], timeout=20, stdout=subprocess.DEVNULL)
        finally:
            subprocess.run(["docker", "rm", "--force", "--volumes", name], timeout=20,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with zipfile.ZipFile(archive) as package:
            required = {"manifest.json", "client/index.html", "client/sdk.js",
                        "server/main.js"}
            missing = required - set(package.namelist())
            require(not missing, f"shell.zip is missing assets: {sorted(missing)}")
            manifest = json.loads(package.read("manifest.json"))
            require(manifest.get("name") == "shell",
                    f"unexpected bundled manifest: {manifest}")
            require(manifest.get("version") == expected_version,
                    f"bundled manifest does not match VERSION: {manifest}")
        sbom = json.loads(sbom_path.read_text())
        require(sbom.get("bomFormat") == "CycloneDX",
                f"unexpected SBOM format: {sbom.get('bomFormat')}")
        require(sbom.get("specVersion"), "SBOM has no CycloneDX specVersion")

    version = run(["docker", "run", "--rm", image, "--version"], timeout=20,
                  capture_output=True).stdout.strip()
    allowed_identities = {expected_identity} if expected_identity else {
        f"v{expected_version}",
        f"v{expected_version}-dev+g{expected_revision[:12]}",
    }
    require(version in allowed_identities,
            f"runtime version {version!r} is not one of "
            f"{sorted(allowed_identities)!r}")


def postgres_environment():
    pg_env = os.environ.copy()
    for suffix, default in (("HOST", "127.0.0.1"), ("PORT", "5432"),
                            ("USER", "plinth"), ("PASSWORD", "plinth"),
                            ("DATABASE", "plinth_test")):
        pg_env["PG" + suffix] = os.environ.get("PLINTH_PG_" + suffix, default)
    pg_env["PGCONNECT_TIMEOUT"] = "5"
    return pg_env


def container_logs(name):
    try:
        result = subprocess.run(
            ["docker", "logs", name], timeout=20, text=True,
            capture_output=True)
        return result.stdout + result.stderr
    except (OSError, subprocess.TimeoutExpired) as error:
        return f"container logs unavailable: {error}"


def remove_container(name, *, absent_ok=False):
    result = subprocess.run(
        ["docker", "rm", "--force", "--volumes", name], timeout=20,
        text=True, capture_output=True)
    if (result.returncode != 0 and absent_ok and
            "No such container:" in result.stderr):
        return subprocess.CompletedProcess(result.args, 0, result.stdout,
                                           result.stderr)
    return result


def verify_runtime(image):
    if shutil.which("psql") is None:
        raise RuntimeError("psql is required for the runtime image verification")
    pg_env = postgres_environment()
    database = "plinth_runtime_image_" + uuid.uuid4().hex

    def sql(statement):
        run(["psql", "-X", "-v", "ON_ERROR_STOP=1", "-c", statement],
            env=pg_env, timeout=20, stdout=subprocess.DEVNULL)

    sql(f'CREATE DATABASE "{database}"')
    try:
        identifier = uuid.uuid4().hex
        volumes = {
            IMAGE_ROOT + "/data": "plinth-runtime-data-" + identifier,
            IMAGE_ROOT + "/logs": "plinth-runtime-logs-" + identifier,
        }
        server = "plinth-runtime-server-" + identifier
        server_created = False
        logs = ""
        port = 8080
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError as error:
                raise RuntimeError(
                    "the default image port 8080 is unavailable for verification"
                ) from error
        env = os.environ | {
            "PLINTH_PG_" + suffix: pg_env["PG" + suffix]
            for suffix in ("HOST", "PORT", "USER", "PASSWORD")
        }
        env.update(
            PLINTH_PG_DATABASE=database,
            PLINTH_PG_POOL_SIZE="4",
            PLINTH_DEV_MODE="false",
            PLINTH_MIGRATIONS_DIR=IMAGE_SHARE + "/migrations",
        )
        try:
            for volume in volumes.values():
                run(["docker", "volume", "create", volume], timeout=20,
                    stdout=subprocess.DEVNULL)
            mounts = [option for destination, volume in volumes.items()
                      for option in ("--mount", "type=volume,src=" + volume +
                                     ",dst=" + destination)]
            command = [
                "docker", "create", "--name", server,
                "--network", "host",
                *mounts,
            ]
            for key in SERVER_ENV_KEYS:
                if key in env:
                    command.extend(("--env", key))
            command.append(image)
            run(command, env=env, timeout=20, stdout=subprocess.DEVNULL)
            server_created = True
            run(["docker", "start", server], timeout=20,
                stdout=subprocess.DEVNULL)

            deadline = time.monotonic() + 45
            while True:
                state = run(
                    ["docker", "inspect", "--format={{.State.Running}}",
                     server], timeout=10, capture_output=True).stdout.strip()
                if state != "true":
                    raise RuntimeError("runtime image exited during startup")
                try:
                    with urllib.request.urlopen(
                            f"http://127.0.0.1:{port}/healthz",
                            timeout=1) as response:
                        if response.status == 200:
                            break
                except (urllib.error.URLError, TimeoutError):
                    pass
                if time.monotonic() >= deadline:
                    raise TimeoutError("runtime image startup exceeded 45 seconds")
                time.sleep(0.05)

            run([
                "docker", "exec", server, "/bin/sh", "-euc",
                'test "$(id -u)" = 10001; '
                'test "$(id -g)" = 10001; '
                f'test "$(readlink /proc/1/exe)" = {IMAGE_BINARY}; '
                f"test -d {IMAGE_ROOT}/data/extensions/shell",
            ], timeout=20, stdout=subprocess.DEVNULL)
            run(["docker", "kill", "--signal=SIGTERM", server], timeout=20,
                stdout=subprocess.DEVNULL)
            exit_code = run(
                ["docker", "wait", server], timeout=55,
                capture_output=True).stdout.strip()
            logs = container_logs(server)
            require(exit_code == "0",
                    f"runtime image exited with {exit_code}\n{logs}")
        except BaseException as error:
            if server_created and not logs:
                logs = container_logs(server)
            if logs:
                error.add_note("runtime container logs:\n" + logs)
            raise
        finally:
            cleanup_errors = []
            try:
                removed = remove_container(server, absent_ok=True)
                if removed.returncode != 0:
                    cleanup_errors.append(removed.stderr.strip())
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup_errors.append(str(error))
            for volume in reversed(volumes.values()):
                try:
                    removed = subprocess.run(
                        ["docker", "volume", "rm", "--force", volume],
                        timeout=20, text=True, capture_output=True)
                    if (removed.returncode != 0 and
                            "No such volume:" not in removed.stderr):
                        cleanup_errors.append(removed.stderr.strip())
                except (OSError, subprocess.TimeoutExpired) as error:
                    cleanup_errors.append(str(error))
            if cleanup_errors:
                message = "runtime Docker cleanup failed: " + "; ".join(
                    error for error in cleanup_errors if error)
                active_error = sys.exception()
                if active_error is None:
                    raise RuntimeError(message)
                active_error.add_note(message)
    finally:
        drop_database(database, pg_env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True,
                        help="exact locally available production image reference")
    parser.add_argument("--expected-revision",
                        help="exact 40-character source revision expected in OCI metadata")
    parser.add_argument("--expected-identity",
                        help="exact identity expected from plinth --version")
    args = parser.parse_args()
    if args.expected_revision and not re.fullmatch(r"[0-9a-f]{40}", args.expected_revision):
        parser.error("--expected-revision must be a lowercase 40-character Git SHA")
    metadata = inspect_image(args.image)
    version = verify_config(metadata, args.expected_revision)
    revision = (metadata["Config"].get("Labels") or {})[
        "org.opencontainers.image.revision"]
    verify_filesystem(args.image)
    verify_assets(args.image, version, revision, args.expected_identity)
    verify_runtime(args.image)
    print(f"runtime image verified: {args.image}")


if __name__ == "__main__":
    main()
