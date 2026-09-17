#!/usr/bin/env python3
"""Prove the Docker context allowlist cannot re-include credential-like files."""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class DockerContextTest(unittest.TestCase):
    def test_nested_sensitive_files_stay_out_of_the_effective_context(self):
        if shutil.which("docker") is None:
            self.skipTest("docker is unavailable")

        with tempfile.TemporaryDirectory(prefix="plinth-docker-context-") as temporary:
            temporary_path = Path(temporary)
            context = temporary_path / "context"
            output = temporary_path / "output"
            docker_config = temporary_path / "docker-config"
            context.mkdir()
            docker_config.mkdir()
            shutil.copy2(ROOT / ".dockerignore", context / ".dockerignore")
            (context / "probe.Dockerfile").write_text(
                "FROM scratch\nCOPY . /context\n", encoding="utf-8"
            )

            included = context / "src" / "included.cpp"
            included.parent.mkdir()
            included.write_text("included\n", encoding="utf-8")
            shell_config = context / "client" / "shell" / "config.json"
            shell_config.parent.mkdir(parents=True)
            shell_config.write_text("{}\n", encoding="utf-8")
            fixture_config = (
                context / "tests" / "fixtures" / "install_lifecycle" /
                "valid-install" / "config.json"
            )
            fixture_config.parent.mkdir(parents=True)
            fixture_config.write_text("{}\n", encoding="utf-8")
            sensitive = (
                context / "config.json",
                context / "src" / "config.yaml",
                context / "tests" / "config.yml",
                context / "src" / ".env",
                context / "tests" / "fixture.key",
                context / "client" / "shell" / "fixture.pem",
                context / "third_party" / ".git" / "config",
                context / "AGENTS.override.md",
                context / "tests" / "browser" / "node_modules" / "module.js",
                context / "tests" / "__pycache__" / "module.pyc",
            )
            for path in sensitive:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("must-not-enter-context\n", encoding="utf-8")

            subprocess.run(
                [
                    "docker", "build", "--file", "probe.Dockerfile",
                    "--output", "type=local,dest=" + str(output), ".",
                ],
                cwd=context,
                env=os.environ | {"DOCKER_CONFIG": str(docker_config)},
                check=True,
                timeout=60,
                stdout=subprocess.DEVNULL,
            )

            exported = output / "context"
            self.assertTrue((exported / "src" / "included.cpp").is_file())
            self.assertTrue(
                (exported / "client" / "shell" / "config.json").is_file()
            )
            self.assertTrue(
                (exported / fixture_config.relative_to(context)).is_file()
            )
            for path in sensitive:
                with self.subTest(path=path.relative_to(context)):
                    self.assertFalse((exported / path.relative_to(context)).exists())


if __name__ == "__main__":
    unittest.main()
