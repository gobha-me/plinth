#!/usr/bin/env bash
set -euo pipefail

readonly llvm_version=${1:?usage: install-llvm.sh VERSION}
readonly key_sha256=8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d
key_file=$(mktemp /tmp/llvm-snapshot-key.XXXXXX)
trap 'cmake -E remove -f "$key_file"' EXIT

curl --fail --location --silent --show-error \
  --output "$key_file" https://apt.llvm.org/llvm-snapshot.gpg.key
printf '%s  %s\n' "$key_sha256" "$key_file" | sha256sum --check --status

sudo install -d -m 0755 /etc/apt/keyrings
sudo install -m 0644 "$key_file" /etc/apt/keyrings/apt.llvm.org.asc
printf '%s\n' \
  "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/noble/ llvm-toolchain-noble-${llvm_version} main" \
  | sudo tee /etc/apt/sources.list.d/llvm.list >/dev/null
sudo apt-get update
sudo apt-get install -y "clang-${llvm_version}"
