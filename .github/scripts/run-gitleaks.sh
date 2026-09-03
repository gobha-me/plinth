#!/usr/bin/env bash
set -euo pipefail

readonly scanner_version=8.30.1
readonly scanner_sha256=551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb
archive=$(mktemp /tmp/gitleaks.XXXXXX.tar.gz)
scanner_dir=$(mktemp -d /tmp/gitleaks.XXXXXX)
trap 'cmake -E remove -f "$archive"; cmake -E remove_directory "$scanner_dir"' EXIT

curl --fail --location --silent --show-error \
  --output "$archive" \
  "https://github.com/gitleaks/gitleaks/releases/download/v${scanner_version}/gitleaks_${scanner_version}_linux_x64.tar.gz"
printf '%s  %s\n' "$scanner_sha256" "$archive" | sha256sum --check --status
tar --extract --gzip --file "$archive" --directory "$scanner_dir" gitleaks

"$scanner_dir/gitleaks" git --redact --log-opts='--all'
"$scanner_dir/gitleaks" dir --redact .
