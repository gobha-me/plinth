// SPDX-License-Identifier: MIT
//
// Tier 2 extension capability dispatch — ICD-0.5.0.3 §Test plan
// groups R/E/P/H/C. Exercises `plinth::extensions::dispatch` both
// directly and through the resolver's `call_capability_async` path,
// covering the handler-invocation pipeline, error taxonomy, identity
// propagation, and lifecycle race windows.
//
// Library-level install: each case stages an extension on-disk under
// a temp data_dir, seeds the Tier 2 cache with the matching row, and
// calls `create_pool` directly. This avoids the full install_package
// + PG round-trip for cases that exercise dispatch alone; PG-backed
// install paths are covered by lifecycle_transitions_test.cpp's
// existing D.*/U.*/X.* groups now that those transitions call
// `plinth::extensions::create_pool` / `destroy_pool`.

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/config.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/logging.hpp"

#include "../js/async_bridge_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

using plinth::async_bridge_test::ensure_drogon_running;
using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::test_config;

namespace {

// Global counter to derive unique scratch paths per test instance
// (Catch2 runs tests in the same subprocess; a new test case starts
// in the same process as the previous).
std::atomic<std::uint64_t> scratch_counter{0};

// Per-case scratch environment: temp data_dir + initialized registry.
// The registry's `cfg_ptr` is stashed from the Config pointer passed
// to `init_registry`, so the Config must outlive the scratch.
struct ExtScratch {
  plinth::Config cfg{};
  fs::path base;

  ExtScratch() {
    // PG + DbClient required so resolver `emit_rbac_denied` and
    // registry `emit_extension_error_audit` paths land on a real
    // audit row instead of asserting inside Drogon's
    // DbClientManager (debug-build contract). Every test in this
    // file is `[integration]` + `pg_available()`-gated for that
    // reason. Caller must check `pg_available()` before constructing.
    ensure_drogon_with_db_running();

    auto id = std::to_string(::getpid()) + "_" +
              std::to_string(scratch_counter.fetch_add(1));
    base = fs::temp_directory_path() / ("plinth_extdisp_" + id);
    fs::create_directories(base / "data");

    cfg = test_config();
    cfg.packages_data_dir = (base / "data").string();
    // init_registry tries to SELECT from plinth.packages. In
    // these unit-level tests the scan may find nothing (tests do
    // not seed the table) but the PG connect succeeds, so
    // `cfg_ptr` is set for `create_pool` to consume.
    plinth::extensions::init_registry(cfg);
  }

  ~ExtScratch() {
    (void)plinth::extensions::shutdown_registry();
    plinth::capabilities::clear_resolver_for_test();
    std::error_code ec;
    fs::remove_all(base, ec);
  }

  ExtScratch(const ExtScratch&) = delete;
  auto operator=(const ExtScratch&) -> ExtScratch& = delete;
  ExtScratch(ExtScratch&&) = delete;
  auto operator=(ExtScratch&&) -> ExtScratch& = delete;

