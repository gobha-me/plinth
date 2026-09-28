// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <json/value.h>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

class OwnedValue {
 public:
  OwnedValue(JSContext* ctx, JSValue value) : context(ctx), value(value) {}
  ~OwnedValue() { JS_FreeValue(context, value); }
  OwnedValue(const OwnedValue&) = delete;
  auto operator=(const OwnedValue&) -> OwnedValue& = delete;
  OwnedValue(OwnedValue&&) = delete;
  auto operator=(OwnedValue&&) -> OwnedValue& = delete;
  auto get() const -> JSValue { return value; }

 private:
  JSContext* context;
  JSValue value;
};

class ContextLease {
 public:
  explicit ContextLease(plinth::js::RuntimePool& pool)
      : pool(pool), context(pool.acquire()) {}
  ~ContextLease() { destroy(); }
  ContextLease(const ContextLease&) = delete;
  auto operator=(const ContextLease&) -> ContextLease& = delete;
  ContextLease(ContextLease&&) = delete;
  auto operator=(ContextLease&&) -> ContextLease& = delete;
  auto get() const -> plinth::js::BridgeContext* { return context; }
  auto release() -> void {
    pool.release(context);
    context = nullptr;
  }
  auto destroy() -> void {
    if (context != nullptr) {
      pool.destroy(context);
      context = nullptr;
    }
  }

 private:
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;
};

auto fake_user() -> plinth::capabilities::UserContext {
  return {.user_id = "fake-user",
          .username = "fake-caller",
          .auth_type = "session",
          .effective_rules = {},
          .session_id = "fake-session",
          .ip_address = "192.0.2.1"};
}

auto evaluate(plinth::js::BridgeContext& bc, std::string_view source)
    -> OwnedValue {
  JS_UpdateStackTop(bc.rt);
  return {bc.ctx, JS_Eval(bc.ctx, source.data(), source.size(),
                          "<audit-authority-regression>", JS_EVAL_TYPE_GLOBAL)};
}

auto string_property(JSContext* ctx, JSValueConst value, const char* name)
    -> std::string {
  OwnedValue property{ctx, JS_GetPropertyStr(ctx, value, name)};
  std::size_t len = 0;
  const char* bytes = JS_ToCStringLen(ctx, &len, property.get());
  std::string result;
  if (bytes != nullptr) {
    result.assign(bytes, len);
    JS_FreeCString(ctx, bytes);
  }
  return result;
}

auto drain_jobs(plinth::js::BridgeContext& bc) -> bool {
  JS_UpdateStackTop(bc.rt);
  for (int i = 0; i < 64 && JS_IsJobPending(bc.rt); ++i) {
    JSContext* job_context = nullptr;
    if (JS_ExecutePendingJob(bc.rt, &job_context) < 0) {
      OwnedValue exception{job_context, JS_GetException(job_context)};
      return false;
    }
  }
  return !JS_IsJobPending(bc.rt);
}

auto require_rejection(plinth::js::BridgeContext& bc, std::string_view source,
                       std::string_view code, std::string_view message)
    -> void {
  auto promise = evaluate(bc, source);
  REQUIRE_FALSE(JS_IsException(promise.get()));
  REQUIRE(JS_PromiseState(bc.ctx, promise.get()) == JS_PROMISE_REJECTED);
  OwnedValue reason{bc.ctx, JS_PromiseResult(bc.ctx, promise.get())};
  REQUIRE(string_property(bc.ctx, reason.get(), "code") == code);
  REQUIRE(string_property(bc.ctx, reason.get(), "message") == message);
  REQUIRE(bc.pending_ops.empty());
  REQUIRE(bc.callbacks.empty());
  REQUIRE(bc.next_callback_id == 0);
  REQUIRE(drain_jobs(bc));
}

auto require_acceptance(plinth::js::BridgeContext& bc, std::string_view source,
                        std::string_view owner) -> void {
  auto promise = evaluate(bc, source);
  REQUIRE_FALSE(JS_IsException(promise.get()));
  REQUIRE(JS_PromiseState(bc.ctx, promise.get()) == JS_PROMISE_PENDING);
  REQUIRE(bc.pending_ops.size() == 1);
  REQUIRE(bc.callbacks.size() == 1);
  const auto& op = bc.pending_ops.front();
  REQUIRE(op.type == plinth::js::AsyncOp::Type::AUDIT_WRITE);
  REQUIRE(op.audit_payload["extension_id"].asString() == owner);
  REQUIRE(op.audit_payload["call_depth"].asInt() == bc.call_depth);
  const int id = op.callback_id;
  bc.pending_ops.clear();
  bc.resolve(id, Json::Value{});
  REQUIRE(JS_PromiseState(bc.ctx, promise.get()) == JS_PROMISE_FULFILLED);
  REQUIRE(bc.callbacks.empty());
  REQUIRE(drain_jobs(bc));
}

} // namespace

