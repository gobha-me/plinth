cmake_minimum_required(VERSION 3.20)

# VERSION is authoritative so exported GitHub source archives build with the
# same version as a Git checkout. Git contributes only development metadata.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PLINTH_VERSION)
string(STRIP "${PLINTH_VERSION}" PLINTH_VERSION)
if(NOT PLINTH_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "VERSION must contain one MAJOR.MINOR.PATCH value")
endif()
set(PLINTH_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PLINTH_VERSION_MINOR ${CMAKE_MATCH_2})
set(PLINTH_VERSION_PATCH ${CMAKE_MATCH_3})
set(PLINTH_GIT_VERSION "v${PLINTH_VERSION}")

set(PLINTH_BUILD_REVISION "" CACHE STRING
    "Exact 40-character source revision supplied by a hermetic build")
option(PLINTH_BUILD_EXACT_TAG
       "Report the release version without development revision metadata" OFF)
if(PLINTH_BUILD_REVISION)
  string(LENGTH "${PLINTH_BUILD_REVISION}" PLINTH_BUILD_REVISION_LENGTH)
  if(NOT PLINTH_BUILD_REVISION_LENGTH EQUAL 40 OR
     NOT PLINTH_BUILD_REVISION MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
            "PLINTH_BUILD_REVISION must be a 40-character lowercase Git SHA")
  endif()
  if(NOT PLINTH_BUILD_EXACT_TAG)
    string(SUBSTRING "${PLINTH_BUILD_REVISION}" 0 12 PLINTH_GIT_SHA)
    set(PLINTH_GIT_VERSION
        "v${PLINTH_VERSION}-dev+g${PLINTH_GIT_SHA}")
  endif()
elseif(PLINTH_BUILD_EXACT_TAG)
  message(FATAL_ERROR
          "PLINTH_BUILD_EXACT_TAG requires PLINTH_BUILD_REVISION")
else()
  find_package(Git QUIET)
endif()

if(NOT PLINTH_BUILD_REVISION AND Git_FOUND AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
            --match "v${PLINTH_VERSION}" HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE PLINTH_EXACT_TAG_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
  )
  if(NOT PLINTH_EXACT_TAG_RESULT EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE PLINTH_GIT_SHA
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(PLINTH_GIT_SHA)
      set(PLINTH_GIT_VERSION "v${PLINTH_VERSION}-dev+g${PLINTH_GIT_SHA}")
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE PLINTH_GIT_DIRTY
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(PLINTH_GIT_DIRTY)
        string(APPEND PLINTH_GIT_VERSION ".dirty")
      endif()
    endif()
  endif()
endif()

project(plinth
  VERSION ${PLINTH_VERSION}
  LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Sanitizers (opt-in; mutually exclusive) ──────────
option(PLINTH_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(PLINTH_TSAN "Enable ThreadSanitizer (Clang 21 on Linux x86_64)" OFF)
if(PLINTH_SANITIZERS AND PLINTH_TSAN)
  message(FATAL_ERROR "PLINTH_SANITIZERS and PLINTH_TSAN are mutually exclusive")
endif()
if(PLINTH_SANITIZERS)
  message(STATUS "Sanitizers enabled: address,undefined")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(PLINTH_TSAN)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
     NOT CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 21 OR
     CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 22 OR
     NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
     NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR "PLINTH_TSAN requires Clang 21 on Linux x86_64")
  endif()
  message(STATUS "Sanitizer enabled: thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

# Pass full git version string to source
configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/version.hpp.in
  ${CMAKE_BINARY_DIR}/generated/plinth/version.hpp
)

# ─── Dependencies ─────────────────────────────────────────────
# Drogon
#
# ICD-0.5.3 §OID-Driven PG-Type → JS-Type Mapping requires the
# Field::oid() accessor added by
# `third_party/drogon-patches/ftype-accessor.patch`. A pre-installed
# Drogon (via `find_package`) doesn't carry this patch, so we always
# FetchContent + patch for determinism. The prior `find_package`
# fast-path was silent — on CI it succeeded and the patch never
# applied, surfacing as "Field has no member named 'oid'" only at
# compile time of the new 0.5.3 TU (see CI build 12339). Until the
# patch is upstreamed to drogonframework/drogon, FetchContent is the
# only supported path.
include(FetchContent)
# Disable Drogon's examples + tests before FetchContent_MakeAvailable:
# its examples directory declares a target named `benchmark`, which
# collides with Google Benchmark's target when PLINTH_BENCHMARKS=ON.
# We never ship Drogon's examples anyway, so this is also a small
# configure-time speedup.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
# third_party/drogon-patches/ applies surgical upstreamable diffs to
# the fetched source tree (see that dir's README). They expose Field::oid()
# and preserve whitespace in PostgreSQL connection parameters.
# The shell wrapper makes the PATCH_COMMAND idempotent:
# FetchContent's update step re-runs PATCH_COMMAND on every cmake
# reconfigure, so we `git apply --reverse --check` first to detect an
# already-applied patch and skip quietly; otherwise apply normally.
set(PLINTH_DROGON_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/ftype-accessor.patch)
set(PLINTH_DROGON_CONNINFO_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/connection-whitespace.patch)
set(PLINTH_DROGON_SHUTDOWN_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/db-client-owned-shutdown.patch)
set(PLINTH_DROGON_BINDER_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/sql-binder-cancel.patch)
set(PLINTH_DROGON_SQLSTATE_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/pg-sqlstate-errors.patch)
FetchContent_Declare(
  drogon
  GIT_REPOSITORY https://github.com/drogonframework/drogon.git
  GIT_TAG 89aca8c7993c8194f2c109c1d06a3b45bf363d5d
  PATCH_COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_PATCH}'"
  COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_CONNINFO_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_CONNINFO_PATCH}'"
  COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_SHUTDOWN_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_SHUTDOWN_PATCH}'"
  COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_BINDER_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_BINDER_PATCH}'"
  COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_SQLSTATE_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_SQLSTATE_PATCH}'"
)
FetchContent_MakeAvailable(drogon)
set(DROGON_TARGET drogon)

# nlohmann/json
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
FetchContent_MakeAvailable(nlohmann_json)

# spdlog
# v1.15.3 bundles fmt 11.1+ which resolves a consteval parse error
# clang-20 hits on v1.15.0's bundled fmt 11.0.x (`FMT_STRING` /
# `basic_format_string` path). gcc-15 compiles v1.15.0 fine, but
# clang-tidy-20 bails with `clang-diagnostic-error` — so the tidy
# target fails on every TU that includes spdlog. Pin bumped here
# alongside the clang-18 → 20 move in 0.3.0.2.
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13
)
FetchContent_MakeAvailable(spdlog)

# argparse
FetchContent_Declare(
  argparse
  GIT_REPOSITORY https://github.com/p-ranav/argparse.git
  GIT_TAG 68fd0277eea5412aff9b91c0b70efc698359dae0
)
FetchContent_MakeAvailable(argparse)

# Catch2
FetchContent_Declare(
  catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14
)
FetchContent_MakeAvailable(catch2)

