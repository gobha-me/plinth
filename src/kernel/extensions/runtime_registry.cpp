// SPDX-License-Identifier: MIT
//
// plinth::extensions::RuntimeRegistry — per-extension RuntimePool
// ownership plus the Tier 2 extension capability dispatch entry point.
// See ICD-0.5.0.3-extension-dispatch §RuntimeRegistry.

#include "kernel/extensions/runtime_registry.hpp"

#include "kernel/js/async_op.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <quickjs.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace plinth::extensions {

namespace {

namespace fs = std::filesystem;

// ── Error-message cap (ICD §Error message sanitization) ──────────────
//
// Handler-supplied messages (JS exception text, JS Error.message) are
// capped before leaving the dispatcher. 1024 bytes is the ICD-pinned
// ceiling — enough for human-readable diagnostics, small enough that a
// handler deliberately blowing up the audit/log pipeline cannot cause
// unbounded growth downstream.
constexpr std::size_t HANDLER_MSG_MAX = 1024;
constexpr std::size_t PREFERENCE_VALUE_MAX = 64ULL * 1024ULL;
constexpr std::size_t PREFERENCE_AUDIT_WINDOWS_MAX = 4096;
constexpr auto PREFERENCE_AUDIT_WINDOW = std::chrono::seconds(60);

auto clamp_message(std::string s) -> std::string {
  if (s.size() > HANDLER_MSG_MAX) {
    s.resize(HANDLER_MSG_MAX);
  }
  return s;
}

auto compact_json(const Json::Value& value) -> std::string {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

auto preference_value_class(const Json::Value& value) -> std::string_view {
  switch (value.type()) {
    case Json::nullValue: return "null";
    case Json::intValue:
    case Json::uintValue:
    case Json::realValue: return "number";
    case Json::stringValue: return "string";
    case Json::booleanValue: return "boolean";
    case Json::arrayValue: return "array";
    case Json::objectValue: return "object";
  }
  return "unknown";
}

auto validate_shell_preference_call(std::string_view extension_name,
                                    std::string_view function,
                                    const Json::Value& args)
    -> std::optional<plinth::js::PromiseRejection> {
  using plinth::js::PromiseRejection;
  if (extension_name != "shell" ||
      (function != "preferences.get" && function != "preferences.set" &&
       function != "preferences.get_all")) {
    return std::nullopt;
  }
  if (function == "preferences.get_all") {
    return std::nullopt;
  }
  if (!args.isObject() || !args.isMember("key") || !args["key"].isString()) {
    return PromiseRejection{
        .code = "invalid_argument",
        .message = "key must be a 1..255 byte string",
        .sqlstate = std::nullopt,
    };
  }
  auto key = args["key"].asString();
  if (key.empty() || key.size() > 255) {
    return PromiseRejection{
        .code = "invalid_argument",
        .message = "key must be a 1..255 byte string",
        .sqlstate = std::nullopt,
    };
  }
  if (function == "preferences.set" && args.isMember("value")) {
    const auto& value = args["value"];
    if (key == "shell.theme" &&
        (!value.isString() ||
         (value.asString() != "light" && value.asString() != "dark" &&
          value.asString() != "system"))) {
      return PromiseRejection{
          .code = "invalid_argument",
          .message = "value not valid for key shell.theme",
          .sqlstate = std::nullopt,
      };
    }
    if (key == "shell.scale_pct" &&
        (!value.isIntegral() || value.asInt64() < 80 ||
         value.asInt64() > 175)) {
      return PromiseRejection{
          .code = "invalid_argument",
          .message = "value not valid for key shell.scale_pct",
          .sqlstate = std::nullopt,
      };
    }
  }
  if (function == "preferences.set" && args.isMember("value") &&
      compact_json(args["value"]).size() > PREFERENCE_VALUE_MAX) {
    return PromiseRejection{
        .code = "payload_too_large",
        .message = "value exceeds 64 KiB serialized limit",
        .sqlstate = std::nullopt,
    };
  }
  return std::nullopt;
}

std::mutex preference_audit_mutex;
std::unordered_map<std::string, std::chrono::steady_clock::time_point>
    preference_audit_windows;

auto claim_preference_audit_slot(std::string_view user_id, std::string_view key)
    -> bool {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(preference_audit_mutex);
  std::erase_if(preference_audit_windows, [now](const auto& item) {
    return now - item.second >= PREFERENCE_AUDIT_WINDOW;
  });

  std::string dedup_key;
  dedup_key.reserve(user_id.size() + 1 + key.size());
  dedup_key.append(user_id);
  dedup_key.push_back('\x1f');
  dedup_key.append(key);
  if (preference_audit_windows.contains(dedup_key)) {
    return false;
  }
  if (preference_audit_windows.size() >= PREFERENCE_AUDIT_WINDOWS_MAX) {
    auto oldest =
        std::ranges::min_element(preference_audit_windows, {},
                                 [](const auto& item) { return item.second; });
    preference_audit_windows.erase(oldest);
  }
  preference_audit_windows.emplace(std::move(dedup_key), now);
  return true;
}

auto emit_preference_set_audit(std::string_view extension_name,
                               std::string_view function,
                               const Json::Value& args,
                               const plinth::capabilities::UserContext& caller)
    -> void {
  if (extension_name != "shell" || function != "preferences.set" ||
      !args.isObject() || !args.isMember("key") || !args["key"].isString()) {
    return;
  }
  const auto key = args["key"].asString();
  if (!claim_preference_audit_slot(caller.user_id, key)) {
    return;
  }

  Json::Value detail(Json::objectValue);
  detail["key"] = key;
  detail["deduped"] = false;
  if (!args.isMember("value")) {
    detail["value_class"] = "deleted";
    detail["value_size"] = Json::UInt64{0};
  } else {
    detail["value_class"] = std::string{preference_value_class(args["value"])};
    detail["value_size"] =
        static_cast<Json::UInt64>(compact_json(args["value"]).size());
  }
  try {
    plinth::log::audit("shell.preferences.set", detail,
                       plinth::log::AuditCtx{
                           .user_id = caller.user_id,
                           .session_id = caller.session_id,
                           .ip_address = caller.ip_address,
                       });
  } catch (...) {
    // Auditing is fire-and-forget and must not turn a committed preference
    // mutation into a failed capability response.
  }
}

// ── Module-local state ──────────────────────────────────────────────

// shared_mutex protecting the registry map; lock() mutates.
std::shared_mutex registry_mutex;
// process-lifetime pool registry; populated by create_pool / listener /
// bootstrap.
std::unordered_map<std::string, std::shared_ptr<plinth::js::RuntimePool>> pools;
// Pointer — the Config owned by main is longer-lived than this registry.
// Null outside init/shutdown; readers check before use.
// process-lifetime config pointer; set once in init_registry.
const Config* cfg_ptr = nullptr;
bool accepting_dispatches = false;
std::mutex drain_mutex;
std::condition_variable drain_cv;
std::size_t inflight_dispatches = 0;

class DispatchLease {
 public:
  explicit DispatchLease(std::shared_ptr<plinth::js::RuntimePool> pool_in)
      : pool(std::move(pool_in)) {
    std::lock_guard lock(drain_mutex);
    ++inflight_dispatches;
  }
  DispatchLease(const DispatchLease&) = delete;
  DispatchLease(DispatchLease&&) = delete;
  auto operator=(const DispatchLease&) -> DispatchLease& = delete;
  auto operator=(DispatchLease&&) -> DispatchLease& = delete;
  ~DispatchLease() {
    pool.reset();
    {
      std::lock_guard lock(drain_mutex);
      --inflight_dispatches;
    }
    drain_cv.notify_all();
  }

  std::shared_ptr<plinth::js::RuntimePool> pool;
};

// ── JSON → JSValue converter ─────────────────────────────────────────
//
// Mirrors the anonymous-namespace helper in bridge_context.cpp (and the
// third copy inside eval.cpp noted at conversion.hpp:7). Kept local to
// avoid leaking runtime-registry internals into a shared TU; the
// "unify all three" cleanup in conversion.hpp's note will eventually
// pull all four into one shared helper.
auto json_to_js(JSContext* ctx, const Json::Value& v) -> JSValue {
  switch (v.type()) {
    case Json::nullValue: return JS_NULL;
    case Json::booleanValue: return JS_NewBool(ctx, v.asBool());
    case Json::intValue: return JS_NewInt64(ctx, v.asInt64());
    case Json::uintValue:
      return JS_NewInt64(ctx, static_cast<int64_t>(v.asUInt64()));
    case Json::realValue: return JS_NewFloat64(ctx, v.asDouble());
    case Json::stringValue: {
      auto s = v.asString();
      return JS_NewStringLen(ctx, s.data(), s.size());
    }
    case Json::arrayValue: {
      JSValue arr = JS_NewArray(ctx);
      for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, i, json_to_js(ctx, v[i]));
      }
      return arr;
    }
    case Json::objectValue: {
      JSValue obj = JS_NewObject(ctx);
      for (const auto& key : v.getMemberNames()) {
        JS_SetPropertyStr(ctx, obj, key.c_str(), json_to_js(ctx, v[key]));
      }
      return obj;
    }
  }
  return JS_UNDEFINED;
}

