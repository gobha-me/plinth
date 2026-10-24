# curl 8.16.0 public manifest metadata

Captured on 2026-10-24 by an anonymous read-only request to
`https://registry-1.docker.io/v2/curlimages/curl/manifests/8.16.0` and then its
selected amd64 manifest by digest. These are exact public index/manifest bytes,
not simulated OCI metadata. No config object, environment, layer bytes, registry
credentials, or raw runtime objects are included.

- Parent index: `sha256:463eaf6072688fe96ac64fa623fe73e1dbe25d8ad6c34404a669ad3ce1f104b6`, 1193 bytes.
- Its sole linux/amd64 child: `sha256:5a91ea0c9c3ee27b4abe657b68cf6bf0676afa13b236b3bda34283cb3924d4f6`, 751 bytes.
- Child config descriptor: `sha256:51674a38c3309d85352d13cf0955f588d7fe6a49cbb0c8b049c544c36441f6dc`, 1464 bytes.

The child equals the curl 8.16.0 manifest pinned directly in
[k3d_lifecycle_test.py at 3e3a0d7132a5](https://github.com/gobha-me/plinth/blob/3e3a0d7132a5decdfc09bf79df0a92e9212826b6/tests/deployment/k3d_lifecycle_test.py#L57).
Pinning this parent therefore preserves the selected amd64 content graph.
These metadata fixtures alone provide no indexed-runtime execution, Plinth
provenance, or published release-pair recovery proof. Both min/max candidate
journeys must independently pass the required indexed-helper observation.
