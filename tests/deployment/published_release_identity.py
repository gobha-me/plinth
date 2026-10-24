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
                            digest.update(chunk)
                            require(count <= maximum, "identity stdout exceeds limit")
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


def selected_runtime_fields(pod, cri, image, container, container_name="plinth"):
    """Whitelist only identity fields; never persist runtime Spec/Env/Secrets."""
    status = cri.get("status", {})
    image_status = image.get("status", {})
    pod_container = next(item for item in pod["spec"]["containers"] if item.get("name") == container_name)
    pod_status = next(item for item in pod["status"]["containerStatuses"] if item.get("name") == container_name)
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
                "container": {"name": container_name, "image": scalar(pod_container.get("image")),
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




@dataclass(frozen=True)
class ImageReference:
    repository: str
    digest: str
    tag: str | None = None

    @property
    def canonical(self):
        return self.repository + "@" + self.digest


def image_reference(value):
    require(isinstance(value, str) and len(value) <= 2048, "image reference unavailable")
    name, separator, digest = value.rpartition("@")
    require(separator and DIGEST.fullmatch(digest)
            and re.fullmatch(r"[a-z0-9][a-z0-9./:_-]*", name), "unsupported digest-pinned image reference")
    tag = None
    if name.rfind(":") > name.rfind("/"):
        name, tag = name.rsplit(":", 1)
        require(bool(re.fullmatch(r"[a-z0-9_][a-z0-9_.-]{0,127}", tag)), "unsupported image tag")
    require(name and not name.endswith("/"), "image repository missing")
    return ImageReference(name, digest, tag)


@dataclass(frozen=True)
class NodeGraph:
    node: str
    reference: str
    root: Blob
    root_kind: str
    root_document: dict
    manifest: Blob
    manifest_document: dict
    config: Blob
    config_document: dict
    layers: tuple[Blob, ...]
    diff_ids: tuple[str, ...]
    owner: tuple[str, str]

    def summary(self):
        def descriptor(item):
            return {"digest": item.digest, "size": item.size}
        return {"root": descriptor(self.root), "root_kind": self.root_kind,
                "manifest": descriptor(self.manifest), "config": descriptor(self.config),
                "platform": "linux/amd64", "rootfs_diff_ids": list(self.diff_ids),
                "layers": [descriptor(item) for item in self.layers]}


def node_owner(node, runner):
    # A fixed safe projection; never capture Docker Env/Config/Spec here.
    owner = runner.json(["docker", "inspect", "--type", "container", "--format",
                         '{"id":{{json .Id}},"started_at":{{json .State.StartedAt}}}', node])
    require(isinstance(owner, dict) and isinstance(owner.get("id"), str)
            and re.fullmatch(r"[0-9a-f]{64}", owner["id"])
            and isinstance(owner.get("started_at"), str)
            and re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z", owner["started_at"])
            and not owner["started_at"].startswith("0001-"), "owned node lifetime unavailable")
    return owner["id"], owner["started_at"]


def selected_amd64(index):
    entries = index.get("manifests")
    require(isinstance(entries, list) and 0 < len(entries) <= 128, "invalid node image index")
    matches = []
    for entry in entries:
        descriptor = blob(entry)
        require(entry.get("mediaType") in MANIFEST_TYPES, "unsupported indexed object kind")
        platform = entry.get("platform", {})
        require(isinstance(platform, dict), "invalid node index platform")
        if platform.get("os") == "linux" and platform.get("architecture") == "amd64":
            require(not platform.get("variant"), "unsupported amd64 variant")
            matches.append(descriptor)
    require(len(matches) == 1, "node index lacks one unique amd64 manifest")
    return matches[0]


def validate_graph_edges(graph):
    if graph.root_kind == "index":
        require(graph.root_document.get("mediaType") in INDEX_TYPES
                and selected_amd64(graph.root_document) == graph.manifest, "cached index edges differ")
    else:
        require(graph.root_kind == "manifest" and graph.root == graph.manifest
                and graph.root_document == graph.manifest_document, "cached root kind differs")
    require(graph.manifest_document.get("mediaType") in MANIFEST_TYPES
            and blob(graph.manifest_document.get("config")) == graph.config
            and tuple(blob(item) for item in graph.manifest_document.get("layers", [])) == graph.layers
            and tuple(graph.config_document.get("rootfs", {}).get("diff_ids", [])) == graph.diff_ids,
            "cached manifest or config edges differ")


def read_node_graph(reference, node, runner, cache):
    expected = image_reference(reference)
    owner = node_owner(node, runner)
    key = (node, *owner, expected.canonical)
    if key in cache:
        graph = cache[key]
        require(isinstance(graph, NodeGraph) and graph.node == node
                and graph.reference == expected.canonical and graph.root.digest == expected.digest
                and graph.owner == owner, "node content cache identity differs")
        validate_graph_edges(graph)
        return graph
    prefix = ["docker", "exec", node, "ctr", "--namespace", "k8s.io", "content", "get"]

    def content(digest, size=None):
        raw = runner.run(prefix + [digest], maximum=min(METADATA_LIMIT, size or METADATA_LIMIT)).stdout
        require("sha256:" + hashlib.sha256(raw).hexdigest() == digest
                and (size is None or len(raw) == size), "node content identity differs")
        document = parse_json(raw)
        require(isinstance(document, dict), "node content is not an OCI object")
        return document, Blob(digest, len(raw))

    root, root_blob = content(expected.digest)
    require(root.get("schemaVersion") == 2, "unsupported node root schema")
    if root.get("mediaType") in INDEX_TYPES:
        manifest_blob = selected_amd64(root)
        manifest, _ = content(manifest_blob.digest, manifest_blob.size)
        root_kind = "index"
    elif root.get("mediaType") in MANIFEST_TYPES:
        manifest, manifest_blob, root_kind = root, root_blob, "manifest"
    else:
        raise ValueError("unsupported node root object kind")
    require(manifest.get("schemaVersion") == 2 and manifest.get("mediaType") in MANIFEST_TYPES,
            "selected node object is not an image manifest")
    config_entry = manifest.get("config")
    require(isinstance(config_entry, dict) and config_entry.get("mediaType") in CONFIG_TYPES,
            "unsupported node config kind")
    config_blob = blob(config_entry)
    require(config_blob.digest not in (root_blob.digest, manifest_blob.digest), "ambiguous config object role")
    config, _ = content(config_blob.digest, config_blob.size)
    require(config.get("architecture") == "amd64" and config.get("os") == "linux", "node config platform differs")
    layer_entries = manifest.get("layers")
    require(isinstance(layer_entries, list) and 0 < len(layer_entries) <= 256, "invalid node layer list")
    require(all(isinstance(item, dict) and item.get("mediaType") in LAYER_TYPES for item in layer_entries),
            "unsupported node layer kind")
    layers = tuple(blob(entry) for entry in layer_entries)
    rootfs = config.get("rootfs")
    require(isinstance(rootfs, dict), "node rootfs is missing")
    diffs = rootfs.get("diff_ids")
    require(sum(item.size for item in layers) <= CONTENT_LIMIT and isinstance(diffs, list)
            and len(diffs) == len(layers)
            and all(isinstance(item, str) and DIGEST.fullmatch(item) for item in diffs), "node rootfs descriptors invalid")
    # Ordering and repeated layers/diff IDs are part of the image identity.
    for layer in layers:
        runner.hash_stream(prefix + [layer.digest], layer)
    graph = NodeGraph(node, expected.canonical, root_blob, root_kind, root, manifest_blob, manifest,
                      config_blob, config, layers, tuple(diffs), owner)
    validate_graph_edges(graph)
    cache[key] = graph
    return graph


def resolve_image_role(value, expected, graph):
    """Resolve a qualified image reference via verified OCI object kinds/edges."""
    actual = image_reference(value)
    require(actual.repository == expected.repository and actual.tag in (None, expected.tag),
            "runtime image repository or tag differs")
    if actual.digest == graph.root.digest:
        require(graph.root_kind in ("index", "manifest"), "unknown root object role")
        return "root_index" if graph.root_kind == "index" else "root_manifest"
    if graph.root_kind == "index" and actual.digest == graph.manifest.digest:
        require(graph.manifest_document.get("mediaType") in MANIFEST_TYPES, "unknown selected manifest role")
        return "selected_manifest"
    # A config or a foreign-platform manifest cannot stand in for an image reference.
    raise ValueError("runtime reference does not resolve to the selected image graph")


def bind_selected_runtime(reference, selected, node, container_id, graph, container_name="plinth"):
    expected = image_reference(reference)
    require(graph.node == node and graph.reference == expected.canonical and graph.root.digest == expected.digest,
            "runtime graph owner or root differs")
    require(isinstance(container_id, str) and re.fullmatch(r"[0-9a-f]{64}", container_id), "unsupported container identity")
    pod, cri, image, ctr = (selected[name] for name in ("pod", "cri", "image", "containerd"))
    require(isinstance(pod["uid"], str) and pod["uid"] and pod["node"] == node
            and pod["container"]["name"] == container_name
            and pod["container"]["containerID"] == "containerd://" + container_id
            and pod["container"]["image"] == reference and cri["id"] == container_id
            and cri["name"] == container_name and ctr["ID"] == container_id
            and ctr["SnapshotKey"] == container_id
            and ctr["Runtime.Name"] == "io.containerd.runc.v2" and ctr["Snapshotter"] == "overlayfs",
            "runtime container or pinned runtime profile differs")
    configured = image_reference(ctr["Image"])
    require((ctr["Image"] == reference if expected.tag is None else
             configured.canonical == expected.canonical and configured.tag in (None, expected.tag)),
            "containerd container does not retain the configured root reference")
    require(isinstance(cri["image"], dict) and cri["image"].get("image") == graph.config.digest
            and image["id"] == graph.config.digest, "CRI config identity differs from verified config")
    require(isinstance(image["size"], str) and isinstance(image["repoTags"], list),
            "unsupported CRI image metadata shape")
    digests = image["repoDigests"]
    require(isinstance(digests, list) and digests, "CRI repository identities missing")
    roles = {"pod.imageID": resolve_image_role(pod["container"]["imageID"], expected, graph),
             "cri.imageRef": resolve_image_role(cri["imageRef"], expected, graph),
             "image.repoDigests": [resolve_image_role(item, expected, graph) for item in digests]}
    return roles


def observe_runtime(reference, pod, node, container_id, runner, cache, container_name="plinth"):
    expected = image_reference(reference)
    require(pod.get("spec", {}).get("nodeName") == node
            and isinstance(container_id, str) and re.fullmatch(r"[0-9a-f]{64}", container_id),
            "runtime Pod owner differs")
    containers = [item for item in pod["spec"].get("containers", []) if item.get("name") == container_name]
    statuses = [item for item in pod.get("status", {}).get("containerStatuses", [])
                if item.get("name") == container_name]
    require(len(containers) == len(statuses) == 1 and containers[0].get("image") == reference
            and statuses[0].get("containerID") == "containerd://" + container_id,
            "runtime Pod container differs")
    owner_before = node_owner(node, runner)
    prefix = ["docker", "exec", node]
    cri = runner.json(prefix + ["crictl", "inspect", container_id])
    ctr = runner.json(prefix + ["ctr", "--namespace", "k8s.io", "containers", "info", container_id])
    graph = read_node_graph(reference, node, runner, cache)
    actual_ref = cri.get("status", {}).get("imageRef")
    resolve_image_role(actual_ref, expected, graph)
    # Query the actual CRI reference, never a guessed root/config alias.
    image = runner.json(prefix + ["crictl", "inspecti", actual_ref])
    selected = selected_runtime_fields(pod, cri, image, ctr, container_name)
    roles = bind_selected_runtime(reference, selected, node, container_id, graph, container_name)
    require(graph.owner == owner_before == node_owner(node, runner), "node lifetime changed during observation")
    return {"selected": selected, "content": graph.summary(), "roles": roles,
            "node_owner": {"id": graph.owner[0], "started_at": graph.owner[1]}}, graph


def validate_release_graph(release, graph):
    require(graph.reference == release.spec.reference and graph.root_kind == "index"
            and graph.root.digest == release.spec.digest and graph.root_document == release.index
            and graph.manifest == release.platform and graph.manifest_document == release.manifest
            and graph.config == release.config and graph.layers == release.layers,
            "node graph differs from verified published preflight")
    config = graph.config_document.get("config", {})
    require(isinstance(config, dict), "actual Plinth config is missing")
    labels = config.get("Labels", {})
    require(isinstance(labels, dict), "actual Plinth labels are missing")
    require(config.get("User") == "10001:10001" and config.get("Entrypoint") == ["/usr/local/bin/plinth"]
            and labels.get("org.opencontainers.image.version") == release.spec.version
            and labels.get("org.opencontainers.image.revision") == release.spec.revision,
            "actual node config differs from the Plinth release contract")
    require(release.image_config.get("Id") == graph.config.digest,
            "preflight config identity differs from actual node config")
    preflight = release.image_config.get("Config", {})
    require(all(config.get(name) == preflight.get(name) for name in ("User", "Entrypoint", "Labels")),
            "preflight Plinth config fields differ from actual node config")


def verify_runtime(release, pod, node, container_id, runner, cache):
    receipt, graph = observe_runtime(release.spec.reference, pod, node, container_id, runner, cache)
    validate_release_graph(release, graph)
    receipt["reference"] = release.spec.reference
    receipt["qualification"] = "verified published preflight/node binding; actual pair journey still required"
    return receipt


def verify_source_runtime(reference, pod, node, container_id, runner, cache):
    receipt, _graph = observe_runtime(reference, pod, node, container_id, runner, cache)
    receipt["qualification"] = "verified source-image node binding; no published credit"
    return receipt


def log_identity(receipt):
    names = ("root", "root_kind", "manifest", "config", "platform", "rootfs_diff_ids")
    return {"selected": receipt["selected"], "content": {name: receipt["content"][name] for name in names},
            "roles": receipt["roles"], "node_owner": receipt["node_owner"],
            "qualification": receipt["qualification"]}