// ── import_from_src intrinsic ────────────────────────────────────────
//
// Compile `source` as an ES module, execute the module body, and return
// the module namespace object. The wrapper source (built by
// `invoke_handler`) calls this to load the handler module, then
// reaches `.default` on the returned namespace.
//
// Top-level await is NOT supported in 0.5.0.4 — if `JS_EvalFunction`
// returns a pending promise we throw an internal error. Handlers that
// need async initialization should do it inside their default export,
// where `run_on_context`'s job drain handles awaits correctly.
//
// Small RAII guard over a JS_ToCString result — ensures every early
// return path frees the const char* back to the QuickJS allocator.
class CStrGuard {
 public:
  CStrGuard(JSContext* ctx_in, const char* s_in) noexcept
      : ctx(ctx_in), s(s_in) {}
  CStrGuard(const CStrGuard&) = delete;
  CStrGuard(CStrGuard&&) = delete;
  auto operator=(const CStrGuard&) -> CStrGuard& = delete;
  auto operator=(CStrGuard&&) -> CStrGuard& = delete;
  ~CStrGuard() {
    if (s != nullptr) {
      JS_FreeCString(ctx, s);
    }
  }

 private:
  JSContext* ctx;
  const char* s;
};

// bookkeeping across early returns is structurally linear and documented;
// splitting would hide invariants.
extern "C" auto import_from_src_intrinsic(JSContext* ctx,
                                          JSValueConst /*this_val*/, int argc,
                                          JSValueConst* argv) -> JSValue {
  if (argc < 2) {
    // API; format string is a literal.
    return JS_ThrowTypeError(ctx, "import_from_src: expected (src, id) args");
  }
  std::size_t src_len = 0;
  // QuickJS-owned C array whose bounds are fixed by the argc check above.
  const char* src_ptr = JS_ToCStringLen(ctx, &src_len, argv[0]);
  if (src_ptr == nullptr) {
    return JS_EXCEPTION;
  }
  // above; argv[1] is bounded by the same argc check.
  const char* id_ptr = JS_ToCString(ctx, argv[1]);
  if (id_ptr == nullptr) {
    JS_FreeCString(ctx, src_ptr);
    return JS_EXCEPTION;
  }

  JSValue compiled = JS_Eval(ctx, src_ptr, src_len, id_ptr,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  JS_FreeCString(ctx, src_ptr);
  // id_ptr freed at function exit so every return path is covered.
  CStrGuard id_guard(ctx, id_ptr);

  if (JS_IsException(compiled)) {
    // Compile failed; propagate the SyntaxError / internal error up.
    return JS_EXCEPTION;
  }
  if (JS_VALUE_GET_TAG(compiled) != JS_TAG_MODULE) {
    JS_FreeValue(ctx, compiled);
    // API; format string is a literal.
    return JS_ThrowInternalError(
        ctx, "import_from_src: compile-only returned non-module tag");
  }

  // The JSValue wraps the JSModuleDef pointer — snapshot it before
  // JS_EvalFunction transfers ownership of the JSValue.
  auto* mdef = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));

  JSValue eval_result = JS_EvalFunction(ctx, compiled);
  if (JS_IsException(eval_result)) {
    return JS_EXCEPTION;
  }

  // For sync modules, JS_EvalFunction returns undefined. For async
  // (top-level-await) modules, it returns a promise — not supported
  // in 0.5.0.4 per ICD §Handler contract.
  JSPromiseStateEnum pstate = JS_PromiseState(ctx, eval_result);
  if (pstate != static_cast<JSPromiseStateEnum>(-1)) {
    if (pstate == JS_PROMISE_PENDING) {
      JS_FreeValue(ctx, eval_result);
      // API; format string is a literal.
      return JS_ThrowInternalError(
          ctx, "import_from_src: handler module uses top-level await "
               "(not supported)");
    }
    if (pstate == JS_PROMISE_REJECTED) {
      JSValue reason = JS_PromiseResult(ctx, eval_result);
      JS_FreeValue(ctx, eval_result);
      return JS_Throw(ctx, reason);
    }
    // JS_PROMISE_FULFILLED — fall through; the resolved value is
    // undefined by spec for module eval, so nothing to keep.
  }
  JS_FreeValue(ctx, eval_result);

  return JS_GetModuleNamespace(ctx, mdef);
}

