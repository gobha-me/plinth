#!/usr/bin/env python3
"""Hermetic identity/control-flow checks, not actual published-release proof."""

from __future__ import annotations

from dataclasses import replace
import copy
import hashlib
import json
import os
import signal
import textwrap
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import published_release_identity as identity
import k3d_recovery_test as recovery
from k3d_lifecycle_test import Harness, CHART


def encoded(value):
    return json.dumps(value, separators=(",", ":")).encode()


def descriptor(data):
    return {"digest": "sha256:" + hashlib.sha256(data).hexdigest(), "size": len(data)}


def simulated_release(root, version="0.6.8", revision="a" * 40, config_overrides=None, repeated=False):
    chart = root / version / "deploy/helm/plinth"
    chart.mkdir(parents=True)
    (chart / "Chart.yaml").write_text("name: plinth\n")
    (chart.parents[2] / "VERSION").write_text(version + "\n")
    layer = b"simulated compressed image layer " + version.encode()
    config_body = {"User": "10001:10001", "Entrypoint": ["/usr/local/bin/plinth"],
                   "Labels": {"org.opencontainers.image.version": version,
                              "org.opencontainers.image.revision": revision}}
    config_body.update(config_overrides or {})
    diffs = ["sha256:" + "e" * 64] * (2 if repeated else 1)
    config = encoded({"architecture": "amd64", "os": "linux", "config": config_body,
                      "rootfs": {"diff_ids": diffs}})
    manifest = encoded({"schemaVersion": 2, "mediaType": "application/vnd.oci.image.manifest.v1+json",
                        "config": {**descriptor(config), "mediaType": "application/vnd.oci.image.config.v1+json"},
                        "layers": [{**descriptor(layer), "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip"}]
                        * (2 if repeated else 1)})
    index = encoded({"schemaVersion": 2, "mediaType": "application/vnd.oci.image.index.v1+json", "manifests": [
        {**descriptor(manifest), "mediaType": "application/vnd.oci.image.manifest.v1+json", "platform": {"os": "linux", "architecture": "amd64"}},
        {"digest": "sha256:" + "f" * 64, "size": 123, "mediaType": "application/vnd.oci.image.manifest.v1+json",
         "platform": {"os": "linux", "architecture": "arm64"}},
    ]})
    spec = identity.ReleaseSpec(version, revision, descriptor(index)["digest"], 42, chart)
    image = {"Id": descriptor(config)["digest"], "Architecture": "amd64", "Os": "linux",
             "RepoDigests": [spec.reference],
             "Config": {"User": "10001:10001", "Entrypoint": ["/usr/local/bin/plinth"],
                        "Labels": {"org.opencontainers.image.version": version,
                                   "org.opencontainers.image.revision": revision}}}
    verified = identity.VerifiedRelease(spec, json.loads(index), json.loads(manifest),
                                        identity.blob(descriptor(manifest)), identity.blob(descriptor(config)),
                                        (identity.blob(descriptor(layer)),) * (2 if repeated else 1), image)
    blobs = {descriptor(value)["digest"]: value for value in (layer, config, manifest, index)}
    return verified, blobs


class FakeRunner(identity.CommandRunner):
    def __init__(self, releases):
        super().__init__()
        self.releases = releases
        self.calls = []
        self.change = lambda kind, data: data

    def run(self, argv, **_kwargs):
        argv = [str(item) for item in argv]
        self.calls.append(argv)
        if argv[0] == "git":
            release = next(item for item in self.releases if str(item.spec.chart.parents[2]) == argv[2])
            if "ls-tree" in argv:
                chart_file = release.spec.chart / "Chart.yaml"
                data = b"100644 blob simulated-blob\tdeploy/helm/plinth/Chart.yaml\0"
                return subprocess.CompletedProcess(argv, 0, data, b"")
            if "hash-object" in argv:
                return subprocess.CompletedProcess(argv, 0, b"simulated-blob", b"")
            data = (str(release.spec.chart.parents[2]) if argv[-1] == "--show-toplevel"
                    else "" if "status" in argv or "merge-base" in argv else release.spec.revision)
            return subprocess.CompletedProcess(argv, 0, data.encode(), b"")
        if argv[:2] == ["gh", "api"]:
            if "/jobs?" in argv[-1]:
                data = {"total_count": 1, "jobs": [{"name": "Publish immutable multi-architecture image",
                                                   "status": "completed", "conclusion": "success"}]}
                kind = "jobs"
            else:
                release = self.releases[0]
                data = {"head_sha": release.spec.revision, "head_branch": "v" + release.spec.version,
                        "event": "push", "status": "completed", "conclusion": "success",
                        "path": ".github/workflows/runtime-image.yml", "run_attempt": 1}
                kind = "run"
            return subprocess.CompletedProcess(argv, 0, encoded(self.change(kind, data)), b"")
        if argv[:3] == ["gh", "attestation", "verify"]:
            self.change("attestation", argv)
            return subprocess.CompletedProcess(argv, 0, b"verified simulated input", b"")
        release = self.releases[0]
        if argv[:4] == ["docker", "buildx", "imagetools", "inspect"]:
            if "--raw" not in argv:
                return subprocess.CompletedProcess(argv, 0, ("Name: fake\nDigest: " + release.spec.digest
                                                             + "\nManifests:\nDigest: nested\n").encode(), b"")
            data = release.index if argv[-1] == release.spec.reference else release.manifest
            return subprocess.CompletedProcess(argv, 0, encoded(self.change("graph", data)), b"")
        if argv[:3] == ["docker", "image", "inspect"]:
            return subprocess.CompletedProcess(argv, 0, encoded([self.change("image", release.image_config)]), b"")
        if argv[:2] == ["docker", "pull"]:
            return subprocess.CompletedProcess(argv, 0, b"", b"")
        raise AssertionError("unexpected fake command: " + str(argv))


class PairAndPublicationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release, self.blobs = simulated_release(self.root)

    def pair_file(self, **changes):
        def entry(spec):
            return {"version": spec.version, "revision": spec.revision, "index_digest": spec.digest,
                    "publication_run": spec.publication_run, "chart": str(spec.chart)}
        previous = entry(self.release.spec)
        current = {**previous, "version": "0.6.9", "revision": "b" * 40,
                   "index_digest": "sha256:" + "b" * 64}
        value = {"schema_version": 1, "previous": previous, "current": current,
                 "compatibility": "simulated reviewed forward compatibility reference"}
        value.update(changes)
        path = self.root / "pair.json"
        path.write_text(json.dumps(value))
        return path

    def test_valid_ordered_pair_and_strict_missing_same_or_reversed_identity(self):
        path = self.pair_file()
        self.assertEqual(identity.load_pair(path).previous, self.release.spec)
        original = json.loads(path.read_text())
        for key, value in (("version", "0.6.8"), ("version", "0.6.5"),
                           ("revision", "a" * 40), ("index_digest", self.release.spec.digest),
                           ("publication_run", True)):
            with self.subTest(key=key, value=value):
                changed = json.loads(json.dumps(original))
                changed["current"][key] = value
                path.write_text(json.dumps(changed))
                with self.assertRaises(ValueError):
                    identity.load_pair(path)

    def test_duplicate_unknown_and_oversized_descriptor_fail(self):
        path = self.pair_file(extra=True)
        with self.assertRaises(ValueError):
            identity.load_pair(path)
        path.write_text('{"schema_version":1,"schema_version":1}')
        with self.assertRaisesRegex(ValueError, "duplicate"):
            identity.load_pair(path)
        path.write_bytes(b" " * 65537)
        with self.assertRaisesRegex(ValueError, "64 KiB"):
            identity.load_pair(path)

    def test_simulated_positive_verifies_both_exact_predicates_and_never_writes_registry(self):
        runner = FakeRunner([self.release])
        observed = identity.verify_release(self.release.spec, runner)
        self.assertEqual(observed, self.release)
        attestations = [args for args in runner.calls if args[:3] == ["gh", "attestation", "verify"]]
        self.assertEqual(len(attestations), 2)
        for args in attestations:
            self.assertIn(identity.WORKFLOW, args)
            self.assertIn("refs/tags/v0.6.8", args)
            self.assertIn(self.release.spec.revision, args)
            self.assertIn("oci://" + self.release.spec.reference, args)
        self.assertEqual(attestations[1][-2:], ["--predicate-type", "https://cyclonedx.org/bom"])
        self.assertFalse(any(part in ("push", "tag", "build", "import") for args in runner.calls for part in args))

    def test_changed_run_job_or_config_fails_instead_of_using_labels(self):
        for kind, key, value in (("run", "event", "workflow_dispatch"), ("run", "head_sha", "b" * 40),
                                 ("run", "head_branch", "main"), ("run", "conclusion", "failure"),
                                 ("image", "Id", "sha256:" + "b" * 64)):
            with self.subTest(kind=kind, key=key):
                runner = FakeRunner([self.release])
                runner.change = lambda current, data: {**data, key: value} if current == kind else data
                with self.assertRaises(ValueError):
                    identity.verify_release(self.release.spec, runner)
        runner = FakeRunner([self.release])
        runner.change = lambda kind, data: {"total_count": 0, "jobs": []} if kind == "jobs" else data
        with self.assertRaisesRegex(ValueError, "publish job"):
            identity.verify_release(self.release.spec, runner)

    def test_failed_final_inspect_fails_closed_and_public_cache_is_never_removed(self):
        runner = FakeRunner([self.release])
        original = runner.run
        def failed_inspect(argv, **kwargs):
            if list(argv)[:3] == ["docker", "image", "inspect"]:
                raise ValueError("simulated daemon inspect failure")
            return original(argv, **kwargs)
        runner.run = failed_inspect
        with self.assertRaisesRegex(ValueError, "inspect failure"):
            identity.verify_release(self.release.spec, runner)
        runner.cleanup()
        self.assertFalse(any(args[:3] == ["docker", "image", "rm"] for args in runner.calls))
        cache = identity.CommandRunner()
        with patch.object(cache, "run") as command:
            cache.cleanup()
        command.assert_not_called()

    def test_verifier_failure_never_pulls_or_allocates(self):
        runner = FakeRunner([self.release])
        def fail(kind, data):
            if kind == "attestation":
                raise ValueError("simulated verifier failure")
            return data
        runner.change = fail
        with self.assertRaisesRegex(ValueError, "verifier failure"):
            identity.verify_release(self.release.spec, runner)
        self.assertFalse(any(args[:2] == ["docker", "pull"] for args in runner.calls))

    def test_dirty_chart_or_wrong_head_fails_before_publication_reads(self):
        runner = FakeRunner([self.release])
        original = runner.run
        def dirty(argv, **kwargs):
            if "status" in argv:
                return subprocess.CompletedProcess(argv, 0, b" M Chart.yaml", b"")
            return original(argv, **kwargs)
        runner.run = dirty
        with self.assertRaisesRegex(ValueError, "dirty"):
            identity.verify_release(self.release.spec, runner)
        self.assertFalse(any(args[0] == "gh" for args in runner.calls))


