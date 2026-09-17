#!/usr/bin/env bash
set -euo pipefail

# Install the exact deployment-validation toolchain without modifying the host.
# Upstream release checksums are repeated here so a moved or replaced asset
# fails closed before any executable reaches PATH.

HELM_VERSION=v3.22.0
KUBECTL_VERSION=v1.36.4
K3D_VERSION=v5.9.0
KUBECONFORM_VERSION=v0.8.0

mode=${1:-all}
if [[ $mode != all && $mode != contract ]]; then
  echo "usage: $0 [all|contract]" >&2
  exit 2
fi

case "$(uname -m)" in
  x86_64)
    architecture=amd64
    helm_sha256=1e4ab49e429626cf6c6958d914248b78c9730803c2751b87627e171dc800e7bb
    kubectl_sha256=8b8f088da2dab964f853b38464033b1be15ede2839eca751482357c45abdd05a
    k3d_sha256=06d8f25bc3a971c4eb29e0ff08429b180402db0f4dec838c9eac427e296800a0
    kubeconform_sha256=9bc2bffbf71f261128533edaf912153948b7ff238f9a531ae6d34466ec287883
    ;;
  aarch64 | arm64)
    architecture=arm64
    helm_sha256=f14e804dfee240f55525b667488fe9adca349e63e00c9af634c0beb1421ac310
    kubectl_sha256=0ecf44450ee6063bf19dd166a103ee6df4a9034455c2abce626e6eea657d73fb
    k3d_sha256=03cde5cf23e6e8e67de5a039ecf26e5b85aca82fba3e5d13dadf904cd218a250
    kubeconform_sha256=1f53fc8e81258197a35e8603054162a5af1de8c5af13746c71ab680d9534ed87
    ;;
  *)
    echo "unsupported deployment-tool architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

destination=${PLINTH_DEPLOYMENT_TOOLS_DIR:-/tmp/plinth-deployment-tools/bin}
mkdir -p "$destination"
temporary=$(mktemp -d /tmp/plinth-deployment-tools.XXXXXX)
cleanup() {
  rm -rf -- "$temporary"
}
trap cleanup EXIT

download() {
  local url=$1
  local output=$2
  local expected=$3
  curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 \
    --retry 4 --retry-all-errors --retry-delay 2 \
    --output "$output" "$url"
  printf '%s  %s\n' "$expected" "$output" | sha256sum --check --status
}

helm_archive="$temporary/helm.tar.gz"
download \
  "https://get.helm.sh/helm-${HELM_VERSION}-linux-${architecture}.tar.gz" \
  "$helm_archive" "$helm_sha256"
tar -xzf "$helm_archive" -C "$temporary"
install -m 0755 "$temporary/linux-${architecture}/helm" "$destination/helm"

kubeconform_archive="$temporary/kubeconform.tar.gz"
download \
  "https://github.com/yannh/kubeconform/releases/download/${KUBECONFORM_VERSION}/kubeconform-linux-${architecture}.tar.gz" \
  "$kubeconform_archive" "$kubeconform_sha256"
tar -xzf "$kubeconform_archive" -C "$temporary" kubeconform
install -m 0755 "$temporary/kubeconform" "$destination/kubeconform"

"$destination/helm" version --short | grep -F "${HELM_VERSION#v}" >/dev/null
[[ $("$destination/kubeconform" -v) == "$KUBECONFORM_VERSION" ]]

if [[ $mode == all ]]; then
  if ! command -v jq >/dev/null; then
    echo "all mode requires jq to verify kubectl and k3d versions" >&2
    exit 1
  fi

  download \
    "https://dl.k8s.io/release/${KUBECTL_VERSION}/bin/linux/${architecture}/kubectl" \
    "$temporary/kubectl" "$kubectl_sha256"
  install -m 0755 "$temporary/kubectl" "$destination/kubectl"

  download \
    "https://github.com/k3d-io/k3d/releases/download/${K3D_VERSION}/k3d-linux-${architecture}" \
    "$temporary/k3d" "$k3d_sha256"
  install -m 0755 "$temporary/k3d" "$destination/k3d"

  [[ $("$destination/kubectl" version --client=true --output=json \
    | jq -r '.clientVersion.gitVersion') == "$KUBECTL_VERSION" ]]
  [[ $("$destination/k3d" version --output json | jq -r '.k3d') == "$K3D_VERSION" ]]
fi

printf 'deployment tools installed in %s\n' "$destination"