// Bind `import_from_src` onto globalThis on `ctx`. Called once per
// dispatch after acquire — the RuntimePool's state-reset in
// release() wipes globalThis own props, so the binding is local to a
// single handler invocation and never leaks into an unrelated acquire.
auto install_import_from_src(JSContext* ctx) -> void {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn =
      JS_NewCFunction(ctx, &import_from_src_intrinsic, "import_from_src", 2);
  JS_SetPropertyStr(ctx, global, "import_from_src", fn);
  JS_FreeValue(ctx, global);
}

// ── Handler path + fs read ───────────────────────────────────────────

auto handler_source_path(std::string_view data_dir,
                         std::string_view extension_name,
                         std::string_view function) -> fs::path {
  fs::path root{data_dir};
  root /= "extensions";
  root /= std::string{extension_name};
  root /= "active";
  root /= "server";
  root /= "handlers";
  root /= std::string{function} + ".js";
  return root;
}

// Read the handler source from disk. Returns nullopt when the file is
// missing; returns the content string otherwise. IO errors other than
// "not found" (e.g. permission denied, read failure) surface as an
// empty string and a spdlog::warn — mapped by the caller to
// `cap.handler_load_failed`.
auto read_handler_file(const fs::path& path, bool& missing, bool& io_error)
    -> std::string {
  missing = false;
  io_error = false;
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    missing = true;
    return {};
  }
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    io_error = true;
    plinth::log::warn("extension dispatch: handler fs read failed (open): {}",
                      path.string());
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) {
    io_error = true;
    plinth::log::warn(
        "extension dispatch: handler fs read failed (stream bad): {}",
        path.string());
    return {};
  }
  return ss.str();
}

