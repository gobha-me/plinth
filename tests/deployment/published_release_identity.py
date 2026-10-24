#!/usr/bin/env python3
"""Verify published recovery inputs; this helper never publishes an artifact."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import signal
import subprocess
import sys
import time

REPOSITORY = "gobha-me/plinth"
IMAGE = "ghcr.io/" + REPOSITORY
WORKFLOW = REPOSITORY + "/.github/workflows/runtime-image.yml"
DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
METADATA_LIMIT = 4 * 1024 * 1024
CONTENT_LIMIT = 4 * 1024 * 1024 * 1024
INDEX_TYPES = {"application/vnd.oci.image.index.v1+json", "application/vnd.docker.distribution.manifest.list.v2+json"}
MANIFEST_TYPES = {"application/vnd.oci.image.manifest.v1+json", "application/vnd.docker.distribution.manifest.v2+json"}
CONFIG_TYPES = {"application/vnd.oci.image.config.v1+json", "application/vnd.docker.container.image.v1+json"}
LAYER_TYPES = {"application/vnd.oci.image.layer.v1.tar", "application/vnd.oci.image.layer.v1.tar+gzip",
               "application/vnd.oci.image.layer.v1.tar+zstd", "application/vnd.docker.image.rootfs.diff.tar.gzip"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate JSON key")
        result[key] = value
    return result


def parse_json(data):
    return json.loads(data, object_pairs_hook=unique_object)


def keys(value, expected):
    require(isinstance(value, dict) and set(value) == set(expected),
            "unexpected or missing descriptor fields")


@dataclass(frozen=True)
class ReleaseSpec:
    version: str
    revision: str
    digest: str
    publication_run: int
    chart: Path

    @property
    def reference(self):
        return IMAGE + "@" + self.digest


@dataclass(frozen=True)
class PairSpec:
    previous: ReleaseSpec
    current: ReleaseSpec
    compatibility: str


@dataclass(frozen=True)
class Blob:
    digest: str
    size: int


def blob(value):
    require(isinstance(value, dict), "invalid OCI descriptor")
    digest, size = value.get("digest"), value.get("size")
    require(isinstance(digest, str) and DIGEST.fullmatch(digest), "invalid OCI digest")
    require(type(size) is int and 0 < size <= CONTENT_LIMIT, "invalid OCI size")
    return Blob(digest, size)


@dataclass(frozen=True)
class VerifiedRelease:
    spec: ReleaseSpec
    index: dict
    manifest: dict
    platform: Blob
    config: Blob
    layers: tuple[Blob, ...]
    image_config: dict


@dataclass(frozen=True)
class VerifiedPair:
    previous: VerifiedRelease
    current: VerifiedRelease
    compatibility: str


def load_pair(path):
    with Path(path).open("rb") as source:
        data = source.read(65537)
    require(len(data) <= 65536, "pair descriptor exceeds 64 KiB")
    value = parse_json(data)
    keys(value, ("schema_version", "previous", "current", "compatibility"))
    require(type(value["schema_version"]) is int and value["schema_version"] == 1,
            "unsupported pair schema")
    compatibility = value["compatibility"]
    require(isinstance(compatibility, str) and compatibility.strip() and len(compatibility) <= 4096,
            "a reviewed forward compatibility evidence reference is required")

    def release(entry):
        keys(entry, ("version", "revision", "index_digest", "publication_run", "chart"))
        version = entry["version"]
        require(isinstance(version, str) and VERSION.fullmatch(version), "invalid release version")
        require(tuple(map(int, version.split("."))) >= (0, 6, 6), "unsupported release version")
        require(isinstance(entry["revision"], str) and REVISION.fullmatch(entry["revision"]),
                "invalid release source revision")
        require(isinstance(entry["index_digest"], str) and DIGEST.fullmatch(entry["index_digest"]),
                "invalid release index digest")
        require(type(entry["publication_run"]) is int and entry["publication_run"] > 0,
                "invalid publication run")
        require(isinstance(entry["chart"], str) and entry["chart"], "missing chart")
        chart = Path(entry["chart"])
        require(chart.is_absolute() and str(chart) == entry["chart"]
                and chart.resolve(strict=True) == chart and len(chart.parents) >= 3
                and chart == chart.parents[2] / "deploy/helm/plinth",
                "chart input must be an existing canonical absolute checkout path")
        return ReleaseSpec(version, entry["revision"], entry["index_digest"],
                           entry["publication_run"], chart)

    previous, current = release(value["previous"]), release(value["current"])
    require(tuple(map(int, previous.version.split("."))) < tuple(map(int, current.version.split("."))),
            "release versions must increase")
    require(previous.revision != current.revision and previous.digest != current.digest,
            "release identities must be distinct")
    return PairSpec(previous, current, compatibility)


class CommandRunner:
    """Own and bound both pipes and the complete child process group."""

    def __init__(self):
        self.receipts = []

    def _run(self, argv, *, timeout, maximum, streaming=False, check=True):
        argv = [str(part) for part in argv]
        digest = hashlib.sha256()
        output, errors = bytearray(), bytearray()
        count = 0
        process = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        deadline = time.monotonic() + timeout
        receipt = {"argv": argv, "pid": process.pid, "outcome": "failed"}
        self.receipts.append(receipt)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ, "stdout")
                selector.register(process.stderr, selectors.EVENT_READ, "stderr")
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "identity command timed out")
                    for key, _events in selector.select(min(remaining, 0.1)):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                        elif key.data == "stderr":
                            require(len(errors) + len(chunk) <= 65536, "identity stderr exceeds limit")
                            errors.extend(chunk)
                        else:
                            count += len(chunk)
                            require(count <= maximum, "identity stdout exceeds limit")
                            digest.update(chunk)
                            if not streaming:
                                output.extend(chunk)
                process.wait(timeout=max(0.001, deadline - time.monotonic()))
            require(not check or process.returncode == 0, "identity command failed: " + argv[0])
            receipt["outcome"] = "completed"
            return subprocess.CompletedProcess(argv, process.returncode, bytes(output), bytes(errors)), count, digest.hexdigest()
        finally:
            primary = sys.exc_info()[0] is not None
            cleanup_errors = []
            # A reaped leader does not imply descendants closed their pipes.
            for number in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(process.pid, number)
                except ProcessLookupError:
                    pass
                except OSError as error:
                    cleanup_errors.append(type(error).__name__)
                if number == signal.SIGTERM and process.poll() is None:
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
            try:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    cleanup_errors.append("leader_wait_timeout")
                    try:
                        process.kill()
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=5)
            except (OSError, subprocess.SubprocessError) as error:
                cleanup_errors.append(type(error).__name__)
            finally:
                receipt.update(returncode=process.returncode, stdout_bytes=count,
                               stdout_sha256=digest.hexdigest())
                process.stdout.close()
                process.stderr.close()
            if cleanup_errors:
                receipt["cleanup_errors"] = cleanup_errors
                receipt["outcome"] = "failed"
                if not primary:
                    raise RuntimeError("identity command cleanup failed")

    def run(self, argv, *, timeout=60, maximum=METADATA_LIMIT, check=True):
        result, _count, _digest = self._run(argv, timeout=timeout, maximum=maximum, check=check)
        return result

    def text(self, argv, **kwargs):
        return self.run(argv, **kwargs).stdout.decode("utf-8")

    def json(self, argv, **kwargs):
        return parse_json(self.run(argv, **kwargs).stdout)

    def hash_stream(self, argv, expected):
        _result, count, digest = self._run(argv, timeout=180, maximum=expected.size, streaming=True)
        require(count == expected.size and "sha256:" + digest == expected.digest,
                "runtime content bytes differ from verified descriptor")

    def cleanup(self):
        # Fixed public immutable references are shared host cache, not task tags.
        # A pull or a failed inspect never establishes exclusive ownership.
        pass


def check_chart(spec, runner):
    chart = spec.chart.resolve(strict=True)
    require(len(chart.parents) >= 3, "invalid chart root")
    root = chart.parents[2]
    require(chart == root / "deploy/helm/plinth" and chart == spec.chart,
            "chart must be an exact canonical checkout path")
    require((chart / "Chart.yaml").is_file(), "chart missing Chart.yaml")
    git = ["git", "-C", root]
    require(Path(runner.text(git + ["rev-parse", "--show-toplevel"]).strip()) == root,
            "chart is outside its source checkout")
    require(runner.text(git + ["rev-parse", "HEAD"]).strip() == spec.revision,
            "chart HEAD differs from release source")
    require(runner.text(git + ["rev-parse", "refs/tags/v" + spec.version + "^{commit}"]).strip() == spec.revision,
            "chart tag differs from release source")
    require(not runner.text(git + ["status", "--porcelain", "--untracked-files=all"]).strip(),
            "release chart checkout is dirty")
    require((root / "VERSION").read_text().strip() == spec.version, "chart VERSION differs from release")
    # Git status omits ignored files and may honor assume-unchanged bits. Helm
    # reads the working tree, so compare its actual chart files to exact blobs.
    tree = runner.run(git + ["ls-tree", "-rz", "HEAD", "--", "deploy/helm/plinth"]).stdout
    expected = {}
    for record in tree.split(b"\0"):
        if not record:
            continue
        metadata, name = record.split(b"\t", 1)
        mode, kind, object_id = metadata.decode("ascii").split()
        require(mode in ("100644", "100755") and kind == "blob", "chart contains a symlink or submodule")
        expected[name.decode("utf-8")] = (mode, object_id)
    actual = {}
    for path in chart.rglob("*"):
        require(not path.is_symlink(), "chart contains a working-tree symlink")
        if path.is_dir():
            continue
        require(path.is_file(), "chart contains a nonregular file")
        actual[path.relative_to(root).as_posix()] = path
    require(expected and set(actual) == set(expected), "chart file inventory differs from exact source")
    for name, path in actual.items():
        mode, object_id = expected[name]
        require(bool(path.stat().st_mode & 0o111) == (mode == "100755"), "chart file mode differs from source")
        observed = runner.text(git + ["hash-object", "--no-filters", "--", name]).strip()
        require(observed == object_id, "chart file bytes differ from exact source")



def index_platforms(index):
    require(index.get("mediaType") in INDEX_TYPES, "unsupported index media type")
    platforms, attestations = {}, []
    for entry in index["manifests"]:
        require(isinstance(entry, dict), "invalid platform descriptor")
        descriptor = blob(entry)
        require(entry.get("mediaType") in MANIFEST_TYPES, "unsupported platform media type")
        platform = entry.get("platform", {})
        require(isinstance(platform, dict), "invalid platform metadata")
        key = (platform.get("os"), platform.get("architecture"))
        if key == ("unknown", "unknown"):
            annotations = entry.get("annotations", {})
            require(entry.get("mediaType") == "application/vnd.oci.image.manifest.v1+json"
                    and isinstance(annotations, dict)
                    and annotations.get("vnd.docker.reference.type") == "attestation-manifest"
                    and descriptor.size <= METADATA_LIMIT and not platform.get("variant"),
                    "unrecognized attestation descriptor")
            attestations.append(annotations.get("vnd.docker.reference.digest"))
            continue
        require(key in (("linux", "amd64"), ("linux", "arm64")) and key not in platforms
                and not platform.get("variant"), "unexpected or ambiguous image platform")
        platforms[key] = descriptor
    require(set(platforms) == {("linux", "amd64"), ("linux", "arm64")}, "supported platforms missing")
    runnable_digests = {item.digest for item in platforms.values()}
    require(len(runnable_digests) == 2, "runnable platform identities must be distinct")
    require(all(isinstance(item, str) and item in runnable_digests for item in attestations),
            "attestation does not reference a runnable platform")
    return platforms


def verify_release(spec, runner):
    check_chart(spec, runner)
    endpoint = f"repos/{REPOSITORY}/actions/runs/{spec.publication_run}"
    run = runner.json(["gh", "api", endpoint])
    require(run.get("head_sha") == spec.revision and run.get("head_branch") == "v" + spec.version
            and run.get("event") == "push" and run.get("status") == "completed"
            and run.get("conclusion") == "success"
            and run.get("path") == ".github/workflows/runtime-image.yml",
            "release publication run is not a successful exact tag push")
    attempt = run.get("run_attempt")
    require(type(attempt) is int and attempt > 0, "publication attempt missing")
    jobs = []
    for page in range(1, 21):
        result = runner.json(["gh", "api", endpoint + f"/attempts/{attempt}/jobs?per_page=100&page={page}"])
        require(isinstance(result.get("jobs"), list), "publication jobs missing")
        jobs.extend(result["jobs"])
        if len(jobs) == result.get("total_count"):
            break
    else:
        raise ValueError("publication job pagination exceeds limit")
    publish = [job for job in jobs if job.get("name") == "Publish immutable multi-architecture image"]
    require(len(publish) == 1 and publish[0].get("status") == "completed"
            and publish[0].get("conclusion") == "success", "official publish job did not succeed")
    for extra in ([], ["--predicate-type", "https://cyclonedx.org/bom"]):
        runner.run(["gh", "attestation", "verify", "oci://" + spec.reference,
                    "--repo", REPOSITORY, "--signer-workflow", WORKFLOW,
                    "--source-ref", "refs/tags/v" + spec.version,
                    "--source-digest", spec.revision, *extra], timeout=120)
    inspection = runner.text(["docker", "buildx", "imagetools", "inspect", IMAGE + ":v" + spec.version])
    top = inspection.split("Manifests:", 1)[0]
    roots = re.findall(r"^Digest:\s*(sha256:[0-9a-f]{64})\s*$", top, re.MULTILINE)
    require(roots == [spec.digest], "official version tag differs from verified index")
    index = runner.json(["docker", "buildx", "imagetools", "inspect", "--raw", spec.reference])
    require(isinstance(index, dict) and index.get("schemaVersion") == 2
            and isinstance(index.get("manifests"), list) and len(index["manifests"]) <= 128,
            "release is not an image index")
    platforms = index_platforms(index)
    selected = platforms[("linux", "amd64")]
    manifest = runner.json(["docker", "buildx", "imagetools", "inspect", "--raw", IMAGE + "@" + selected.digest])
    require(isinstance(manifest, dict) and manifest.get("schemaVersion") == 2 and isinstance(manifest.get("layers"), list)
            and 0 < len(manifest["layers"]) <= 256, "invalid selected image manifest")
    require(manifest.get("mediaType") in MANIFEST_TYPES, "unsupported manifest media type")
    require(isinstance(manifest.get("config"), dict) and manifest["config"].get("mediaType") in CONFIG_TYPES,
            "unsupported config media type")
    require(all(isinstance(item, dict) and item.get("mediaType") in LAYER_TYPES for item in manifest["layers"]),
            "unsupported layer media type")
    config = blob(manifest.get("config"))
    layers = tuple(blob(entry) for entry in manifest["layers"])
    require(sum(item.size for item in layers) <= CONTENT_LIMIT, "selected image exceeds content limit")
    runner.run(["docker", "pull", "--platform", "linux/amd64", spec.reference], timeout=180)
    images = runner.json(["docker", "image", "inspect", spec.reference])
    require(isinstance(images, list) and len(images) == 1, "selected image inspect missing")
    image = images[0]
    labels = image.get("Config", {}).get("Labels", {})
    require(image.get("Id") == config.digest and image.get("Architecture") == "amd64"
            and image.get("Os") == "linux" and spec.reference in image.get("RepoDigests", [])
            and labels.get("org.opencontainers.image.version") == spec.version
            and labels.get("org.opencontainers.image.revision") == spec.revision
            and image.get("Config", {}).get("User") == "10001:10001"
            and image.get("Config", {}).get("Entrypoint") == ["/usr/local/bin/plinth"],
            "selected image config differs from release identity")
    return VerifiedRelease(spec, index, manifest, selected, config, layers, image)


def verify_pair(spec, runner):
    previous = verify_release(spec.previous, runner)
    current = verify_release(spec.current, runner)
    runner.run(["git", "-C", spec.current.chart.parents[2], "merge-base", "--is-ancestor",
                spec.previous.revision, spec.current.revision])
    return VerifiedPair(previous, current, spec.compatibility)


def verify_runtime(_release, _pod, _node, _container_id, _runner, _cache):
    """Fail closed until actual pinned CI schema evidence validates the adapter."""
    raise ValueError("published runtime binding requires reviewed actual CRI/containerd schema fixtures")


def selected_runtime_fields(pod, cri, image, container):
    """Whitelist only identity fields; never persist runtime Spec/Env/Secrets."""
    status = cri.get("status", {})
    image_status = image.get("status", {})
    pod_container = next(item for item in pod["spec"]["containers"] if item.get("name") == "plinth")
    pod_status = next(item for item in pod["status"]["containerStatuses"] if item.get("name") == "plinth")
    def scalar(value):
        return value if type(value) in (str, int) else None

    def strings(value):
        return value if isinstance(value, list) and all(isinstance(item, str) for item in value) else None

    source_image = status.get("image")
    if isinstance(source_image, dict):
        source_image = {"image": scalar(source_image.get("image"))}
    else:
        source_image = scalar(source_image)
    return {
        "pod": {"uid": scalar(pod["metadata"]["uid"]), "node": scalar(pod["spec"]["nodeName"]),
                "container": {"name": "plinth", "image": scalar(pod_container.get("image")),
                              "containerID": scalar(pod_status.get("containerID")),
                              "imageID": scalar(pod_status.get("imageID"))}},
        "cri": {"id": scalar(status.get("id")), "name": scalar(status.get("metadata", {}).get("name")),
                "image": source_image, "imageRef": scalar(status.get("imageRef"))},
        "image": {"id": scalar(image_status.get("id")), "repoTags": strings(image_status.get("repoTags")),
                  "repoDigests": strings(image_status.get("repoDigests")), "size": scalar(image_status.get("size"))},
        "containerd": {"ID": scalar(container.get("ID")), "Image": scalar(container.get("Image")),
                       "Runtime.Name": scalar(container.get("Runtime", {}).get("Name")),
                       "SnapshotKey": scalar(container.get("SnapshotKey")), "Snapshotter": scalar(container.get("Snapshotter"))},
    }



def verify_source_runtime(reference, pod, node, container_id, runner, cache):
    """Actual source-image graph proof; CRI digest forms remain opaque here.

    This verifies task-node content, not publication, attestations or the held
    strict published binding adapter. No runtime environment fields are returned.
    """
    repository, separator, digest = reference.rpartition("@")
    require(separator and repository and DIGEST.fullmatch(digest), "source image is not digest pinned")
    prefix = ["docker", "exec", node]
    cri = runner.json(prefix + ["crictl", "inspect", container_id])
    image_ref = cri.get("status", {}).get("imageRef")
    require(isinstance(image_ref, str) and image_ref and len(image_ref) <= 2048
            and not image_ref.startswith("-") and "\x00" not in image_ref, "CRI image reference unavailable")
    image = runner.json(prefix + ["crictl", "inspecti", image_ref])
    container = runner.json(prefix + ["ctr", "--namespace", "k8s.io", "containers", "info", container_id])
    selected = selected_runtime_fields(pod, cri, image, container)
    require(selected["pod"]["node"] == node and selected["cri"]["id"] == container_id
            and selected["pod"]["container"]["containerID"] == "containerd://" + container_id
            and selected["pod"]["container"]["image"] == reference
            and selected["containerd"]["ID"] == container_id
            and selected["containerd"]["Image"] == reference,
            "source container does not bind the exact task image reference")
    key = (node, reference)
    if key not in cache:
        def content(expected_digest, expected_size=None):
            raw = runner.run(prefix + ["ctr", "--namespace", "k8s.io", "content", "get", expected_digest],
                             maximum=min(METADATA_LIMIT, expected_size or METADATA_LIMIT)).stdout
            require("sha256:" + hashlib.sha256(raw).hexdigest() == expected_digest
                    and (expected_size is None or len(raw) == expected_size), "node content identity differs")
            return parse_json(raw), Blob(expected_digest, len(raw))
        root, root_blob = content(digest)
        require(isinstance(root, dict) and root.get("schemaVersion") == 2, "unsupported source root manifest")
        if root.get("mediaType") in INDEX_TYPES:
            matches = [entry for entry in root.get("manifests", [])
                       if entry.get("platform", {}).get("os") == "linux"
                       and entry.get("platform", {}).get("architecture") == "amd64"]
            require(len(matches) == 1, "ambiguous source amd64 platform")
            selected_manifest = blob(matches[0])
            manifest, _ = content(selected_manifest.digest, selected_manifest.size)
        else:
            manifest, selected_manifest = root, root_blob
        require(manifest.get("mediaType") in MANIFEST_TYPES, "unsupported source image manifest")
        config_blob = blob(manifest.get("config"))
        config, _ = content(config_blob.digest, config_blob.size)
        require(config.get("architecture") == "amd64" and config.get("os") == "linux",
                "source node config platform differs")
        layers = tuple(blob(entry) for entry in manifest.get("layers", []))
        diffs = config.get("rootfs", {}).get("diff_ids", [])
        require(0 < len(layers) <= 256 and sum(item.size for item in layers) <= CONTENT_LIMIT
                and len(diffs) == len(layers)
                and all(isinstance(item, str) and DIGEST.fullmatch(item) for item in diffs),
                "source rootfs descriptors invalid")
        for layer in layers:
            runner.hash_stream(prefix + ["ctr", "--namespace", "k8s.io", "content", "get", layer.digest], layer)
        cache[key] = {"root": {"digest": root_blob.digest, "size": root_blob.size},
                      "manifest": {"digest": selected_manifest.digest, "size": selected_manifest.size},
                      "config": {"digest": config_blob.digest, "size": config_blob.size},
                      "platform": "linux/amd64", "rootfs_diff_ids": diffs,
                      "layers": [{"digest": item.digest, "size": item.size} for item in layers]}
    return {"qualification": "selected actual source identity fields; CRI digests opaque; no published credit",
            "selected": selected, "content": cache[key]}
