// SPDX-License-Identifier: MIT
//
// `db.*` host bindings — ICD-0.3.3 §Injected `db.*` Surface.
//
// Each binding (db.query / db.exec):
//   1. Validates argument types synchronously. Type errors throw a
//      JS TypeError; the promise is NOT created (the JS caller's
//      try/catch around the call site catches sync misuse, the
//      try/catch around `await` catches async failures).
//   2. Creates a Promise capability via JS_NewPromiseCapability.
//   3. Registers the resolve/reject callable pair on the BridgeContext
//      under a fresh callback_id.
//   4. Builds an AsyncOp with payload + callback_id, pushes onto
//      bc.pending_ops.
//   5. Returns the promise object to JS.
//
// Real PG dispatch happens in run_on_context.cpp's dispatch_async_op
// arm (Step 8). In Step 6 the JS surface is wired up; the dispatch arm
// rejects with `internal/unimplemented async op` until Step 8 lands.

#include "kernel/js/async_op.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_batch_schema_check.hpp"
#include "kernel/js/stdlib_inject.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <json/value.h>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace plinth::js {

namespace {

constexpr int64_t SAFE_INT_MIN = -(int64_t{1} << 53);
constexpr int64_t SAFE_INT_MAX = int64_t{1} << 53;

auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto get_bc(JSContext* ctx) -> BridgeContext* {
  return static_cast<BridgeContext*>(JS_GetContextOpaque(ctx));
}

// Per-type sub-converters split out of convert_param to keep the
// top-level function under clang-tidy's cognitive-complexity ceiling.
// Each returns std::optional — engaged on success, std::nullopt to
// fall through to the next branch, false-via-throw inside on a real
// type error (the ctx already has the TypeError pending).

auto try_convert_bool(JSContext* ctx, JSValueConst v)
    -> std::optional<Json::Value> {
  if (!JS_IsBool(v)) {
    return std::nullopt;
  }
  int b = JS_ToBool(ctx, v);
  if (b < 0) {
    return std::nullopt; // QuickJS already threw — caller propagates
  }
  return Json::Value{static_cast<bool>(b)};
}

auto try_convert_number(JSContext* ctx, JSValueConst v)
    -> std::optional<Json::Value> {
  if (!JS_IsNumber(v)) {
    return std::nullopt;
  }
  if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
    int32_t i = 0;
    if (JS_ToInt32(ctx, &i, v) == 0) {
      return Json::Value{static_cast<Json::Int>(i)};
    }
  }
  double d = 0.0;
  if (JS_ToFloat64(ctx, &d, v) != 0) {
    return std::nullopt; // QuickJS threw
  }
  // d is integral (no fraction lost in the round-trip) AND fits the
  // IEEE 754 safe-integer range → Json::Int64; otherwise Json::Real.
  auto as_int = static_cast<int64_t>(d);
  bool integral = (static_cast<double>(as_int) == d);
  if (integral && d >= static_cast<double>(SAFE_INT_MIN) &&
      d <= static_cast<double>(SAFE_INT_MAX)) {
    return Json::Value{static_cast<Json::Int64>(as_int)};
  }
  return Json::Value{d};
}

auto try_convert_string(JSContext* ctx, JSValueConst v)
    -> std::optional<Json::Value> {
  if (!JS_IsString(v)) {
    return std::nullopt;
  }
  const char* s = JS_ToCString(ctx, v);
  if (s == nullptr) {
    return std::nullopt;
  }
  Json::Value out{std::string{s}};
  JS_FreeCString(ctx, s);
  return out;
}