// ── Wrapper source ──────────────────────────────────────────────────
//
// The fixed wrapper the ICD pins. `__handler_src`, `__handler_args`,
// and `__handler_ctx` are injected onto globalThis before eval;
// `__handler_entered` is used post-mortem to classify load-failed
// vs threw (ICD §Handler invocation step 5 bullet "Distinguishing
// during import vs after default-export invocation"). The handler's
// default export is invoked with two positionals — `(args, ctx)` —
// to match the shell handler signature contract; ctx carries an
// audit-frame projection of the caller's identity (see
// invoke_handler below for the field list and exclusions).
//
// The `JSON.stringify` fallback on non-serializable returns is
// already covered by the JSON round-trip: any return that `js_to_json`
// cannot serialize (functions, Symbols, cycles) surfaces as a
// conversion-time EvalError inside `run_on_context`, which maps to
// `cap.handler_threw` per the classification rules below.
constexpr auto WRAPPER_SRC = R"(
(async () => {
  globalThis.__handler_entered = false;
  const __mod = await globalThis.import_from_src(
    globalThis.__handler_src,
    globalThis.__handler_module_id);
  globalThis.__handler_entered = true;
  return await __mod.default(globalThis.__handler_args,
                             globalThis.__handler_ctx);
})()
)";

auto read_handler_entered_flag(JSContext* ctx) -> bool {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue v = JS_GetPropertyStr(ctx, global, "__handler_entered");
  bool entered = false;
  if (JS_IsBool(v)) {
    entered = JS_ToBool(ctx, v) != 0;
  }
  JS_FreeValue(ctx, v);
  JS_FreeValue(ctx, global);
  return entered;
}

