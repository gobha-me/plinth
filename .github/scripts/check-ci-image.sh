#!/usr/bin/env bash
set -euo pipefail

if (($# != 0)); then
  echo "usage: .github/scripts/check-ci-image.sh" >&2
  exit 2
fi

# Run inside the freshly built docker/ci.Dockerfile image, against a disposable
# PostgreSQL instance. Require the database configuration so PG/WS tests cannot
# silently skip because the image-validation job forgot to provide it.
: "${PLINTH_PG_HOST:?disposable PostgreSQL host is required}"
: "${PLINTH_PG_PORT:?disposable PostgreSQL port is required}"
: "${PLINTH_PG_USER:?disposable PostgreSQL user is required}"
: "${PLINTH_PG_PASSWORD:?disposable PostgreSQL password is required}"
: "${PLINTH_PG_DATABASE:?disposable PostgreSQL database is required}"

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"
build_dir=${PROJECT_TIDY_BUILD_DIR:-/build-ci-image}

clang-21 --version
clang-format-21 --version
clang-tidy-21 --version
PGPASSWORD="$PLINTH_PG_PASSWORD" psql --no-psqlrc --set=ON_ERROR_STOP=1 \
  --host="$PLINTH_PG_HOST" --port="$PLINTH_PG_PORT" \
  --username="$PLINTH_PG_USER" --dbname="$PLINTH_PG_DATABASE" \
  --command='SELECT 1'

tools/format.sh --check
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure --parallel 1
"$build_dir/plinth" --version
PROJECT_TIDY_BUILD_DIR="$build_dir" PROJECT_TIDY_JOBS=2 tools/lint.sh