  // Stage an extension on disk at {data_dir}/extensions/<name>/1.0.0/
  // + the `active` symlink. Handlers is a list of (function, source)
  // pairs written verbatim to server/handlers/<fn>.js. The caller
  // also seeds the Tier 2 cache for any capabilities they intend to
  // dispatch via `call_capability_async`.
  auto stage_extension(
      std::string_view name,
      std::initializer_list<std::pair<std::string_view, std::string_view>>
          handlers) const -> void {
    fs::path ext_root = base / "data" / "extensions" / std::string{name};
    fs::path version_dir = ext_root / "1.0.0";
    fs::create_directories(version_dir / "server" / "handlers");

    // server/main.js — re-export per fixture convention (stub
    // contents; the registry's dispatcher reads handlers/<fn>.js
    // directly, not server/main.js).
    std::ofstream(version_dir / "server" / "main.js")
        << "// extension dispatch fixture\n";

    for (const auto& [fn, src] : handlers) {
      std::ofstream(version_dir / "server" / "handlers" /
                    (std::string{fn} + ".js"))
          << src;
    }

    // Create (or replace) the `active` symlink to point at 1.0.0.
    fs::path active = ext_root / "active";
    std::error_code ec;
    fs::remove(active, ec);
    fs::create_directory_symlink("1.0.0", active, ec);
  }
};

// Seed a single Tier 2 cache row matching an extension handler.
auto seed_tier2(std::string_view signature, std::string_view extension_name,
                std::string_view rbac_rule = "ext.call") -> void {
  plinth::capabilities::CachedCapability entry{
      .signature = std::string{signature},
      .provider_type = "extension",
      .extension_name = std::string{extension_name},
      .scope = "instance",
      .user_id = {},
      .rbac_rule = std::string{rbac_rule},
      .enabled = true,
  };
  plinth::capabilities::seed_tier2_cache_for_test(entry);
}

auto admin_ctx() -> plinth::capabilities::UserContext {
  return plinth::capabilities::UserContext{
      .user_id = "u-test",
      .username = "tester",
      .auth_type = "session",
      .effective_rules = {"kernel.admin"}, // universal match
      .session_id = {},
      .ip_address = {},
  };
}

auto run_dispatch(std::string_view ext_name, std::string_view fn,
                  const Json::Value& args)
    -> std::expected<Json::Value, plinth::js::PromiseRejection> {
  return drogon::sync_wait(
      plinth::extensions::dispatch(ext_name, fn, args, admin_ctx(), 0));
}

auto run_resolver(std::string_view signature, const Json::Value& args,
                  std::string* ext_code_out = nullptr,
                  std::string* ext_msg_out = nullptr)
    -> plinth::capabilities::ResolveResult {
  plinth::capabilities::CapabilityCall call{
      .signature = std::string{signature},
      .args = args,
      .call_depth = 0,
  };
  return drogon::sync_wait(plinth::capabilities::call_capability_async(
      call, admin_ctx(), ext_code_out, ext_msg_out));
}

} // namespace

// Sentinel for early-skip when PG isn't available. Every TEST_CASE
// in this file starts with `SKIP_WITHOUT_PG();` so `plinth_tests_pg`
// SKIPs cleanly when the env is absent instead of tripping the
// DbClientManager assert via ExtScratch's Drogon bring-up.
// Must remain a function-like macro rather than a helper function:
// Catch2's `SKIP(...)` relies on exception-based flow from within the
// test body; a wrapper function works at runtime but confuses the
// compiler's reachability analysis for tests that touch objects whose
// RAII teardown races with Catch2's SKIP handler — this bit
// repeatedly in 0.5.0.4 pre-merge. Keep the macro; suppress the lint.
#define SKIP_WITHOUT_PG()                                                      \
  do {                                                                         \
    if (!pg_available()) {                                                     \
      SKIP("PG not available");                                                \
    }                                                                          \
  } while (false)

// ─── Group R — Resolve (happy paths) ─────────────────────────────

TEST_CASE("R.01: cap_call_extdispatch_echo_happy_path",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"echo", R"(
            export default function echo(args) {
              return { x: args.x, doubled: args.x * 2 };
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:echo", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  Json::Value args(Json::objectValue);
  args["x"] = 42;
  auto r = run_resolver("extdispatch:1:echo", args);
  REQUIRE(r.has_value());
  REQUIRE(r->data["x"].asInt() == 42);
  REQUIRE(r->data["doubled"].asInt() == 84);
  REQUIRE(r->resolved_tier == "tier2");
  REQUIRE(r->provider_type == "extension");
}

// ─── Group E — Errors ────────────────────────────────────────────

TEST_CASE("E.01: handler_throw_maps_to_cap_handler_threw",
          "[cap][res][ext][integration]") {
  // Emits a `capability.extension.error` audit row; requires a
  // DbClient attached to drogon::app(). Gate on PG so the
  // DbClient path doesn't assert inside Drogon's debug
  // `DbClientManager::getDbClient` when the client is absent.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"thrower", R"(
            export default function thrower() {
              throw new Error("boom");
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:thrower", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:thrower", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::EXTENSION_DISPATCH_FAILED);
  REQUIRE(code == "cap.handler_threw");
  REQUIRE(msg.find("boom") != std::string::npos);
}

TEST_CASE("E.02: unknown_signature_rejects_at_resolver",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // No cache seed, no pool — bare miss at the resolver.
  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:ghost", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::CAPABILITY_NOT_FOUND);
  REQUIRE(code.empty()); // never reaches the extension dispatcher
}

TEST_CASE("E.03: missing_handler_file_rejects_cap_handler_not_found",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo", R"(export default function echo() { return {}; })"},
      });
  seed_tier2("extdispatch:1:gone", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  // The capability is cached; the handler file is not on disk.
  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:gone", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.handler_not_found");
  // ICD §Handler invocation step 2 — do not leak the fs path.
  REQUIRE(msg.find("/tmp") == std::string::npos);
  REQUIRE(msg.find("data/extensions") == std::string::npos);
  REQUIRE(msg.find("extdispatch:gone") != std::string::npos);
}