// ── Audit for handler errors (ICD §Audit) ────────────────────────────

auto emit_extension_error_audit(std::string_view extension_name,
                                std::string_view function,
                                std::string_view code, std::string_view message,
                                const plinth::capabilities::UserContext& caller)
    -> void {
  Json::Value detail(Json::objectValue);
  detail["extension"] = std::string{extension_name};
  detail["function"] = std::string{function};
  detail["code"] = std::string{code};
  detail["message"] = std::string{message};
  if (!caller.user_id.empty()) {
    detail["caller_user_id"] = caller.user_id;
  }
  // `plinth::log::audit` short-circuits when the audit subsystem
  // reports not-ready, but its internal `drogon::app().getDbClient()`
  // can throw in test subprocesses that called `log::init` without
  // attaching a DbClient. Audit is fire-and-forget per the
  // feedback_deterministic_teardown.md + logging.cpp contract — a
  // missing DbClient is a harness condition, not a dispatch failure,
  // so the catch-all swallows it silently.
  try {
    plinth::log::audit("capability.extension.error", detail,
                       plinth::log::AuditCtx{
                           .user_id = caller.user_id,
                           .session_id = caller.session_id,
                           .ip_address = caller.ip_address,
                       });
  } catch (...) {
    // Intentional no-op; see comment above.
  }
}