// Uint8Array → tag-encoded {"__bytea__": "<bytes>"} JSON object so
// the dispatch arm can disambiguate from a JSON string and bind as
// BYTEA. QuickJS doesn't have JS_IsUint8Array; we use
// JS_GetTypedArrayBuffer which succeeds only for typed arrays.
auto try_convert_uint8array(JSContext* ctx, JSValueConst v)
    -> std::optional<Json::Value> {
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per = 0;
  JSValue ab =
      JS_GetTypedArrayBuffer(ctx, v, &byte_offset, &byte_length, &bytes_per);
  if (JS_IsException(ab)) {
    return std::nullopt;
  }
  size_t ab_size = 0;
  const uint8_t* ab_buf = JS_GetArrayBuffer(ctx, &ab_size, ab);
  if (ab_buf == nullptr || bytes_per != 1) {
    JS_FreeValue(ctx, ab);
    return std::nullopt;
  }
  std::string raw;
  raw.reserve(byte_length);
  for (std::size_t k = 0; k < byte_length; ++k) {
    raw.push_back(static_cast<char>(ab_buf[byte_offset + k]));
  }
  JS_FreeValue(ctx, ab);
  Json::Value obj(Json::objectValue);
  obj["__bytea__"] = raw;
  return obj;
}

// Convert a JSValue parameter into a Json::Value per ICD-0.3.3
// §Argument Rules. Throws a JS TypeError on `ctx` (and returns false)
// when no per-type sub-converter matches.
auto convert_param(JSContext* ctx, JSValueConst v, std::size_t idx,
                   Json::Value& out) -> bool {
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    out = Json::Value{Json::nullValue};
    return true;
  }
  if (auto r = try_convert_bool(ctx, v); r.has_value()) {
    out = std::move(*r);
    return true;
  }
  if (auto r = try_convert_number(ctx, v); r.has_value()) {
    out = std::move(*r);
    return true;
  }
  if (auto r = try_convert_string(ctx, v); r.has_value()) {
    out = std::move(*r);
    return true;
  }
  if (auto r = try_convert_uint8array(ctx, v); r.has_value()) {
    out = std::move(*r);
    return true;
  }
  JS_ThrowTypeError(ctx, "db: unsupported parameter type at index %zu", idx);
  return false;
}

// Convert the optional `params` array (JS Array of scalars) into a
// std::vector<Json::Value>. Returns true on success; on failure throws
// a TypeError and returns false.
auto convert_params(JSContext* ctx, JSValueConst params,
                    std::vector<Json::Value>& out) -> bool {
  if (JS_IsUndefined(params) || JS_IsNull(params)) {
    return true;
  }
  if (!JS_IsArray(params)) {
    JS_ThrowTypeError(ctx, "db: params must be an array of scalars");
    return false;
  }
  int64_t len = 0;
  if (JS_GetLength(ctx, params, &len) < 0) {
    return false;
  }
  out.reserve(static_cast<std::size_t>(len));
  for (int64_t i = 0; i < len; ++i) {
    JSValue v = JS_GetPropertyUint32(ctx, params, static_cast<uint32_t>(i));
    if (JS_IsException(v)) {
      JS_FreeValue(ctx, v);
      return false;
    }
    Json::Value slot;
    bool ok = convert_param(ctx, v, static_cast<std::size_t>(i), slot);
    JS_FreeValue(ctx, v);
    if (!ok) {
      return false;
    }
    out.push_back(std::move(slot));
  }
  return true;
}

// Reject the new op's promise inline with the supplied error, used by
// Security Constraint 7 (cancelled-context: never enqueue). Returns
// the promise so JS receives a real rejection.
auto reject_inline(JSContext* ctx, std::string code, std::string msg)
    -> JSValue {
  std::array<JSValue, 2> resolving{};
  JSValue prom = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(prom)) {
    return prom;
  }
  // Build {code, message} and call reject directly.
  JSValue err = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, err, "code",
                    JS_NewStringLen(ctx, code.data(), code.size()));
  JS_SetPropertyStr(ctx, err, "message",
                    JS_NewStringLen(ctx, msg.data(), msg.size()));
  JSValue ret = JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &err);
  JS_FreeValue(ctx, err);
  JS_FreeValue(ctx, ret);
  JS_FreeValue(ctx, resolving[0]);
  JS_FreeValue(ctx, resolving[1]);
  return prom;
}