TEST_CASE("E.04: handler_syntax_error_rejects_cap_handler_load_failed",
          "[cap][res][ext][integration]") {
  // `cap.handler_load_failed` also emits a
  // `capability.extension.error` audit row (ICD §Audit); gate on PG
  // for the same reason as E.01.
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();

  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"broken", "export default function broken( { return 1; }"},
      });
  seed_tier2("extdispatch:1:broken", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:broken", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.handler_load_failed");
}

TEST_CASE("E.05: destroyed_pool_rejects_extension_not_loaded",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo", R"(export default function echo() { return {}; })"},
      });
  seed_tier2("extdispatch:1:echo", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  plinth::extensions::destroy_pool("extdispatch");

  std::string code;
  std::string msg;
  auto r = run_resolver("extdispatch:1:echo", Json::Value{}, &code, &msg);
  REQUIRE_FALSE(r.has_value());
  REQUIRE(code == "cap.extension_not_loaded");
}

TEST_CASE(
    "E.06: sync_call_capability_on_extension_entry_returns_async_required",
    "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // Only the cache seed — no on-disk handler needed; the sync path
  // rejects before reaching `dispatch_tier2`'s extension arm.
  seed_tier2("extdispatch:1:echo", "extdispatch");

  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = plinth::capabilities::call_capability(call, admin_ctx());
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() == plinth::capabilities::CapabilityError::ASYNC_REQUIRED);
}

// ─── Group P — Propagation + identity ─────────────────────────────

TEST_CASE("P.02: extension_handler_cannot_spoof_other_extension_publish",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  // Handler attempts to publish on another extension's channel
  // prefix. The pubsub binding's identity gate rejects.
  s.stage_extension("extdispatch", {
                                       {"spoof", R"(
            export default async function spoof() {
              try {
                await pubsub.publish("plinth:ext:other:test", {hi: 1});
                return { ok: true };
              } catch (e) {
                return { caught: e.code || "<no-code>" };
              }
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:spoof", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto r = run_resolver("extdispatch:1:spoof", Json::Value{});
  REQUIRE(r.has_value());
  // ICD §Security Constraint 3 — `bc.extension_name` is set by the
  // pool (the callee's own name); publishing on another extension's
  // channel hits the identity gate at
  // `pubsub_bindings.cpp:154-167` and rejects with
  // `pubsub.extension_mismatch`.
  REQUIRE(r->data["caught"].asString() == "pubsub.extension_mismatch");
}

TEST_CASE("P.03: rbac_enforced_at_resolver_not_callee",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"echo",
           R"(export default function echo() { return { ok: true }; })"},
      });
  // Seed with a specific rule that our non-admin context lacks.
  seed_tier2("extdispatch:1:echo", "extdispatch", "extdispatch.call");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  // Non-admin, no matching rules → resolver denies BEFORE dispatch.
  plinth::capabilities::UserContext no_rules{
      .user_id = "u-no-rules",
      .username = "nobody",
      .auth_type = "session",
      .effective_rules = {},
      .session_id = {},
      .ip_address = {},
  };
  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = drogon::sync_wait(
      plinth::capabilities::call_capability_async(call, no_rules));
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error() ==
          plinth::capabilities::CapabilityError::PERMISSION_DENIED);
}