// ── invoke_handler ──────────────────────────────────────────────────
//
// Drives the handler-invocation pipeline per ICD §Handler invocation:
//   1. Resolve + exist-check the handler path.
//   2. Read the source; IO errors → `cap.handler_load_failed`.
//   3. Install the `import_from_src` intrinsic + inject `__args`,
//      `__handler_src`, `__handler_module_id`, `__handler_entered`.
//   4. `co_await run_on_context(...)` on the pinned wrapper source.
//   5. Classify outcome into a PromiseRejection per ICD §Error Mapping.
//
// The `bc`'s identity fields (`user`, `call_depth`, `extension_name`)
// are set by the caller BEFORE calling this function — the registry
// owns the caller-identity propagation (ICD Security Constraint 1, 4).
//
// outlives this coroutine (owned by the pool + the dispatch frame above), args
// outlives via the caller's coroutine frame. Copying either would double every
// JSON hop and defeat the pool contract.
auto invoke_handler(plinth::js::BridgeContext& bc,
                    std::string_view extension_name, std::string_view function,
                    const Json::Value& args, std::string_view data_dir)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>> {

  using plinth::js::PromiseRejection;

  // Step 1/2 — resolve + read.
  auto path = handler_source_path(data_dir, extension_name, function);
  bool missing = false;
  bool io_error = false;
  std::string src = read_handler_file(path, missing, io_error);
  std::string ident;
  ident.reserve(extension_name.size() + 1 + function.size());
  ident.append(extension_name);
  ident.push_back(':');
  ident.append(function);

  if (missing) {
    // ICD: do NOT leak the fs path into the rejection message.
    co_return std::unexpected(PromiseRejection{
        .code = "cap.handler_not_found",
        .message = ident,
        .sqlstate = std::nullopt,
    });
  }
  if (io_error) {
    co_return std::unexpected(PromiseRejection{
        .code = "cap.handler_load_failed",
        .message = ident + ": handler read failed",
        .sqlstate = std::nullopt,
    });
  }

  // Step 3 — install the import_from_src intrinsic + set the wrapper
  // globals. These are per-dispatch — release() clears globalThis on
  // return to pool; acquire() starts from a fresh surface.
  install_import_from_src(bc.ctx);

  JSValue global = JS_GetGlobalObject(bc.ctx);
  JS_SetPropertyStr(bc.ctx, global, "__handler_src",
                    JS_NewStringLen(bc.ctx, src.data(), src.size()));
  std::string module_id;
  module_id.reserve(extension_name.size() + 1 + function.size());
  module_id.append(extension_name);
  module_id.push_back('/');
  module_id.append(function);
  JS_SetPropertyStr(
      bc.ctx, global, "__handler_module_id",
      JS_NewStringLen(bc.ctx, module_id.data(), module_id.size()));
  JS_SetPropertyStr(bc.ctx, global, "__handler_args", json_to_js(bc.ctx, args));
  // Audit-frame projection of BridgeContext made available to the
  // handler as the second positional. UserContext slice mirrors the
  // AuditCtx fields (line 348) the kernel already considers safe to
  // surface; effective_rules is excluded — handlers must not
  // self-introspect RBAC, the resolver step 3 owns the gate.
  Json::Value handler_ctx(Json::objectValue);
  Json::Value user_obj(Json::objectValue);
  user_obj["id"] = bc.user.user_id;
  user_obj["username"] = bc.user.username;
  user_obj["auth_type"] = bc.user.auth_type;
  handler_ctx["user"] = user_obj;
  handler_ctx["session_id"] = bc.user.session_id;
  handler_ctx["ip_address"] = bc.user.ip_address;
  handler_ctx["call_depth"] = bc.call_depth;
  handler_ctx["extension"] = std::string{extension_name};
  JS_SetPropertyStr(bc.ctx, global, "__handler_ctx",
                    json_to_js(bc.ctx, handler_ctx));
  JS_SetPropertyStr(bc.ctx, global, "__handler_entered", JS_FALSE);
  JS_FreeValue(bc.ctx, global);

  // Step 4 — eval. run_on_context drives the async-bridge loop; the
  // top-level promise (returned by the async IIFE) is settled inside.
  auto eval_outcome = co_await plinth::js::run_on_context(bc, WRAPPER_SRC);

  // Step 5 — classify outcome.
  if (eval_outcome.value.has_value()) {
    co_return std::move(*eval_outcome.value);
  }

  const auto& err = eval_outcome.value.error();

  // CANCELLED has its own taxonomy slot — caller-initiated, not a
  // handler failure; no audit.
  if (err.kind == plinth::js::EvalErrorKind::CANCELLED) {
    co_return std::unexpected(PromiseRejection{
        .code = "cap.cancelled",
        .message = ident + ": cancelled",
        .sqlstate = std::nullopt,
    });
  }

  bool entered = read_handler_entered_flag(bc.ctx);
  const char* code = entered ? "cap.handler_threw" : "cap.handler_load_failed";

  // Kind-prefix the message so operators can distinguish memory/
  // wall-clock/etc. without a new taxonomy axis (ICD §Error Mapping
  // "folded into one cap.handler_threw variant").
  std::string_view kind_tag = "error";
  switch (err.kind) {
    case plinth::js::EvalErrorKind::SYNTAX_ERROR:
      kind_tag = "syntax_error";
      break;
    case plinth::js::EvalErrorKind::RUNTIME_ERROR:
      kind_tag = "runtime_error";
      break;
    case plinth::js::EvalErrorKind::MEMORY_LIMIT:
      kind_tag = "memory_limit";
      break;
    case plinth::js::EvalErrorKind::CPU_TIME_EXCEEDED:
      kind_tag = "cpu_time_exceeded";
      break;
    case plinth::js::EvalErrorKind::WALL_CLOCK_EXCEEDED:
      kind_tag = "wall_clock_exceeded";
      break;
    case plinth::js::EvalErrorKind::STACK_OVERFLOW:
      kind_tag = "stack_overflow";
      break;
    case plinth::js::EvalErrorKind::ASYNC_CONCURRENCY_LIMIT:
      kind_tag = "async_concurrency_limit";
      break;
    case plinth::js::EvalErrorKind::PROMISE_REJECTED_UNHANDLED:
      kind_tag = "promise_rejected";
      break;
    case plinth::js::EvalErrorKind::PROMISE_RESOLVE_AFTER_CANCEL:
      kind_tag = "promise_resolve_after_cancel";
      break;
    case plinth::js::EvalErrorKind::INTERNAL: kind_tag = "internal"; break;
    case plinth::js::EvalErrorKind::INTERNAL_ASYNC:
      kind_tag = "internal_async";
      break;
    case plinth::js::EvalErrorKind::ASYNC_RESULT_SIZE_EXCEEDED:
      kind_tag = "async_result_size_exceeded";
      break;
    case plinth::js::EvalErrorKind::UNICODE_SMUGGLE_DETECTED:
      kind_tag = "unicode_smuggle_detected";
      break;
    case plinth::js::EvalErrorKind::CANCELLED:
      // Handled above — pacify the switch coverage linter.
      break;
  }

  std::string msg_raw;
  msg_raw.reserve(ident.size() + 4 + kind_tag.size() + err.message.size());
  msg_raw.append(ident);
  msg_raw.append(": ");
  msg_raw.append(kind_tag);
  msg_raw.append(": ");
  msg_raw.append(err.message);
  std::string msg = clamp_message(std::move(msg_raw));

  // Audit only the handler-threw / handler-load-failed outcomes
  // (ICD §Audit). `cap.cancelled` + `cap.handler_not_found` +
  // `cap.extension_not_loaded` already return above without auditing.
  // Caller identity lives on `bc.user` (the pool copied it in at
  // dispatch() time).
  emit_extension_error_audit(extension_name, function, code, msg, bc.user);

  co_return std::unexpected(PromiseRejection{
      .code = code,
      .message = std::move(msg),
      .sqlstate = std::nullopt,
  });
}

