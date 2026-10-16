// SPDX-License-Identifier: MIT
//
// plinth::extensions::RuntimeRegistry — per-extension RuntimePool
// ownership plus the Tier 2 extension capability dispatch entry point.
// See ICD-0.5.0.3-extension-dispatch §RuntimeRegistry.

#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/capabilities/drain.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include "kernel/js/async_op.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/generation_capture.hpp"
#include "kernel/packages/migrations_internal.hpp"

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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace plinth::extensions {

struct FixedStorageSource {
  // Declared first so its charge is released only after all source bytes.
  packages::generation::BudgetTicket charge;
  PoolIdentity identity;
  std::string artifact_digest;
  std::string code;
};

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

auto valid_handler_defaults(const Json::Value& value) -> bool {
  if (!value.isObject() || value.size() != 2 || !value.isMember("version") ||
      !value["version"].isIntegral() || !value["version"].isUInt64() ||
      value["version"].asUInt64() != 1 || !value.isMember("choices") ||
      !value["choices"].isArray() || value["choices"].size() > 128 ||
      compact_json(value).size() > std::size_t{32} * 1024) {
    return false;
  }
  std::set<std::string> types;
  auto identifier = [](std::string_view text, bool panel) {
    if (text.size() < (panel ? 1U : 2U) || text.size() > 64 ||
        text.front() < 'a' || text.front() > 'z') {
      return false;
    }
    return std::ranges::all_of(text, [panel](char byte) {
      return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
             byte == '-' || (panel && byte == '_');
    });
  };
  auto literal_type = [](std::string_view text) {
    if (text.empty() || text.size() > 128) {
      return false;
    }
    bool separator = false;
    bool first = true;
    for (char byte : text) {
      const bool alnum =
          (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
      if (byte == '/') {
        if (first || separator) {
          return false;
        }
        separator = true;
        first = true;
      } else {
        if ((!alnum && first) ||
            (!alnum && !std::string_view{"!#$&^_.+-"}.contains(byte))) {
          return false;
        }
        first = false;
      }
    }
    return separator && !first;
  };
  for (const auto& choice : value["choices"]) {
    if (!choice.isObject() || choice.size() != 3 ||
        !choice["content_type"].isString() ||
        !choice["application_id"].isString() ||
        !choice["panel_id"].isString()) {
      return false;
    }
    const auto type = choice["content_type"].asString();
    if (!literal_type(type) || !types.insert(type).second ||
        !identifier(choice["application_id"].asString(), false) ||
        !identifier(choice["panel_id"].asString(), true)) {
      return false;
    }
  }
  return true;
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
  if (function == "preferences.set" && key == "shell.floats") {
    return PromiseRejection{
        .code = "invalid_argument",
        .message = "shell.floats requires guarded layout storage",
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
    if (key == "shell.handler_defaults" && !valid_handler_defaults(value)) {
      return PromiseRejection{
          .code = "invalid_argument",
          .message = "value not valid for key shell.handler_defaults",
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
struct PoolSlot {
  PoolIdentity identity;
  std::shared_ptr<plinth::js::RuntimePool> pool;
  std::shared_ptr<const FixedStorageSource> storage;
};
std::unordered_map<std::string, PoolSlot> pools;
class PoolConstructionGuard;
std::unordered_map<std::string, std::shared_ptr<PoolConstructionGuard>>
    constructions;
std::uint64_t shutdown_epoch = 0;
std::uint64_t next_pool_serial = 0;
std::mutex construction_hook_mutex;
PoolCreationHook construction_hook;
// A pool whose bounded close could not finish stays owned for the coordinator
// to retry. Successful replacement/disable never accumulates pools here.
std::vector<std::shared_ptr<plinth::js::RuntimePool>> failed_pools;
// Pointer — the Config owned by main is longer-lived than this registry.
// Null outside init/shutdown; readers check before use.
// process-lifetime config pointer; set once in init_registry.
const Config* cfg_ptr = nullptr;
bool accepting_dispatches = false;
std::mutex drain_mutex;
std::condition_variable drain_cv;
std::size_t inflight_dispatches = 0;
std::size_t retirements_in_progress = 0;
std::atomic<std::size_t> owner_loop_handoffs = 0;

constexpr auto POOL_RETIRE_TIMEOUT = std::chrono::seconds{5};

class PoolConstructionGuard {
 public:
  PoolConstructionGuard() {
    std::lock_guard lock(drain_mutex);
    ++retirements_in_progress;
  }
  ~PoolConstructionGuard() {
    {
      std::lock_guard lock(drain_mutex);
      --retirements_in_progress;
    }
    drain_cv.notify_all();
  }
  PoolConstructionGuard(const PoolConstructionGuard&) = delete;
  auto operator=(const PoolConstructionGuard&)
      -> PoolConstructionGuard& = delete;
  PoolConstructionGuard(PoolConstructionGuard&&) = delete;
  auto operator=(PoolConstructionGuard&&) -> PoolConstructionGuard& = delete;
};

auto notify_pool_creation(std::string_view name, PoolCreationStage stage)
    -> void {
  PoolCreationHook hook;
  {
    std::lock_guard lock(construction_hook_mutex);
    hook = construction_hook;
  }
  if (hook) hook(name, stage);
}

// Native callbacks already retain the accepted process lease. An alias shares
// this owner's control block so the SAME name guard survives JS abandonment
// until the final issued SQL/transaction callback has physically settled.
struct FixedDispatchOwner {
  explicit FixedDispatchOwner(
      std::shared_ptr<lifecycle::AsyncTaskLease> admitted,
      std::unique_ptr<capabilities::drain::DispatchGuard> guard)
      : accepted(std::move(admitted)), guard(std::move(guard)) {}
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
};

auto retain_failed_pool(std::shared_ptr<plinth::js::RuntimePool> pool) -> void {
  std::unique_lock lock(registry_mutex);
  failed_pools.push_back(std::move(pool));
}

// Every shared RuntimePool owner transition is serialized by registry_mutex.
// A sole owner is moved into an explicit claim; a non-final owner is reset
// while still holding the lock, so exactly one caller can own retirement.
auto claim_retirement_locked(std::shared_ptr<plinth::js::RuntimePool>& owner)
    -> std::shared_ptr<plinth::js::RuntimePool> {
  if (!owner) {
    return {};
  }
  if (owner.use_count() != 1) {
    owner.reset();
    return {};
  }
  auto claim = std::move(owner);
  {
    std::lock_guard lock(drain_mutex);
    ++retirements_in_progress;
  }
  return claim;
}

auto finish_retirement(std::shared_ptr<plinth::js::RuntimePool> claim,
                       bool may_shutdown_here) noexcept -> void {
  bool closed = false;
  if (may_shutdown_here) {
    try {
      closed = claim->shutdown(POOL_RETIRE_TIMEOUT);
    } catch (...) {
      spdlog::error("RuntimePool retirement threw");
    }
  }
  if (!closed) {
    retain_failed_pool(std::move(claim));
  } else {
    claim.reset();
  }
  {
    std::lock_guard lock(drain_mutex);
    --retirements_in_progress;
  }
  drain_cv.notify_all();
}

auto complete_dispatch_release(std::shared_ptr<plinth::js::RuntimePool> pool,
                               bool may_shutdown_here) -> void {
  std::shared_ptr<plinth::js::RuntimePool> claim;
  {
    std::unique_lock lock(registry_mutex);
    claim = claim_retirement_locked(pool);
  }
  if (claim) {
    finish_retirement(std::move(claim), may_shutdown_here);
  }
  {
    std::lock_guard lock(drain_mutex);
    --inflight_dispatches;
  }
  drain_cv.notify_all();
}

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
    auto* owner_loop = drogon::app().getLoop();
    if (owner_loop != nullptr) {
      owner_loop_handoffs.fetch_add(1, std::memory_order_relaxed);
      owner_loop->queueInLoop([owned = std::move(pool)]() mutable {
        complete_dispatch_release(std::move(owned), true);
      });
      return;
    }
    // Without the application loop, preserve the final owner for the
    // coordinator instead of running shutdown on an arbitrary callback thread.
    complete_dispatch_release(std::move(pool), false);
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
auto invoke_owned_source(
    plinth::js::BridgeContext& bc, std::string_view extension_name,
    std::string_view function, const Json::Value& args, std::string_view src,
    std::shared_ptr<plinth::lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>> {

  using plinth::js::PromiseRejection;

  std::string ident;
  ident.reserve(extension_name.size() + 1 + function.size());
  ident.append(extension_name);
  ident.push_back(':');
  ident.append(function);

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
  auto eval_outcome =
      co_await plinth::js::run_on_context(bc, WRAPPER_SRC, std::move(accepted));

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

// Ordinary dispatch deliberately preserves the ICD's fresh-per-call read.
auto invoke_handler(plinth::js::BridgeContext& bc,
                    std::string_view extension_name, std::string_view function,
                    const Json::Value& args, std::string_view data_dir,
                    std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>> {
  bool missing = false;
  bool io_error = false;
  auto source =
      read_handler_file(handler_source_path(data_dir, extension_name, function),
                        missing, io_error);
  if (missing || io_error) {
    auto ident = std::string{extension_name} + ":" + std::string{function};
    co_return std::unexpected(js::PromiseRejection{
        .code = missing ? "cap.handler_not_found" : "cap.handler_load_failed",
        .message = missing ? ident : ident + ": handler read failed",
        .sqlstate = std::nullopt,
    });
  }
  co_return co_await invoke_owned_source(bc, extension_name, function, args,
                                         source, std::move(accepted));
}

auto storage_rejection(std::string_view code = "storage_unavailable")
    -> js::PromiseRejection {
  return {.code = std::string{code},
          .message = "fixed shell storage unavailable",
          .sqlstate = std::nullopt};
}

auto fixed_operation_name(FixedStorageOperation operation) -> std::string_view {
  switch (operation) {
    case FixedStorageOperation::FLOATS_READ: return "floats_read";
    case FixedStorageOperation::OWN_DEFAULTS_READ: return "own_defaults_read";
    case FixedStorageOperation::ADMIN_DEFAULT_READ: return "admin_default_read";
    case FixedStorageOperation::ADMIN_DEFAULT_PUT: return "admin_default_put";
    case FixedStorageOperation::ADMIN_DEFAULT_DELETE:
      return "admin_default_delete";
    case FixedStorageOperation::RESOLVE_ADMIN_CANDIDATE:
      return "resolve_admin_candidate";
  }
  return {};
}

auto dispatch_fixed_source(
    PoolIdentity expected, Json::Value args, capabilities::UserContext caller,
    int caller_depth, std::shared_ptr<lifecycle::AsyncTaskLease> accepted,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>> {
  if (caller_depth < 0 || caller_depth >= capabilities::MAX_CALL_DEPTH ||
      caller.user_id.empty() || caller.auth_type == "anonymous") {
    co_return std::unexpected(storage_rejection("invalid_argument"));
  }
  if (!accepted) {
    accepted = lifecycle::async_tasks().try_acquire();
  }
  if (!accepted) {
    co_return std::unexpected(storage_rejection("cap.cancelled"));
  }
  auto physical = std::make_shared<FixedDispatchOwner>(std::move(accepted),
                                                       std::move(guard));
  if (!physical->guard || !physical->guard->admitted() ||
      capabilities::drain::is_application_blocked("shell")) {
    co_return std::unexpected(storage_rejection());
  }
  std::shared_ptr<DispatchLease> lease;
  std::shared_ptr<const FixedStorageSource> source;
  {
    std::shared_lock lock(registry_mutex);
    const auto found = pools.find("shell");
    if (!accepting_dispatches || cfg_ptr == nullptr || found == pools.end() ||
        !found->second.pool || !found->second.storage) {
      co_return std::unexpected(storage_rejection());
    }
    if (found->second.identity != expected ||
        found->second.storage->identity != expected) {
      co_return std::unexpected(storage_rejection("stale_generation"));
    }
    // Source and counted pool lease come from ONE slot under ONE lock. The
    // source owns no pool alias; retirement accounting remains unchanged.
    source = found->second.storage;
    lease = std::make_shared<DispatchLease>(found->second.pool);
  }
  auto* context = lease->pool->acquire();
  if (context == nullptr) {
    co_return std::unexpected(storage_rejection());
  }
  context->user = caller;
  context->call_depth = caller_depth + 1;
  // extension_name remains the callee shell from the pool. Caller/session/depth
  // are unchanged, with no injected rules or asserted browser-app identity.
  auto result = co_await invoke_owned_source(
      *context, "shell", "platform-storage-v1", args, source->code,
      std::shared_ptr<lifecycle::AsyncTaskLease>{physical,
                                                 physical->accepted.get()});
  if (result) {
    lease->pool->release(context);
  } else {
    lease->pool->destroy(context);
  }
  co_return result;
}

// ── PG bootstrap scan ────────────────────────────────────────────────
//
// Mirrors `load_tier2_cache_locked` in resolution.cpp — deliberately
// sync libpq rather than Drogon's coroutine DbClient because this
// runs inside init_registry which the main thread calls before
// `drogon::app().run()` (no event loop available yet).
struct ActiveExtension {
  std::string package_id;
  std::string name;
  std::string version;
};

auto connect_and_list_active_extensions(const Config::Database& db_cfg,
                                        std::vector<ActiveExtension>& out)
    -> bool {
  auto conninfo = plinth::db::connection_info(db_cfg);
  PGconn* conn = plinth::db::connect(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    spdlog::error("extensions::init_registry: PG connect failed: {}", err);
    return false;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);
  PGresult* res = plinth::db::exec(
      conn, "SELECT name, version, id::text FROM plinth.packages "
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
    out.push_back({.package_id = PQgetvalue(res, i, 2),
                   .name = PQgetvalue(res, i, 0),
                   .version = PQgetvalue(res, i, 1)});
  }
  PQclear(res);
  return true;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────

FixedStorageCall::FixedStorageCall(
    FixedStorageOperation operation, PoolIdentity identity,
    Json::Value arguments,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard)
    : op(operation), expected(std::move(identity)), args(std::move(arguments)),
      guard(std::move(guard)) {
}

ValidatedFloatCommit::ValidatedFloatCommit(
    PoolIdentity identity, Json::Value snapshot,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard)
    : expected(std::move(identity)), layout(std::move(snapshot)),
      guard(std::move(guard)) {
}

auto pool_identity(std::string_view name) -> std::optional<PoolIdentity> {
  std::shared_lock lock(registry_mutex);
  const auto found = pools.find(std::string{name});
  if (!accepting_dispatches || found == pools.end() || !found->second.pool) {
    return std::nullopt;
  }
  return found->second.identity;
}

auto fixed_shell_storage_binding() -> std::optional<FixedShellStorageBinding> {
  std::shared_lock lock(registry_mutex);
  const auto found = pools.find("shell");
  if (!accepting_dispatches || found == pools.end() || !found->second.pool ||
      !found->second.storage) {
    return std::nullopt;
  }
  return FixedShellStorageBinding{found->second.identity,
                                  found->second.storage->artifact_digest};
}

auto fixed_shell_storage_identity() -> std::optional<PoolIdentity> {
  auto binding = fixed_shell_storage_binding();
  if (!binding) return std::nullopt;
  return std::move(binding->identity);
}

auto prepare_fixed_shell_storage(
    const packages::generation::CapturedArtifact& artifact,
    const PoolIdentity& expected, const FixedShellStorageDigests& digests,
    packages::generation::Budget& budget)
    -> std::expected<PreparedShellStorage, js::PromiseRejection> try {
  const auto& key = artifact.key();
  const auto* module = artifact.find("server/platform-storage-v1.js");
  const auto* contract = artifact.find("server/plinth-platform.json");
  const auto* migration =
      artifact.find("migrations/002_platform_storage_v1.sql");
  auto valid_digest = [](std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char byte) {
             return (byte >= '0' && byte <= '9') ||
                    (byte >= 'a' && byte <= 'f');
           });
  };
  if (key.profile != 1 || expected.name != "shell" || expected.serial == 0 ||
      expected.package_id.empty() || expected.version.empty() ||
      key.name != expected.name || key.package_id != expected.package_id ||
      key.version != expected.version || !module || !contract || !migration ||
      module->kind != packages::restore::ArtifactKind::REGULAR ||
      contract->kind != packages::restore::ArtifactKind::REGULAR ||
      migration->kind != packages::restore::ArtifactKind::REGULAR ||
      module->content.empty() ||
      module->content.size() > std::size_t{32} * 1024 ||
      contract->content.size() > 1024 ||
      migration->content.size() > std::size_t{8} * 1024 ||
      !valid_digest(digests.module) || !valid_digest(digests.contract) ||
      !valid_digest(digests.migration) ||
      digests.applied_migration != digests.migration ||
      packages::detail::sha256_hex(module->content) != digests.module ||
      packages::detail::sha256_hex(contract->content) != digests.contract ||
      packages::detail::sha256_hex(migration->content) != digests.migration) {
    return std::unexpected(storage_rejection());
  }
  // Reserve the whole bounded copy envelope BEFORE allocating. Includes the
  // owned source object, identity/digest/string storage and terminators, not
  // merely module bytes. Framework allocator overhead is not process RSS proof.
  constexpr auto COPY_ENVELOPE = std::size_t{64} * 1024;
  auto charge = budget.try_reserve(COPY_ENVELOPE);
  if (!charge) {
    return std::unexpected(storage_rejection());
  }
  auto source = std::make_shared<FixedStorageSource>();
  source->identity = expected;
  source->artifact_digest = artifact.digest();
  source->charge = std::move(*charge);
  source->code = module->content;
  const auto resident =
      sizeof(FixedStorageSource) + source->identity.package_id.capacity() +
      source->identity.name.capacity() + source->identity.version.capacity() +
      source->artifact_digest.capacity() + source->code.capacity() + 5;
  if (resident > COPY_ENVELOPE) return std::unexpected(storage_rejection());
  source->charge.shrink(resident);
  PreparedShellStorage prepared;
  prepared.source = std::move(source);
  return prepared;
} catch (const std::exception&) {
  return std::unexpected(storage_rejection());
}

auto publish_fixed_shell_storage(PreparedShellStorage prepared) -> bool {
  if (!prepared.source) {
    return false;
  }
  std::unique_lock lock(registry_mutex);
  const auto found = pools.find("shell");
  if (!accepting_dispatches || cfg_ptr == nullptr || found == pools.end() ||
      !found->second.pool ||
      found->second.identity != prepared.source->identity) {
    return false;
  }
  if (found->second.storage) {
    // A stamp's published storage binding is immutable. Reconciliation may
    // confirm it, but changed bytes require an explicitly replaced pool.
    return found->second.storage->artifact_digest ==
               prepared.source->artifact_digest &&
           found->second.storage->code == prepared.source->code;
  }
  found->second.storage = std::move(prepared.source);
  return true;
}

auto dispatch_fixed_shell_storage(
    FixedStorageCall call, capabilities::UserContext caller,
    int caller_call_depth, std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>> {
  auto args = call.arguments();
  const auto operation = fixed_operation_name(call.operation());
  if (!args.isObject() || operation.empty()) {
    co_return std::unexpected(storage_rejection("invalid_argument"));
  }
  // The trusted enum always overwrites any JSON property. It has no commit arm.
  args["operation"] = std::string{operation};
  co_return co_await dispatch_fixed_source(
      call.identity(), std::move(args), std::move(caller), caller_call_depth,
      std::move(accepted), call.take_guard());
}

auto dispatch_fixed_shell_commit(
    ValidatedFloatCommit commit, capabilities::UserContext caller,
    int caller_call_depth, std::shared_ptr<lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::expected<Json::Value, js::PromiseRejection>> {
  Json::Value args(Json::objectValue);
  args["operation"] = "floats_commit";
  args["layout"] = commit.snapshot();
  co_return co_await dispatch_fixed_source(
      commit.identity(), std::move(args), std::move(caller), caller_call_depth,
      std::move(accepted), commit.take_guard());
}

auto init_registry(const Config& cfg) -> void {
  {
    std::unique_lock lock(registry_mutex);
    std::lock_guard drain_lock(drain_mutex);
    if (cfg_ptr != nullptr || accepting_dispatches || !pools.empty() ||
        !failed_pools.empty() || inflight_dispatches != 0 ||
        retirements_in_progress != 0) {
      throw std::logic_error(
          "extensions::init_registry requires a fully stopped registry");
    }
    cfg_ptr = &cfg;
    accepting_dispatches = true;
  }
  std::vector<ActiveExtension> active_extensions;
  if (!connect_and_list_active_extensions(cfg.db, active_extensions)) {
    spdlog::warn("extensions::init_registry: proceeding with empty pool "
                 "registry; install-lifecycle hooks will repopulate");
    return;
  }
  std::size_t created = 0;
  for (const auto& extension : active_extensions) {
    plinth::db::OperationScope::checkpoint_current();
    const auto extension_root =
        fs::path{cfg.packages_data_dir} / "extensions" / extension.name;
    std::error_code ec;
    const bool generation_matches = fs::equivalent(
        extension_root / "active", extension_root / extension.version, ec);
    if (ec || !generation_matches) {
      spdlog::warn(
          "extensions::init_registry: active pointer for {} does not match "
          "database version {}; leaving runtime unavailable",
          extension.name, extension.version);
      continue;
    }
    if (create_pool(extension.name, extension.package_id, extension.version)) {
      ++created;
    }
  }
  spdlog::info(
      "extensions::init_registry: active_extensions={} pools_created={}",
      active_extensions.size(), created);
}

auto shutdown_registry(std::chrono::milliseconds timeout) -> bool {
  decltype(pools) local_pools;
  decltype(failed_pools) local_failed;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  {
    std::unique_lock lock(registry_mutex);
    accepting_dispatches = false;
    ++shutdown_epoch;
    constructions.clear();
    cfg_ptr = nullptr;
    local_pools.swap(pools);
    local_failed.swap(failed_pools);
  }

  std::unique_lock drain_lock(drain_mutex);
  bool drained = drain_cv.wait_for(drain_lock, timeout, [] {
    return inflight_dispatches == 0 && retirements_in_progress == 0;
  });
  auto remaining = inflight_dispatches;
  auto retiring = retirements_in_progress;
  drain_lock.unlock();
  if (!drained) {
    spdlog::error(
        "extensions::shutdown_registry timed out with {} dispatch(es) and {} "
        "pool retirement(s)",
        remaining, retiring);
    // Fail closed without abandoning the durable owners. A later coordinator
    // retry takes them back and may release them after the leases drain.
    std::unique_lock lock(registry_mutex);
    pools.merge(local_pools);
    failed_pools.insert(failed_pools.end(),
                        std::make_move_iterator(local_failed.begin()),
                        std::make_move_iterator(local_failed.end()));
    for (auto& [name, slot] : local_pools) {
      (void)name;
      failed_pools.push_back(std::move(slot.pool));
    }
    local_pools.clear();
    return false;
  }
  // A final lease can fail its owner-loop retirement immediately before it
  // decrements inflight_dispatches to zero. That failure lands in the global
  // vector after our initial snapshot, so take it into this same attempt
  // before deciding that shutdown succeeded.
  {
    std::unique_lock lock(registry_mutex);
    local_failed.insert(local_failed.end(),
                        std::make_move_iterator(failed_pools.begin()),
                        std::make_move_iterator(failed_pools.end()));
    failed_pools.clear();
  }
  // DispatchLease has released all aliases. Shut down every active or
  // previously failed pool here, on the coordinator thread, within the
  // original deadline.
  for (auto& [name, slot] : local_pools) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (now >= deadline || !slot.pool->shutdown(remaining)) {
      spdlog::error("extensions::shutdown_registry could not stop pool {}",
                    name);
      local_failed.push_back(std::move(slot.pool));
    }
  }
  for (auto& pool : local_failed) {
    if (!pool) {
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (now < deadline && pool->shutdown(remaining)) {
      pool.reset();
    }
  }
  local_pools.clear();
  std::erase(local_failed, nullptr);
  if (!local_failed.empty()) {
    std::unique_lock lock(registry_mutex);
    failed_pools.insert(failed_pools.end(),
                        std::make_move_iterator(local_failed.begin()),
                        std::make_move_iterator(local_failed.end()));
    return false;
  }
  return true;
}

auto create_pool(std::string_view extension_name) -> bool {
  // Legacy/test callers have no authoritative generation and cannot publish
  // fixed platform storage until a stamped lifecycle replacement is installed.
  return create_pool(extension_name, {}, {});
}

auto create_pool(std::string_view extension_name, std::string_view package_id,
                 std::string_view version) -> bool {
  // Local nodes own unpublished/replaced pools, never an incidental temporary
  // constructed while registry_mutex is held. Strings and node allocations
  // precede cutover; on every exception the old route remains registered.
  // The process lease releases last, after all local pool/token owners.
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  decltype(pools) staging;
  decltype(pools)::node_type node;
  std::shared_ptr<PoolConstructionGuard> construction;
  std::string name;
  bool created = false;
  try {
    accepted = lifecycle::async_tasks().try_acquire();
    if (!accepted) return false;
    name = extension_name;
    PoolSlot slot{
        .identity = {.package_id = std::string{package_id},
                     .name = name,
                     .version = std::string{version},
                     .serial = 0},
        .pool = {},
        .storage = {},
    };
    staging.emplace(name, std::move(slot));
    node = staging.extract(name);
    construction = std::make_shared<PoolConstructionGuard>();
    decltype(constructions) pending;
    pending.emplace(name, construction);
    auto reservation = pending.extract(name);
    Config config;
    std::uint64_t epoch = 0;
    {
      std::unique_lock lock(registry_mutex);
      if (cfg_ptr == nullptr || !accepting_dispatches) return false;
      config = *cfg_ptr;
      epoch = shutdown_epoch;
      if (auto current = constructions.find(name);
          current != constructions.end()) {
        // Last-started same-name attempt wins, without cancelling any other
        // package. The old builder retains its own counted token until exit.
        current->second = construction;
      } else {
        constructions.reserve(constructions.size() + 1);
        constructions.insert(std::move(reservation));
      }
    }
    notify_pool_creation(name, PoolCreationStage::RESERVED);
    const auto server_root = fs::path{config.packages_data_dir} / "extensions" /
                             name / "active" / "server";
    std::error_code ec;
    if (fs::exists(server_root, ec) && !ec) {
      node.mapped().pool = std::make_shared<plinth::js::RuntimePool>(
          /*ext=*/nullptr, plinth::js::default_runtime_limits(), config,
          /*pool_size=*/-1, /*user=*/nullptr, name);
      notify_pool_creation(name, PoolCreationStage::CONSTRUCTED);
      {
        std::unique_lock lock(registry_mutex);
        const auto pending = constructions.find(name);
        if (cfg_ptr != nullptr && accepting_dispatches &&
            shutdown_epoch == epoch && pending != constructions.end() &&
            pending->second == construction &&
            next_pool_serial != std::numeric_limits<std::uint64_t>::max()) {
          // reserve may throw, but it is BEFORE old extraction/swap. The
          // preallocated node remains an explicit unpublished owner outside
          // this lock and follows bounded retirement in the common tail.
          pools.reserve(pools.size() + 1);
          node.mapped().identity.serial = ++next_pool_serial;
          auto previous = pools.find(name);
          if (previous != pools.end()) {
            static_assert(std::is_nothrow_swappable_v<PoolSlot>);
            std::swap(previous->second, node.mapped());
            created = true;
          } else {
            // Same allocator, preallocated node and sufficient bucket space;
            // std::hash<string>/equal_to introduce no throwing user callback.
            auto inserted = pools.insert(std::move(node));
            node = std::move(inserted.node);
            created = inserted.inserted;
          }
        }
      }
    }
  } catch (const std::exception&) {
    // No raw native/PG diagnostic or config is exposed. In particular an
    // allocation exception must not run a pool destructor under the lock.
    // The caller receives false. Avoid allocating another diagnostic while an
    // allocation failure's explicitly owned pool still needs retirement.
  }
  std::shared_ptr<plinth::js::RuntimePool> retirement;
  {
    std::unique_lock lock(registry_mutex);
    const auto pending = constructions.find(name);
    if (pending != constructions.end() && pending->second == construction)
      constructions.erase(pending);
    if (!node.empty()) retirement = claim_retirement_locked(node.mapped().pool);
  }
  if (retirement) {
    finish_retirement(std::move(retirement), true);
  }
  return created;
}

auto destroy_pool(std::string_view extension_name) -> void {
  std::shared_ptr<plinth::js::RuntimePool> retirement;
  {
    std::unique_lock lock(registry_mutex);
    const std::string name{extension_name};
    constructions.erase(name);
    auto node = pools.extract(name);
    if (!node.empty()) {
      retirement = claim_retirement_locked(node.mapped().pool);
    }
  }
  if (retirement) {
    finish_retirement(std::move(retirement), true);
  }
}

auto has_pool(std::string_view extension_name) -> bool {
  std::shared_lock lock(registry_mutex);
  return pools.contains(std::string{extension_name});
}

auto inflight_dispatch_count_for_test() -> std::size_t {
  std::lock_guard lock(drain_mutex);
  return inflight_dispatches;
}

auto owner_loop_handoff_count_for_test() -> std::size_t {
  return owner_loop_handoffs.load(std::memory_order_relaxed);
}

auto failed_pool_count_for_test() -> std::size_t {
  std::shared_lock lock(registry_mutex);
  return failed_pools.size();
}

auto set_pool_creation_hook_for_test(PoolCreationHook hook) -> void {
  std::lock_guard lock(construction_hook_mutex);
  construction_hook = std::move(hook);
}
auto pending_pool_constructions_for_test() -> std::size_t {
  std::shared_lock lock(registry_mutex);
  return constructions.size();
}
auto fixed_storage_retained_bytes_for_test() -> std::size_t {
  std::shared_lock lock(registry_mutex);
  const auto found = pools.find("shell");
  return found != pools.end() && found->second.storage
             ? found->second.storage->charge.bytes()
             : 0;
}

// header docstring: the resolver holds args + caller on its coroutine frame
// across this await; copying would double every JSON round-trip and force a
// second UserContext copy beyond the one the pool already snapshots onto
// bc.user.
auto dispatch(std::string_view extension_name, std::string_view function,
              const Json::Value& args,
              const plinth::capabilities::UserContext& caller,
              int caller_call_depth,
              std::shared_ptr<plinth::lifecycle::AsyncTaskLease> accepted)
    -> drogon::Task<std::expected<Json::Value, plinth::js::PromiseRejection>> {

  using plinth::js::PromiseRejection;

  std::string ident_outer;
  ident_outer.reserve(extension_name.size() + 1 + function.size());
  ident_outer.append(extension_name);
  ident_outer.push_back(':');
  ident_outer.append(function);

  if (!accepted) {
    accepted = plinth::lifecycle::async_tasks().try_acquire();
  }
  if (!accepted) {
    co_return std::unexpected(
        PromiseRejection{.code = "cap.cancelled",
                         .message = ident_outer + ": server is shutting down",
                         .sqlstate = std::nullopt});
  }

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
    if (it == pools.end() || it->second.pool == nullptr) {
      co_return std::unexpected(PromiseRejection{
          .code = "cap.extension_not_loaded",
          .message = ident_outer,
          .sqlstate = std::nullopt,
      });
    }
    packages_data_dir = cfg_ptr->packages_data_dir;
    lease = std::make_shared<DispatchLease>(it->second.pool);
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
                                        packages_data_dir, std::move(accepted));

  if (result.has_value()) {
    lease->pool->release(bc);
    emit_preference_set_audit(extension_name, function, args, caller);
  } else {
    lease->pool->destroy(bc);
  }
  co_return result;
}

} // namespace plinth::extensions