TEST_CASE("audit owner is the named executing callee even without Extension",
          "[js][audit][authority-regression]") {
  plinth::Config config{};
  const auto user = fake_user();
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1, &user, "demo");
  ContextLease lease{pool};
  auto* bc = lease.get();
  REQUIRE(bc != nullptr);
  REQUIRE(bc->extension == nullptr);
  REQUIRE(bc->extension_name == "demo");
  constexpr std::array<std::string_view, 5> INVALID_EVENTS = {
      "ext.sibling.event", "ext.demographic.event", "ext.Demo.event",
      "ext.demo", "ext.demoX.event"};
  for (const auto event : INVALID_EVENTS) {
    CAPTURE(event);
    require_rejection(*bc, "audit.log('" + std::string{event} + "', {})",
                      "audit.invalid_prefix",
                      "extension audit events must start with 'ext.demo.'");
  }
  // The delimiter is required; no additional suffix grammar is introduced.
  require_acceptance(*bc, "audit.log('ext.demo.', {})", "demo");
  require_acceptance(*bc, "audit.log('ext.demo.Upper/suffix', {})", "demo");
  lease.destroy();
  REQUIRE(pool.shutdown(2s));
}

TEST_CASE("audit host fallback does not rename anonymous contexts",
          "[js][audit][authority-regression]") {
  plinth::Config config{};
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1);
  ContextLease lease{pool};
  auto* bc = lease.get();
  REQUIRE(bc != nullptr);
  REQUIRE(bc->extension_name.empty());
  require_rejection(*bc, "audit.log('ext.demo.event', {})",
                    "audit.invalid_prefix",
                    "extension audit events must start with 'ext.host.'");
  bc->call_depth = 3;
  require_acceptance(*bc, "audit.log('ext.host.event', {})", "host");
  REQUIRE(bc->extension_name.empty());
  REQUIRE(bc->extension == nullptr);
  lease.destroy();
  REQUIRE(pool.shutdown(2s));
}

TEST_CASE("audit rejects every reserved root key and preserves policy order",
          "[js][audit][authority-regression]") {
  plinth::Config config{};
  config.node_id = "fake-node";
  const auto user = fake_user();
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1, &user, "demo");
  ContextLease lease{pool};
  auto* bc = lease.get();
  REQUIRE(bc != nullptr);
  bc->call_depth = 3;
  constexpr std::array<std::pair<std::string_view, std::string_view>, 7>
      RESERVED = {{{"user_id", "'fake-user'"},
                   {"session_id", "'fake-session'"},
                   {"ip_address", "'192.0.2.1'"},
                   {"extension_id", "'demo'"},
                   {"node_id", "'fake-node'"},
                   {"call_depth", "3"},
                   {"timestamp", "'2026-09-28T00:00:00Z'"}}};
  for (const auto& [key, matching_value] : RESERVED) {
    CAPTURE(key);
    for (const auto value : {matching_value, std::string_view{"null"}}) {
      require_rejection(*bc,
                        "audit.log('ext.demo.event', {" + std::string{key} +
                            ":" + std::string{value} + "})",
                        "audit.reserved_field",
                        "payload contains non-forgeable field: " +
                            std::string{key});
    }
  }
  bc->cancelled.store(true);
  require_rejection(*bc, "audit.log('user.login', {extension_id:'demo'})",
                    "audit.cancelled", "execution cancelled");
  bc->cancelled.store(false);
  require_rejection(*bc, R"(audit.log('user.login\u0000tail', {user_id:'x'}))",
                    "audit.reserved_prefix",
                    "event_type uses kernel-reserved prefix: user.");
  require_rejection(*bc, R"(audit.log('malformed\u0000tail', {user_id:'x'}))",
                    "audit.invalid_prefix",
                    "extension audit events must start with 'ext.<id>.'");
  require_rejection(
      *bc, R"(audit.log('ext.sibling.event\u0000tail', {user_id:'x'}))",
      "audit.invalid_prefix",
      "extension audit event_type must not contain NUL");
  require_rejection(*bc, "audit.log('ext.sibling.event', {user_id:'x'})",
                    "audit.invalid_prefix",
                    "extension audit events must start with 'ext.demo.'");
  lease.destroy();
  REQUIRE(pool.shutdown(2s));
}