// ── PG bootstrap scan ────────────────────────────────────────────────
//
// Mirrors `load_tier2_cache_locked` in resolution.cpp — deliberately
// sync libpq rather than Drogon's coroutine DbClient because this
// runs inside init_registry which the main thread calls before
// `drogon::app().run()` (no event loop available yet).
auto connect_and_list_active_extensions(const Config::Database& db_cfg,
                                        std::vector<std::string>& out) -> bool {
  std::ostringstream conninfo;
  conninfo << "host=" << db_cfg.host << " port=" << db_cfg.port
           << " dbname=" << db_cfg.database << " user=" << db_cfg.user
           << " password=" << db_cfg.password;
  PGconn* conn = PQconnectdb(conninfo.str().c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    spdlog::error("extensions::init_registry: PG connect failed: {}", err);
    return false;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  PGresult* res = PQexec(conn, "SELECT name FROM plinth.packages "
                               "WHERE state IN ('ACTIVE', 'ACTIVE_FLAGGED')");
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    spdlog::error("extensions::init_registry: SELECT failed: {}",
                  PQresultErrorMessage(res));
    PQclear(res);
    return false;
  }
  int rows = PQntuples(res);
  out.reserve(static_cast<std::size_t>(rows));
  for (int i = 0; i < rows; ++i) {
    out.emplace_back(PQgetvalue(res, i, 0));
  }
  PQclear(res);
  return true;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

auto init_registry(const Config& cfg) -> void {
  {
    std::unique_lock lock(registry_mutex);
    cfg_ptr = &cfg;
    pools.clear();
    accepting_dispatches = true;
  }
  std::vector<std::string> active_names;
  if (!connect_and_list_active_extensions(cfg.db, active_names)) {
    spdlog::warn("extensions::init_registry: proceeding with empty pool "
                 "registry; install-lifecycle hooks will repopulate");
    return;
  }
  std::size_t created = 0;
  for (const auto& name : active_names) {
    if (create_pool(name)) {
      ++created;
    }
  }
  spdlog::info(
      "extensions::init_registry: active_extensions={} pools_created={}",
      active_names.size(), created);
}

auto shutdown_registry(std::chrono::milliseconds timeout) -> bool {
  decltype(pools) local;
  {
    std::unique_lock lock(registry_mutex);
    accepting_dispatches = false;
    cfg_ptr = nullptr;
    local.swap(pools);
  }
  // Destroy idle pools outside registry_mutex. Active dispatches retain their
  // exact pool through DispatchLease and publish completion after releasing it.
  local.clear();

  std::unique_lock drain_lock(drain_mutex);
  bool drained = drain_cv.wait_for(drain_lock, timeout,
                                   [] { return inflight_dispatches == 0; });
  auto remaining = inflight_dispatches;
  drain_lock.unlock();
  if (!drained) {
    spdlog::error(
        "extensions::shutdown_registry timed out with {} dispatch(es)",
        remaining);
  }
  return drained;
}

auto create_pool(std::string_view extension_name) -> bool {
  std::unique_lock lock(registry_mutex);
  if (cfg_ptr == nullptr || !accepting_dispatches) {
    spdlog::warn("extensions::create_pool({}): registry not initialized; "
                 "skipping",
                 extension_name);
    return false;
  }
  fs::path server_root = fs::path{cfg_ptr->packages_data_dir} / "extensions" /
                         std::string{extension_name} / "active" / "server";
  std::error_code ec;
  if (!fs::exists(server_root, ec)) {
    // Client-only package — nothing to dispatch to. Silent skip
    // (not a warning).
    return false;
  }

  std::string name_copy{extension_name};
  // Destroy any existing pool first — idempotent semantics, and the
  // UPGRADE-to-ACTIVE call site needs the destroy-then-rebuild
  // ordering to route dispatches to the new version's handlers.
  pools.erase(name_copy);

  try {
    auto pool = std::make_shared<plinth::js::RuntimePool>(
        /*ext=*/nullptr, plinth::js::default_runtime_limits(), *cfg_ptr,
        /*pool_size=*/-1,
        /*user=*/nullptr, name_copy);
    pools.emplace(std::move(name_copy), std::move(pool));
    return true;
  } catch (const std::exception& e) {
    spdlog::error("extensions::create_pool({}): RuntimePool ctor threw: {}",
                  extension_name, e.what());
    return false;
  }
}

auto destroy_pool(std::string_view extension_name) -> void {
  std::shared_ptr<plinth::js::RuntimePool> local;
  {
    std::unique_lock lock(registry_mutex);
    auto node = pools.extract(std::string{extension_name});
    if (!node.empty()) {
      local = std::move(node.mapped());
    }
  }
  local.reset();
}

auto inflight_dispatch_count_for_test() -> std::size_t {
  std::lock_guard lock(drain_mutex);
  return inflight_dispatches;
}

// header docstring: the resolver holds args + caller on its coroutine frame
// across this await; copying would double every JSON round-trip and force a
// second UserContext copy beyond the one the pool already snapshots onto
// bc.user.
auto dispatch(std::string_view extension_name, std::string_view function,
              const Json::Value& args,
              const plinth::capabilities::UserContext& caller,
              int caller_call_depth)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>> {

  using plinth::js::PromiseRejection;

  std::string ident_outer;
  ident_outer.reserve(extension_name.size() + 1 + function.size());
  ident_outer.append(extension_name);
  ident_outer.push_back(':');
  ident_outer.append(function);

  std::shared_ptr<DispatchLease> lease;
  std::string packages_data_dir;
  {
    std::shared_lock<std::shared_mutex> lock(registry_mutex);
    if (cfg_ptr == nullptr || !accepting_dispatches) {
      co_return std::unexpected(PromiseRejection{
          .code = "cap.extension_not_loaded",
          .message = ident_outer + ": registry not initialized",
          .sqlstate = std::nullopt,
      });
    }

    auto it = pools.find(std::string{extension_name});
    if (it == pools.end() || it->second == nullptr) {
      co_return std::unexpected(PromiseRejection{
          .code = "cap.extension_not_loaded",
          .message = ident_outer,
          .sqlstate = std::nullopt,
      });
    }
    packages_data_dir = cfg_ptr->packages_data_dir;
    lease = std::make_shared<DispatchLease>(it->second);
  }

  if (auto validation =
          validate_shell_preference_call(extension_name, function, args)) {
    co_return std::unexpected(std::move(*validation));
  }

  plinth::js::BridgeContext* bc = lease->pool->acquire();
  if (bc == nullptr) {
    co_return std::unexpected(PromiseRejection{
        .code = "cap.extension_not_loaded",
        .message = ident_outer + ": pool acquire returned null",
        .sqlstate = std::nullopt,
    });
  }

  // ICD §Security Constraint 1 — callee runs under caller's identity
  // for audit + cap.call / db.query / pubsub.publish attribution.
  // `bc.extension_name` is already the callee's own (pool ctor) for
  // the `pubsub.publish` identity gate.
  bc->user = caller;
  bc->call_depth = caller_call_depth + 1;

  auto result = co_await invoke_handler(*bc, extension_name, function, args,
                                        packages_data_dir);

  if (result.has_value()) {
    lease->pool->release(bc);
    emit_preference_set_audit(extension_name, function, args, caller);
  } else {
    lease->pool->destroy(bc);
  }
  co_return result;
}

} // namespace plinth::extensions
