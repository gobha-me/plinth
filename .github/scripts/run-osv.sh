#!/usr/bin/env bash
set -euo pipefail

readonly scanner_version=2.5.1
readonly scanner_sha256=f9f25499a2c8cc367b3af45df2ea7eeca7fbccceab9c35079968f4b3652194be
scanner=$(mktemp /tmp/osv-scanner.XXXXXX)
trap 'cmake -E remove -f "$scanner"' EXIT

curl --fail --location --silent --show-error \
  --output "$scanner" \
  "https://github.com/google/osv-scanner/releases/download/v${scanner_version}/osv-scanner_linux_amd64"
printf '%s  %s\n' "$scanner_sha256" "$scanner" | sha256sum --check --status
chmod 0700 "$scanner"

"$scanner" scan source --recursive .