// ICD-0.5.3 §`db.batch()` — admission gate for the in-batch enqueue
// path of `db.query` and `db.exec`. Runs three synchronous checks
// in priority order: §B.06 timeout > §SC3 cross-extension > §quota.
// Returns nullopt on success; on failure, returns a JS rejection
// promise the caller should pass back to JS (for `db.exec`, the
// caller must also `JS_FreeValue` the promise capability it
// allocated before calling this helper).
auto check_in_batch_admission(JSContext* ctx, BridgeContext* bc,
                              const std::string& sql)
    -> std::optional<JSValue> {
  if (bc->batch_state.timed_out) {
    return reject_inline(ctx, "db.batch.timeout", "batch wall-clock exceeded");
  }
  if (!bc->extension_name.empty() &&
      plinth::js::db::classify_cross_extension(sql, bc->extension_name)) {
    plinth::js::db::audit_batch_cross_extension_rejected(bc->extension_name,
                                                         sql);
    return reject_inline(ctx, "db.batch.cross_extension_not_allowed",
                         "batch op targets a different extension schema");
  }
  if (bc->batch_state.ops_in_batch >= plinth::js::batch_max_ops_per_batch()) {
    return reject_inline(ctx, "db.batch.quota_exceeded",
                         "batch op quota exceeded");
  }
  return std::nullopt;
}

// db.query(sql, params?) -> Promise<{rows, row_count}>
auto db_query(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);

  // Synchronous arg validation per ICD §Argument Rules.
  if (args.empty() || !JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "db.query: sql must be a string");
  }
  const char* sql_cs = JS_ToCString(ctx, args[0]);
  if (sql_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string sql(sql_cs);
  JS_FreeCString(ctx, sql_cs);
  if (sql.empty()) {
    return JS_ThrowTypeError(ctx, "db.query: sql must be non-empty");
  }
  std::vector<Json::Value> params;
  if (args.size() >= 2 && !convert_params(ctx, args[1], params)) {
    return JS_EXCEPTION;
  }

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "db.query: bridge context unavailable");
  }
  // Security Constraint 7: cancelled context → reject inline, do not
  // enqueue.
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "db.cancelled", "execution cancelled");
  }

  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "db");
  AsyncOp op{.type = AsyncOp::Type::DB_QUERY,
             .callback_id = id,
             .sql = std::move(sql),
             .sql_params = std::move(params),
             .silent = false,
             .audit_event_type = {},
             .audit_payload = {}};
  op.bc_extension_name = bc->extension_name;
  // ICD-0.5.3 §`db.batch()` §AsyncOp additions — stamp the scope id
  // when inside a batch so the dispatch arm routes through the
  // pinned connection and the coalescer tags counters accordingly.
  // Admission checks (timeout / cross-extension / quota) live in
  // `check_in_batch_admission` so the function bodies stay flat.
  if (bc->batch_state.depth > 0) {
    if (auto err = check_in_batch_admission(ctx, bc, op.sql)) {
      return *err;
    }
    op.batch_scope_id = bc->batch_state.scope_id;
    ++bc->batch_state.ops_in_batch;
  }
  bc->pending_ops.push_back(std::move(op));
  return promise;
}

