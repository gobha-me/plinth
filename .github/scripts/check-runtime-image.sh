#!/usr/bin/env bash
set -euo pipefail

if (($# != 3)); then
  echo "usage: .github/scripts/check-runtime-image.sh IMAGE VERSION REVISION" >&2
  exit 2
fi

image=$1
expected_version=$2
expected_revision=$3
expected_arch=${PLINTH_EXPECTED_ARCH:-}
expected_release=${PLINTH_EXPECTED_RELEASE:-0}

if [[ ! $expected_version =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
  echo "expected VERSION must be MAJOR.MINOR.PATCH" >&2
  exit 2
fi
if [[ ! $expected_revision =~ ^[0-9a-f]{40}$ ]]; then
  echo "expected REVISION must be a full lowercase Git SHA" >&2
  exit 2
fi
if [[ $expected_release != 0 && $expected_release != 1 ]]; then
  echo "PLINTH_EXPECTED_RELEASE must be 0 or 1" >&2
  exit 2
fi

inspect=$(docker image inspect "$image")
user=$(jq -er '.[0].Config.User' <<<"$inspect")
workdir=$(jq -er '.[0].Config.WorkingDir' <<<"$inspect")
entrypoint=$(jq -cer '.[0].Config.Entrypoint' <<<"$inspect")
version_label=$(jq -er '.[0].Config.Labels["org.opencontainers.image.version"]' \
  <<<"$inspect")
revision_label=$(jq -er '.[0].Config.Labels["org.opencontainers.image.revision"]' \
  <<<"$inspect")
architecture=$(jq -er '.[0].Architecture' <<<"$inspect")

[[ $user == "10001:10001" ]] || {
  echo "runtime image user is $user, expected 10001:10001" >&2
  exit 1
}
[[ $workdir == "/var/lib/plinth" ]] || {
  echo "runtime image workdir is $workdir, expected /var/lib/plinth" >&2
  exit 1
}
[[ $entrypoint == '["/usr/local/bin/plinth"]' ]] || {
  echo "runtime image entrypoint is $entrypoint" >&2
  exit 1
}
[[ $version_label == "$expected_version" ]] || {
  echo "runtime image version label is $version_label, expected $expected_version" >&2
  exit 1
}
[[ $revision_label == "$expected_revision" ]] || {
  echo "runtime image revision label is $revision_label, expected $expected_revision" >&2
  exit 1
}
if [[ -n $expected_arch && $architecture != "$expected_arch" ]]; then
  echo "runtime image architecture is $architecture, expected $expected_arch" >&2
  exit 1
fi

docker run --rm --entrypoint /bin/sh "$image" -euc '
  test "$(id -u)" = 10001
  test "$(id -g)" = 10001
  test -x /usr/local/bin/plinth
  test -s /usr/local/share/plinth/bundled/shell.zip
  test -s /usr/local/share/plinth/migrations/schema.sql
  test -s /usr/local/share/plinth/migrations/extension_database.sql
  test -d /var/lib/plinth/data
  test -d /var/lib/plinth/logs
  touch /var/lib/plinth/data/.runtime-image-write-check
  rm /var/lib/plinth/data/.runtime-image-write-check
  test ! -e /src
  test ! -e /work
  test ! -e /.git
  test ! -e /var/lib/plinth/.env
  test ! -e /var/lib/plinth/config.json
  ! command -v cc >/dev/null 2>&1
  ! command -v c++ >/dev/null 2>&1
  ! command -v cmake >/dev/null 2>&1
  ! command -v git >/dev/null 2>&1
  ! command -v make >/dev/null 2>&1
'

version_output=$(docker run --rm "$image" --version)
if [[ $expected_release == 1 ]]; then
  expected_identity="v$expected_version"
else
  expected_identity="v$expected_version-dev+g${expected_revision:0:12}"
fi
if [[ $version_output != "$expected_identity" ]]; then
  echo "runtime binary reported $version_output, expected $expected_identity" >&2
  exit 1
fi

python3 tests/runtime_image/verify.py \
  --image "$image" --expected-revision "$expected_revision" \
  --expected-identity "$expected_identity"