TEST_CASE("audit enqueue owns enriched payload and existing caller snapshots",
          "[js][audit][authority-regression]") {
  plinth::Config config{};
  const auto user = fake_user();
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1, &user, "demo");
  ContextLease lease{pool};
  auto* bc = lease.get();
  REQUIRE(bc != nullptr);
  bc->call_depth = 4;
  {
    auto promise = evaluate(*bc, R"(
      globalThis.input = {marker: 1, nested: {user_id: 'application-detail'}};
      globalThis.done = false;
      globalThis.auditPromise = audit.log('ext.demo.saved', input);
      auditPromise.then(() => { globalThis.done = true; });
      auditPromise
    )");
    REQUIRE_FALSE(JS_IsException(promise.get()));
    REQUIRE(bc->pending_ops.size() == 1);
    REQUIRE(bc->callbacks.size() == 1);
    Json::Value expected(Json::objectValue);
    expected["marker"] = 1;
    expected["nested"]["user_id"] = "application-detail";
    expected["extension_id"] = "demo";
    expected["call_depth"] = 4;
    bc->extension_name = "sibling";
    bc->call_depth = 7;
    bc->user.user_id = "other-user";
    bc->user.session_id = "other-session";
    bc->user.ip_address = "192.0.2.2";
    auto mutation =
        evaluate(*bc, "input.marker = 99; input.nested.user_id = 'other'");
    REQUIRE_FALSE(JS_IsException(mutation.get()));
    const auto& op = bc->pending_ops.front();
    REQUIRE(op.type == plinth::js::AsyncOp::Type::AUDIT_WRITE);
    REQUIRE(op.audit_event_type == "ext.demo.saved");
    REQUIRE(op.audit_payload == expected);
    REQUIRE(op.audit_user_id == user.user_id);
    REQUIRE(op.audit_session_id == user.session_id);
    REQUIRE(op.audit_ip_address == user.ip_address);
    const int id = op.callback_id;
    bc->pending_ops.clear();
    bc->resolve(id, Json::Value{});
    REQUIRE(bc->callbacks.empty());
    REQUIRE(JS_PromiseState(bc->ctx, promise.get()) == JS_PROMISE_FULFILLED);
    REQUIRE(drain_jobs(*bc));
    auto done = evaluate(*bc, "done");
    REQUIRE(JS_ToBool(bc->ctx, done.get()) == 1);
  }
  lease.destroy();
  REQUIRE(pool.shutdown(2s));
}

TEST_CASE("audit settlements cleanly support pool reset and bounded shutdown",
          "[js][audit][authority-regression]") {
  plinth::Config config{};
  plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                               config, 1, nullptr, "demo");
  {
    ContextLease lease{pool};
    auto* bc = lease.get();
    REQUIRE(bc != nullptr);
    require_acceptance(*bc, "audit.log('ext.demo.resolved', {})", "demo");
    {
      auto promise = evaluate(*bc, R"(
        globalThis.caught = '';
        globalThis.auditPromise = audit.log('ext.demo.rejected', {});
        auditPromise.catch(e => { globalThis.caught = e.code; });
        auditPromise
      )");
      REQUIRE_FALSE(JS_IsException(promise.get()));
      REQUIRE(bc->pending_ops.size() == 1);
      const int id = bc->pending_ops.front().callback_id;
      bc->pending_ops.clear();
      bc->reject(id, {.code = "audit.test_rejection",
                      .message = "fake outcome",
                      .sqlstate = std::nullopt});
      REQUIRE(JS_PromiseState(bc->ctx, promise.get()) == JS_PROMISE_REJECTED);
      REQUIRE(bc->callbacks.empty());
      REQUIRE(drain_jobs(*bc));
      auto caught = evaluate(*bc, "({code: caught})");
      REQUIRE(string_property(bc->ctx, caught.get(), "code") ==
              "audit.test_rejection");
    }
    REQUIRE(bc->pending_ops.empty());
    REQUIRE_FALSE(JS_IsJobPending(bc->rt));
    lease.release();
  }
  REQUIRE(pool.active_count() == 0);
  REQUIRE(pool.free_count() == 1);
  pool.rebuild();
  ContextLease lease{pool};
  auto* bc = lease.get();
  REQUIRE(bc != nullptr);
  REQUIRE(bc->extension_name == "demo");
  REQUIRE(bc->call_depth == 0);
  REQUIRE(bc->callbacks.empty());
  REQUIRE(bc->pending_ops.empty());
  REQUIRE_FALSE(JS_IsJobPending(bc->rt));
  require_acceptance(*bc, "audit.log('ext.demo.after_rebuild', {})", "demo");
  REQUIRE_FALSE(pool.shutdown(0ms));
  lease.destroy();
  REQUIRE(pool.shutdown(2s));
  REQUIRE(pool.active_count() == 0);
}
