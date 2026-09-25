// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/async_op.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/js/stdlib_inject.hpp"

#include <chrono>
#include <quickjs.h>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace {

auto string_property(JSContext* ctx, JSValueConst value, const char* name)
    -> std::string {
  JSValue property = JS_GetPropertyStr(ctx, value, name);
  std::size_t len = 0;
  const char* bytes = JS_ToCStringLen(ctx, &len, property);
  std::string result;
  if (bytes != nullptr) {
    result.assign(bytes, len);
    JS_FreeCString(ctx, bytes);
  }
  JS_FreeValue(ctx, property);
  return result;
}

auto evaluate(JSContext* ctx, std::string_view source) -> bool {
  JSValue value = JS_Eval(ctx, source.data(), source.size(),
                          "<host-string-boundary>", JS_EVAL_TYPE_GLOBAL);
  const bool succeeded = !JS_IsException(value);
  JS_FreeValue(ctx, value);
  if (!succeeded) {
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
  }
  return succeeded;
}

auto evaluate_nul_type_error(JSContext* ctx, std::string_view source) -> bool {
  JSValue value = JS_Eval(ctx, source.data(), source.size(),
                          "<host-string-boundary>", JS_EVAL_TYPE_GLOBAL);
  const bool failed = JS_IsException(value);
  JS_FreeValue(ctx, value);
  if (!failed) {
    return false;
  }
  JSValue exception = JS_GetException(ctx);
  const bool expected =
      string_property(ctx, exception, "name") == "TypeError" &&
      string_property(ctx, exception, "message").find("NUL") !=
          std::string::npos;
  JS_FreeValue(ctx, exception);
  return expected;
}

auto rejection_field(JSContext* ctx, std::string_view source, const char* field)
    -> std::string {
  JSValue promise = JS_Eval(ctx, source.data(), source.size(),
                            "<host-string-boundary>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(promise)) {
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
    return {};
  }
  if (JS_PromiseState(ctx, promise) != JS_PROMISE_REJECTED) {
    JS_FreeValue(ctx, promise);
    return {};
  }
  JSValue reason = JS_PromiseResult(ctx, promise);
  std::string result = string_property(ctx, reason, field);
  JS_FreeValue(ctx, reason);
  JS_FreeValue(ctx, promise);
  return result;
}

auto make_pool() -> plinth::js::RuntimePool {
  plinth::Config config{};
  return {nullptr, plinth::js::default_runtime_limits(), config, 1};
}

struct BridgeLease {
  plinth::js::RuntimePool& pool;
  plinth::js::BridgeContext* context;

  ~BridgeLease() { release(); }

  auto release() -> void {
    if (context != nullptr) {
      pool.destroy(context);
      context = nullptr;
    }
  }
};

} // namespace

TEST_CASE("host strings retain complete bytes or reject before enqueue",
          "[js][stdlib][host-string-boundary]") {
  auto pool = make_pool();
  BridgeLease lease{pool, pool.acquire()};
  auto* bc = lease.context;
  REQUIRE(bc != nullptr);
  bc->extension_name = "demo";

  SECTION("capability signatures retain embedded NUL") {
    REQUIRE(evaluate(bc->ctx, R"(cap.call('kernel:1:log\u0000more', []))"));
    REQUIRE(bc->pending_ops.size() == 1);
    REQUIRE(bc->pending_ops.front().type ==
            plinth::js::AsyncOp::Type::CAP_CALL);
    REQUIRE(bc->pending_ops.front().cap_signature ==
            std::string{"kernel:1:log\0more", 17});
  }

  SECTION("audit event identifiers reject embedded NUL") {
    REQUIRE(evaluate(bc->ctx, "audit.log('ext.demo.event', {})"));
    REQUIRE(bc->pending_ops.size() == 1);
    REQUIRE(rejection_field(bc->ctx,
                            R"(audit.log('ext.demo.event\u0000more', {}))",
                            "code") == "audit.invalid_prefix");
    REQUIRE(rejection_field(bc->ctx, "audit.log('malformed', {})", "code") ==
            "audit.invalid_prefix");
    REQUIRE(rejection_field(bc->ctx, "audit.log('malformed', {})", "message") ==
            "extension audit events must start with 'ext.<id>.'");
    REQUIRE(bc->pending_ops.size() == 1);
  }

  SECTION("SQL and TEXT parameters reject embedded NUL") {
    REQUIRE(
        evaluate_nul_type_error(bc->ctx, R"(db.query('SELECT 1\u0000more'))"));
    REQUIRE(
        evaluate_nul_type_error(bc->ctx, R"(db.exec('SELECT 1\u0000more'))"));
    REQUIRE(evaluate_nul_type_error(bc->ctx,
                                    R"(db.query('SELECT $1', ['a\u0000b']))"));
    REQUIRE(evaluate_nul_type_error(bc->ctx,
                                    R"(db.exec('SELECT $1', ['a\u0000b']))"));
    REQUIRE(bc->pending_ops.empty());
  }

  SECTION("publish and subscribe validate complete channels") {
    REQUIRE(rejection_field(
                bc->ctx,
                R"(pubsub.publish('plinth:ext:demo:event\u0000more', {}))",
                "code") == "pubsub.channel_invalid");
    REQUIRE(
        rejection_field(
            bc->ctx,
            R"(pubsub.subscribe('plinth:ext:demo:event\u0000more', () => {}))",
            "code") == "pubsub.channel_invalid");
    REQUIRE(bc->pending_ops.empty());
    REQUIRE(bc->persistent_callbacks.empty());
  }

  SECTION("unsubscribe closure retains captured channel bytes") {
    const std::string channel{"plinth:ext:demo:event\0more", 26};
    JSValue unsubscribe =
        plinth::js::make_unsubscribe_function(bc->ctx, channel);
    REQUIRE_FALSE(JS_IsException(unsubscribe));
    JSValue result = JS_Call(bc->ctx, unsubscribe, JS_UNDEFINED, 0, nullptr);
    const bool succeeded = !JS_IsException(result);
    JS_FreeValue(bc->ctx, result);
    JS_FreeValue(bc->ctx, unsubscribe);
    REQUIRE(succeeded);
    REQUIRE(bc->pending_ops.size() == 1);
    REQUIRE(bc->pending_ops.front().type ==
            plinth::js::AsyncOp::Type::PUBSUB_UNSUBSCRIBE);
    REQUIRE(bc->pending_ops.front().pubsub_channel == channel);
  }

  lease.release();
  REQUIRE(pool.shutdown(2s));
}
