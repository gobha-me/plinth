"""Hermetic command-contract tests for the runtime image verifier."""

from pathlib import Path
import subprocess
import unittest
from unittest import mock

import verify


class Response:
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        pass


class RuntimeImageVerifierTest(unittest.TestCase):
    def test_config_requires_the_documented_default_runtime_contract(self):
        version = (verify.REPO / "VERSION").read_text().strip()
        revision = "a" * 40
        metadata = {"Config": {
            "User": "10001:10001",
            "WorkingDir": verify.IMAGE_ROOT,
            "Entrypoint": [verify.IMAGE_BINARY],
            "Cmd": ["serve", "--host", "0.0.0.0"],
            "StopSignal": "SIGTERM",
            "ExposedPorts": {"8080/tcp": {}},
            "Volumes": {
                verify.IMAGE_ROOT + "/data": {},
                verify.IMAGE_ROOT + "/logs": {},
            },
            "Env": ["PLINTH_MIGRATIONS_DIR=" + verify.IMAGE_SHARE + "/migrations"],
            "Labels": {
                "org.opencontainers.image.version": version,
                "org.opencontainers.image.source": "https://github.com/gobha-me/plinth",
                "org.opencontainers.image.revision": revision,
            },
        }}

        self.assertEqual(verify.verify_config(metadata, revision), version)
        for key in ("Cmd", "StopSignal", "ExposedPorts", "Volumes"):
            broken = {"Config": metadata["Config"] | {key: None}}
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                verify.verify_config(broken, revision)

    def test_runtime_probe_uses_owned_volumes_and_cleans_every_object(self):
        commands = []

        def fake_run(command, **_kwargs):
            commands.append(command)
            if command[:2] == ["docker", "cp"]:
                self.assertTrue(Path(command[2]).is_file())
            if command[:3] == ["docker", "inspect", "--format={{.State.Running}}"]:
                return subprocess.CompletedProcess(command, 0, "true\n", "")
            if command[:2] == ["docker", "wait"]:
                return subprocess.CompletedProcess(command, 0, "0\n", "")
            return subprocess.CompletedProcess(command, 0, "", "")

        direct_commands = []

        def fake_subprocess_run(command, **_kwargs):
            direct_commands.append(command)
            output = "kernel log\n" if command[:2] == ["docker", "logs"] else ""
            return subprocess.CompletedProcess(command, 0, output, "")

        with mock.patch.object(verify, "run", side_effect=fake_run), \
             mock.patch.object(verify.subprocess, "run", side_effect=fake_subprocess_run), \
             mock.patch.object(verify.shutil, "which", return_value="/usr/bin/psql"), \
             mock.patch.object(verify.urllib.request, "urlopen", return_value=Response()), \
             mock.patch.object(verify, "drop_database") as drop_database:
            verify.verify_runtime("plinth:test")

        docker_commands = [command for command in commands if command[0] == "docker"]
        creates = [command for command in docker_commands
                   if command[:3] == ["docker", "volume", "create"]]
        self.assertEqual(len(creates), 2)
        volume_names = {command[-1] for command in creates}
        self.assertEqual(sum(name.startswith("plinth-runtime-")
                             for name in volume_names), 2)

        container_creates = [command for command in docker_commands
                             if command[:2] == ["docker", "create"]]
        self.assertEqual(len(container_creates), 1)
        for command in container_creates:
            self.assertNotIn("--volume", command)
            mounts = [command[index + 1] for index, item in enumerate(command)
                      if item == "--mount"]
            self.assertEqual(len(mounts), 2)
            self.assertTrue(all(item.startswith("type=volume,src=plinth-runtime-")
                                for item in mounts))
        server_create = container_creates[0]
        self.assertEqual(server_create[-1], "plinth:test")
        self.assertEqual(server_create[server_create.index("--network") + 1],
                         "host")
        self.assertIn("PLINTH_PG_PASSWORD", server_create)
        self.assertFalse(any(item.startswith("PLINTH_PG_PASSWORD=")
                             for item in server_create))

        removals = [command for command in direct_commands
                    if command[:2] == ["docker", "rm"]]
        volume_removals = [command for command in direct_commands
                           if command[:3] == ["docker", "volume", "rm"]]
        self.assertEqual(len(removals), 1)
        self.assertEqual({command[-1] for command in volume_removals}, volume_names)
        self.assertTrue(any(command[:2] == ["docker", "kill"]
                            for command in docker_commands))
        self.assertTrue(any(command[:2] == ["docker", "wait"]
                            for command in docker_commands))
        drop_database.assert_called_once()

    def test_volume_create_timeout_still_cleans_every_owned_name(self):
        def fake_run(command, **_kwargs):
            if command[:3] == ["docker", "volume", "create"]:
                raise subprocess.TimeoutExpired(command, 20)
            return subprocess.CompletedProcess(command, 0, "", "")

        direct_commands = []

        def fake_subprocess_run(command, **_kwargs):
            direct_commands.append(command)
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(verify, "run", side_effect=fake_run), \
             mock.patch.object(verify.subprocess, "run",
                               side_effect=fake_subprocess_run), \
             mock.patch.object(verify.shutil, "which", return_value="/usr/bin/psql"), \
             mock.patch.object(verify, "drop_database") as drop_database:
            with self.assertRaises(subprocess.TimeoutExpired):
                verify.verify_runtime("plinth:test")

        self.assertEqual(sum(command[:2] == ["docker", "rm"]
                             for command in direct_commands), 1)
        self.assertEqual(sum(command[:3] == ["docker", "volume", "rm"]
                             for command in direct_commands), 2)
        drop_database.assert_called_once()

    def test_container_create_timeout_still_cleans_exact_container(self):
        def fake_run(command, **_kwargs):
            if command[:2] == ["docker", "create"]:
                raise subprocess.TimeoutExpired(command, 20)
            return subprocess.CompletedProcess(command, 0, "", "")

        direct_commands = []

        def fake_subprocess_run(command, **_kwargs):
            direct_commands.append(command)
            return subprocess.CompletedProcess(command, 0, "", "")

        with mock.patch.object(verify, "run", side_effect=fake_run), \
             mock.patch.object(verify.subprocess, "run",
                               side_effect=fake_subprocess_run), \
             mock.patch.object(verify.shutil, "which", return_value="/usr/bin/psql"), \
             mock.patch.object(verify, "drop_database") as drop_database:
            with self.assertRaises(subprocess.TimeoutExpired):
                verify.verify_runtime("plinth:test")

        removals = [command for command in direct_commands
                    if command[:2] == ["docker", "rm"]]
        self.assertEqual(len(removals), 1)
        self.assertTrue(removals[0][-1].startswith("plinth-runtime-server-"))
        drop_database.assert_called_once()


if __name__ == "__main__":
    unittest.main()
