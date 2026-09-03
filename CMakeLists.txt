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

find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
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

# ─── Sanitizers (opt-in via -DPLINTH_SANITIZERS=ON) ──────────
option(PLINTH_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(PLINTH_SANITIZERS)
  message(STATUS "Sanitizers enabled: address,undefined")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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
# the fetched source tree (see that dir's README). Currently only
# ftype-accessor.patch — exposes Field::oid() for ICD-0.5.3 OID-driven
# type mapping. The shell wrapper makes the PATCH_COMMAND idempotent:
# FetchContent's update step re-runs PATCH_COMMAND on every cmake
# reconfigure, so we `git apply --reverse --check` first to detect an
# already-applied patch and skip quietly; otherwise apply normally.
set(PLINTH_DROGON_PATCH
    ${CMAKE_SOURCE_DIR}/third_party/drogon-patches/ftype-accessor.patch)
FetchContent_Declare(
  drogon
  GIT_REPOSITORY https://github.com/drogonframework/drogon.git
  GIT_TAG 89aca8c7993c8194f2c109c1d06a3b45bf363d5d
  PATCH_COMMAND sh -c
    "git apply --reverse --check '${PLINTH_DROGON_PATCH}' 2>/dev/null \
     || git apply --whitespace=nowarn '${PLINTH_DROGON_PATCH}'"
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

file(GLOB_RECURSE PLINTH_SHELL_INPUTS CONFIGURE_DEPENDS
     ${PLINTH_SHELL_SRC_DIR}/*)

add_custom_command(
  OUTPUT ${PLINTH_SHELL_ZIP}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${PLINTH_SHELL_ZIP}
  COMMAND ${CMAKE_COMMAND} -E tar cf ${PLINTH_SHELL_ZIP} --format=zip --
          manifest.json capabilities.json rbac.json panels.json config.json
          client server migrations
  WORKING_DIRECTORY ${PLINTH_SHELL_SRC_DIR}
  DEPENDS ${PLINTH_SHELL_INPUTS}
  VERBATIM
  COMMENT "Packing client/shell/ -> shell.zip"
)
add_custom_target(plinth_shell_zip DEPENDS ${PLINTH_SHELL_ZIP})

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
        DESTINATION ${CMAKE_INSTALL_DATADIR}/plinth/bundled)

# QuickJS — vendored via FetchContent (quickjs-ng fork; bellard/quickjs has
# no version tags as of 2026-04). Upstream CMakeLists declares extra
# executables (qjsc, qjs_exe, api-test, lre-test, ...) unconditionally;
# EXCLUDE_FROM_ALL keeps them out of `all` so only the `qjs` static library
# builds (as a transitive dep of plinth / plinth_tests).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "QuickJS vendoring (ICD-0.3.0) is Linux-only for now.")
endif()
FetchContent_Declare(
  quickjs
  GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
  GIT_TAG 3c051980ab7e783dfbfb1c70c014ce5e05ecf24c
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
  src/kernel/lifecycle/async_task_registry.cpp
  src/kernel/lifecycle/shutdown_coordinator.cpp
  src/kernel/config.cpp
  src/kernel/logging.cpp
  src/kernel/db/bootstrap.cpp
  src/kernel/audit/handlers.cpp
  src/kernel/auth/crypto.cpp
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
  src/kernel/shell/active_frontend.cpp
  src/kernel/shell/firstboot.cpp
  src/kernel/frontend/api_frontend.cpp
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

# ─── Tests ────────────────────────────────────────────────────
enable_testing()
add_executable(plinth_tests
  tests/kernel/main_test.cpp
  tests/kernel/lifecycle/async_task_registry_test.cpp
  tests/kernel/lifecycle/shutdown_coordinator_test.cpp
  tests/kernel/lifecycle/process_shutdown_test.cpp
  tests/kernel/config_test.cpp
  tests/kernel/db/bootstrap_test.cpp
  tests/kernel/audit/audit_test.cpp
  tests/kernel/audit/audit_teardown_test.cpp
  tests/kernel/auth/crypto_test.cpp
  tests/kernel/auth/rate_limiter_test.cpp
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
  tests/kernel/rbac/anonymous_identity_test.cpp
  tests/kernel/rbac/rule_registrar_test.cpp
  tests/kernel/rbac/rbac_manifest_test.cpp
  tests/kernel/rbac/rule_validator_test.cpp
  tests/kernel/rbac/ephemeral_user_test.cpp
  tests/kernel/rbac/subscribe_rule_test.cpp
  tests/kernel/ws/ws_test_fixture.cpp
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
  tests/kernel/js/async_bridge_test.cpp
  tests/kernel/js/async_bridge_fixture.cpp
  tests/kernel/js/db_batch_test.cpp
  tests/kernel/js/db_search_path_test.cpp
  tests/kernel/js/db_silent_test.cpp
  tests/kernel/js/pubsub_test.cpp
  tests/kernel/js/pubsub_subscribe_test.cpp
  tests/kernel/js/limits_test.cpp
  tests/kernel/js/async_hardening_test.cpp
  tests/kernel/js/eval_guard_test.cpp
  tests/kernel/packages/manifest_test.cpp
  tests/kernel/packages/capabilities_manifest_test.cpp
  tests/kernel/packages/panels_manifest_test.cpp
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
  tests/kernel/packages/rbac_test_report_test.cpp
  tests/kernel/packages/rbac_test_runner_test.cpp
  tests/kernel/main/cli_test_rbac_test.cpp
  tests/kernel/security/unicode_scanner_test.cpp
  tests/kernel/shell/active_frontend_test.cpp
  tests/kernel/shell/firstboot_test.cpp
  tests/kernel/shell/preferences_test.cpp
  tests/kernel/shell/preferences_dispatch_test.cpp
  tests/kernel/frontend/api_frontend_test.cpp
  tests/kernel/cap/api_cap_test.cpp
  src/kernel/config.cpp
  src/kernel/lifecycle/async_task_registry.cpp
  src/kernel/lifecycle/shutdown_coordinator.cpp
  src/kernel/logging.cpp
  src/kernel/db/bootstrap.cpp
  src/kernel/audit/handlers.cpp
  src/kernel/auth/crypto.cpp
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
  src/kernel/shell/active_frontend.cpp
  src/kernel/shell/firstboot.cpp
  src/kernel/frontend/api_frontend.cpp
  src/kernel/cap/api_cap.cpp
)

target_include_directories(plinth_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/generated
  ${Argon2_INCLUDE_DIRS}
)

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
                 plinth_lifecycle_fixture_zips plinth_rbac_test_runner_fixture_zips)

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

add_test(NAME plinth_tests_js
  COMMAND plinth_tests "[js]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_js
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 300)

add_test(NAME plinth_tests_pg
  COMMAND plinth_tests "[integration] ~[ws] ~[js]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_pg
  PROPERTIES RESOURCE_LOCK plinth_pg_schema
             LABELS "needs_pg"
             TIMEOUT 300)

add_test(NAME plinth_tests_ws
  COMMAND plinth_tests "[ws]"
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(plinth_tests_ws
  PROPERTIES RESOURCE_LOCK "plinth_pg_schema;plinth_ws_port_28099"
             LABELS "needs_pg"
             TIMEOUT 180)

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

# ─── libFuzzer harness (opt-in via -DPLINTH_FUZZ=ON, Clang only) ──
# CI wiring is tracked as ROADMAP 0.2.1.1. Local-only for now.
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
    message(STATUS
      "PLINTH_FUZZ=ON requires Clang; skipping fuzz targets (compiler is "
      "${CMAKE_CXX_COMPILER_ID}).")
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
find_program(RUN_CLANG_TIDY_EXE NAMES run-clang-tidy-20 run-clang-tidy)
find_program(CLANG_TIDY_EXE NAMES clang-tidy-20 clang-tidy)
file(GLOB_RECURSE KERNEL_SOURCES
  ${CMAKE_SOURCE_DIR}/src/kernel/*.cpp
  ${CMAKE_SOURCE_DIR}/src/kernel/*.hpp
  ${CMAKE_SOURCE_DIR}/tests/kernel/*.cpp
  ${CMAKE_SOURCE_DIR}/tests/kernel/*.hpp
)
if(RUN_CLANG_TIDY_EXE)
  # clang-tidy has a large per-translation-unit working set. Keep the default
  # within the repository's 16 GiB development/CI memory envelope.
  set(TIDY_JOBS 2 CACHE STRING "Parallel clang-tidy jobs")
  add_custom_target(tidy
    COMMAND ${RUN_CLANG_TIDY_EXE}
      -p ${CMAKE_BINARY_DIR}
      -j ${TIDY_JOBS}
      -quiet
      ${KERNEL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy (run-clang-tidy -j${TIDY_JOBS}) on kernel sources"
    VERBATIM
  )
elseif(CLANG_TIDY_EXE)
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_EXE}
      -p ${CMAKE_BINARY_DIR}
      ${KERNEL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy (serial fallback) on kernel sources"
    VERBATIM
  )
else()
  message(STATUS "clang-tidy not found — 'tidy' target disabled")
endif()