class ExactChartInventoryTest(unittest.TestCase):
    def test_real_git_ignored_extra_and_assume_unchanged_bytes_cannot_pass_exact_chart(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            chart = root / "deploy/helm/plinth"
            chart.mkdir(parents=True)
            source = chart / "Chart.yaml"
            source.write_text("name: plinth\n")
            (root / "VERSION").write_text("0.6.8\n")
            (root / ".gitignore").write_text("deploy/helm/plinth/ignored-*\n")
            def git(*args):
                return subprocess.run(["git", "-C", root, *args], check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q")
            git("add", ".")
            # This commit belongs only to the test's disposable fixture repository.
            git("-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm", "fixture")
            revision = git("rev-parse", "HEAD")
            git("tag", "v0.6.8")
            spec = identity.ReleaseSpec("0.6.8", revision, "sha256:" + "a" * 64, 42, chart)
            runner = identity.CommandRunner()
            identity.check_chart(spec, runner)
            git("tag", "-d", "v0.6.8")
            git("branch", "v0.6.8")
            with self.assertRaisesRegex(ValueError, "command failed"):
                identity.check_chart(spec, runner)
            git("branch", "-D", "v0.6.8")
            with self.assertRaisesRegex(ValueError, "command failed"):
                identity.check_chart(spec, runner)
            git("tag", "v0.6.8")
            ignored = chart / "ignored-template.yaml"
            ignored.write_text("extra: ignored by Git, visible to Helm\n")
            self.assertEqual(git("status", "--porcelain", "--untracked-files=all"), "")
            with self.assertRaisesRegex(ValueError, "inventory"):
                identity.check_chart(spec, runner)
            ignored.unlink()
            git("update-index", "--assume-unchanged", "deploy/helm/plinth/Chart.yaml")
            source.write_text("name: changed-behind-index-flag\n")
            self.assertEqual(git("status", "--porcelain", "--untracked-files=all"), "")
            with self.assertRaisesRegex(ValueError, "bytes"):
                identity.check_chart(spec, runner)


class IndexDescriptorTest(unittest.TestCase):
    def test_only_valid_recognized_platform_attestations_are_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            release, _ = simulated_release(Path(directory))
            original = release.index
            attestation = {"digest": "sha256:" + "c" * 64, "size": 200,
                           "mediaType": "application/vnd.oci.image.manifest.v1+json",
                           "platform": {"os": "unknown", "architecture": "unknown"},
                           "annotations": {"vnd.docker.reference.type": "attestation-manifest",
                                           "vnd.docker.reference.digest": release.platform.digest}}
            accepted = {**original, "manifests": original["manifests"] + [attestation]}
            self.assertEqual(identity.index_platforms(accepted)[("linux", "amd64")], release.platform)
            for field, value in (("digest", "invalid"), ("size", True), ("annotations", {}),
                                 ("mediaType", "application/unrelated"),
                                 ("annotations", {"vnd.docker.reference.type": "attestation-manifest",
                                                  "vnd.docker.reference.digest": "sha256:" + "d" * 64})):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    identity.index_platforms({**original, "manifests": original["manifests"]
                                              + [{**attestation, field: value}]})


class ModeTest(unittest.TestCase):
    def test_mode_inputs_are_explicit_and_source_call_is_unchanged(self):
        source = recovery.parse_args(["--image", "new", "--previous-image", "old",
                                      "--previous-chart", "/old/chart"])
        self.assertIsNone(source.published_pair)
        self.assertEqual(source.image, "new")
        published = recovery.parse_args(["--published-pair", "/pair.json", "--evidence-dir", "/receipt"])
        self.assertIsNone(published.image)
        for args in ([], ["--image", "new"], ["--published-pair", "/pair"],
                     ["--published-pair", "/pair", "--evidence-dir", "/receipt", "--image", "new"]):
            with self.subTest(args=args), patch("sys.stderr"), self.assertRaises(SystemExit):
                recovery.parse_args(args)

    def test_source_preparation_hook_preserves_existing_publish_boundary(self):
        harness = object.__new__(recovery.RecoveryHarness)
        harness.publish_previous = Mock()
        harness.prepare_pair()
        harness.publish_previous.assert_called_once_with()
        self.assertEqual(harness.chart, CHART)

    def test_published_selects_both_charts_and_has_no_local_registry_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            previous, _ = simulated_release(Path(directory))
            current, _ = simulated_release(Path(directory), "0.6.9", "b" * 40)
            harness = object.__new__(recovery.PublishedRecoveryHarness)
            harness.pair = identity.VerifiedPair(previous, current, "simulated")
            harness.previous_digest = previous.spec.digest
            harness.select_release(previous.spec.digest, previous.spec.chart)
            self.assertEqual(harness.chart, previous.spec.chart)
            self.assertEqual(harness.current_chart, current.spec.chart)
            harness.select_release(current.spec.digest, current.spec.chart)
            self.assertEqual(harness.chart, current.spec.chart)
            harness.select_release(previous.spec.digest, previous.spec.chart)
            self.assertEqual(harness.chart, previous.spec.chart)
            with self.assertRaises(AssertionError):
                harness.select_release(current.spec.digest, previous.spec.chart)
            harness.run = Mock(side_effect=AssertionError("no external commands allowed"))
            self.assertEqual(harness.registry_arguments(), [])
            self.assertEqual(harness.cleanup_image(), [])
            self.assertEqual(harness.cleanup_previous_image(), [])
            harness.run.assert_not_called()

    def test_published_bootstrap_delegates_to_existing_modern_auth_only(self):
        harness = object.__new__(recovery.PublishedRecoveryHarness)
        harness.active_pod = Mock()
        for method in ("install_chart", "bootstrap_admin", "remove_bootstrap_authority", "enable_public_route"):
            with self.subTest(method=method), patch.object(Harness, method) as existing:
                getattr(harness, method)()
                existing.assert_called_once_with(harness)


class ChartPreflightTest(unittest.TestCase):
    def test_both_exact_charts_render_modern_isolated_and_exposed_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous, _ = simulated_release(root)
            current, _ = simulated_release(root, "0.6.9", "b" * 40)
            pair = identity.VerifiedPair(previous, current, "simulated")
            observed = []
            class ChartRunner:
                wrong = False
                exposed_kind = None
                def text(self, argv):
                    values = recovery.yaml.safe_load(Path(argv[-1]).read_text())
                    chart = argv[3]
                    bootstrap = bool(values["registration"]["bootstrapSecret"]["name"])
                    observed.append((chart, bootstrap))
                    image = ("wrong" if self.wrong else values["image"]["repository"]
                             + "@" + values["image"]["digest"])
                    container = {"name": "plinth", "image": image}
                    if bootstrap:
                        container["env"] = [{"name": "PLINTH_BOOTSTRAP_TOKEN", "valueFrom": {
                            "secretKeyRef": {"name": "plinth-bootstrap", "key": "bootstrap-token"}}}]
                    objects = [
                        {"kind": "StatefulSet", "spec": {"template": {"spec": {"containers": [container]}}}},
                        {"kind": "ConfigMap", "data": {"config.json": json.dumps({"registration": {"mode": "disabled"}})}}]
                    if bootstrap and self.exposed_kind:
                        objects.append({"kind": self.exposed_kind, "metadata": {"name": "unexpected"}})
                    return recovery.yaml.safe_dump_all(objects)
            runner = ChartRunner()
            recovery.preflight_charts(pair, SimpleNamespace(helm="helm"), runner, root)
            self.assertEqual(observed, [(previous.spec.chart, True), (previous.spec.chart, False),
                                        (current.spec.chart, True), (current.spec.chart, False)])
            for kind in ("IngressRoute", "Middleware", "ServersTransport"):
                runner.exposed_kind = kind
                with self.subTest(kind=kind), self.assertRaisesRegex(AssertionError, "Traefik exposure"):
                    recovery.preflight_charts(pair, SimpleNamespace(helm="helm"), runner, root)
            runner.exposed_kind = None
            runner.wrong = True
            with self.assertRaisesRegex(AssertionError, "image identity"):
                recovery.preflight_charts(pair, SimpleNamespace(helm="helm"), runner, root)

    def test_missing_published_tools_fail_before_descriptor_verification_pull_or_allocation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            args = recovery.parse_args(["--published-pair", str(root / "not-opened.json"),
                                        "--evidence-dir", str(root / "not-created")])
            for missing in ("node", "curl", "openssl", "k3d", "kubectl", "gh"):
                with self.subTest(missing=missing), \
                        patch.object(recovery.shutil, "which", side_effect=lambda tool: None if tool == missing else "/fake/tool"), \
                        patch.object(identity, "load_pair") as descriptor, \
                        patch.object(identity, "verify_pair") as verification, \
                        patch.object(identity, "CommandRunner") as commands, \
                        patch.object(recovery, "PublishedRecoveryHarness") as allocation, \
                        self.assertRaisesRegex(AssertionError, "required executable is unavailable: " + missing):
                    recovery.run_published(args)
                descriptor.assert_not_called()
                verification.assert_not_called()
                commands.assert_not_called()
                allocation.assert_not_called()
                self.assertFalse(args.evidence_dir.exists())

    def test_chart_alias_and_evidence_symlink_ancestry_never_write_into_release_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release, _ = simulated_release(root)
            context = SimpleNamespace(root=root, release=release)
            path = PairAndPublicationTest.pair_file(context)
            original = json.loads(path.read_text())
            release_root = release.spec.chart.parents[2]
            alias_root = root / "linked-release"
            alias_root.symlink_to(release_root, target_is_directory=True)
            aliases = [str(release_root / ".." / release_root.name / "deploy/helm/plinth"),
                       str(alias_root / "deploy/helm/plinth")]
            before = sorted(str(item.relative_to(release_root)) for item in release_root.rglob("*"))
            for alias in aliases:
                changed = json.loads(json.dumps(original))
                changed["previous"]["chart"] = alias
                path.write_text(json.dumps(changed))
                args = recovery.parse_args(["--published-pair", str(path), "--evidence-dir",
                                            str(release_root / "forbidden-evidence")])
                with self.subTest(chart=alias), patch.object(recovery, "require_tools"), \
                        patch.object(identity, "CommandRunner") as runner, \
                        self.assertRaisesRegex(ValueError, "canonical"):
                    recovery.run_published(args)
                runner.assert_not_called()
                self.assertFalse((release_root / "forbidden-evidence").exists())
            path.write_text(json.dumps(original))
            for evidence in (release_root / "inside-evidence", alias_root / "inside-evidence"):
                args = recovery.parse_args(["--published-pair", str(path), "--evidence-dir", str(evidence)])
                with self.subTest(evidence=evidence), patch.object(recovery, "require_tools"), \
                        patch.object(identity, "CommandRunner") as runner, \
                        self.assertRaisesRegex(AssertionError, "outside release"):
                    recovery.run_published(args)
                runner.assert_not_called()
                self.assertFalse((release_root / "inside-evidence").exists())
            after = sorted(str(item.relative_to(release_root)) for item in release_root.rglob("*"))
            self.assertEqual(before, after)

    def test_failed_publication_preflight_does_not_allocate_harness_and_leaves_failed_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release, _ = simulated_release(root)
            context = SimpleNamespace(root=root, release=release)
            path = PairAndPublicationTest.pair_file(context)
            args = recovery.parse_args(["--published-pair", str(path), "--evidence-dir", str(root / "evidence")])
            runner = Mock(receipts=[])
            with patch.object(recovery, "require_tools"), \
                    patch.object(identity, "CommandRunner", return_value=runner), \
                    patch.object(identity, "verify_pair", side_effect=ValueError("simulated publication failure")), \
                    patch.object(recovery, "PublishedRecoveryHarness") as allocation, \
                    self.assertRaisesRegex(ValueError, "publication failure"):
                recovery.run_published(args)
            allocation.assert_not_called()
            runner.cleanup.assert_called_once_with()
            report = json.loads((root / "evidence/receipt.json").read_text())
            self.assertEqual(report["outcome"], "failed")
            self.assertEqual(report["runtime"], [])


class CommandOwnershipTest(unittest.TestCase):
    def test_bounded_python_success_and_exact_binary_hash(self):
        runner = identity.CommandRunner()
        self.assertEqual(runner.text([sys.executable, "-c", "print('owned')"]), "owned\n")
        data = b"binary\x00\xff\r\n"
        expected = identity.Blob("sha256:" + hashlib.sha256(data).hexdigest(), len(data))
        runner.hash_stream([sys.executable, "-c", "import sys;sys.stdout.buffer.write(" + repr(data) + ")"], expected)
        self.assertEqual(runner.receipts[-1]["stdout_bytes"], len(data))

    def test_nonzero_oversize_partial_output_and_timeout_fail_without_lingering_child(self):
        runner = identity.CommandRunner()
        for code, options in (("raise SystemExit(3)", {}),
                              ("print('oversize')", {"maximum": 2}),
                              ("import sys,time;print('partial',flush=True);time.sleep(30)", {"timeout": 0.05})):
            with self.subTest(code=code), self.assertRaises(ValueError):
                runner.run([sys.executable, "-c", code], **options)
            receipt = runner.receipts[-1]
            self.assertEqual(receipt["outcome"], "failed")
            self.assertIsNotNone(receipt["returncode"])
            with self.assertRaises(ProcessLookupError):
                os.kill(receipt["pid"], 0)

    def test_oversize_receipt_hash_accounts_for_every_consumed_byte_before_rejection(self):
        runner = identity.CommandRunner()
        with self.assertRaisesRegex(ValueError, "stdout exceeds"):
            runner.run([sys.executable, "-c", "import sys;sys.stdout.buffer.write(b'x'*100000)"], maximum=257)
        receipt = runner.receipts[-1]
        self.assertGreater(receipt["stdout_bytes"], 257)
        self.assertLessEqual(receipt["stdout_bytes"], 100000)
        self.assertEqual(receipt["stdout_sha256"], hashlib.sha256(b"x" * receipt["stdout_bytes"]).hexdigest())

    def test_group_cleanup_error_does_not_mask_primary_failure_or_leave_leader_unreaped(self):
        runner = identity.CommandRunner()
        with patch.object(identity.os, "killpg", side_effect=PermissionError("not archived")), \
                self.assertRaisesRegex(ValueError, "stdout exceeds"):
            runner.run([sys.executable, "-c", "print('oversized output')"], maximum=1)
        receipt = runner.receipts[-1]
        self.assertEqual(receipt["outcome"], "failed")
        self.assertIn("PermissionError", receipt["cleanup_errors"])
        self.assertNotIn("not archived", json.dumps(receipt))
        with self.assertRaises(ProcessLookupError):
            os.kill(receipt["pid"], 0)

    def descendant_fixture(self, simulate_regression=False):
        """An isolated subreaper owns fallback cleanup even if the runner fails."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pidfile, leaderfile = root / "descendant.pid", root / "leader.pid"
            module_dir = str(Path(__file__).resolve().parent)
            child_code = ("import os,signal,time;from pathlib import Path;"
                          "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
                          "Path(" + repr(str(pidfile)) + ").write_text(str(os.getpid()));time.sleep(60)")
            leader_code = ("import os,subprocess,sys,time;from pathlib import Path;"
                           "Path(" + repr(str(leaderfile)) + ").write_text(str(os.getpid()));"
                           "subprocess.Popen([sys.executable,'-c'," + repr(child_code) + "]);"
                           "exec(\"deadline=time.monotonic()+5\\nwhile not Path(" + repr(str(pidfile))
                           + ").exists():\\n if time.monotonic()>deadline: raise RuntimeError('fixture startup deadline')"
                           "\\n time.sleep(.01)\")")
            supervisor = textwrap.dedent(f"""
                import ctypes,os,signal,subprocess,sys,time
                from pathlib import Path
                sys.path.insert(0,{module_dir!r})
                from published_release_identity import CommandRunner
                assert ctypes.CDLL(None,use_errno=True).prctl(36,1,0,0,0)==0
                pidfile=Path({str(pidfile)!r})
                leaderfile=Path({str(leaderfile)!r})
                runner=CommandRunner()
                leaked=None
                reaped={{}}
                triggered=False
                simulate={simulate_regression!r}
                def deadline(_number,_frame):
                    raise TimeoutError('isolated fixture deadline')
                signal.signal(signal.SIGALRM,deadline)
                signal.signal(signal.SIGTERM,deadline)
                signal.alarm(20)
                try:
                    if simulate:
                        leaked=subprocess.Popen([sys.executable,'-c',{leader_code!r}],
                                                stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
                        startup=time.monotonic()+5
                        while not pidfile.exists():
                            assert time.monotonic()<startup,'fixture startup deadline'
                            time.sleep(.01)
                        signal.alarm(1)
                        while True:
                            time.sleep(1)
                    else:
                        try:
                            runner.run([sys.executable,'-c',{leader_code!r}],timeout=8)
                            raise AssertionError('runner deadline did not fail')
                        except ValueError:
                            pass
                        assert pidfile.exists(),'descendant readiness handshake was not reached'
                        assert runner.receipts[-1]['returncode']==0
                        assert runner.receipts[-1]['outcome']=='failed'
                except TimeoutError:
                    assert simulate,'normal fixture exceeded its independent deadline'
                    triggered=True
                finally:
                    signal.alarm(0)
                    # Group IDs come only from children created by this fixture.
                    groups=set()
                    if leaked is not None:
                        groups.add(leaked.pid)
                    groups.update(item['pid'] for item in runner.receipts)
                    if leaderfile.exists():
                        groups.add(int(leaderfile.read_text()))
                    for group in groups:
                        try:
                            os.killpg(group,signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if leaked is not None:
                        leaked.wait(timeout=5)
                        leaked.stdout.close()
                        leaked.stderr.close()
                    until=time.monotonic()+5
                    while True:
                        try:
                            pid,status=os.waitpid(-1,os.WNOHANG)
                        except ChildProcessError:
                            break
                        if pid:
                            reaped[pid]=status
                        else:
                            assert time.monotonic()<until,'fixture descendant reap deadline'
                            time.sleep(.01)
                descendant=int(pidfile.read_text())
                assert descendant in reaped
                assert os.WIFSIGNALED(reaped[descendant]) and os.WTERMSIG(reaped[descendant])==signal.SIGKILL
                assert not simulate or triggered
                print('fixture cleanup reaped all owned children; simulated regression='+str(simulate))
            """)
            process = subprocess.Popen([sys.executable, "-c", supervisor], stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, start_new_session=True)
            def stop_supervisor():
                # Also used on cancellation, not just subprocess timeout. Kill
                # the known fixture group before asking its subreaper to exit.
                if leaderfile.exists():
                    try:
                        os.killpg(int(leaderfile.read_text()), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    process.send_signal(signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.communicate(timeout=7)
                except subprocess.TimeoutExpired:
                    if leaderfile.exists():
                        try:
                            os.killpg(int(leaderfile.read_text()), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    process.communicate(timeout=5)
            try:
                output, errors = process.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                stop_supervisor()
                self.fail("isolated fixture exceeded its outer deadline")
            finally:
                if process.poll() is None:
                    stop_supervisor()
            self.assertEqual(process.returncode, 0, errors)
            self.assertIn("fixture cleanup reaped all owned children", output)

    @unittest.skipUnless(sys.platform == "linux", "isolated descendant reap check requires Linux")
    def test_exited_leader_with_term_ignoring_pipe_owner_is_killed_and_reaped_by_test_supervisor(self):
        self.descendant_fixture()

    @unittest.skipUnless(sys.platform == "linux", "isolated descendant reap check requires Linux")
    def test_fixture_guard_kills_and_reaps_descendants_when_runner_cleanup_is_simulated_broken(self):
        self.descendant_fixture(simulate_regression=True)



class RuntimeRunner(identity.CommandRunner):
    """Synthetic container metadata only; never an actual runtime-schema fixture."""

    def __init__(self, release, blobs, reference=None, container_name="plinth"):
        super().__init__()
        self.release, self.blobs = release, dict(blobs)
        self.reference = reference or "k3d-owned-reg:5000/plinth@" + release.platform.digest
        self.layer_reads = []
        self.container_id = "1" * 64
        self.calls = []
        self.owner = {"id": "2" * 64, "started_at": "2026-10-24T05:00:00Z"}
        self.pod = {"metadata": {"uid": "pod-uid", "labels": {"canary": "PRIVATE-CANARY"}},
                    "spec": {"nodeName": "k3d-owned-server-0", "containers": [
                        {"name": "plinth", "image": self.reference,
                         "env": [{"name": "DATABASE_PASSWORD", "value": "PRIVATE-CANARY"}]}]},
                    "status": {"containerStatuses": [{"name": "plinth", "containerID": "containerd://" + self.container_id,
                                                       "imageID": self.reference}]}}
        self.cri = {"status": {"id": self.container_id, "metadata": {"name": "plinth"},
                               "image": {"image": release.config.digest, "annotations": {"canary": "PRIVATE-CANARY"}},
                               "imageRef": identity.image_reference(self.reference).canonical},
                    "info": {"runtimeSpec": {"process": {"env": ["PRIVATE-CANARY"]}}}}
        self.image = {"status": {"id": release.config.digest, "repoTags": [],
                                 "repoDigests": [identity.image_reference(self.reference).canonical], "size": "123"},
                      "info": {"imageSpec": {"config": {"Env": ["PRIVATE-CANARY"]}}}}
        self.container = {"ID": self.container_id, "Image": identity.image_reference(self.reference).canonical,
                          "Runtime": {"Name": "io.containerd.runc.v2"},
                          "SnapshotKey": self.container_id, "Snapshotter": "overlayfs",
                          "Spec": {"process": {"env": ["PRIVATE-CANARY"]}}}
        self.pod["spec"]["containers"][0]["name"] = container_name
        self.pod["status"]["containerStatuses"][0]["name"] = container_name
        self.cri["status"]["metadata"]["name"] = container_name

    def run(self, argv, **_kwargs):
        self.calls.append(list(argv))
        if list(argv)[:2] == ["docker", "inspect"]:
            data = encoded(self.owner)
        elif "content" in argv:
            data = self.blobs[argv[-1]]
        elif "inspecti" in argv:
            data = encoded(self.image)
        elif "inspect" in argv:
            data = encoded(self.cri)
        elif "containers" in argv:
            data = encoded(self.container)
        else:
            raise AssertionError(argv)
        return subprocess.CompletedProcess(argv, 0, data, b"")

    def hash_stream(self, argv, expected):
        self.layer_reads.append(argv[-1])
        actual = descriptor(self.blobs[argv[-1]])
        identity.require(actual["digest"] == expected.digest and actual["size"] == expected.size,
                         "runtime content bytes differ")


class SourceRuntimeEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.release, self.blobs = simulated_release(Path(self.temporary.name))
        self.runner = RuntimeRunner(self.release, self.blobs)

    def verify(self, cache=None):
        return identity.verify_source_runtime(self.runner.reference, self.runner.pod,
                                               "k3d-owned-server-0", self.runner.container_id, self.runner,
                                               {} if cache is None else cache)

    def test_simulated_source_manifest_binding_filters_every_secret_surface(self):
        result = self.verify()
        self.assertEqual(result["content"]["manifest"]["digest"], self.release.platform.digest)
        self.assertEqual(result["selected"]["cri"]["imageRef"], self.runner.reference)
        self.assertEqual(result["selected"]["image"]["id"], self.release.config.digest)
        serialized = json.dumps(result)
        self.assertNotIn("PRIVATE-CANARY", serialized)
        self.assertNotIn("Env", serialized)
        self.assertNotIn("Spec", serialized)
        self.assertIn("no published credit", result["qualification"])
        self.assertEqual(self.runner.layer_reads, [self.release.layers[0].digest])

    def test_only_same_node_reference_reuses_verified_content(self):
        cache = {}
        self.verify(cache)
        self.verify(cache)
        self.assertEqual(len(self.runner.layer_reads), 1)
        self.assertEqual(set(cache), {("k3d-owned-server-0", self.runner.owner["id"],
                                      self.runner.owner["started_at"], self.runner.reference)})

    def test_foreign_container_or_image_and_changed_metadata_or_layer_fail(self):
        for field, replacement in (("ID", "foreign-id"), ("Image", "foreign-reference")):
            with self.subTest(field=field):
                original = self.runner.container[field]
                self.runner.container[field] = replacement
                with self.assertRaises(ValueError):
                    self.verify()
                self.runner.container[field] = original
        for digest in (self.release.config.digest, self.release.layers[0].digest):
            with self.subTest(digest=digest):
                original = self.runner.blobs[digest]
                self.runner.blobs[digest] += b"changed"
                with self.assertRaises(ValueError):
                    self.verify()
                self.runner.blobs[digest] = original

    def test_optional_source_hook_keeps_original_assertion_and_writes_only_selected_fields(self):
        harness = object.__new__(recovery.RecoveryHarness)
        path = Path(self.temporary.name) / "identity.json"
        harness.args = SimpleNamespace(runtime_identity_evidence=path)
        harness.internal_repository, harness.candidate_digest = self.runner.reference.split("@")
        harness.container_runtime_identity = Mock(return_value=("k3d-owned-server-0", self.runner.container_id))
        pod = self.runner.pod
        container = pod["spec"]["containers"][0]
        status = pod["status"]["containerStatuses"][0]
        with patch.object(identity, "CommandRunner", return_value=self.runner), patch("builtins.print") as log:
            harness.verify_pod_image(pod, container, status)
            harness.verify_pod_image(pod, container, status)
        self.assertEqual(log.call_count, 1)
        self.assertNotIn("PRIVATE-CANARY", str(log.call_args))
        logged = json.loads(log.call_args.args[0].split(": ", 1)[1])
        self.assertEqual(logged["content"]["manifest"]["digest"], self.release.platform.digest)
        self.assertEqual(logged["content"]["config"]["digest"], self.release.config.digest)
        self.assertEqual(logged["content"]["platform"], "linux/amd64")
        self.assertEqual(logged["content"]["rootfs_diff_ids"], ["sha256:" + "e" * 64])
        self.assertNotIn("layers", logged["content"])

        self.assertNotIn("PRIVATE-CANARY", path.read_text())
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertFalse(json.loads(path.read_text())["published_credit"])
        with self.assertRaises(AssertionError), patch.object(identity, "verify_source_runtime") as verify:
            harness.verify_pod_image(pod, {**container, "image": "wrong"}, status)
        verify.assert_not_called()


class GraphBindingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.release, self.blobs = simulated_release(self.root, repeated=True)

    def runner(self):
        return RuntimeRunner(self.release, self.blobs, self.release.spec.reference)

    def verify(self, runner, cache=None, release=None):
        return identity.verify_runtime(release or self.release, runner.pod, "k3d-owned-server-0",
                                       runner.container_id, runner, {} if cache is None else cache)

    def test_simulated_index_root_and_selected_manifest_handles_bind_without_kind_guessing(self):
        runner = self.runner()
        receipt = self.verify(runner)
        self.assertEqual(receipt["roles"]["pod.imageID"], "root_index")
        self.assertEqual(receipt["content"]["root_kind"], "index")
        self.assertEqual(receipt["content"]["rootfs_diff_ids"], ["sha256:" + "e" * 64] * 2)
        self.assertEqual(runner.layer_reads, [self.release.layers[0].digest] * 2)
        selected = identity.IMAGE + "@" + self.release.platform.digest
        runner = self.runner()
        runner.pod["status"]["containerStatuses"][0]["imageID"] = selected
        runner.cri["status"]["imageRef"] = selected
        runner.image["status"]["repoDigests"] = [selected, self.release.spec.reference]
        receipt = self.verify(runner)
        self.assertEqual(receipt["roles"]["cri.imageRef"], "selected_manifest")
        self.assertEqual(receipt["roles"]["image.repoDigests"], ["selected_manifest", "root_index"])
        self.assertIn(["docker", "exec", "k3d-owned-server-0", "crictl", "inspecti", selected], runner.calls)

    def test_foreign_references_bare_config_and_config_object_handles_never_stand_in_for_images(self):
        for field, value in (("imageID", self.release.config.digest),
                             ("imageID", identity.IMAGE + "@" + self.release.config.digest),
                             ("imageID", "other.invalid/plinth@" + self.release.spec.digest),
                             ("imageRef", identity.IMAGE + "@sha256:" + "a" * 64),
                             ("imageRef", self.release.config.digest)):
            with self.subTest(field=field, value=value):
                runner = self.runner()
                if field == "imageID":
                    runner.pod["status"]["containerStatuses"][0][field] = value
                else:
                    runner.cri["status"][field] = value
                with self.assertRaises(ValueError):
                    self.verify(runner)
        runner = self.runner()
        # A different verified-looking index can share config; it is still not this root.
        alternate = {**self.release.index, "annotations": {"different": "root"}}
        raw = encoded(alternate)
        other = descriptor(raw)["digest"]
        runner.blobs[other] = raw
        runner.cri["status"]["imageRef"] = identity.IMAGE + "@" + other
        with self.assertRaisesRegex(ValueError, "selected image graph"):
            self.verify(runner)
        runner = self.runner()
        alternate_manifest = {**self.release.manifest, "annotations": {"different": "manifest"}}
        raw = encoded(alternate_manifest)
        other = descriptor(raw)["digest"]
        runner.blobs[other] = raw
        runner.pod["status"]["containerStatuses"][0]["imageID"] = identity.IMAGE + "@" + other
        with self.assertRaisesRegex(ValueError, "selected image graph"):
            self.verify(runner)
        runner = self.runner()
        runner.image["status"]["repoDigests"] = ["other.invalid/plinth@" + self.release.spec.digest]
        with self.assertRaises(ValueError):
            self.verify(runner)

    def test_exact_configured_root_and_current_container_are_checked_even_with_cached_content(self):
        runner, cache = self.runner(), {}
        self.verify(runner, cache)
        reads = len(runner.layer_reads)
        for target in ("pod", "ctr"):
            with self.subTest(target=target):
                selected = identity.IMAGE + "@" + self.release.platform.digest
                original = runner.pod["spec"]["containers"][0]["image"] if target == "pod" else runner.container["Image"]
                if target == "pod":
                    runner.pod["spec"]["containers"][0]["image"] = selected
                else:
                    runner.container["Image"] = selected
                with self.assertRaises(ValueError):
                    self.verify(runner, cache)
                if target == "pod":
                    runner.pod["spec"]["containers"][0]["image"] = original
                else:
                    runner.container["Image"] = original
        runner.cri["status"]["image"]["image"] = "sha256:" + "f" * 64
        with self.assertRaisesRegex(ValueError, "config identity"):
            self.verify(runner, cache)
        self.assertEqual(len(runner.layer_reads), reads)

    def test_wrong_node_cid_and_runtime_profile_fail_explicitly(self):
        runner = self.runner()
        for node, cid in (("k3d-foreign-server-0", runner.container_id), ("k3d-owned-server-0", "f" * 64)):
            with self.subTest(node=node, cid=cid), self.assertRaises(ValueError):
                identity.verify_runtime(self.release, runner.pod, node, cid, runner, {})
        self.assertEqual(runner.calls, [])
        for field, value in (("SnapshotKey", "different"), ("Snapshotter", "unknown")):
            runner = self.runner()
            runner.container[field] = value
            with self.assertRaisesRegex(ValueError, "runtime profile"):
                self.verify(runner)
        runner = self.runner()
        runner.image["status"]["id"] = "sha256:" + "f" * 64
        with self.assertRaisesRegex(ValueError, "config identity"):
            self.verify(runner)

    def test_node_lifetime_and_expected_preflight_revalidate_cached_graph(self):
        runner, cache = self.runner(), {}
        self.verify(runner, cache)
        layer_reads = len(runner.layer_reads)
        runner.owner["started_at"] = "2026-10-24T06:00:00Z"
        self.verify(runner, cache)
        self.assertEqual(len(runner.layer_reads), layer_reads * 2)
        self.assertEqual(len(cache), 2)
        wrong = replace(self.release, manifest={**self.release.manifest, "unexpected": True})
        with self.assertRaisesRegex(ValueError, "published preflight"):
            self.verify(runner, cache, wrong)
        wrong = replace(self.release, platform=replace(self.release.platform, size=self.release.platform.size + 1))
        with self.assertRaisesRegex(ValueError, "published preflight"):
            self.verify(runner, cache, wrong)
        key = next(key for key in cache if key[2] == runner.owner["started_at"])
        cache[key] = replace(cache[key], node="foreign")
        with self.assertRaisesRegex(ValueError, "cache identity"):
            self.verify(runner, cache)

    def test_mid_observation_node_lifetime_change_is_rejected(self):
        runner = self.runner()
        original = runner.run
        reads = 0
        def changing_owner(argv, **kwargs):
            nonlocal reads
            if list(argv)[:2] == ["docker", "inspect"]:
                reads += 1
                if reads == 3:
                    runner.owner["started_at"] = "2026-10-24T07:00:00Z"
            return original(argv, **kwargs)
        runner.run = changing_owner
        with self.assertRaisesRegex(ValueError, "lifetime changed during"):
            self.verify(runner)
        self.assertEqual(reads, 3)

    def test_corrupt_root_manifest_config_layer_and_unknown_root_kind_fail_closed(self):
        for digest in (self.release.spec.digest, self.release.platform.digest,
                       self.release.config.digest, self.release.layers[0].digest):
            with self.subTest(digest=digest):
                runner = self.runner()
                runner.blobs[digest] += b"changed"
                with self.assertRaises(ValueError):
                    self.verify(runner)
        runner = self.runner()
        raw = encoded({"schemaVersion": 2, "mediaType": "application/unknown"})
        digest = descriptor(raw)["digest"]
        runner.blobs[digest] = raw
        with self.assertRaisesRegex(ValueError, "root object kind"):
            identity.read_node_graph(identity.IMAGE + "@" + digest, "k3d-owned-server-0", runner, {})

    def test_plinth_actual_config_labels_user_entrypoint_are_required_but_size_stays_opaque(self):
        for override in ({"User": "101:102"}, {"Entrypoint": ["other"]}, {"Labels": {}}):
            with self.subTest(override=override), tempfile.TemporaryDirectory() as directory:
                release, blobs = simulated_release(Path(directory), config_overrides=override)
                runner = RuntimeRunner(release, blobs, release.spec.reference)
                with self.assertRaisesRegex(ValueError, "Plinth release contract"):
                    identity.verify_runtime(release, runner.pod, "k3d-owned-server-0", runner.container_id, runner, {})
        runner = self.runner()
        runner.image["status"]["size"] = "opaque-size-no-sum-formula"
        receipt = self.verify(runner)
        self.assertEqual(receipt["selected"]["image"]["size"], "opaque-size-no-sum-formula")

    def test_actual_flat_field_fixture_links_and_duplicate_diff_ids_have_only_flat_credit(self):
        path = Path(__file__).parent / "fixtures/runtime-identity/flat-plinth-amd64.json"
        fixture = json.loads(path.read_text())
        selected, content = fixture["receipt"]["selected"], fixture["receipt"]["content"]
        # Recorded role/field links only; raw actual OCI bytes are not in this fixture.
        roles = SimpleNamespace(node=selected["pod"]["node"],
                                reference=selected["pod"]["container"]["image"], root_kind="manifest",
                                root=identity.blob(content["root"]), manifest=identity.blob(content["manifest"]),
                                config=identity.blob(content["config"]))
        result = identity.bind_selected_runtime(roles.reference, selected, roles.node,
                                                selected["cri"]["id"], roles)
        self.assertEqual(result["cri.imageRef"], "root_manifest")
        self.assertIsInstance(selected["image"]["size"], str)
        self.assertEqual(len(content["rootfs_diff_ids"]), 7)
        self.assertEqual(content["rootfs_diff_ids"][-2:], [content["rootfs_diff_ids"][-1]] * 2)
        self.assertIn("no raw OCI bytes", fixture["qualification"])


class PublishedPhaseTest(unittest.TestCase):
    def test_receipt_hit_still_rebinds_current_metadata_and_checks_expected_release(self):
        with tempfile.TemporaryDirectory() as directory:
            release, _ = simulated_release(Path(directory))
            harness = object.__new__(recovery.PublishedRecoveryHarness)
            harness.selected, harness.runner, harness.content_cache = release, Mock(), {}
            harness.namespace = "owned"
            pod = {"metadata": {"name": "plinth-pod"}}
            harness.runtime_receipts, harness.runtime_versions, harness.phase = {}, {}, "initial"
            harness.container_runtime_identity = Mock(return_value=("node", "1" * 64))
            harness.kubectl = Mock(return_value=SimpleNamespace(stdout="v0.6.8\n"))
            receipt = {"node_owner": {"id": "2" * 64, "started_at": "time"}, "reference": release.spec.reference}
            with patch.object(identity, "verify_runtime", return_value=receipt) as bind:
                harness.verify_pod_image(pod, None, None)
                harness.verify_pod_image(pod, None, None)
                self.assertEqual(bind.call_count, 2)
                self.assertEqual(harness.kubectl.call_count, 1)
            with patch.object(identity, "verify_runtime", side_effect=ValueError("current root changed")), \
                    self.assertRaisesRegex(ValueError, "current root"):
                harness.verify_pod_image(pod, None, None)

    def test_initial_is_bound_before_bootstrap_and_all_eight_stage_inputs_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            previous, _ = simulated_release(Path(directory))
            current, _ = simulated_release(Path(directory), "0.6.9", "b" * 40)
            harness = object.__new__(recovery.PublishedRecoveryHarness)
            harness.pair = identity.VerifiedPair(previous, current, "simulated")
            harness.select_release(previous.spec.digest, previous.spec.chart)
            observed = []
            harness.active_pod = Mock(side_effect=lambda: observed.append((harness.phase, harness.selected.spec.digest, str(harness.chart))))
            order = []
            with patch.object(Harness, "install_chart", side_effect=lambda _self: order.append("install")), \
                    patch.object(Harness, "bootstrap_admin", side_effect=lambda _self: order.append("bootstrap")):
                harness.install_chart()
                self.assertEqual([item[0] for item in observed], ["initial"])
                harness.bootstrap_admin()
            self.assertEqual(order, ["install", "bootstrap"])
            with patch.object(Harness, "remove_bootstrap_authority"), patch.object(Harness, "enable_public_route"):
                harness.remove_bootstrap_authority()
                harness.enable_public_route()
            def restored(_self, digest, _nonce, chart):
                harness.select_release(digest, chart)
            with patch.object(recovery.RecoveryHarness, "install_restored", side_effect=restored):
                harness.install_restored(previous.spec.digest, "restored-previous", previous.spec.chart)
            with patch.object(recovery.RecoveryHarness, "upgrade_current", side_effect=lambda *_: harness.select_release(current.spec.digest, current.spec.chart)):
                harness.upgrade_current({})
            with patch.object(recovery.RecoveryHarness, "verify_active_signal"):
                harness.verify_active_signal("SIGINT")
                harness.verify_active_signal("SIGTERM")
            with patch.object(recovery.RecoveryHarness, "install_restored", side_effect=restored):
                harness.install_restored(previous.spec.digest, "matched-rollback", previous.spec.chart)
            self.assertEqual([item[0] for item in observed],
                             ["initial", "bootstrap_removed", "public", "restore", "upgrade", "SIGINT", "SIGTERM", "rollback"])
            self.assertEqual([item[1] for item in observed], [previous.spec.digest] * 4 + [current.spec.digest] * 3 + [previous.spec.digest])
    def test_complete_ledger_is_required_before_passed_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            previous, _ = simulated_release(Path(directory))
            current, _ = simulated_release(Path(directory), "0.6.9", "b" * 40)
            harness = object.__new__(recovery.PublishedRecoveryHarness)
            harness.pair = identity.VerifiedPair(previous, current, "simulated")
            phases = ("initial", "bootstrap_removed", "public", "restore", "upgrade", "SIGINT", "SIGTERM", "rollback")
            good = {}
            for index, phase in enumerate(phases):
                release = current if phase in ("upgrade", "SIGINT", "SIGTERM") else previous
                cid = f"{index + 1:064x}"
                good[phase] = {"phase": phase, "reference": release.spec.reference,
                               "chart": str(release.spec.chart), "version": "v" + release.spec.version,
                               "content": {"root_kind": "index", "root": {"digest": release.spec.digest},
                                           "manifest": {"digest": release.platform.digest, "size": release.platform.size},
                                           "config": {"digest": release.config.digest, "size": release.config.size}},
                               "selected": {"cri": {"id": cid}, "pod": {"container": {"image": release.spec.reference,
                                                                                        "containerID": "containerd://" + cid}}}}
            harness.runtime_receipts = copy.deepcopy(good)
            with patch("builtins.print") as summary:
                harness.print_summary()
            summary.assert_called_once()
            missing = copy.deepcopy(good)
            del missing["bootstrap_removed"]
            wrong_root = copy.deepcopy(good)
            wrong_root["restore"]["content"]["root"]["digest"] = current.spec.digest
            wrong_chart = copy.deepcopy(good)
            wrong_chart["initial"]["chart"] = str(current.spec.chart)
            wrong_version = copy.deepcopy(good)
            wrong_version["SIGTERM"]["version"] = "v0.6.8"
            duplicate = copy.deepcopy(good)
            duplicate["rollback"]["selected"] = copy.deepcopy(duplicate["initial"]["selected"])
            for value in (missing, wrong_root, wrong_chart, wrong_version, duplicate):
                harness.runtime_receipts = value
                with self.subTest(value=value), patch("builtins.print") as summary, self.assertRaises(AssertionError):
                    harness.print_summary()
                summary.assert_not_called()

    def restarted_harness(self, wrong_new=False):
        harness = object.__new__(recovery.PublishedRecoveryHarness)
        harness.namespace, harness.release, harness.cluster = "owned", "owned", "owned"
        old_id, new_id = "1" * 64, "2" * 64
        def pod(cid):
            return {"metadata": {"name": "plinth-pod", "uid": "pod"},
                    "spec": {"nodeName": "k3d-owned-server-0", "containers": [{"name": "plinth", "image": "exact-root"}]},
                    "status": {"containerStatuses": [{"name": "plinth", "containerID": "containerd://" + cid}]}}
        old, new = pod(old_id), pod(new_id)
        harness.ready_pod_snapshot = Mock(side_effect=[old, new])
        harness.kubectl = Mock(return_value=SimpleNamespace(returncode=0))
        def verify(_pod, _container, status):
            if status["containerID"] == "containerd://" + old_id:
                raise ValueError("retired old CRI must not be inspected")
            if wrong_new:
                raise ValueError("new binding invalid")
        harness.verify_pod_image = Mock(side_effect=verify)
        return harness, old_id, new

    def test_old_ready_retired_cri_is_not_bound_and_new_snapshot_is_fully_verified(self):
        harness, old_id, new = self.restarted_harness()
        with patch.object(recovery.time, "sleep") as sleep:
            self.assertEqual(harness._wait_restarted(old_id), new)
        sleep.assert_called_once_with(0.5)
        harness.verify_pod_image.assert_called_once()
        self.assertEqual(harness.verify_pod_image.call_args.args[0], new)

    def test_wrong_new_binding_propagates_without_blanket_retry(self):
        harness, old_id, _new = self.restarted_harness(wrong_new=True)
        with patch.object(recovery.time, "sleep"), self.assertRaisesRegex(ValueError, "new binding invalid"):
            harness._wait_restarted(old_id)
        self.assertEqual(harness.ready_pod_snapshot.call_count, 2)
        harness.verify_pod_image.assert_called_once()



class SupportingObservationTest(unittest.TestCase):
    def test_default_is_noop_and_preserves_original_ready_and_denial_commands(self):
        harness = object.__new__(Harness)
        harness.args, harness.namespace, harness.release = SimpleNamespace(), "owned", "owned"
        harness.apply_json = Mock()
        harness.kubectl = Mock(side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(returncode=22)])
        harness.verify_network_policy_denial()
        self.assertEqual(harness.kubectl.call_count, 2)
        self.assertEqual(harness.kubectl.call_args_list[0].args[:3], ("wait", "--for=condition=Ready", "pod/attacker"))
        self.assertEqual(harness.kubectl.call_args_list[1].args[:5], ("exec", "-n", "owned", "attacker", "--"))

    def test_existing_attacker_index_and_flat_profiles_use_shared_binding_without_plinth_policy(self):
        import k3d_lifecycle_test as lifecycle
        for indexed, required in ((True, False), (False, False), (True, True)):
            with self.subTest(indexed=indexed, required=required), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                release, blobs = simulated_release(root, config_overrides={"User": "101:102", "Entrypoint": ["curl"], "Labels": {}})
                digest = release.spec.digest if indexed else release.platform.digest
                reference = "docker.io/curlimages/curl:8.16.0@" + digest
                runner = RuntimeRunner(release, blobs, reference, "attacker")
                runner.pod["metadata"].update(name="attacker", namespace="owned")
                harness = object.__new__(Harness)
                path = root / "identity.json"
                harness.args = SimpleNamespace(runtime_identity_evidence=path, require_indexed_helper_proof=required)
                harness.namespace, harness.release, harness.cluster = "owned", "owned", "owned"
                harness.apply_json = Mock()
                harness.kubectl = Mock(side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(stdout=json.dumps(runner.pod)), SimpleNamespace(returncode=22)])
                with patch.object(lifecycle, "CURL_IMAGE", reference), patch.object(identity, "CommandRunner", return_value=runner), \
                        patch.object(identity, "validate_release_graph") as plinth_policy, patch("builtins.print") as log:
                    harness.verify_network_policy_denial()
                plinth_policy.assert_not_called()
                self.assertEqual(harness.kubectl.call_count, 3)
                self.assertEqual(harness.kubectl.call_args_list[2].args[:5], ("exec", "-n", "owned", "attacker", "--"))
                report = json.loads(path.read_text())
                self.assertEqual(report["receipt"]["indexed_runtime_proof"], indexed)
                self.assertEqual(report["receipt"]["selected"]["pod"]["container"]["name"], "attacker")
                self.assertEqual(report["receipt"]["selected"]["pod"]["container"]["image"], reference)
                self.assertNotIn("PRIVATE-CANARY", path.read_text())
                self.assertNotIn("PRIVATE-CANARY", str(log.call_args))
                self.assertEqual(log.call_count, 1)

    def test_required_flat_observation_fails_without_receipt_log_or_denial_credit(self):
        import k3d_lifecycle_test as lifecycle
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            release, blobs = simulated_release(root)
            reference = "docker.io/curlimages/curl:8.16.0@" + release.platform.digest
            runner = RuntimeRunner(release, blobs, reference, "attacker")
            runner.pod["metadata"].update(name="attacker", namespace="owned")
            path = root / "identity.json"
            harness = object.__new__(Harness)
            harness.args = SimpleNamespace(runtime_identity_evidence=path, require_indexed_helper_proof=True)
            harness.namespace, harness.release, harness.cluster = "owned", "owned", "owned"
            harness.apply_json = Mock()
            harness.kubectl = Mock(side_effect=[SimpleNamespace(returncode=0), SimpleNamespace(stdout=json.dumps(runner.pod))])
            with patch.object(lifecycle, "CURL_IMAGE", reference), patch.object(identity, "CommandRunner", return_value=runner), \
                    patch("builtins.print") as log, self.assertRaisesRegex(AssertionError, "required indexed helper proof observed a flat root"):
                harness.verify_network_policy_denial()
            self.assertEqual(harness.kubectl.call_count, 2)  # Ready and selected Pod; denial curl was not credited.
            self.assertFalse(path.exists())
            log.assert_not_called()
            self.assertEqual(runner.layer_reads, [release.layers[0].digest])

    def test_required_index_flag_without_evidence_fails_before_tools_or_allocation(self):
        import k3d_lifecycle_test as lifecycle
        with patch.object(sys, "argv", ["k3d_lifecycle_test.py", "--image", "simulated", "--require-indexed-helper-proof"]), \
                patch.object(lifecycle.shutil, "which") as which, patch.object(lifecycle, "Harness") as harness, \
                patch("sys.stderr"), self.assertRaises(SystemExit) as error:
            lifecycle.main()
        self.assertEqual(error.exception.code, 2)
        which.assert_not_called()
        harness.assert_not_called()

    def test_public_curl_parent_index_retains_the_previous_exact_amd64_graph(self):
        import k3d_lifecycle_test as lifecycle
        root = Path(__file__).parent / "fixtures/runtime-identity"
        index_raw = (root / "curl-8.16.0-parent-index.json").read_bytes()
        manifest_raw = (root / "curl-8.16.0-amd64-manifest.json").read_bytes()
        index, manifest = json.loads(index_raw), json.loads(manifest_raw)
        expected_index = "sha256:463eaf6072688fe96ac64fa623fe73e1dbe25d8ad6c34404a669ad3ce1f104b6"
        expected_child = "sha256:5a91ea0c9c3ee27b4abe657b68cf6bf0676afa13b236b3bda34283cb3924d4f6"
        self.assertEqual(descriptor(index_raw), {"digest": expected_index, "size": 1193})
        self.assertEqual(lifecycle.CURL_IMAGE, "docker.io/curlimages/curl:8.16.0@" + expected_index)
        self.assertEqual(descriptor(manifest_raw), {"digest": expected_child, "size": 751})
        self.assertEqual(identity.selected_amd64(index), identity.blob(descriptor(manifest_raw)))
        self.assertEqual(manifest["config"], {"digest": "sha256:51674a38c3309d85352d13cf0955f588d7fe6a49cbb0c8b049c544c36441f6dc",
                                            "size": 1464, "mediaType": "application/vnd.docker.container.image.v1+json"})
        duplicate = copy.deepcopy(index)
        duplicate["manifests"].append(copy.deepcopy(next(item for item in index["manifests"]
                                                        if item["platform"]["architecture"] == "amd64")))
        with self.assertRaisesRegex(ValueError, "one unique amd64"):
            identity.selected_amd64(duplicate)

    def test_generic_index_can_select_amd64_among_additional_upstream_platforms(self):
        with tempfile.TemporaryDirectory() as directory:
            release, blobs = simulated_release(Path(directory))
            index = copy.deepcopy(release.index)
            index["manifests"].append({"digest": "sha256:" + "9" * 64, "size": 123,
                                        "mediaType": "application/vnd.oci.image.manifest.v1+json",
                                        "platform": {"os": "linux", "architecture": "386"}})
            raw = encoded(index)
            digest = descriptor(raw)["digest"]
            blobs[digest] = raw
            reference = "docker.io/curlimages/curl:8.16.0@" + digest
            runner = RuntimeRunner(release, blobs, reference, "attacker")
            receipt, graph = identity.observe_runtime(reference, runner.pod, "k3d-owned-server-0",
                                                      runner.container_id, runner, {}, "attacker")
            self.assertEqual(graph.root_kind, "index")
            self.assertEqual(receipt["roles"]["cri.imageRef"], "root_index")
            with self.assertRaises(ValueError):
                identity.index_platforms(index)

    def test_missing_node_content_fails_explicitly_without_an_index_success_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            release, blobs = simulated_release(Path(directory))
            runner = RuntimeRunner(release, blobs, release.spec.reference)
            del runner.blobs[release.spec.digest]
            with self.assertRaises(KeyError):
                identity.observe_runtime(release.spec.reference, runner.pod, "k3d-owned-server-0", runner.container_id, runner, {})


if __name__ == "__main__":
    unittest.main(verbosity=2)