// db.exec(sql, params?, opts?) -> Promise<{row_count}>
auto db_exec(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);

  if (args.empty() || !JS_IsString(args[0])) {
    return JS_ThrowTypeError(ctx, "db.exec: sql must be a string");
  }
  const char* sql_cs = JS_ToCString(ctx, args[0]);
  if (sql_cs == nullptr) {
    return JS_EXCEPTION;
  }
  std::string sql(sql_cs);
  JS_FreeCString(ctx, sql_cs);
  if (sql.empty()) {
    return JS_ThrowTypeError(ctx, "db.exec: sql must be non-empty");
  }
  std::vector<Json::Value> params;
  if (args.size() >= 2 && !convert_params(ctx, args[1], params)) {
    return JS_EXCEPTION;
  }
  bool silent = false;
  if (args.size() >= 3 && !JS_IsUndefined(args[2]) && !JS_IsNull(args[2])) {
    if (!JS_IsObject(args[2])) {
      return JS_ThrowTypeError(ctx, "db.exec: opts must be an object");
    }
    JSValue silent_v = JS_GetPropertyStr(ctx, args[2], "silent");
    if (!JS_IsUndefined(silent_v) && !JS_IsNull(silent_v)) {
      int b = JS_ToBool(ctx, silent_v);
      JS_FreeValue(ctx, silent_v);
      if (b < 0) {
        return JS_EXCEPTION;
      }
      silent = (b != 0);
    } else {
      JS_FreeValue(ctx, silent_v);
    }
  }

  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "db.exec: bridge context unavailable");
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "db.cancelled", "execution cancelled");
  }

  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "db");
  AsyncOp op{.type = AsyncOp::Type::DB_EXEC,
             .callback_id = id,
             .sql = std::move(sql),
             .sql_params = std::move(params),
             .silent = silent,
             .audit_event_type = {},
             .audit_payload = {}};
  op.bc_extension_name = bc->extension_name;
  if (bc->batch_state.depth > 0) {
    // Free the promise capability before returning the rejection
    // — register_pending already took ownership of the
    // resolve/reject callbacks for this op (they'll be released
    // at bc teardown), but the outer Promise object itself is
    // ours to drop.
    if (auto err = check_in_batch_admission(ctx, bc, op.sql)) {
      JS_FreeValue(ctx, promise);
      return *err;
    }
    op.batch_scope_id = bc->batch_state.scope_id;
    // Snapshot the pinned connection so the outcome helper can
    // route through it without reading bc from the worker thread.
    op.batch_pinned_conn = bc->batch_state.pinned_conn;
    ++bc->batch_state.ops_in_batch;
  }
  bc->pending_ops.push_back(std::move(op));
  return promise;
}

// ICD-0.5.3 §`db.batch()` Transactional Wrapper.
//
// The three internal helpers below enqueue one AsyncOp each; the
// public `db.batch` binding below ties them together via a JS
// orchestrator installed at stdlib-inject time. The split keeps the
// C++ surface small — each helper is a textbook one-shot async op —
// while the orchestration (sequencing BEGIN → user fn → COMMIT or
// ROLLBACK, rejecting with the user's error) lives in JS where
// promise semantics are ergonomic.

auto parse_scope_id(JSContext* ctx, JSValueConst v) -> std::uint64_t {
  std::int64_t sid = 0;
  if (JS_ToInt64(ctx, &sid, v) < 0 || sid <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(sid);
}

auto enqueue_batch_op(JSContext* ctx, AsyncOp::Type type,
                      std::uint64_t scope_id,
                      std::optional<Json::Value> rollback_err = std::nullopt)
    -> JSValue {
  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "__db_batch__: bc unavailable");
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "db.cancelled", "execution cancelled");
  }
  std::array<JSValue, 2> resolving{};
  JSValue promise = JS_NewPromiseCapability(ctx, resolving.data());
  if (JS_IsException(promise)) {
    return promise;
  }
  int id = bc->register_pending(resolving[0], resolving[1], "db");
  AsyncOp op{.type = type,
             .callback_id = id,
             .sql = {},
             .sql_params = {},
             .silent = false,
             .audit_event_type = {},
             .audit_payload = {}};
  op.bc_extension_name = bc->extension_name;
  op.batch_scope_id = scope_id;
  if (rollback_err.has_value()) {
    op.rollback_error = std::move(*rollback_err);
  }
  bc->pending_ops.push_back(std::move(op));
  return promise;
}

