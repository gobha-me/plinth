#!/usr/bin/env python3
"""Check public-release provenance and generate Plinth's CycloneDX SBOM."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import uuid


ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = ROOT / "third_party" / "dependencies.json"
SBOM = ROOT / "sbom.cdx.json"


def load_dependencies() -> list[dict[str, object]]:
    document = json.loads(DEPENDENCIES.read_text(encoding="utf-8"))
    if document.get("schema") != 1 or not isinstance(document.get("components"), list):
        raise ValueError("third_party/dependencies.json has an unsupported schema")
    return document["components"]


def component_purl(component: dict[str, object]) -> str:
    source = str(component["source"])
    repository = source.removeprefix("https://github.com/")
    revision = str(component.get("commit", component["version"]))
    return f"pkg:github/{repository}@{revision}"


def make_sbom(components: list[dict[str, object]]) -> str:
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    canonical = json.dumps(components, sort_keys=True, separators=(",", ":"))
    serial = uuid.uuid5(uuid.NAMESPACE_URL, f"plinth:{version}:{canonical}")
    entries = []
    for component in components:
        entry: dict[str, object] = {
            "type": "library",
            "name": component["name"],
            "version": component["version"],
            "licenses": [{"license": {"id": component["license"]}}],
            "purl": component_purl(component),
            "externalReferences": [
                {"type": "vcs", "url": component["source"]}
            ],
            "properties": [
                {"name": "plinth:dependency-kind", "value": component["kind"]}
            ],
        }
        if "sha256" in component:
            entry["hashes"] = [{"alg": "SHA-256", "content": component["sha256"]}]
        entries.append(entry)
    document = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "plinth",
                "version": version,
                "licenses": [{"license": {"id": "MIT"}}],
            }
        },
        "components": entries,
    }
    return json.dumps(document, indent=2, sort_keys=True) + "\n"


def tracked_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [ROOT / item.decode() for item in result.stdout.split(b"\0") if item]


def check(components: list[dict[str, object]]) -> list[str]:
    errors: list[str] = []
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    if re.fullmatch(r"\d+\.\d+\.\d+", version) is None:
        errors.append("VERSION is not MAJOR.MINOR.PATCH")

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for component in components:
        license_path = ROOT / str(component.get("license_path", ""))
        if not license_path.is_file():
            errors.append(f"{component['name']}: license text is missing")
        kind = component.get("kind")
        if kind == "fetchcontent":
            commit = str(component.get("commit", ""))
            if re.fullmatch(r"[0-9a-f]{40}", commit) is None:
                errors.append(f"{component['name']}: commit is not a full Git hash")
            elif f"GIT_TAG {commit}" not in cmake:
                errors.append(f"{component['name']}: CMake pin does not match inventory")
        elif kind == "vendored":
            path = ROOT / str(component.get("path", ""))
            if not path.is_file():
                errors.append(f"{component['name']}: vendored file is missing")
                continue
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest != component.get("sha256"):
                errors.append(f"{component['name']}: vendored SHA-256 does not match")
        else:
            errors.append(f"{component['name']}: unknown dependency kind {kind!r}")

    manifest = json.loads(
        (ROOT / "client" / "shell" / "manifest.json").read_text(encoding="utf-8")
    )
    if manifest.get("version") != version:
        errors.append("client/shell/manifest.json version must match VERSION")
    if manifest.get("license") != "MIT":
        errors.append("client/shell/manifest.json must declare MIT")

    for path in tracked_files():
        if not path.is_file() or not (path.parts[-1].endswith((".cpp", ".hpp"))):
            continue
        if "SPDX-License-Identifier: Apache-2.0" in path.read_text(
            encoding="utf-8", errors="replace"
        ):
            errors.append(f"{path.relative_to(ROOT)} still declares Apache-2.0")

    for workflow in (ROOT / ".github" / "workflows").glob("*.yml"):
        workflow_text = workflow.read_text(encoding="utf-8")
        for action in re.findall(r"^\s*-?\s*uses:\s*([^\s#]+)", workflow_text, re.MULTILINE):
            if action.startswith("./"):
                continue
            revision = action.rsplit("@", maxsplit=1)[-1]
            if re.fullmatch(r"[0-9a-f]{40}", revision) is None:
                errors.append(f"{workflow.relative_to(ROOT)}: action is not SHA-pinned: {action}")

    forbidden_text = (
        ".claude/plans/",
        "claude-design-handoff",
        "plinth.xcaliber",
        "/home/jeff/",
    )
    for path in tracked_files():
        if not path.is_file() or path.suffix.lower() not in {
            ".md",
            ".yml",
            ".yaml",
            ".json",
        }:
            continue
        content = path.read_text(encoding="utf-8", errors="replace")
        for token in forbidden_text:
            if token in content:
                errors.append(f"{path.relative_to(ROOT)} contains private path {token}")

    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", readme):
        if "://" not in target and not (ROOT / target).is_file():
            errors.append(f"README.md link target is missing: {target}")

    dockerignore_path = ROOT / ".dockerignore"
    dockerignore = (
        {
            line.strip()
            for line in dockerignore_path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        }
        if dockerignore_path.is_file()
        else set()
    )
    for required_pattern in (
        ".git",
        ".env",
        ".env.*",
        "config.json",
        "config.yml",
        "config.yaml",
        "AGENTS.override.md",
        "*.pem",
        "*.key",
    ):
        if required_pattern not in dockerignore:
            errors.append(f".dockerignore must exclude {required_pattern}")

    expected_sbom = make_sbom(components)
    if not SBOM.is_file() or SBOM.read_text(encoding="utf-8") != expected_sbom:
        errors.append("sbom.cdx.json is stale; run tools/public_readiness.py --write-sbom")
    for required in ("LICENSE", "THIRD_PARTY_NOTICES.md", ".gitleaks.toml"):
        if not (ROOT / required).is_file():
            errors.append(f"{required} is missing")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write-sbom", action="store_true")
    args = parser.parse_args()
    components = load_dependencies()
    if args.write_sbom:
        SBOM.write_text(make_sbom(components), encoding="utf-8")
    errors = check(components)
    for error in errors:
        print(f"public-readiness: {error}", file=sys.stderr)
    if errors:
        return 1
    print("public-readiness: dependency, license, and SBOM checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
