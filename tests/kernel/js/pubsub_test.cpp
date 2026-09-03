// SPDX-License-Identifier: MIT
//
// ICD-0.5.0 §Test Cases — P.* pubsub.publish JS binding tests.
//
// P.01 — happy path end-to-end. Publishes via JS; listener receives
//        the envelope. PG + drogon DbClient required.
// P.02 — extension mismatch (caller="notes" on channel "...terminal:...").
// P.03 — Layer-1 shape rejected (JS can't emit Layer 1).
// P.04 — oversized payload rejected pre-enqueue.
// P.05 — kernel-scope BridgeContext rejected with extension_mismatch.
//
// P.02–P.05 validate inline rejection paths that fire BEFORE the
// async-op enqueue. They still require a running drogon loop because
// run_on_context is a drogon coroutine; we share the
// async_bridge_fixture which spins one up on first use.

#include <catch2/catch_test_macros.hpp>

#include "async_bridge_fixture.hpp"
#include "kernel/config.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/listener.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using plinth::async_bridge_test::ensure_drogon_with_db_running;
using plinth::async_bridge_test::pg_available;
using plinth::async_bridge_test::test_config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::EvalResult;
using plinth::js::run_on_context;
using plinth::js::RuntimePool;

namespace {

struct ScopedMaxPayload {
  std::size_t prev;
  explicit ScopedMaxPayload(std::size_t bytes)
      : prev(plinth::realtime::get_max_payload_bytes()) {
    plinth::realtime::set_max_payload_bytes(bytes);
  }
  ~ScopedMaxPayload() { plinth::realtime::set_max_payload_bytes(prev); }
  ScopedMaxPayload(const ScopedMaxPayload&) = delete;
  auto operator=(const ScopedMaxPayload&) -> ScopedMaxPayload& = delete;
  ScopedMaxPayload(ScopedMaxPayload&&) = delete;
  auto operator=(ScopedMaxPayload&&) -> ScopedMaxPayload& = delete;
};

auto drive(BridgeContext& bc, std::string_view source) -> EvalResult {
  return drogon::sync_wait(run_on_context(bc, source));
}

auto eval_with_extension(RuntimePool& pool, const std::string& ext_name,
                         std::string_view source) -> EvalResult {
  auto* bc = pool.acquire();
  bc->extension_name = ext_name;
  auto r = drive(*bc, source);
  pool.destroy(bc);
  return r;
}

auto make_pool() -> RuntimePool {
  auto cfg = test_config();
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, 1};
}

} // namespace

TEST_CASE("P.01 pubsub.publish happy path — listener receives envelope",
          "[js][realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available");
  }
  ensure_drogon_with_db_running();
  plinth::realtime::clear_handlers_for_test();

  std::mutex mu;
  std::condition_variable cv;
  std::vector<plinth::realtime::DispatchedEvent> received;
  plinth::realtime::register_handler(
      [&mu, &cv, &received](const plinth::realtime::DispatchedEvent& ev) {
        std::lock_guard lock(mu);
        received.push_back(ev);
        cv.notify_all();
      });

  auto cfg = test_config();
  plinth::Config::Realtime::Listener lcfg;
  lcfg.reconnect_backoff_ms = 200;
  plinth::realtime::start_listener(cfg.db, lcfg);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  auto pool = make_pool();
  auto result =
      eval_with_extension(pool, "notes",
                          R"(pubsub.publish("plinth:ext:notes:chat_typing",
                          {user_id: "u1", room_id: "r7"})
             .then(() => "ok", e => "err:" + e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "ok");

  {
    std::unique_lock lock(mu);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(5),
                        [&received] { return !received.empty(); }));
  }
  plinth::realtime::stop_listener();

  REQUIRE(received.size() == 1);
  REQUIRE(received[0].layer == "extension");
  REQUIRE(received[0].channel == "plinth:ext:notes:chat_typing");
  REQUIRE(received[0].envelope["payload"]["user_id"].asString() == "u1");
  REQUIRE(received[0].envelope["payload"]["room_id"].asString() == "r7");
}

TEST_CASE("P.02 pubsub.publish — extension mismatch rejects",
          "[js][realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — fixture requires drogon DbClient");
  }
  ensure_drogon_with_db_running();

  auto pool = make_pool();
  auto result =
      eval_with_extension(pool, "notes",
                          R"(pubsub.publish("plinth:ext:terminal:evil", {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.extension_mismatch");
}

TEST_CASE("P.03 pubsub.publish — Layer-1 shape rejects as invalid channel",
          "[js][realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — fixture requires drogon DbClient");
  }
  ensure_drogon_with_db_running();

  auto pool = make_pool();
  auto result =
      eval_with_extension(pool, "notes",
                          R"(pubsub.publish("plinth:data:ext_notes.notes", {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.channel_invalid");
}

TEST_CASE("P.04 pubsub.publish — oversized envelope rejects",
          "[js][realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — fixture requires drogon DbClient");
  }
  ensure_drogon_with_db_running();

  ScopedMaxPayload guard{200};

  auto pool = make_pool();
  auto result = eval_with_extension(pool, "notes",
                                    R"(pubsub.publish("plinth:ext:notes:chat",
                          {big: "x".repeat(400)})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.payload_too_large");
}

TEST_CASE("P.05 pubsub.publish — no extension context rejects",
          "[js][realtime][integration]") {
  if (!pg_available()) {
    SKIP("PG not available — fixture requires drogon DbClient");
  }
  ensure_drogon_with_db_running();

  auto pool = make_pool();
  // Empty extension_name simulates a kernel-scope BridgeContext.
  auto result =
      eval_with_extension(pool, "",
                          R"(pubsub.publish("plinth:ext:anything:foo", {})
             .then(() => "ok", e => e.code))");

  REQUIRE(result.value.has_value());
  REQUIRE(result.value->asString() == "pubsub.extension_mismatch");
}