auto db_batch_begin_internal(JSContext* ctx, JSValue /*this_val*/, int argc,
                             JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty()) {
    return JS_ThrowTypeError(ctx, "__db_batch_begin__: scope_id required");
  }
  std::uint64_t sid = parse_scope_id(ctx, args[0]);
  if (sid == 0) {
    return JS_ThrowTypeError(
        ctx, "__db_batch_begin__: scope_id must be a positive integer");
  }
  return enqueue_batch_op(ctx, AsyncOp::Type::DB_BATCH_BEGIN, sid);
}

auto db_batch_commit_internal(JSContext* ctx, JSValue /*this_val*/, int argc,
                              JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty()) {
    return JS_ThrowTypeError(ctx, "__db_batch_commit__: scope_id required");
  }
  std::uint64_t sid = parse_scope_id(ctx, args[0]);
  if (sid == 0) {
    return JS_ThrowTypeError(
        ctx, "__db_batch_commit__: scope_id must be a positive integer");
  }
  // ICD-0.5.3 §B.06 — when the orchestrator reaches COMMIT after
  // a user-fn that did no further awaits past the timer fire,
  // intercept here and reject with `db.batch.timeout` so the
  // orchestrator's catch path runs ROLLBACK instead.
  BridgeContext* bc = get_bc(ctx);
  if (bc != nullptr && bc->batch_state.depth > 0 &&
      bc->batch_state.scope_id == sid && bc->batch_state.timed_out) {
    return reject_inline(ctx, "db.batch.timeout", "batch wall-clock exceeded");
  }
  return enqueue_batch_op(ctx, AsyncOp::Type::DB_BATCH_COMMIT, sid);
}

auto db_batch_rollback_internal(JSContext* ctx, JSValue /*this_val*/, int argc,
                                JSValue* argv) -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty()) {
    return JS_ThrowTypeError(ctx, "__db_batch_rollback__: scope_id required");
  }
  std::uint64_t sid = parse_scope_id(ctx, args[0]);
  if (sid == 0) {
    return JS_ThrowTypeError(
        ctx, "__db_batch_rollback__: scope_id must be a positive integer");
  }
  // argv[1] is the user-fn error (any JSON-compatible value) — used
  // by the dispatch arm to stamp the `db.batch.rolled_back` audit
  // error_code when the value is an object with `.code`. The arm
  // rejects the outer promise with this value via the callback map.
  Json::Value err_val;
  if (args.size() >= 2 && !JS_IsUndefined(args[1]) && !JS_IsNull(args[1])) {
    auto conv = detail::js_to_json(ctx, args[1]);
    if (conv.has_value()) {
      err_val = std::move(*conv);
    }
  }
  return enqueue_batch_op(ctx, AsyncOp::Type::DB_BATCH_ROLLBACK, sid,
                          std::move(err_val));
}

// db.batch(fn) → Promise — the public binding. Pre-flight checks on
// the JS thread; the orchestrator JS function chains the three
// internal helpers around fn().
auto db_batch(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  auto args = make_arg_span(argc, argv);
  if (args.empty() || !JS_IsFunction(ctx, args[0])) {
    return JS_ThrowTypeError(ctx, "db.batch: fn must be a function");
  }
  BridgeContext* bc = get_bc(ctx);
  if (bc == nullptr) {
    return JS_ThrowTypeError(ctx, "db.batch: bridge context unavailable");
  }
  if (bc->cancelled.load(std::memory_order_acquire)) {
    return reject_inline(ctx, "db.cancelled", "execution cancelled");
  }
  // ICD §Binding implementation step 2 — nested-batch rejected
  // synchronously. Conservative for phase 4; future tuning may
  // relax once Promise.all-concurrent-batch semantics are pinned.
  if (bc->batch_state.depth > 0) {
    return reject_inline(ctx, "db.batch.nested_not_allowed",
                         "db.batch cannot nest inside another db.batch");
  }

  // Allocate scope id + mark bc as in-batch BEFORE the orchestrator
  // runs — the first statement the user's fn awaits is a db.exec
  // whose quota check reads bc.batch_state.depth.
  std::uint64_t sid = alloc_batch_scope_id();
  bc->batch_state.depth = 1;
  bc->batch_state.scope_id = sid;
  bc->batch_state.ops_in_batch = 0;

  // Look up the orchestrator injected at stdlib-inject time.
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue orch = JS_GetPropertyStr(ctx, global, "__db_batch_orchestrator__");
  JS_FreeValue(ctx, global);
  if (!JS_IsFunction(ctx, orch)) {
    JS_FreeValue(ctx, orch);
    // Unreachable if stdlib_inject ran; leave bc.batch_state set
    // so the eventual rollback-on-destroy path catches it.
    return JS_ThrowTypeError(ctx, "db.batch: orchestrator not installed");
  }
  std::array<JSValue, 2> orch_args{JS_NewInt64(ctx, static_cast<int64_t>(sid)),
                                   JS_DupValue(ctx, args[0])};
  JSValue result =
      JS_Call(ctx, orch, JS_UNDEFINED, static_cast<int>(orch_args.size()),
              orch_args.data());
  JS_FreeValue(ctx, orch_args[0]);
  JS_FreeValue(ctx, orch_args[1]);
  JS_FreeValue(ctx, orch);
  return result;
}