# libzip — zip extraction for package install (ICD-0.4.4 UPLOADING).
# Built static, zlib-only compression; no BZ2/LZMA/ZSTD, no encryption-at-rest
# codecs (admin uploads are plaintext). BUILD_TOOLS/REGRESS/EXAMPLES/DOC off
# to avoid adding targets to `all`.
set(ENABLE_COMMONCRYPTO   OFF CACHE BOOL "" FORCE)
set(ENABLE_GNUTLS         OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS        OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL        ON  CACHE BOOL "" FORCE)
set(ENABLE_WINDOWS_CRYPTO OFF CACHE BOOL "" FORCE)
set(ENABLE_BZIP2          OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA           OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD           OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS           OFF CACHE BOOL "" FORCE)
set(BUILD_REGRESS         OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(BUILD_DOC             OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libzip
  GIT_REPOSITORY https://github.com/nih-at/libzip.git
  GIT_TAG 6f8a0cdd24a0dc6cce9dac4a7679da784ab124ea
)
FetchContent_MakeAvailable(libzip)

# ─── install-lifecycle test fixture zips (ICD-0.4.4 slice B C9) ────
# Pre-zip each tests/fixtures/install_lifecycle/<name>/ subtree to
# ${CMAKE_BINARY_DIR}/fixtures/<name>.zip at build time so the PG-gated
# install driver reads fixed-on-disk blobs instead of shelling out at
# test time. Uses the same `cmake -E tar --format=zip` path as the
# shell-blob rule so zip format is identical to production uploads.
set(PLINTH_INSTALL_FIXTURES
    valid-install
    valid-install-no-panels
    valid-install-frontend
    missing-manifest
    fail-validator
    fail-migration)
set(PLINTH_INSTALL_FIXTURE_ZIPS)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures)
foreach(FIXTURE IN LISTS PLINTH_INSTALL_FIXTURES)
  set(FIXTURE_SRC
      ${CMAKE_SOURCE_DIR}/tests/fixtures/install_lifecycle/${FIXTURE})
  set(FIXTURE_ZIP
      ${CMAKE_BINARY_DIR}/fixtures/${FIXTURE}.zip)
  file(GLOB_RECURSE FIXTURE_INPUTS CONFIGURE_DEPENDS ${FIXTURE_SRC}/*)
  add_custom_command(
    OUTPUT ${FIXTURE_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${FIXTURE_ZIP}
    COMMAND sh -c "cd '${FIXTURE_SRC}' && '${CMAKE_COMMAND}' -E tar cf '${FIXTURE_ZIP}' --format=zip -- $(ls -A)"
    DEPENDS ${FIXTURE_INPUTS}
    VERBATIM
    COMMENT "Packing install_lifecycle fixture: ${FIXTURE}"
  )
  list(APPEND PLINTH_INSTALL_FIXTURE_ZIPS ${FIXTURE_ZIP})
endforeach()
add_custom_target(plinth_install_fixture_zips
                  DEPENDS ${PLINTH_INSTALL_FIXTURE_ZIPS})

# ─── lifecycle-transitions test fixture zips (ICD-0.4.5 B10) ───
# Mirror of the install-lifecycle fixture rule above, with zips placed
# alongside for the upgrade integration tests (X.*).
set(PLINTH_LIFECYCLE_FIXTURES
    upgrade-v2
    upgrade-v2-broken-migration
    upgrade-v1-slow
    upgrade-v2-slow)
set(PLINTH_LIFECYCLE_FIXTURE_ZIPS)
foreach(FIXTURE IN LISTS PLINTH_LIFECYCLE_FIXTURES)
  set(FIXTURE_SRC
      ${CMAKE_SOURCE_DIR}/tests/fixtures/lifecycle_transitions/${FIXTURE})
  set(FIXTURE_ZIP
      ${CMAKE_BINARY_DIR}/fixtures/${FIXTURE}.zip)
  file(GLOB_RECURSE FIXTURE_INPUTS CONFIGURE_DEPENDS ${FIXTURE_SRC}/*)
  add_custom_command(
    OUTPUT ${FIXTURE_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${FIXTURE_ZIP}
    COMMAND sh -c "cd '${FIXTURE_SRC}' && '${CMAKE_COMMAND}' -E tar cf '${FIXTURE_ZIP}' --format=zip -- $(ls -A)"
    DEPENDS ${FIXTURE_INPUTS}
    VERBATIM
    COMMENT "Packing lifecycle_transitions fixture: ${FIXTURE}"
  )
  list(APPEND PLINTH_LIFECYCLE_FIXTURE_ZIPS ${FIXTURE_ZIP})
endforeach()
add_custom_target(plinth_lifecycle_fixture_zips
                  DEPENDS ${PLINTH_LIFECYCLE_FIXTURE_ZIPS})

# ─── Test-only extension fixtures (ICD-0.6.3 §D — sdk-demo) ──────
# Test-only fixtures live under tests/extensions/<name>/ and are NOT
# bundled into shell.zip. Architects exercise them via real
# `POST /api/packages` during manual FE smoke. Zipped at build time
# under fixtures/extensions/<name>.zip so the smoke workflow has a
# stable byte source.
set(PLINTH_TEST_EXTENSIONS sdk-demo)
set(PLINTH_TEST_EXTENSION_ZIPS)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures/extensions)
foreach(EXT IN LISTS PLINTH_TEST_EXTENSIONS)
  set(EXT_SRC ${CMAKE_SOURCE_DIR}/tests/extensions/${EXT})
  set(EXT_ZIP ${CMAKE_BINARY_DIR}/fixtures/extensions/${EXT}.zip)
  file(GLOB_RECURSE EXT_INPUTS CONFIGURE_DEPENDS ${EXT_SRC}/*)
  add_custom_command(
    OUTPUT ${EXT_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${EXT_ZIP}
    COMMAND sh -c "cd '${EXT_SRC}' && '${CMAKE_COMMAND}' -E tar cf '${EXT_ZIP}' --format=zip -- $(ls -A)"
    DEPENDS ${EXT_INPUTS}
    VERBATIM
    COMMENT "Packing test-extension fixture: ${EXT}"
  )
  list(APPEND PLINTH_TEST_EXTENSION_ZIPS ${EXT_ZIP})
endforeach()
add_custom_target(plinth_test_extension_zips
                  DEPENDS ${PLINTH_TEST_EXTENSION_ZIPS})

# ─── RBAC test-runner fixture zips (ICD-0.4.7 slice B) ───────────
# Same pattern as install-lifecycle fixtures above. Zips land in
# fixtures/rbac_test_runner/<name>.zip so tests can disambiguate via
# read_fixture_zip("rbac_test_runner/<name>"). Each fixture is a full
# package tree consumed by `install_package` to drive the RBAC test end-to-end.
set(PLINTH_RBAC_TEST_RUNNER_FIXTURES
    happy-all-pass
    broken-assert-deny
    broken-assert-allow
    mixed-pass-fail
    no-test-contracts
    assert-allow-side-effect)
set(PLINTH_RBAC_TEST_RUNNER_FIXTURE_ZIPS)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/fixtures/rbac_test_runner)
foreach(FIXTURE IN LISTS PLINTH_RBAC_TEST_RUNNER_FIXTURES)
  set(FIXTURE_SRC
      ${CMAKE_SOURCE_DIR}/tests/fixtures/rbac_test_runner/${FIXTURE})
  set(FIXTURE_ZIP
      ${CMAKE_BINARY_DIR}/fixtures/rbac_test_runner/${FIXTURE}.zip)
  file(GLOB_RECURSE FIXTURE_INPUTS CONFIGURE_DEPENDS ${FIXTURE_SRC}/*)
  add_custom_command(
    OUTPUT ${FIXTURE_ZIP}
    COMMAND ${CMAKE_COMMAND} -E rm -f ${FIXTURE_ZIP}
    COMMAND sh -c "cd '${FIXTURE_SRC}' && '${CMAKE_COMMAND}' -E tar cf '${FIXTURE_ZIP}' --format=zip -- $(ls -A)"
    DEPENDS ${FIXTURE_INPUTS}
    VERBATIM
    COMMENT "Packing rbac_test_runner fixture: ${FIXTURE}"
  )
  list(APPEND PLINTH_RBAC_TEST_RUNNER_FIXTURE_ZIPS ${FIXTURE_ZIP})
endforeach()
add_custom_target(plinth_rbac_test_runner_fixture_zips
                  DEPENDS ${PLINTH_RBAC_TEST_RUNNER_FIXTURE_ZIPS})

# ─── Bundled shell artifact (ICD-0.6.1 §3.2) ─────────────────
# Packs client/shell/ into shell.zip on disk. The kernel reads bytes from
# `<bundle_path>/shell.zip` at first-boot per ICD-0.6.1 §3.2 — the linker-
# embedded blob path (ICD-0.4.4 slice B) was removed in 0.6.1 phase 3.

set(PLINTH_SHELL_SRC_DIR ${CMAKE_SOURCE_DIR}/client/shell)
set(PLINTH_SHELL_ZIP ${CMAKE_BINARY_DIR}/shell.zip)
set(PLINTH_SHELL_PACKER ${CMAKE_SOURCE_DIR}/tools/pack_shell.py)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB_RECURSE PLINTH_SHELL_INPUTS CONFIGURE_DEPENDS
     ${PLINTH_SHELL_SRC_DIR}/*)

# Recognize only this kernel's exact bundled platform client graph. Expected
# bytes are compiled into the kernel, never supplied by an installed frontend's
# version string or profile declaration. Keep ordinary custom/legacy frontends
# outside this optional typed-platform profile.
file(GLOB_RECURSE PLINTH_PLATFORM_CLIENT_INPUTS LIST_DIRECTORIES false
     CONFIGURE_DEPENDS ${PLINTH_SHELL_SRC_DIR}/client/*)
list(SORT PLINTH_PLATFORM_CLIENT_INPUTS)
list(LENGTH PLINTH_PLATFORM_CLIENT_INPUTS PLINTH_PLATFORM_CLIENT_COUNT)
if(PLINTH_PLATFORM_CLIENT_COUNT GREATER 4096)
  message(FATAL_ERROR "Bundled platform graph exceeds capture inventory profile")
endif()
set(PLINTH_PLATFORM_CLIENT_DIGESTS "")
foreach(PLATFORM_FILE IN LISTS PLINTH_PLATFORM_CLIENT_INPUTS)
  file(RELATIVE_PATH PLATFORM_PATH ${PLINTH_SHELL_SRC_DIR} ${PLATFORM_FILE})
  if(PLATFORM_PATH MATCHES "[\"\\\\;\n\r]")
    message(FATAL_ERROR "Bundled platform graph contains an unsupported file name")
  endif()
  file(SHA256 ${PLATFORM_FILE} PLATFORM_SHA)
  string(APPEND PLINTH_PLATFORM_CLIENT_DIGESTS
         "  {\"${PLATFORM_PATH}\", \"${PLATFORM_SHA}\"},\n")
endforeach()
set(PLINTH_PLATFORM_MODULE_FILE
    ${PLINTH_SHELL_SRC_DIR}/server/platform-storage-v1.js)
set(PLINTH_PLATFORM_CONTRACT_FILE
    ${PLINTH_SHELL_SRC_DIR}/server/plinth-platform.json)
set(PLINTH_PLATFORM_MIGRATION_FILE
    ${PLINTH_SHELL_SRC_DIR}/migrations/002_platform_storage_v1.sql)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${PLINTH_PLATFORM_CLIENT_INPUTS} ${PLINTH_PLATFORM_MODULE_FILE}
    ${PLINTH_PLATFORM_CONTRACT_FILE} ${PLINTH_PLATFORM_MIGRATION_FILE})
file(SHA256 ${PLINTH_PLATFORM_MODULE_FILE} PLINTH_PLATFORM_MODULE_SHA)
file(SHA256 ${PLINTH_PLATFORM_CONTRACT_FILE} PLINTH_PLATFORM_CONTRACT_SHA)
file(SHA256 ${PLINTH_PLATFORM_MIGRATION_FILE} PLINTH_PLATFORM_MIGRATION_SHA)
configure_file(cmake/platform_profile.hpp.in generated/plinth/platform_profile.hpp @ONLY)

add_custom_command(
  OUTPUT ${PLINTH_SHELL_ZIP}
  COMMAND ${Python3_EXECUTABLE} ${PLINTH_SHELL_PACKER}
          --source ${PLINTH_SHELL_SRC_DIR}
          --output ${PLINTH_SHELL_ZIP}
  DEPENDS ${PLINTH_SHELL_INPUTS} ${PLINTH_SHELL_PACKER}
  VERBATIM
  COMMENT "Packing deterministic client/shell/ -> shell.zip"
)
add_custom_target(plinth_shell_zip DEPENDS ${PLINTH_SHELL_ZIP})

# The administration package is an ordinary, separately API-installed ZIP.
# It is an explicit local/test target, never part of Runtime installation or
# the kernel's first-boot bundled-package directory.
set(PLINTH_ADMIN_SRC_DIR ${CMAKE_SOURCE_DIR}/client/admin)
set(PLINTH_ADMIN_PACKER ${CMAKE_SOURCE_DIR}/tools/pack_admin.py)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${PLINTH_ADMIN_SRC_DIR}/manifest.json)
file(READ ${PLINTH_ADMIN_SRC_DIR}/manifest.json PLINTH_ADMIN_MANIFEST_JSON)
string(JSON PLINTH_ADMIN_NAME GET ${PLINTH_ADMIN_MANIFEST_JSON} name)
string(JSON PLINTH_ADMIN_VERSION GET ${PLINTH_ADMIN_MANIFEST_JSON} version)
if(NOT PLINTH_ADMIN_NAME STREQUAL "admin" OR
   NOT PLINTH_ADMIN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([+-][A-Za-z0-9][A-Za-z0-9.+-]*)?$")
  message(FATAL_ERROR "Admin package identity or version is not a safe local artifact name")
endif()
set(PLINTH_ADMIN_ZIP
    ${CMAKE_BINARY_DIR}/packages/admin-${PLINTH_ADMIN_VERSION}.zip)
file(GLOB_RECURSE PLINTH_ADMIN_INPUTS CONFIGURE_DEPENDS
     ${PLINTH_ADMIN_SRC_DIR}/*)
add_custom_command(
  OUTPUT ${PLINTH_ADMIN_ZIP}
  COMMAND ${Python3_EXECUTABLE} ${PLINTH_ADMIN_PACKER}
          --source ${PLINTH_ADMIN_SRC_DIR}
          --output ${PLINTH_ADMIN_ZIP}
  DEPENDS ${PLINTH_ADMIN_INPUTS} ${PLINTH_ADMIN_PACKER}
          ${CMAKE_SOURCE_DIR}/tools/pack_shell.py
  VERBATIM
  COMMENT "Packing separately API-installed admin package"
)
add_custom_target(plinth_admin_zip DEPENDS ${PLINTH_ADMIN_ZIP})

# Stage shell.zip in the build-tree datadir layout so dev runs and tests
# resolve `share/plinth/bundled/shell.zip` relative to the binary without
# an explicit `shell.bundle_path` config override (ICD-0.6.1 §9.2 default).
# Production install ships the same file under ${CMAKE_INSTALL_DATADIR}.
set(PLINTH_SHELL_BUNDLE_STAGE_DIR ${CMAKE_BINARY_DIR}/share/plinth/bundled)
set(PLINTH_SHELL_BUNDLE_STAGED ${PLINTH_SHELL_BUNDLE_STAGE_DIR}/shell.zip)
add_custom_command(
  OUTPUT ${PLINTH_SHELL_BUNDLE_STAGED}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PLINTH_SHELL_BUNDLE_STAGE_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${PLINTH_SHELL_ZIP}
                                   ${PLINTH_SHELL_BUNDLE_STAGED}
  DEPENDS ${PLINTH_SHELL_ZIP}
  VERBATIM
  COMMENT "Staging shell.zip -> share/plinth/bundled/"
)
add_custom_target(plinth_shell_bundle_staged
                  DEPENDS ${PLINTH_SHELL_BUNDLE_STAGED})

install(FILES ${PLINTH_SHELL_ZIP}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/plinth/bundled
        COMPONENT Runtime)

install(FILES
          ${CMAKE_SOURCE_DIR}/migrations/schema.sql
          ${CMAKE_SOURCE_DIR}/migrations/extension_database.sql
        DESTINATION ${CMAKE_INSTALL_DATADIR}/plinth/migrations
        COMPONENT Runtime)

install(FILES
          ${CMAKE_SOURCE_DIR}/LICENSE
          ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
          ${CMAKE_SOURCE_DIR}/sbom.cdx.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/plinth
        COMPONENT Runtime)

# QuickJS — vendored via FetchContent (quickjs-ng fork; bellard/quickjs has
# no version tags as of 2026-04). Upstream CMakeLists declares extra
# executables (qjsc, qjs_exe, api-test, lre-test, ...) unconditionally;
# EXCLUDE_FROM_ALL keeps them out of `all` so only the `qjs` static library
# builds (as a transitive dep of plinth / plinth_tests).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "QuickJS vendoring (ICD-0.3.0) is Linux-only for now.")
endif()
# The pinned QuickJS-ng revision leaks a temporary string reference when
# JS_ToCStringLen2 cannot allocate its UTF-8 conversion. Apply the narrow fix
# on fresh population and skip it on reconfigure when already present.
set(PLINTH_QUICKJS_CSTRING_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/quickjs-patches/cstring-conversion-failure-leak.patch)
FetchContent_Declare(
  quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG 3c051980ab7e783dfbfb1c70c014ce5e05ecf24c
  PATCH_COMMAND sh -c
    "git apply --reverse --check '${PLINTH_QUICKJS_CSTRING_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_QUICKJS_CSTRING_PATCH}'"
)
FetchContent_GetProperties(quickjs)
if(NOT quickjs_POPULATED)
  FetchContent_Populate(quickjs)
  add_subdirectory(${quickjs_SOURCE_DIR} ${quickjs_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
# Plinth-owned wrapper: all Plinth TUs link `plinth_quickjs` rather than
# `qjs` directly so we can tighten flags later without touching call sites.
add_library(plinth_quickjs INTERFACE)
target_link_libraries(plinth_quickjs INTERFACE qjs)
# Silence vendored-header warnings (e.g. -Wunused-parameter, -Wshadow) that
# QuickJS triggers under Plinth's -Wall -Wextra -Werror. This only applies
# to translation units of the vendored `qjs` target itself.
if(TARGET qjs)
  target_compile_options(qjs PRIVATE -w)
endif()

# ─── Find libpq (required for bootstrap connection) ──────────
find_package(PostgreSQL REQUIRED)

# ─── Find libargon2 (password hashing) ───────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(Argon2 REQUIRED libargon2)

# ─── Find OpenSSL (SHA-256, CSPRNG) ─────────────────────────
find_package(OpenSSL REQUIRED)

# ─── Kernel ───────────────────────────────────────────────────
add_executable(plinth
  src/kernel/main.cpp
  src/kernel/browser_origin.cpp
  src/kernel/lifecycle/async_task_registry.cpp
  src/kernel/lifecycle/shutdown_coordinator.cpp
  src/kernel/config.cpp
  src/kernel/logging.cpp
  src/kernel/db/bootstrap.cpp
  src/kernel/audit/handlers.cpp
  src/kernel/auth/crypto.cpp
  src/kernel/auth/csrf.cpp
  src/kernel/auth/rate_limiter.cpp
  src/kernel/auth/middleware.cpp
  src/kernel/auth/handlers.cpp
  src/kernel/auth/pat_handlers.cpp
  src/kernel/capabilities/validation.cpp
  src/kernel/capabilities/parser.cpp
  src/kernel/capabilities/registration.cpp
  src/kernel/capabilities/bootstrap.cpp
  src/kernel/capabilities/resolution.cpp
  src/kernel/capabilities/listener.cpp
  src/kernel/capabilities/batch.cpp
  src/kernel/capabilities/drain.cpp
  src/kernel/realtime/channel.cpp
  src/kernel/realtime/listener.cpp
  src/kernel/realtime/emit.cpp
  src/kernel/realtime/sql_classify.cpp
  src/kernel/realtime/coalescer.cpp
  src/kernel/realtime/broker.cpp
  src/kernel/realtime/events_writer.cpp
  src/kernel/realtime/cursor_store.cpp
  src/kernel/realtime/replay.cpp
  src/kernel/scheduled_tasks/cleanup_events.cpp
  src/kernel/extensions/runtime_registry.cpp
  src/kernel/groups/handlers.cpp
  src/kernel/rbac/enforcement.cpp
  src/kernel/rbac/rule_registrar.cpp
  src/kernel/rbac/rbac_manifest.cpp
  src/kernel/rbac/rule_validator.cpp
  src/kernel/rbac/ephemeral_user.cpp
  src/kernel/rbac/subscribe_rule.cpp
  src/kernel/ws/connection_registry.cpp
  src/kernel/ws/authority.cpp
  src/kernel/ws/auth_flow.cpp
  src/kernel/ws/heartbeat.cpp
  src/kernel/ws/subscriptions.cpp
  src/kernel/ws/publish.cpp
  src/kernel/ws/events_controller.cpp
  src/kernel/ws/call_dispatch.cpp
  src/kernel/ws/js_stress.cpp
  src/kernel/ws/registration.cpp
  src/kernel/js/eval.cpp
  src/kernel/js/eval_guard.cpp
  src/kernel/js/bridge_context.cpp
  src/kernel/js/db_batch_audit.cpp
  src/kernel/js/db_batch_schema_check.cpp
  src/kernel/js/db_search_path.cpp
  src/kernel/js/extension_database.cpp
  src/kernel/js/db_silent_audit.cpp
  src/kernel/js/runtime_pool.cpp
  src/kernel/js/stdlib_inject.cpp
  src/kernel/js/conversion.cpp
  src/kernel/js/run_on_context.cpp
  src/kernel/js/stdlib/log_bindings.cpp
  src/kernel/js/stdlib/config_bindings.cpp
  src/kernel/js/stdlib/crypto_bindings.cpp
  src/kernel/js/stdlib/db_bindings.cpp
  src/kernel/js/stdlib/db_error_map.cpp
  src/kernel/js/stdlib/db_result_to_json.cpp
  src/kernel/js/stdlib/audit_bindings.cpp
  src/kernel/js/stdlib/cap_bindings.cpp
  src/kernel/js/stdlib/pubsub_bindings.cpp
  src/kernel/packages/manifest.cpp
  src/kernel/packages/capabilities_manifest.cpp
  src/kernel/packages/panels_manifest.cpp
  src/kernel/packages/restore_policy.cpp
  src/kernel/packages/protected_restore_policy.cpp
  src/kernel/packages/generation_capture.cpp
  src/kernel/packages/generation_cache.cpp
  src/kernel/packages/generation_receipts.cpp
  src/kernel/packages/generation_binding.cpp
  src/kernel/packages/installed_generation_registry.cpp
  src/kernel/packages/name_operation.cpp
  src/kernel/packages/restore_digest.cpp
  src/kernel/packages/detail/restore_json.cpp
  src/kernel/packages/config_manifest.cpp
  src/kernel/packages/cross_file_validator.cpp
  src/kernel/packages/validator.cpp
  src/kernel/packages/migrations.cpp
  src/kernel/packages/panels.cpp
  src/kernel/packages/asset_server.cpp
  src/kernel/packages/install_lifecycle.cpp
  src/kernel/packages/rbac_test_runner.cpp
  src/kernel/packages/handlers.cpp
  src/kernel/security/unicode_scanner.cpp
  src/kernel/security/response_headers.cpp
  src/kernel/shell/active_frontend.cpp
  src/kernel/shell/platform_graph.cpp
  src/kernel/shell/firstboot.cpp
  src/kernel/frontend/api_frontend.cpp
  src/kernel/frontend/applications.cpp
  src/kernel/frontend/admin_defaults_contract.cpp
  src/kernel/frontend/typed_requests.cpp
  src/kernel/frontend/persistence_projection.cpp
  src/kernel/frontend/typed_assets.cpp
  src/kernel/frontend/owned_frontend_worker.cpp
  src/kernel/frontend/surface_authority.cpp
  src/kernel/frontend/float_layout_service.cpp
  src/kernel/frontend/surface_protocol.cpp
  src/kernel/frontend/float_layout_contract.cpp
  src/kernel/frontend/platform_startup.cpp
  src/kernel/frontend/float_routes.cpp
  src/kernel/cap/api_cap.cpp
)

target_include_directories(plinth PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated
  ${Argon2_INCLUDE_DIRS}
)

target_link_libraries(plinth PRIVATE
  ${DROGON_TARGET}
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  argparse
  PostgreSQL::PostgreSQL
  ${Argon2_LIBRARIES}
  OpenSSL::Crypto
  plinth_quickjs
  libzip::zip
  # jemalloc
)

add_dependencies(plinth plinth_shell_bundle_staged)

install(TARGETS plinth
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime)

# ─── Tests ────────────────────────────────────────────────────
enable_testing()

add_test(NAME plinth_shell_package_determinism
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/tools/pack_shell_test.py
          --packer ${PLINTH_SHELL_PACKER}
          --source ${PLINTH_SHELL_SRC_DIR}
)
set_tests_properties(plinth_shell_package_determinism PROPERTIES TIMEOUT 30)

add_test(NAME plinth_admin_package_determinism
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/tools/pack_admin_test.py
)
set_tests_properties(plinth_admin_package_determinism PROPERTIES TIMEOUT 30)

add_test(NAME plinth_admin_browser_supervisor_units
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tests/browser/run_admin_package_test.py
)
set_tests_properties(plinth_admin_browser_supervisor_units PROPERTIES TIMEOUT 30)

add_executable(plinth_tests
  tests/kernel/main_test.cpp
  tests/kernel/lifecycle/async_task_registry_test.cpp
  tests/kernel/lifecycle/async_cleanup_registry_test.cpp
  tests/kernel/lifecycle/shutdown_coordinator_test.cpp
  tests/kernel/lifecycle/process_shutdown_test.cpp
  tests/kernel/config_test.cpp
  tests/kernel/browser_origin_test.cpp
  tests/kernel/db/bootstrap_test.cpp
  tests/kernel/db/connection_info_test.cpp
  tests/kernel/audit/audit_test.cpp
  tests/kernel/audit/audit_teardown_test.cpp
  tests/kernel/audit/audit_drain_test.cpp
  tests/kernel/audit/audit_write_registry_test.cpp
  tests/kernel/audit/detail_encoding_test.cpp
  tests/kernel/auth/crypto_test.cpp
  tests/kernel/auth/csrf_test.cpp
  tests/kernel/auth/csrf_route_inventory_test.cpp
  tests/kernel/auth/rate_limiter_test.cpp
  tests/kernel/auth/middleware_test.cpp
  tests/kernel/auth/auth_integration_test.cpp
  tests/kernel/auth/pat_integration_test.cpp
  tests/kernel/capabilities/validation_test.cpp
  tests/kernel/capabilities/parser_test.cpp
  tests/kernel/capabilities/resolution_test.cpp
  tests/kernel/capabilities/dispatch_extension_test.cpp
  tests/kernel/capabilities/batch_test.cpp
  tests/kernel/capabilities/drain_test.cpp
  tests/kernel/capabilities/registration_integration_test.cpp
  tests/kernel/capabilities/bootstrap_integration_test.cpp
  tests/kernel/capabilities/listener_integration_test.cpp
  tests/kernel/realtime/shared_pg_client.cpp
  tests/kernel/realtime/listener_test.cpp
  tests/kernel/realtime/emit_test.cpp
  tests/kernel/realtime/sql_classify_test.cpp
  tests/kernel/realtime/coalescer_test.cpp
  tests/kernel/realtime/coalescer_integration_test.cpp
  tests/kernel/realtime/broker_test.cpp
  tests/kernel/realtime/broker_integration_test.cpp
  tests/kernel/realtime/cursor_store_test.cpp
  tests/kernel/realtime/events_writer_test.cpp
  tests/kernel/realtime/replay_test.cpp
  tests/kernel/realtime/seq_generation_test.cpp
  tests/kernel/realtime/live_replay_ordering_test.cpp
  tests/kernel/realtime/envelope_shape_test.cpp
  tests/kernel/realtime/debounce_advisory_test.cpp
  tests/kernel/realtime/events_writer_advisory_test.cpp
  tests/kernel/realtime/gap_detection_test.cpp
  tests/kernel/scheduled_tasks/cleanup_events_test.cpp
  tests/integration/events_replay_integration_test.cpp
  tests/kernel/groups/groups_integration_test.cpp
  tests/kernel/groups/rbac_integration_test.cpp
  tests/kernel/rbac/enforcement_test.cpp
  tests/kernel/rbac/property_graph_test.cpp
  tests/kernel/rbac/property_http_test.cpp
  tests/kernel/rbac/property_ownership_test.cpp
  tests/kernel/rbac/anonymous_identity_test.cpp
  tests/kernel/rbac/rule_registrar_test.cpp
  tests/kernel/rbac/rbac_manifest_test.cpp
  tests/kernel/rbac/rule_validator_test.cpp
  tests/kernel/rbac/ephemeral_user_test.cpp
  tests/kernel/rbac/subscribe_rule_test.cpp
  tests/kernel/ws/ws_test_fixture.cpp
  tests/kernel/ws/listener_readiness_test.cpp
  tests/kernel/ws/registry_test.cpp
  tests/kernel/ws/auth_test.cpp
  tests/kernel/ws/heartbeat_test.cpp
  tests/kernel/ws/subscribe_test.cpp
  tests/kernel/ws/subscriptions_rbac_test.cpp
  tests/kernel/ws/delta_sync_test.cpp
  tests/kernel/ws/publish_test.cpp
  tests/kernel/ws/call_test.cpp
  tests/kernel/js/eval_test.cpp
  tests/kernel/js/runtime_pool_test.cpp
  tests/kernel/js/stdlib_test.cpp
  tests/kernel/js/quickjs_cstring_oom_test.cpp
  tests/kernel/js/crypto_corpus_test.cpp
  tests/kernel/js/crypto_numeric_regression_test.cpp
  tests/kernel/js/json_nul_preservation_test.cpp
  tests/kernel/js/conversion_corpus_test.cpp
  tests/kernel/js/sync_conversion_corpus_test.cpp
  tests/kernel/js/host_string_boundary_test.cpp
  tests/kernel/js/audit_authority_regression_test.cpp
  tests/kernel/js/audit_corpus_test.cpp
  tests/kernel/js/native_audit_corpus_test.cpp
  tests/kernel/js/pubsub_corpus_test.cpp
  tests/kernel/js/native_pubsub_publish_corpus_test.cpp
  tests/kernel/js/native_pubsub_delivery_corpus_test.cpp
  tests/kernel/js/pubsub_dispatch_owner_test.cpp
  tests/kernel/js/cap_corpus_test.cpp
  tests/kernel/js/db_admission_corpus_test.cpp
  tests/kernel/js/db_batch_corpus_test.cpp
  tests/kernel/js/log_corpus_test.cpp
  tests/kernel/js/config_corpus_test.cpp
  tests/kernel/js/async_bridge_test.cpp
  tests/kernel/js/async_bridge_fixture.cpp
  tests/kernel/js/db_batch_test.cpp
  tests/kernel/js/db_sqlstate_native_test.cpp
  tests/kernel/js/db_sqlstate_projection_test.cpp
  tests/kernel/js/native_pg_result_corpus_test.cpp
  tests/kernel/js/native_capability_corpus_test.cpp
  tests/kernel/js/cap_cancellation_test.cpp
  tests/kernel/js/cap_cancellation_failure_test.cpp
  tests/kernel/js/native_capability_data_test.cpp
  tests/kernel/js/native_capability_edges_test.cpp
  tests/kernel/js/native_capability_ownership_test.cpp
  tests/kernel/js/db_search_path_test.cpp
  tests/kernel/js/db_silent_test.cpp
  tests/kernel/js/pubsub_test.cpp
  tests/kernel/js/pubsub_subscribe_test.cpp
  tests/kernel/js/limits_test.cpp
  tests/kernel/js/async_hardening_test.cpp
  tests/kernel/js/owned_database_completion_test.cpp
  tests/kernel/js/native_setup_failure_test.cpp
  tests/kernel/js/eval_guard_test.cpp
  tests/kernel/packages/manifest_test.cpp
  tests/kernel/packages/capabilities_manifest_test.cpp
  tests/kernel/packages/panels_manifest_test.cpp
  tests/kernel/packages/restore_policy_test.cpp
  tests/kernel/packages/protected_restore_policy_test.cpp
  tests/kernel/packages/generation_capture_test.cpp
  tests/kernel/packages/generation_cache_test.cpp
  tests/kernel/packages/generation_receipts_test.cpp
  tests/kernel/packages/generation_binding_test.cpp
  tests/kernel/packages/installed_generation_registry_test.cpp
  tests/kernel/packages/generation_lifecycle_test.cpp
  tests/kernel/packages/generation_schema_test.cpp
  tests/kernel/packages/restore_digest_test.cpp
  tests/kernel/packages/config_manifest_test.cpp
  tests/kernel/packages/cross_file_validator_test.cpp
  tests/kernel/packages/validator_test.cpp
  tests/kernel/packages/migrations_parsing_test.cpp
  tests/kernel/packages/migrations_test.cpp
  tests/kernel/packages/install_lifecycle_unit_test.cpp
  tests/kernel/packages/packages_schema_test.cpp
  tests/kernel/packages/asset_server_test.cpp
  tests/kernel/packages/install_lifecycle_test.cpp
  tests/kernel/packages/http_test_fixture.cpp
  tests/kernel/packages/advisory_lock_harness.cpp
  tests/kernel/packages/install_lifecycle_http_test.cpp
  tests/kernel/packages/lifecycle_transitions_http_test.cpp
  tests/kernel/packages/lifecycle_gc_advisory_test.cpp
  tests/kernel/packages/lifecycle_transitions_test.cpp
  tests/kernel/packages/lifecycle_transitions_unit_test.cpp
  tests/kernel/packages/crash_recovery_test.cpp
  tests/kernel/packages/atomic_swap_crash_test.cpp
  tests/kernel/packages/rbac_test_report_test.cpp
  tests/kernel/packages/rbac_test_runner_test.cpp
  tests/kernel/main/cli_test_rbac_test.cpp
  tests/kernel/security/unicode_scanner_test.cpp
  tests/kernel/security/response_headers_test.cpp
  tests/kernel/shell/active_frontend_test.cpp
  tests/kernel/shell/firstboot_test.cpp
  tests/kernel/shell/preferences_test.cpp
  tests/kernel/shell/preferences_dispatch_test.cpp
  tests/kernel/frontend/api_frontend_test.cpp
  tests/kernel/frontend/applications_test.cpp
  tests/kernel/frontend/admin_defaults_contract_test.cpp
  tests/kernel/frontend/typed_requests_test.cpp
  tests/kernel/frontend/typed_assets_test.cpp
  tests/kernel/frontend/owned_frontend_worker_test.cpp
  tests/kernel/frontend/surface_authority_test.cpp
  tests/kernel/frontend/surface_protocol_test.cpp
  tests/kernel/frontend/float_layout_contract_test.cpp
  tests/kernel/frontend/platform_startup_test.cpp
  tests/kernel/frontend/float_layout_service_test.cpp
  tests/kernel/frontend/float_routes_test.cpp
  tests/kernel/frontend/persistence_projection_test.cpp
  tests/kernel/cap/api_cap_test.cpp
  src/kernel/config.cpp
  src/kernel/browser_origin.cpp
  src/kernel/lifecycle/async_task_registry.cpp
  src/kernel/lifecycle/shutdown_coordinator.cpp
  src/kernel/logging.cpp
  src/kernel/db/bootstrap.cpp
  src/kernel/audit/handlers.cpp
  src/kernel/auth/crypto.cpp
  src/kernel/auth/csrf.cpp
  src/kernel/auth/rate_limiter.cpp
  src/kernel/auth/middleware.cpp
  src/kernel/auth/handlers.cpp
  src/kernel/auth/pat_handlers.cpp
  src/kernel/capabilities/validation.cpp
  src/kernel/capabilities/parser.cpp
  src/kernel/capabilities/registration.cpp
  src/kernel/capabilities/bootstrap.cpp
  src/kernel/capabilities/resolution.cpp
  src/kernel/capabilities/listener.cpp
  src/kernel/capabilities/batch.cpp
  src/kernel/capabilities/drain.cpp
  src/kernel/realtime/channel.cpp
  src/kernel/realtime/listener.cpp
  src/kernel/realtime/emit.cpp
  src/kernel/realtime/sql_classify.cpp
  src/kernel/realtime/coalescer.cpp
  src/kernel/realtime/broker.cpp
  src/kernel/realtime/events_writer.cpp
  src/kernel/realtime/cursor_store.cpp
  src/kernel/realtime/replay.cpp
  src/kernel/scheduled_tasks/cleanup_events.cpp
  src/kernel/extensions/runtime_registry.cpp
  src/kernel/groups/handlers.cpp
  src/kernel/rbac/enforcement.cpp
  src/kernel/rbac/rule_registrar.cpp
  src/kernel/rbac/rbac_manifest.cpp
  src/kernel/rbac/rule_validator.cpp
  src/kernel/rbac/ephemeral_user.cpp
  src/kernel/rbac/subscribe_rule.cpp
  src/kernel/ws/connection_registry.cpp
  src/kernel/ws/authority.cpp
  src/kernel/ws/auth_flow.cpp
  src/kernel/ws/heartbeat.cpp
  src/kernel/ws/subscriptions.cpp
  src/kernel/ws/publish.cpp
  src/kernel/ws/events_controller.cpp
  src/kernel/ws/call_dispatch.cpp
  src/kernel/ws/js_stress.cpp
  src/kernel/ws/registration.cpp
  src/kernel/js/eval.cpp
  src/kernel/js/eval_guard.cpp
  src/kernel/js/bridge_context.cpp
  src/kernel/js/db_batch_audit.cpp
  src/kernel/js/db_batch_schema_check.cpp
  src/kernel/js/db_search_path.cpp
  src/kernel/js/extension_database.cpp
  src/kernel/js/db_silent_audit.cpp
  src/kernel/js/runtime_pool.cpp
  src/kernel/js/stdlib_inject.cpp
  src/kernel/js/conversion.cpp
  src/kernel/js/run_on_context.cpp
  src/kernel/js/stdlib/log_bindings.cpp
  src/kernel/js/stdlib/config_bindings.cpp
  src/kernel/js/stdlib/crypto_bindings.cpp
  src/kernel/js/stdlib/db_bindings.cpp
  src/kernel/js/stdlib/db_error_map.cpp
  src/kernel/js/stdlib/db_result_to_json.cpp
  src/kernel/js/stdlib/audit_bindings.cpp
  src/kernel/js/stdlib/cap_bindings.cpp
  src/kernel/js/stdlib/pubsub_bindings.cpp
  src/kernel/packages/manifest.cpp
  src/kernel/packages/capabilities_manifest.cpp
  src/kernel/packages/panels_manifest.cpp
  src/kernel/packages/restore_policy.cpp
  src/kernel/packages/protected_restore_policy.cpp
  src/kernel/packages/generation_capture.cpp
  src/kernel/packages/generation_cache.cpp
  src/kernel/packages/generation_receipts.cpp
  src/kernel/packages/generation_binding.cpp
  src/kernel/packages/installed_generation_registry.cpp
  src/kernel/packages/name_operation.cpp
  src/kernel/packages/restore_digest.cpp
  src/kernel/packages/detail/restore_json.cpp
  src/kernel/packages/config_manifest.cpp
  src/kernel/packages/cross_file_validator.cpp
  src/kernel/packages/validator.cpp
  src/kernel/packages/migrations.cpp
  src/kernel/packages/panels.cpp
  src/kernel/packages/asset_server.cpp
  src/kernel/packages/install_lifecycle.cpp
  src/kernel/packages/rbac_test_runner.cpp
  src/kernel/packages/handlers.cpp
  src/kernel/security/unicode_scanner.cpp
  src/kernel/security/response_headers.cpp
  src/kernel/shell/active_frontend.cpp
  src/kernel/shell/platform_graph.cpp
  src/kernel/shell/firstboot.cpp
  src/kernel/frontend/api_frontend.cpp
  src/kernel/frontend/applications.cpp
  src/kernel/frontend/admin_defaults_contract.cpp
  src/kernel/frontend/typed_requests.cpp
  src/kernel/frontend/persistence_projection.cpp
  src/kernel/frontend/typed_assets.cpp
  src/kernel/frontend/owned_frontend_worker.cpp
  src/kernel/frontend/surface_authority.cpp
  src/kernel/frontend/float_layout_service.cpp
  src/kernel/frontend/surface_protocol.cpp
  src/kernel/frontend/float_layout_contract.cpp
  src/kernel/frontend/platform_startup.cpp
  src/kernel/frontend/float_routes.cpp
  src/kernel/cap/api_cap.cpp
)

target_include_directories(plinth_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated
  ${Argon2_INCLUDE_DIRS}
)

# Only this pinned-framework serializer test uses Drogon's private definition;
# it is not part of the production include interface or a fabricated admission.
set_source_files_properties(tests/kernel/frontend/typed_assets_test.cpp
  PROPERTIES INCLUDE_DIRECTORIES "${drogon_SOURCE_DIR}/lib/src")

# Observe the real compiled PostgreSQL implementation and its owned pipeline
# boundaries. These private headers are not a production include interface.
set_source_files_properties(tests/kernel/js/db_sqlstate_native_test.cpp
  PROPERTIES INCLUDE_DIRECTORIES "${drogon_SOURCE_DIR}/orm_lib/src")

target_compile_definitions(plinth_tests PRIVATE
  CMAKE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  CMAKE_BINARY_DIR="${CMAKE_BINARY_DIR}"
  PLINTH_BINARY_PATH="$<TARGET_FILE:plinth>"
)

# ICD-0.4.2 §Fixture Packages — runtime-state (RT1..RT3) fixtures
# require a live PG + kernel. Opt-in at CMake configure time; when
# unset the RT test cases Skip with the standard message.
option(PLINTH_KERNEL_TESTS "Run runtime-state package validation tests against a live kernel" OFF)
if(PLINTH_KERNEL_TESTS)
  target_compile_definitions(plinth_tests PRIVATE PLINTH_KERNEL_TESTS_ENABLED)
endif()

# validator_test's CLI TEST_CASE spawns the plinth binary via popen();
# without this dependency the test binary could be built and run before
# the plinth binary exists, producing a confusing failure mode.
add_dependencies(plinth_tests plinth)

# -rdynamic so backtrace_symbols() in main_test.cpp's fatal-signal
# handler can resolve symbols from the test binary itself. Without
# this, the crash backtrace is just raw addresses — useless for
# diagnosing the parked WS flake (project_ws_flaky_segfault.md).
set_target_properties(plinth_tests PROPERTIES ENABLE_EXPORTS ON)

target_link_libraries(plinth_tests PRIVATE
  Catch2::Catch2
  ${DROGON_TARGET}
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  argparse
  PostgreSQL::PostgreSQL
  ${Argon2_LIBRARIES}
  OpenSSL::Crypto
  plinth_quickjs
  libzip::zip
)

add_dependencies(plinth_tests plinth_shell_bundle_staged plinth_install_fixture_zips
                 plinth_lifecycle_fixture_zips plinth_rbac_test_runner_fixture_zips
                 plinth_test_extension_zips plinth_admin_zip)

include(Catch)

# 0.4.5.1 — grouped Catch2 entries, single Drogon lifecycle per group.
# Replaces catch_discover_tests(plinth_tests) which spawned one
# subprocess per TEST_CASE (~494) and reliably fired the teardown flake
# family tracked in project_ws_flaky_segfault.md. LH-0.1 (2026-04-21)
# confirmed the production kernel lifecycle tolerates the paths the
# Catch2 subprocess model trips on; the flakes are subprocess-scoped.
# Migration is incremental; already-grouped tags are excluded from the
# legacy per-TEST_CASE discovery via TEST_SPEC until every group has
# moved over.

add_test(NAME plinth_tests_pure
  COMMAND plinth_tests "~[integration] ~[ws] ~[js]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_pure PROPERTIES TIMEOUT 180)

# Exact positive/no-skip receipts for the typed-install foundation. Keep these
# independent of the grouped PG/WS entries: a missing database, renamed case or
# teardown failure must not masquerade as successful typed lifecycle evidence.
add_test(NAME plinth_tests_typed_surface_parser
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "typed surfaces: strict v2 manifest contract"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_typed_surface_parser PROPERTIES TIMEOUT 45)

add_test(NAME plinth_tests_protected_restore_policy
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "protected restore policy: absence is not failed configured trust"
    --timeout 30)
set_tests_properties(plinth_tests_protected_restore_policy PROPERTIES TIMEOUT 40)

# This gate deliberately needs a privileged fixture provisioner and then runs
# the test as a different unprivileged identity. A service-owned chmod 0444
# fixture is not equivalent. CI/Linux runners must support sudo -n (or root).
add_test(NAME plinth_tests_protected_restore_policy_deployment
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/protected_policy_fixture.py
    --binary $<TARGET_FILE:plinth_tests>)
set_tests_properties(plinth_tests_protected_restore_policy_deployment
  PROPERTIES TIMEOUT 90)

foreach(GENERATION_CASE IN ITEMS
    "capture|generation capture: bounded immutable physical inventory"
    "cache|generation cache: charged shared ownership not authority"
    "receipts|generation receipts: write-once transaction and lock ownership"
    "schema|generation receipts schema: validated retained bootstrap"
    "binding|generation binding: exact captured and registered inventory"
    "installed_registry|installed generation registry: receipt commit and immutable cache ownership"
    "request|typed admission: exact bounded raw lookup variants")
  string(REPLACE "|" ";" CASE_PARTS "${GENERATION_CASE}")
  list(GET CASE_PARTS 0 CASE_NAME)
  list(GET CASE_PARTS 1 CASE_TITLE)
  add_test(NAME plinth_tests_generation_${CASE_NAME}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${CASE_TITLE}"
      --timeout 45)
  set_tests_properties(plinth_tests_generation_${CASE_NAME}
    PROPERTIES TIMEOUT 55)
endforeach()

# Installed-float physical-owner gates. These remain exact positive/no-skip
# cases, not an installed-browser acceptance substitute.
foreach(ENTRY IN ITEMS
    "stream_owner|typed assets: physical callback-copy ownership"
    "stream_bounds|typed assets: bounded source admission and stop"
    "stream_registry|typed assets: state outlives registry and invalid sources refuse"
    "stream_headers|typed assets: bodyless native header serialization"
    "worker_owner|frontend worker: physical work and completion retain one slot"
    "worker_errors|frontend worker: fixed error completion and rejected admission"
    "worker_failed_drain|frontend worker: failed completion is sticky not a clean drain"
    "worker_fifo|frontend worker: bounded FIFO admits parallel module metadata"
    "worker_queued_expiry|frontend worker: queued expiry and caller cancellation skip work"
    "worker_queued_stop|frontend worker: stop physically drains every queued closure FIFO"
    "worker_disposal|frontend worker: closure disposal retains occupied capacity"
    "worker_capacity|frontend worker: capacity is finite before thread construction"
    "worker_failed_delivery|frontend worker: failed delivery cancels queued work without retry"
    "authority_credentials|surface authority: exact fresh credential and current rule gates"
    "authority_targets|surface authority: both gates provider ownership and bounded ordinal prefix"
    "storage_schema|P.platform.01: shell storage migration and generic layout mutation guard preserve existing data"
    "storage_cutover|P.platform.02: pool construction has per-name reservations and exception-safe bounded cutover"
    "storage_physical|P.platform.03: fixed storage keeps the shell fence and service lane until actual native SQL settlement"
    "rejected_delivery|P.platform.04: rejected request delivery is attempted once"
    "protocol|surface protocol: canonical asset paths float wire and bound cursors"
    "layout_contract|float layout contract: strict raw saved descriptors and complete bindings"
    "projection_failure|persistence projection: operational failure is not semantic ineligibility"
    "generation_lifecycle|generation lifecycle: optional receipts follow acknowledged package cutover"
    "platform_startup|platform startup: exact bundled publication and explicit receipt ownership"
    "routes|float routes: strict raw facade and one-shot redacted responses"
    "service_catalog|float layout service: real installed catalog admission and credential refresh"
    "service_storage|float layout service: native storage restore envelope and policy refusal"
    "service_assets|float layout service: admitted asset physical ownership and public bootstrap"
    "admin_service|float layout service: native admin defaults fresh authority and exact storage"
    "admin_ack|float layout service: admin native settlement and real lifecycle ACK fence"
    "admin_physical|float layout service: blocked admin SQL retains physical owner after logical cancellation"
    "db_deadline|ordinary nested PostgreSQL scopes cannot renew an absolute deadline"
    "db_cleanup|absolute PostgreSQL expiry still permits bounded confirmed cancellation")
  string(REPLACE "|" ";" PARTS "${ENTRY}")
  list(GET PARTS 0 NAME)
  list(GET PARTS 1 CASE)
  add_test(NAME plinth_tests_installed_float_${NAME}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${CASE}"
      --timeout 90
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_installed_float_${NAME}
    PROPERTIES TIMEOUT 105)
endforeach()
foreach(NAME IN ITEMS authority_credentials authority_targets storage_schema
                      storage_cutover storage_physical db_cleanup
                      generation_lifecycle platform_startup
                      service_catalog service_storage service_assets
                      admin_service admin_ack admin_physical)
  set_tests_properties(plinth_tests_installed_float_${NAME}
    PROPERTIES RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
               LABELS "needs_pg;native_ownership")
endforeach()
set_tests_properties(plinth_tests_generation_receipts
                     plinth_tests_generation_schema
                     plinth_tests_generation_binding
                     plinth_tests_generation_installed_registry
  PROPERTIES RESOURCE_LOCK plinth_pg_schema LABELS needs_pg)

# These are pure format tests, not persistence approval or installed-browser
# acceptance. Exact positive receipts prevent a renamed/empty case from passing.
foreach(ENTRY IN ITEMS
    "requests|admin defaults: closed raw and native requests"
    "results|admin defaults: closed fixed module results and schema envelopes"
    "cursor|admin defaults: independent bounded authenticated cursor domain")
  string(REPLACE "|" ";" PARTS "${ENTRY}")
  list(GET PARTS 0 NAME)
  list(GET PARTS 1 CASE)
  add_test(NAME plinth_tests_admin_defaults_${NAME}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${CASE}"
      --timeout 45
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_admin_defaults_${NAME} PROPERTIES TIMEOUT 60)
endforeach()

foreach(ENTRY IN ITEMS
    "policy|restore policy: strict bounded raw contract"
    "digests|restore digests: canonical bounded byte formats")
  string(REPLACE "|" ";" PARTS "${ENTRY}")
  list(GET PARTS 0 NAME)
  list(GET PARTS 1 CASE)
  add_test(NAME plinth_tests_restore_${NAME}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${CASE}"
      --timeout 45
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_restore_${NAME} PROPERTIES TIMEOUT 60)
endforeach()

set(PLINTH_TYPED_SURFACE_CASES
    "schema|typed surface schema preserves legacy provenance during retained bootstrap"
    "order|typed float order is transactional exact and lifetime-stable"
    "commit_order|typed float order allocation follows committed installation order"
    "lifecycle|typed panel lifecycle validates every explicit surface without primary leakage"
    "discovery|primary discovery excludes validated float and tray registrations"
    "package_api|typed package API registration preserves surface types"
    "recovery|typed surface recovery preserves committed order and provenance")
foreach(ENTRY IN LISTS PLINTH_TYPED_SURFACE_CASES)
  string(REPLACE "|" ";" PARTS "${ENTRY}")
  list(GET PARTS 0 NAME)
  list(GET PARTS 1 CASE)
  add_test(NAME plinth_tests_typed_surface_${NAME}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${CASE}"
      --timeout 90
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_typed_surface_${NAME}
    PROPERTIES RESOURCE_LOCK plinth_pg_schema
               LABELS "needs_pg"
               TIMEOUT 105)
endforeach()

# Repeat the WS group's map, displacement and bounded owner-release cases in
# a focused positive inventory gate alongside the exact close gate.
add_test(NAME plinth_tests_ws_registry
  COMMAND plinth_tests "[ws][registry]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws_registry PROPERTIES TIMEOUT 40)

set(PLINTH_WS_CLOSE_OWNER_CASE
    "registry closes connections on owner loops with bounded retry")
add_test(NAME plinth_tests_ws_close_ownership
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "${PLINTH_WS_CLOSE_OWNER_CASE}"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws_close_ownership PROPERTIES TIMEOUT 40)

set(PLINTH_GROUPED_JS_SELECTOR
    "[js] ~[batch-transaction] ~[isolated-logger] ~[isolated-pubsub] ~[isolated-cap] ~[isolated-db-admission] ~[isolated-db-batch] ~[isolated-audit-drain] ~[isolated-sync-conversion]")

# spdlog's default free APIs cannot overlap a default-logger replacement.
# These five synchronous capture fixtures must run before any background
# database/event-loop fixture is initialized in their process.
add_test(NAME plinth_tests_logger_capture_inventory
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/logger_capture_inventory_test.py
    --binary $<TARGET_FILE:plinth_tests>
    --grouped-selector "${PLINTH_GROUPED_JS_SELECTOR}"
    --ctest-command ${CMAKE_CTEST_COMMAND}
    --build-dir ${CMAKE_BINARY_DIR}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_logger_capture_inventory PROPERTIES TIMEOUT 90)

add_test(NAME plinth_tests_logger_capture_isolated
  COMMAND plinth_tests "[isolated-logger]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_logger_capture_isolated PROPERTIES TIMEOUT 120)

add_test(NAME plinth_tests_log_oracle
  COMMAND plinth_tests "[js][stdlib][log][oracle]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_log_oracle PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_js
  COMMAND plinth_tests "${PLINTH_GROUPED_JS_SELECTOR}"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_js
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 300)

# A named, bounded sanitizer smoke: a missing corpus must fail rather than
# disappearing inside the larger JS group. This is DB-free and uses the
# same test binary and RuntimePool setup as the grouped JS test.
add_test(NAME plinth_tests_crypto_corpus
  COMMAND plinth_tests "[js][stdlib][crypto][corpus]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_crypto_corpus PROPERTIES TIMEOUT 90)

add_test(NAME plinth_tests_crypto_numeric_regression
  COMMAND plinth_tests "[js][stdlib][crypto][numeric-regression]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_crypto_numeric_regression
  PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_log_regression
  COMMAND plinth_tests "[js][stdlib][log][regression]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_log_regression PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_quickjs_cstring_oom
  COMMAND plinth_tests "[js][quickjs][cstring-oom]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_quickjs_cstring_oom PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_json_nul_regression
  COMMAND plinth_tests "[js][conversion][nul-regression]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_json_nul_regression PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_conversion_corpus
  COMMAND plinth_tests "[js][conversion][corpus] ~[isolated-sync-conversion]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_conversion_corpus PROPERTIES TIMEOUT 90)

# The one-shot and pooled synchronous helpers have distinct private conversion
# paths. Exercise both in one DB-free process with exact fresh-XML verification.
add_test(NAME plinth_tests_sync_conversion_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS synchronous evaluation has bounded deterministic conversion corpora"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_sync_conversion_corpus PROPERTIES TIMEOUT 75)

add_test(NAME plinth_tests_host_string_boundary
  COMMAND plinth_tests "[js][stdlib][host-string-boundary]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_host_string_boundary PROPERTIES TIMEOUT 30)

add_test(NAME plinth_tests_audit_authority
  COMMAND plinth_tests "[js][audit][authority-regression]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_audit_authority PROPERTIES TIMEOUT 30)

# Exact, DB-free audit corpus; missing cases, skips and nonzero exits fail closed.
add_test(NAME plinth_tests_audit_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS audit.log has a bounded deterministic contract corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_audit_corpus PROPERTIES TIMEOUT 75)

# Native persistence and admission acknowledgement remain distinct contracts.
# Both cases retain ordinary grouped JS inclusion and fail closed without PG.
function(add_plinth_native_audit_case name selector)
  add_test(NAME plinth_tests_native_audit_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_audit_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)
endfunction()
add_plinth_native_audit_case(corpus
  "Native audit writes preserve the owned corpus")
add_plinth_native_audit_case(acknowledgement
  "Native audit acknowledgement precedes blocked persistence")

# Admission and callback ownership run before any broker/logging fixture in
# their process. The inventory gate keeps this exact exclusion independently
# selected; absent cases, skips and nonzero teardown exits fail closed.
add_test(NAME plinth_tests_pubsub_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS pubsub has a bounded deterministic admission and callback corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_pubsub_corpus PROPERTIES TIMEOUT 75)

# Actual native publication and registered callback delivery remain distinct:
# the latter uses the synthetic broker producer, not the listener/writer chain.
# Missing PostgreSQL, skipped cases and teardown failures fail both exact gates.
function(add_plinth_native_pubsub_case name selector)
  add_test(NAME plinth_tests_native_pubsub_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_pubsub_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)
endfunction()
add_plinth_native_pubsub_case(publish
  "Native pubsub publish acknowledgements preserve the owned corpus")
add_plinth_native_pubsub_case(delivery
  "Native pubsub registered delivery preserves owned cleanup")
add_plinth_native_pubsub_case(owner
  "Pubsub dispatch remains on the registered JavaScript owner loop")
add_plinth_native_pubsub_case(retirement
  "Pubsub queued delivery rejects retired checkout ownership")

# Actual capability admission and simulated settlement are isolated from any
# process-lifetime dispatcher/DB fixture. Missing cases and skipped XML fail.
add_test(NAME plinth_tests_cap_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS capability calls have a bounded deterministic admission and batch corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_cap_corpus PROPERTIES TIMEOUT 75)

# Ordinary DB binding admission and labelled host settlement do not open a DB.
# Keep this exact fresh-XML case separate from grouped transaction/DB fixtures.
add_test(NAME plinth_tests_db_admission_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS ordinary database calls have a bounded deterministic admission and ownership corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_db_admission_corpus PROPERTIES TIMEOUT 75)

# Actual public batch admission and installed orchestration; host completion
# is explicitly simulated in a fresh process, never SQL or driver execution.
add_test(NAME plinth_tests_db_batch_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS db.batch has a bounded deterministic admission and orchestration corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_db_batch_corpus PROPERTIES TIMEOUT 75)

# Real PostgreSQL binary parameters, independently required for each public
# route. Missing PG, a skipped case or failed native teardown must fail closed.
function(add_plinth_native_bytea_case name selector)
  add_test(NAME plinth_tests_native_bytea_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_bytea_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)
endfunction()
add_plinth_native_bytea_case(query
  "Native database queries preserve binary parameter bytes")
add_plinth_native_bytea_case(exec
  "Native database exec preserves stored binary parameter bytes")
add_plinth_native_bytea_case(batch
  "Native database batches preserve stored binary parameter bytes")

# Real registered calls and owned PostgreSQL teardown: no skip or absent case
# may satisfy this selected result/error corpus.
add_test(NAME plinth_tests_native_pg_result_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "QuickJS native PostgreSQL calls have a bounded result corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_native_pg_result_corpus PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

# Separate fresh processes make every native capability family fail closed on
# missing PostgreSQL, skipped observations or incomplete owner teardown. The
# ordinary JS group also retains these tests; no existing selector is narrowed.
function(add_plinth_native_capability_case name selector)
  add_test(NAME plinth_tests_native_capability_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_capability_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)
endfunction()
add_plinth_native_capability_case(data
  "QuickJS native capability calls have a bounded data corpus")
add_plinth_native_capability_case(edges
  "Native capability byte and conversion-depth boundaries retain their exact edges")
add_plinth_native_capability_case(errors
  "Native capability error families retain literal rejection and entry contracts")
add_plinth_native_capability_case(ordering
  "Native capability batches retain causal ordering and bounded fanout")
add_plinth_native_capability_case(ownership
  "Native capability callers retain bounded generation ownership")
add_plinth_native_capability_case(nested
  "Native capability cancellation reaches the owned callee generation")

function(add_plinth_cap_cancellation_case name selector)
  add_test(NAME plinth_tests_cap_cancellation_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}" --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_cap_cancellation_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema TIMEOUT 75)
endfunction()
add_plinth_cap_cancellation_case(sticky
  "Capability cancellation tokens are sticky before child registration")
add_plinth_cap_cancellation_case(cpu
  "Capability cancellation interrupts an entered CPU child as cancelled")
add_plinth_cap_cancellation_case(siblings
  "Capability cancellation tokens isolate sibling generations")
add_plinth_cap_cancellation_case(reuse
  "Retired capability generations cannot cancel a reused context")
add_plinth_cap_cancellation_case(teardown
  "Capability cancellation callback teardown joins cross-thread requests")
add_plinth_cap_cancellation_case(setup_failure
  "Capability cancellation setup faults retire owned contexts")
add_plinth_cap_cancellation_case(notification_failure
  "Capability cancellation notification faults retain sticky wall-wake cancellation")

function(add_plinth_sqlstate_case name selector)
  add_test(NAME plinth_tests_sqlstate_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}" --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_sqlstate_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg"
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1" TIMEOUT 75)
endfunction()
add_plinth_sqlstate_case(native
  "PostgreSQL driver preserves authoritative error diagnostics")
add_plinth_sqlstate_case(projection
  "Registered database bindings preserve authoritative SQLSTATE")
# Use the actual generated implementation choice, not an autoBatch runtime flag.
file(STRINGS "${drogon_BINARY_DIR}/drogon/config.h" PLINTH_PG_PIPELINE_DEFINE
  REGEX "^#define LIBPQ_SUPPORTS_BATCH_MODE 1$")
if(PLINTH_PG_PIPELINE_DEFINE)
  add_plinth_sqlstate_case(pipeline
    "PostgreSQL pipeline failures preserve only their causal diagnostics")
endif()

# Exercise the actual registered module loader and original host bytes. A
# missing database, skipped case or failed teardown cannot satisfy this gate.
add_test(NAME plinth_tests_module_source_gate
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Extension module loading enforces the original source gate"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_module_source_gate PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

add_test(NAME plinth_tests_module_import_corpus
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Extension module imports have a bounded deterministic corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_module_import_corpus PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

# Keep the two reverse JSON boundaries independent: actual registry argument
# injection and actual cap.call Promise settlement, not a shared-helper oracle.
add_test(NAME plinth_tests_reverse_json_registry_arguments
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Extension argument injection preserves JSON member names"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_reverse_json_registry_arguments PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

add_test(NAME plinth_tests_reverse_json_promise_settlement
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Capability promise settlement preserves JSON member names"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_reverse_json_promise_settlement PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

# Tagged JSON values have distinct registry-argument and Promise-result
# semantics. Require both real routes independently of the member-name gates.
add_test(NAME plinth_tests_reverse_json_registry_values
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Extension argument injection has a bounded tagged JSON corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_reverse_json_registry_values PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

add_test(NAME plinth_tests_reverse_json_promise_values
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "Capability promise settlement has a bounded tagged JSON corpus"
    --timeout 60
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_reverse_json_promise_values PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg" TIMEOUT 75)

add_test(NAME plinth_tests_log_corpus
  COMMAND plinth_tests "[js][stdlib][log][corpus]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_log_corpus PROPERTIES TIMEOUT 90)

add_test(NAME plinth_tests_config_corpus
  COMMAND plinth_tests "[js][stdlib][config][corpus]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_config_corpus PROPERTIES TIMEOUT 90)

# A single connection exposes accidental transactions opened by batch queries.
add_test(NAME plinth_tests_batch_transaction
  COMMAND plinth_tests "[batch-transaction]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_batch_transaction
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1"
             TIMEOUT 60)

set(PLINTH_BATCH_TIMEOUT_CASE
    "batch commit-time timeout permits reuse in the same evaluation")
foreach(case_name IN ITEMS hermetic cleanup native_rejection)
  if(case_name STREQUAL "hermetic")
    set(batch_selector
        "installed batch orchestrator requests rollback before timeout rejection")
  elseif(case_name STREQUAL "cleanup")
    set(batch_selector "${PLINTH_BATCH_TIMEOUT_CASE}")
  else()
    set(batch_selector
        "installed batch orchestrator preserves enqueued commit rejection")
  endif()
  add_test(NAME plinth_tests_batch_timeout_${case_name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${batch_selector}" --timeout 30
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_batch_timeout_${case_name} PROPERTIES TIMEOUT 40)
endforeach()
set_tests_properties(plinth_tests_batch_timeout_cleanup PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg"
  ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1")

set(PLINTH_AUDIT_DRAIN_CASE
    "audit drain retains blocked writes before Drogon shutdown and retries")
add_test(NAME plinth_tests_audit_drain
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "${PLINTH_AUDIT_DRAIN_CASE}"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_audit_drain PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg"
  ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1" TIMEOUT 40)

# Real blocked SQL must outlive JS retirement without outliving its native
# shutdown owner. Missing cases, skips and nonzero sanitizer exits fail closed.
add_test(NAME plinth_tests_js_cancellation_retirement
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "async cancellation retires context before gated late completion"
    --timeout 45
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_js_cancellation_retirement
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1"
             TIMEOUT 55)

# Actual registry/completion components with controlled native callback timing.
# Each exact case runs alone; no database fixture or skipped oracle is accepted.
function(add_plinth_native_owner_case name selector)
  add_test(NAME plinth_tests_native_owner_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 30
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_owner_${name} PROPERTIES
    LABELS "native_ownership" TIMEOUT 40)
endfunction()
add_plinth_native_owner_case(retained_retry
  "async cleanup failure retains owner and lease until explicit retry")
add_plinth_native_owner_case(audit_callback_tail
  "audit registry retains callback tail after successful settlement")
add_plinth_native_owner_case(audit_sticky_failure
  "audit registry failures stay unclean after owners retire")
add_plinth_native_owner_case(audit_admission_race
  "audit registry seal linearizes concurrent submissions without losing owners")
add_plinth_native_owner_case(duplicate_retention
  "async cleanup duplicate retention registers only one owner")
add_plinth_native_owner_case(republished_revision
  "async cleanup republished revision cannot be discarded as completed")
add_plinth_native_owner_case(dependency_order
  "async cleanup retries dependent records in either insertion order")
add_plinth_native_owner_case(expired_deadline
  "async cleanup expired deadline never launches cleanup work")
add_plinth_native_owner_case(revoked_sql
  "owned SQL callbacks cannot resume a destroyed revoked frame")
add_plinth_native_owner_case(single_sql_wake
  "owned SQL completion consumes exactly one successful wake")
add_plinth_native_owner_case(partial_binder
  "partial SQL binder setup cannot auto-dispatch on unwinding")
add_plinth_native_owner_case(failed_acquisition
  "retained transaction timeout callback survives failed acquisition setup")
add_plinth_native_owner_case(late_acquisition
  "revoked acquisition rolls back a late transaction without resuming")
add_plinth_native_owner_case(rollback_retry
  "native rollback failure remains retained through retry and callback release")

# Inject one failure at an actual native setup boundary; durable state and
# accepted cleanup ownership must agree. Keep each global checkpoint isolated.
function(add_plinth_native_setup_case name selector)
  add_test(NAME plinth_tests_native_setup_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}"
      --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_native_setup_${name} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS "needs_pg;native_ownership"
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1" TIMEOUT 75)
endfunction()
add_plinth_native_setup_case(dispatcher_task
  "batch dispatcher Task setup failure rolls back completed writes")
add_plinth_native_setup_case(launcher_task
  "batch launcher Task setup failure rolls back completed writes")
add_plinth_native_setup_case(batch_child
  "batch child Task setup failure rolls back completed writes")
add_plinth_native_setup_case(commit_completion
  "batch commit completion allocation failure rolls back completed writes")
add_plinth_native_setup_case(commit_callback
  "batch commit callback installation failure rolls back completed writes")
add_plinth_native_setup_case(wrapper_task
  "extension wrapper commit Task failure rolls back completed writes")
add_plinth_native_setup_case(begin_metadata
  "batch BEGIN metadata allocation failure retires its transaction")
add_plinth_native_setup_case(begin_queue
  "batch BEGIN queue allocation failure retires its transaction")
add_plinth_native_setup_case(commit_bookkeeping
  "batch post-ACK metadata failure preserves durable commit bookkeeping")
add_plinth_native_setup_case(domain_retry
  "private database domain failed close is retained until actual retry")

# Keep every integration section in the same process, including repeated schema
# bootstrap. Sanitizer instrumentation needs a larger aggregate runner budget;
# individual database, worker and lifecycle deadlines remain unchanged.
set(PLINTH_PG_AGGREGATE_TIMEOUT 300)
set(PLINTH_PG_REPORT_ARGS)
if(PLINTH_SANITIZERS)
  set(PLINTH_PG_AGGREGATE_TIMEOUT 450)
  set(PLINTH_PG_REPORT_ARGS --durations yes)
endif()
add_test(NAME plinth_tests_pg
  COMMAND plinth_tests "[integration] ~[ws] ~[js] ~[subprocess]"
    ${PLINTH_PG_REPORT_ARGS}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_pg
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT ${PLINTH_PG_AGGREGATE_TIMEOUT})

# Reuse the focused case verifier's exit/positive-assertion/no-skip contract.
add_test(NAME plinth_tests_buffered_realtime_hints
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "realtime listener drains buffered self-notifications before polling"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_buffered_realtime_hints
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 40)

add_test(NAME plinth_tests_writer_timeout_ownership
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "E.09: shutdown timeout preserves owned work until clean retry"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_writer_timeout_ownership
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 40)

# Repeated production boots need their own aggregate budget under sanitizers.
# Keep the per-process lifecycle bounds unchanged and serialize both groups
# against the shared PostgreSQL fixture, including when CTest runs in parallel.
add_test(NAME plinth_tests_subprocess
  COMMAND plinth_tests "[integration] [subprocess] ~[ws] ~[js]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_subprocess
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 300)

add_test(NAME plinth_tests_ws
  COMMAND plinth_tests "[ws] ~[browser-proxy] ~[isolated-ws-startup]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws
  PROPERTIES RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
             LABELS "needs_pg"
             TIMEOUT 180)

# Independent strict oracles keep mapping/worker cleanup and the real HTTP
# contention journey visible even when grouped Catch output is noisy.
set(PLINTH_PACKAGE_STATUS_CASE
  "package upload maps delegated upgrade contention to HTTP 409")
set(PLINTH_RBAC_QUIESCENCE_CASE
  "schema reset settles real RBAC workers and refuses unsafe DDL")
set(PLINTH_RBAC_TAIL_CASE
  "RBAC quiescence includes capture retirement and reentrant tails")
set(PLINTH_PACKAGE_CONTENTION_CASE
  "X.13: two concurrent POSTs same name different versions → one 201 + one 409")
function(add_plinth_package_boundary_case name selector)
  add_test(NAME plinth_tests_package_${name}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${selector}" --timeout 60
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tests_package_${name} PROPERTIES TIMEOUT 70)
endfunction()
add_plinth_package_boundary_case(status "${PLINTH_PACKAGE_STATUS_CASE}")
add_plinth_package_boundary_case(rbac_quiescence "${PLINTH_RBAC_QUIESCENCE_CASE}")
add_plinth_package_boundary_case(rbac_tail "${PLINTH_RBAC_TAIL_CASE}")
add_plinth_package_boundary_case(contention "${PLINTH_PACKAGE_CONTENTION_CASE}")
set_tests_properties(plinth_tests_package_rbac_quiescence
  plinth_tests_package_contention PROPERTIES
  RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099" LABELS "needs_pg")

# The Drogon singleton must be untouched before the controlled first-start
# regression. Exclude only its isolated tag from the shared WS process.
add_test(NAME plinth_tests_ws_listener_readiness
  COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
    --binary $<TARGET_FILE:plinth_tests>
    --case "WebSocket fixture waits for both owned listening sockets"
    --timeout 30
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws_listener_readiness PROPERTIES
  RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
  LABELS "needs_pg"
  TIMEOUT 40)

# Proxy-origin registration is immutable for a process, like the real kernel.
add_test(NAME plinth_tests_ws_browser_proxy
  COMMAND plinth_tests "[browser-proxy]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws_browser_proxy
  PROPERTIES RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
             LABELS "needs_pg"
             ENVIRONMENT "PLINTH_TEST_WS_BROWSER_ORIGIN=https://127.0.0.1:28099"
             TIMEOUT 30)

# Exercise production handlers with a separate disposable database and pool.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME plinth_tests_auth_http
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/auth_http.py
    --binary $<TARGET_FILE:plinth> --scenario disabled-session)
set_tests_properties(plinth_tests_auth_http PROPERTIES
  RESOURCE_LOCK plinth_pg_schema LABELS needs_pg TIMEOUT 180 SKIP_RETURN_CODE 77)
foreach(auth_scenario registration-modes bootstrap-security csrf)
  add_test(NAME plinth_tests_auth_${auth_scenario}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/auth_http.py
      --binary $<TARGET_FILE:plinth> --scenario ${auth_scenario})
  set_tests_properties(plinth_tests_auth_${auth_scenario} PROPERTIES
    RESOURCE_LOCK plinth_pg_schema LABELS needs_pg TIMEOUT 180 SKIP_RETURN_CODE 77)
endforeach()
add_test(NAME plinth_tests_ws_authority
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/ws_authority.py
          --binary $<TARGET_FILE:plinth>)
set_tests_properties(plinth_tests_ws_authority PROPERTIES
  LABELS "needs_pg" SKIP_RETURN_CODE 77 TIMEOUT 180)

# Keep TSan runs narrowly scoped and in separate Catch processes. The JS and
# WebSocket fixtures each own a process-lifetime Drogon instance, and their
# schema resets must not overlap. The production shutdown case covers both
# signals and accepted WebSocket writes; the 10x active-timer stress case and
# short-deadline replay/JS-stress cases remain outside this bounded lane.
if(PLINTH_TSAN)
  target_compile_definitions(plinth_tests PRIVATE PLINTH_TSAN_TEST=1)
  function(add_plinth_tsan_case name selector timeout_seconds)
    math(EXPR ctest_timeout_seconds "${timeout_seconds} + 10")
    add_test(NAME plinth_tsan_${name}
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
        --binary $<TARGET_FILE:plinth_tests>
        --case "${selector}"
        --timeout ${timeout_seconds}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(plinth_tsan_${name} PROPERTIES
      RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
      LABELS "needs_pg;tsan"
      TIMEOUT ${ctest_timeout_seconds})
  endfunction()

  add_plinth_tsan_case(listener_notify
    "R.01 listener delivers valid NOTIFY to handler" 180)
  add_plinth_tsan_case(listener_stop_barrier
    "R.05 stop_listener barriers on in-flight handler dispatch" 180)
  add_plinth_tsan_case(realtime_full_chain
    "I.01: full chain — listener -> writer -> cursor -> WS client" 180)
  add_plinth_tsan_case(js_concurrent_query
    "async_bridge: 10 concurrent contexts each run independent query" 180)
  add_plinth_tsan_case(js_query_audit
    "async_bridge: F.23 two threads query and audit independently" 180)
  add_plinth_tsan_case(audit_drain "${PLINTH_AUDIT_DRAIN_CASE}" 30)
  add_plinth_tsan_case(batch_timeout "${PLINTH_BATCH_TIMEOUT_CASE}" 30)
  set_tests_properties(plinth_tsan_batch_timeout PROPERTIES
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1")
  set_tests_properties(plinth_tsan_audit_drain PROPERTIES
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1")
  add_plinth_tsan_case(audit_admission_race
    "audit registry seal linearizes concurrent submissions without losing owners" 30)
  add_plinth_tsan_case(js_cancellation_retirement
    "async cancellation retires context before gated late completion" 45)
  set_tests_properties(plinth_tsan_js_cancellation_retirement PROPERTIES
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1")
  add_plinth_tsan_case(js_private_domain_retry
    "private database domain failed close is retained until actual retry" 60)
  set_tests_properties(plinth_tsan_js_private_domain_retry PROPERTIES
    ENVIRONMENT "PLINTH_TEST_DB_POOL_SIZE=1")
  add_plinth_tsan_case(production_durable_shutdown
    "production shutdown persists and replays the last extension write" 480)
  add_plinth_tsan_case(production_partial_startup
    "partial startup uses bounded coordinator teardown" 180)
  add_plinth_tsan_case(rbac_quiescence "${PLINTH_RBAC_QUIESCENCE_CASE}" 60)
  add_plinth_tsan_case(rbac_tail "${PLINTH_RBAC_TAIL_CASE}" 60)
  add_plinth_tsan_case(package_contention "${PLINTH_PACKAGE_CONTENTION_CASE}" 60)

  # Real EventLoopThread scheduling with a fake connection: no database or
  # process-lifetime Drogon fixture is needed for this owner-affinity oracle.
  add_test(NAME plinth_tsan_ws_close_ownership
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/tools/verify_catch_case.py
      --binary $<TARGET_FILE:plinth_tests>
      --case "${PLINTH_WS_CLOSE_OWNER_CASE}"
      --timeout 30
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(plinth_tsan_ws_close_ownership PROPERTIES
    LABELS "tsan" TIMEOUT 40)
endif()

# 0.5.5.2 closed the back-pressure spin race in `run_on_context`'s
# main loop, so `[js][async]` tests now share `plinth_tests_js`'s
# subprocess (folded above). Pre-0.5.5.2 they were dispatched per-
# TEST_CASE via `catch_discover_tests` because the race fired at
# ~15-25% under shared-subprocess vs ~10-15% per-TEST_CASE. The fix
# (run_on_context.cpp:1313 — also suspend on AnyCompletionAwaiter
# when concurrent_async_ops is at cap with pending_ops waiting) drops
# the rate to 0% across both shapes.

# IDE integration (VSCode CMake Tools, etc.) wants per-TEST_CASE
# entries in its test explorer. Opt-in flag adds a "dev.<name>"
# prefix so these entries never collide with the grouped entries
# above. Default OFF — CI never flips this.
option(PLINTH_DEVELOPER_TEST_DISCOVERY
  "Per-TEST_CASE catch_discover_tests entries with 'dev.' prefix — IDE use only" OFF)
if(PLINTH_DEVELOPER_TEST_DISCOVERY)
  catch_discover_tests(plinth_tests TEST_PREFIX "dev.")
endif()

# ─── parser libFuzzer harness (opt-in via -DPLINTH_FUZZ=ON, Clang only) ──
# The QuickJS crypto corpus above is a separate deterministic CTest target
# exercised by sanitizer CI. This parser-only libFuzzer target remains local.
option(PLINTH_FUZZ "Build libFuzzer harnesses (Clang only)" OFF)
if(PLINTH_FUZZ)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "libFuzzer enabled: building fuzz targets")
    add_executable(plinth_fuzz_parser
      tests/kernel/capabilities/fuzz_parser.cpp
      src/kernel/capabilities/parser.cpp
      src/kernel/capabilities/validation.cpp
    )
    target_include_directories(plinth_fuzz_parser PRIVATE
      ${CMAKE_SOURCE_DIR}/src
      ${CMAKE_BINARY_DIR}/generated
    )
    target_compile_options(plinth_fuzz_parser PRIVATE
      -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(plinth_fuzz_parser PRIVATE
      -fsanitize=fuzzer,address,undefined)
  else()
    message(FATAL_ERROR
      "PLINTH_FUZZ=ON requires Clang; compiler is ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

# ─── Benchmarks (opt-in via -DPLINTH_BENCHMARKS=ON) ──────────
# Validates ICD-0.2.2 §Performance Targets (< 1μs Tier 1, < 1ms Tier 2).
# Default OFF so a plain `cmake -B build` does not pay the Google Benchmark
# FetchContent cost; CI (and perf-interested devs) opt in explicitly.
option(PLINTH_BENCHMARKS "Build capability resolution benchmarks" OFF)
if(PLINTH_BENCHMARKS)
  message(STATUS "Benchmarks enabled: building capability benchmarks")

  FetchContent_Declare(
    google_benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG c58e6d0710581e3a08d65c349664128a8d9a2461
  )
  # Silence GBench's own test build and keep install rules out of our tree.
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  # GBench defaults to -Werror internally, which trips on some gcc-13
  # warnings in its own sources. Disable for the dep build only.
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(google_benchmark)

  # Minimal source set: resolution.cpp depends on parser/validation
  # (parse_signature) and logging (audit on RBAC denial path — gated
  # by g_audit_ready, so benchmarks never actually write audit rows).
  # extension_dispatch_stub.cpp satisfies the link reference resolution.cpp
  # makes to `plinth::extensions::dispatch` (Tier 2 extension arm). The
  # real definition lives in runtime_registry.cpp alongside the QuickJS
  # coroutine subsystem; benchmarks never exercise that path.
  set(PLINTH_BENCHMARK_KERNEL_SOURCES
    src/kernel/capabilities/resolution.cpp
    src/kernel/capabilities/parser.cpp
    src/kernel/capabilities/validation.cpp
    src/kernel/capabilities/drain.cpp
    src/kernel/logging.cpp
    src/kernel/security/unicode_scanner.cpp
    benchmarks/extension_dispatch_stub.cpp
  )
  set(PLINTH_BENCHMARK_LINK_LIBS
    benchmark::benchmark_main
    ${DROGON_TARGET}
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    PostgreSQL::PostgreSQL
    # 0.5.0.4 added `#include "kernel/extensions/runtime_registry.hpp"` to
    # resolution.cpp for Tier 2 extension dispatch. runtime_registry.hpp
    # includes js/async_op.hpp (for PromiseRejection in the dispatch
    # signature), and async_op.hpp has `#include <quickjs.h>`. The
    # benchmark doesn't call any quickjs entry points but the preprocessor
    # still needs the header; the INTERFACE target propagates `qjs`'s
    # include dir without bloating the binary beyond the transitive link.
    plinth_quickjs
  )

  add_executable(plinth_tier1_benchmark
    benchmarks/tier1_benchmark.cpp
    ${PLINTH_BENCHMARK_KERNEL_SOURCES}
  )
  target_include_directories(plinth_tier1_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
  )
  target_link_libraries(plinth_tier1_benchmark PRIVATE
    ${PLINTH_BENCHMARK_LINK_LIBS}
  )

  add_executable(plinth_tier2_benchmark
    benchmarks/tier2_benchmark.cpp
    ${PLINTH_BENCHMARK_KERNEL_SOURCES}
  )
  target_include_directories(plinth_tier2_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
  )
  target_link_libraries(plinth_tier2_benchmark PRIVATE
    ${PLINTH_BENCHMARK_LINK_LIBS}
  )

  # ICD-0.4.1 §Performance budget — 100 MB/s single-thread on the
  # v0.4.0 CI builder image. Same kernel-source set as tier1/tier2
  # (the scanner has no dependencies inside that set; pulling them
  # all in keeps the executable layout uniform).
  add_executable(plinth_unicode_scanner_benchmark
    benchmarks/unicode_scanner_benchmark.cpp
    ${PLINTH_BENCHMARK_KERNEL_SOURCES}
  )
  target_include_directories(plinth_unicode_scanner_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
  )
  target_link_libraries(plinth_unicode_scanner_benchmark PRIVATE
    ${PLINTH_BENCHMARK_LINK_LIBS}
  )
endif()

# ─── JS test shims (opt-in via -DPLINTH_JS_TEST_SHIMS=ON) ────
# Adds host-side helpers used ONLY by runtime_pool_test.cpp to exercise
# CPU-vs-wall-clock independence (ICD-0.3.1 §Tests 4). The shim
# installs a raw `globalThis.__host_sleep_ms__` function bypassing any
# RBAC check; it must NEVER end up in a production build. Default OFF
# keeps a plain `cmake -B build` clean.
option(PLINTH_JS_TEST_SHIMS
       "Build JS test shims (host sleep for CPU/wall-clock tests)" OFF)
if(PLINTH_JS_TEST_SHIMS)
  message(STATUS "JS test shims enabled")
  target_sources(plinth_tests PRIVATE
    tests/kernel/js/test_host_sleep.cpp
  )
  target_compile_definitions(plinth_tests PRIVATE PLINTH_JS_TEST_SHIMS)
endif()

# ─── clang-tidy ───────────────────────────────────────────────
# Prefer run-clang-tidy (from clang-tools) so per-file invocations run in
# parallel — a single-process clang-tidy invocation over all kernel
# sources dominated CI wall-clock prior to 0.2.1.1a. Fall back to a
# serial clang-tidy invocation when run-clang-tidy is unavailable so
# local devs without clang-tools still get coverage.
# Match tools/lint.sh; an unversioned executable could select another major.
# Versioned cache keys avoid retaining tools from a previous major on upgrade.
find_program(RUN_CLANG_TIDY_21_EXE NAMES run-clang-tidy-21)
find_program(CLANG_TIDY_21_EXE NAMES clang-tidy-21)
file(GLOB_RECURSE KERNEL_SOURCES
  ${CMAKE_SOURCE_DIR}/src/kernel/*.cpp
  ${CMAKE_SOURCE_DIR}/src/kernel/*.hpp
  ${CMAKE_SOURCE_DIR}/tests/kernel/*.cpp
  ${CMAKE_SOURCE_DIR}/tests/kernel/*.hpp
)
if(RUN_CLANG_TIDY_21_EXE AND CLANG_TIDY_21_EXE)
  # clang-tidy has a large per-translation-unit working set. Keep the default
  # within the repository's 16 GiB development/CI memory envelope.
  set(TIDY_JOBS 2 CACHE STRING "Parallel clang-tidy jobs")
  add_custom_target(tidy
    COMMAND ${RUN_CLANG_TIDY_21_EXE}
      -p ${CMAKE_BINARY_DIR}
      -j ${TIDY_JOBS}
      -clang-tidy-binary ${CLANG_TIDY_21_EXE}
      -quiet
      ${KERNEL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy (run-clang-tidy -j${TIDY_JOBS}) on kernel sources"
    VERBATIM
  )
elseif(CLANG_TIDY_21_EXE)
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_21_EXE}
      -p ${CMAKE_BINARY_DIR}
      ${KERNEL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy (serial fallback) on kernel sources"
    VERBATIM
  )
else()
  message(STATUS "clang-tidy 21 not found — 'tidy' target disabled")
endif()