TEST_CASE("P.04: handler_receives_ctx_with_caller_identity_and_audit_frame",
          "[cap][res][ext][integration]") {
  // 0.6.3.N — proves the runtime_registry wrapper injects a second
  // positional `ctx` carrying the audit-frame projection of the
  // caller's UserContext. Pre-fix, every shell handler signature
  // `(args, ctx)` saw `ctx === undefined` and `ctx.user.id` threw
  // TypeError. The handler under test reads each field the
  // wrapper now binds and returns them so the assertion side
  // verifies the wire end-to-end.
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"echo_ctx", R"(
            export default function echo_ctx(_args, ctx) {
              return {
                user_id:   ctx.user.id,
                username:  ctx.user.username,
                auth_type: ctx.user.auth_type,
                session_id: ctx.session_id,
                ip_address: ctx.ip_address,
                call_depth: ctx.call_depth,
                extension:  ctx.extension,
                rules_present: 'effective_rules' in ctx.user,
              };
            }
        )"},
                                   });
  seed_tier2("extdispatch:1:echo_ctx", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  plinth::capabilities::UserContext caller{
      .user_id = "u-p04",
      .username = "alice",
      .auth_type = "session",
      .effective_rules = {"kernel.admin"}, // universal match for resolver
      .session_id = "sess-abc",
      .ip_address = "10.0.0.7",
  };
  plinth::capabilities::CapabilityCall call{
      .signature = "extdispatch:1:echo_ctx",
      .args = Json::Value{},
      .call_depth = 0,
  };
  auto r = drogon::sync_wait(
      plinth::capabilities::call_capability_async(call, caller));
  REQUIRE(r.has_value());
  REQUIRE(r->data["user_id"].asString() == "u-p04");
  REQUIRE(r->data["username"].asString() == "alice");
  REQUIRE(r->data["auth_type"].asString() == "session");
  REQUIRE(r->data["session_id"].asString() == "sess-abc");
  REQUIRE(r->data["ip_address"].asString() == "10.0.0.7");
  // `bc.call_depth` is set to caller_call_depth + 1 at dispatch entry
  // (runtime_registry.cpp:718) so the handler observes the depth of
  // its own frame, not the caller's.
  REQUIRE(r->data["call_depth"].asInt() == 1);
  REQUIRE(r->data["extension"].asString() == "extdispatch");
  // ctx.user MUST NOT carry effective_rules — handlers should not
  // self-introspect RBAC; resolver step 3 owns the gate.
  REQUIRE(r->data["rules_present"].asBool() == false);
}

// ─── Group H — Hot reload / lifecycle ─────────────────────────────

TEST_CASE("H.01: destroy_pool_followed_by_create_pool_cycles",
          "[cap][res][ext][integration]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension(
      "extdispatch",
      {
          {"ping",
           R"(export default function ping() { return { ok: true }; })"},
      });
  seed_tier2("extdispatch:1:ping", "extdispatch");
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto r1 = run_resolver("extdispatch:1:ping", Json::Value{});
  REQUIRE(r1.has_value());
  REQUIRE(r1->data["ok"].asBool());

  plinth::extensions::destroy_pool("extdispatch");
  std::string code1;
  auto r2 = run_resolver("extdispatch:1:ping", Json::Value{}, &code1);
  REQUIRE_FALSE(r2.has_value());
  REQUIRE(code1 == "cap.extension_not_loaded");

  REQUIRE(plinth::extensions::create_pool("extdispatch"));
  auto r3 = run_resolver("extdispatch:1:ping", Json::Value{});
  REQUIRE(r3.has_value());
  REQUIRE(r3->data["ok"].asBool());
}

TEST_CASE("H.02: shutdown rejects new work and drains accepted dispatches",
          "[cap][res][ext][integration][lifecycle]") {
  SKIP_WITHOUT_PG();
  ExtScratch s;
  s.stage_extension("extdispatch", {
                                       {"slow", R"(
            export default async function slow() {
              await db.query('SELECT pg_sleep(0.2)');
              return { ok: true };
            }
        )"},
                                   });
  REQUIRE(plinth::extensions::create_pool("extdispatch"));

  auto dispatch = std::async(std::launch::async, [] {
    return run_dispatch("extdispatch", "slow", Json::Value{});
  });
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (plinth::extensions::inflight_dispatch_count_for_test() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  REQUIRE(plinth::extensions::inflight_dispatch_count_for_test() == 1);

  REQUIRE_FALSE(plinth::extensions::shutdown_registry(0ms));
  auto rejected = run_dispatch("extdispatch", "slow", Json::Value{});
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().code == "cap.extension_not_loaded");

  REQUIRE(dispatch.wait_for(2s) == std::future_status::ready);
  auto result = dispatch.get();
  REQUIRE(result.has_value());
  REQUIRE((*result)["ok"].asBool());
  REQUIRE(plinth::extensions::shutdown_registry(1s));
}