// Orchestrator JS source — evaluated once per context at register_db.
// Uses promise chaining (avoids async/await to stay compatible with
// the kernel's dynamic-code-disabled posture). See ICD §Binding
// implementation steps 6–8.
constexpr const char* DB_BATCH_ORCHESTRATOR_SRC = R"JS(
(function () {
  globalThis.__db_batch_orchestrator__ = function (scope_id, fn) {
    return __db_batch_begin__(scope_id).then(function () {
      var body;
      try {
        body = fn();
      } catch (e) {
        body = Promise.reject(e);
      }
      return Promise.resolve(body).then(
        function (v) {
          return __db_batch_commit__(scope_id).then(function () {
            return v;
          });
        },
        function (e) {
          return __db_batch_rollback__(scope_id, e).then(
            function () { throw e; },
            function () { throw e; }
          );
        }
      );
    });
  };
})();
)JS";

} // namespace

auto register_db(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "db", "query", &db_query, 2);
  inject_sync_fn(ctx, "db", "exec", &db_exec, 3);
  inject_sync_fn(ctx, "db", "batch", &db_batch, 1);

  // Internal helpers installed as globalThis properties — the
  // orchestrator below calls them by name. Named with `__` guard
  // prefix to discourage extension-author use; not a security
  // boundary (an extension could still grab them from globalThis).
  JSValue global = JS_GetGlobalObject(ctx);
  {
    JSValue fn =
        JS_NewCFunction(ctx, &db_batch_begin_internal, "__db_batch_begin__", 1);
    JS_SetPropertyStr(ctx, global, "__db_batch_begin__", fn);
  }
  {
    JSValue fn = JS_NewCFunction(ctx, &db_batch_commit_internal,
                                 "__db_batch_commit__", 1);
    JS_SetPropertyStr(ctx, global, "__db_batch_commit__", fn);
  }
  {
    JSValue fn = JS_NewCFunction(ctx, &db_batch_rollback_internal,
                                 "__db_batch_rollback__", 2);
    JS_SetPropertyStr(ctx, global, "__db_batch_rollback__", fn);
  }
  JS_FreeValue(ctx, global);

  // Install the orchestrator. JS_Eval is the C-side evaluator —
  // unaffected by the `globalThis.eval` / `globalThis.Function`
  // deletion in disable_dynamic_code_entrypoints.
  JSValue r =
      JS_Eval(ctx, DB_BATCH_ORCHESTRATOR_SRC, strlen(DB_BATCH_ORCHESTRATOR_SRC),
              "<db_batch_orchestrator>", JS_EVAL_TYPE_GLOBAL);
  JS_FreeValue(ctx, r);
}

} // namespace plinth::js
