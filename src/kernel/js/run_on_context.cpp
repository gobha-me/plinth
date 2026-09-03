// SPDX-License-Identifier: MIT
//
// run_on_context — see header. This is the heart of the 0.3.3 async
// bridge: a Drogon coroutine that drives the JS job queue + AsyncOp
// dispatch loop until the top-level evaluation completes (or the
// cancel / wall-clock cascade fires).
//
// Loop structure mirrors ICD-0.3.3 §Coroutine Dispatch Loop. Three
// invariants from the ICD that this file MUST uphold:
//   1. QuickJS access is serialized inside this coroutine body. Async
//      ops spawn from here, complete here. No background thread
//      touches bc.rt / bc.ctx.
//   2. CPU timer is bracketed around every JS_Eval / JS_ExecutePendingJob.
//      `co_await` falls outside the bracket.
//   3. Back-pressure is FIFO. Re-queue at the head of pending_ops; the
//      next iteration drains in enqueue order.
//
// ── Parallel dispatch (0.3.3.1) ──
//
// AsyncOps dispatch in parallel via `drogon::async_run`. Each detached
// task does the DB / audit work then `queueInLoop`'s back to the main
// loop to invoke `bc.resolve` / `bc.reject`, decrement
// `bc.inflight_detached`, and fire `bc.signal_completion()` which
// wakes the outer coroutine suspended on `AnyCompletionAwaiter`.
// See the `dispatch_async_op_detached` / `AnyCompletionAwaiter`
// definitions below.

#include "kernel/js/run_on_context.hpp"

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/js/conversion.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/db_silent_audit.hpp"
#include "kernel/js/eval_guard.hpp"
#include "kernel/js/stdlib/cap_bindings.hpp"
#include "kernel/js/stdlib/db_error_map.hpp"
#include "kernel/js/stdlib/db_result_to_json.hpp"
#include "kernel/js/stdlib_inject.hpp"
#include "kernel/logging.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/emit.hpp"
#include "kernel/realtime/sql_classify.hpp"

#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/DbTypes.h>
#include <drogon/orm/Exception.h>
#include <drogon/orm/Field.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/Row.h>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <json/value.h>
#include <json/writer.h>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <string>
#include <string_view>
#include <trantor/net/EventLoop.h>
#include <utility>
#include <vector>

namespace plinth::js {

namespace {

using std::chrono::steady_clock;

// Per-op outcome produced by the detached coroutine arm. The success
// side carries the Json result; the failure side carries the JS-visible
// rejection envelope. `std::expected` keeps the common-case resolve
// path allocation-free (matches 0.3.0.1 kernel convention).
using OpOutcome = std::expected<Json::Value, PromiseRejection>;

// Forward declarations.
auto run_cancellation_cascade(BridgeContext& bc, steady_clock::time_point start)
    -> drogon::Task<EvalResult>;

// Drive the JS job queue (resolved promises' .then handlers, etc.)
// under the CPU-timer bracket. Returns std::nullopt on success, or an
// EvalError if a job execution faulted.
auto drive_jobs(BridgeContext& bc) -> std::optional<EvalError> {
  // Coroutine resumption may have moved us to a different Drogon
  // loop thread; re-anchor the QuickJS stack base for the next
  // batch of JS execution.
  JS_UpdateStackTop(bc.rt);
  bc.resume_cpu_timer();
  while (JS_IsJobPending(bc.rt)) {
    JSContext* pctx = bc.ctx;
    int rc = JS_ExecutePendingJob(bc.rt, &pctx);
    // Capture OOM peaks between jobs — the interrupt handler's
    // 10000-bytecode cadence can miss a tight-loop OOM, but jobs
    // settle promises and fire .then handlers so sampling here
    // covers allocation bursts in async function bodies.
    detail::sample_memory_peak(bc);
    if (rc < 0) {
      bc.pause_cpu_timer();
      return detail::extract_error(pctx, bc);
    }
  }
  bc.pause_cpu_timer();
  return std::nullopt;
}

// Convert a top-level JSValue (which may be a Promise) into the final
// EvalResult value. When `result` is a settled promise, unwrap it; a
// rejected top-level promise surfaces as PROMISE_REJECTED_UNHANDLED.
// Frees `result` regardless of outcome.
auto convert_top_level(BridgeContext& bc, JSValue result)
    -> std::expected<Json::Value, EvalError> {
  if (JS_IsException(result)) {
    JS_FreeValue(bc.ctx, result);
    return std::unexpected(detail::extract_error(bc.ctx, bc));
  }

  // If the value is a Promise, settle it.
  JSPromiseStateEnum state = JS_PromiseState(bc.ctx, result);
  if (state != static_cast<JSPromiseStateEnum>(-1)) {
    if (state == JS_PROMISE_PENDING) {
      // Should not happen at finalize time — the loop continues
      // until the top-level Promise (if any) settles. Treat as
      // an internal invariant violation.
      JS_FreeValue(bc.ctx, result);
      return std::unexpected(
          EvalError{.kind = EvalErrorKind::INTERNAL_ASYNC,
                    .message = "top-level promise still pending at finalize",
                    .line = 0,
                    .column = 0});
    }
    JSValue inner = JS_PromiseResult(bc.ctx, result);
    JS_FreeValue(bc.ctx, result);
    if (state == JS_PROMISE_REJECTED) {
      // Route through the shared classifier so async-IIFE-wrapped
      // OOM / CPU / wall-clock / cancel trips surface with the
      // same EvalErrorKind as the sync-throw path. A rejection
      // that doesn't match any known trigger falls through to
      // PROMISE_REJECTED_UNHANDLED with the stringified reason.
      auto err = detail::classify_rejection(bc.ctx, inner, bc);
      JS_FreeValue(bc.ctx, inner);
      return std::unexpected(std::move(err));
    }
    // Fulfilled — convert the resolved value.
    auto out = detail::js_to_json(bc.ctx, inner);
    JS_FreeValue(bc.ctx, inner);
    return out;
  }

  // Not a promise — convert directly.
  auto out = detail::js_to_json(bc.ctx, result);
  JS_FreeValue(bc.ctx, result);
  return out;
}

auto finalize(BridgeContext& bc, JSValue result, steady_clock::time_point start)
    -> EvalResult {
  auto value = convert_top_level(bc, result);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      steady_clock::now() - start);
  return EvalResult{.value = std::move(value), .duration = duration};
}

// PG → Json::Value result conversion lives in
// `src/kernel/js/stdlib/db_result_to_json.{hpp,cpp}` since 0.5.3. The
// 0.3.3 inline heuristic moved there and gained an OID-driven switch
// table per ICD-0.5.3. See `db::result_to_json`.

// Bind a single Json::Value parameter onto a SqlBinder chain. Handles
// the per-slot mapping from db_bindings.cpp. Drogon's SqlBinder uses
// operator<< with typed values and emits parameterized queries
// internally. We pass scalars by value; the special-cased BYTEA marker
// {"__bytea__": "<bytes>"} from the binding is unpacked back into a
// raw byte buffer.
template <typename Binder>
auto bind_param(Binder& binder, const Json::Value& v) -> void {
  if (v.isNull()) {
    // No native std::nullopt overload; use std::string as NULL via
    // an empty optional<string>.
    binder << std::optional<std::string>{};
    return;
  }
  if (v.isBool()) {
    binder << v.asBool();
    return;
  }
  if (v.isIntegral()) {
    binder << static_cast<int64_t>(v.asInt64());
    return;
  }
  if (v.isDouble()) {
    binder << v.asDouble();
    return;
  }
  if (v.isObject() && v.isMember("__bytea__")) {
    // Tag-encoded bytea from the binding side; bind as raw bytes
    // via Drogon's binary-data handling.
    const auto& raw = v["__bytea__"].asString();
    binder << raw; // Drogon will treat std::string as text/bytea
    return;
  }
  // Strings + fallback.
  binder << v.asString();
}

// Map a Drogon DbException to a PromiseRejection per ICD §Promise
// Rejection Shape (db.*).
//
// Drogon's exception hierarchy:
//   DrogonDbException  (polymorphic, NOT std::exception-derived)
//     ├ Failure : DrogonDbException + std::runtime_error
//     │   └ SqlError : Failure   (carries SQLSTATE)
//     └ InternalError / TimeoutError / ... : DrogonDbException +
//     std::logic_error
//
// SqlError carries `sqlState()`. We try that first via dynamic_cast
// against the DrogonDbException polymorphic base.  Some Drogon paths
// (e.g. PgBatchConnection's "Command didn't run because of an abort
// earlier in a pipeline") wrap the original error in InternalError
// instead of SqlError; in that case we fall back to scraping SQLSTATE
// out of the message text. PG drivers commonly format messages as
// "ERROR:  <text>" — for now we keep db.internal for those, since the
// failing query in the same pipeline is reported separately.
auto pg_exception_to_rejection(const drogon::orm::DrogonDbException& e)
    -> PromiseRejection {
  PromiseRejection rej{.code = "db.internal",
                       .message = std::string{e.base().what()},
                       .sqlstate = std::nullopt};
  const auto* sql_err = dynamic_cast<const drogon::orm::SqlError*>(&e);
  if (sql_err != nullptr) {
    std::string state{sql_err->sqlState()};
    if (!state.empty()) {
      rej.sqlstate = state;
      rej.code = std::string{db_error_map::map_sqlstate(state)};
    }
    return rej;
  }
  // Heuristic SQLSTATE extraction from message text: PG includes the
  // five-char SQLSTATE in the form `... [SQLSTATE 42601]` in some
  // driver paths. Match the keyword.
  auto pos = rej.message.find("SQLSTATE ");
  if (pos != std::string::npos && pos + 14 <= rej.message.size()) {
    std::string state = rej.message.substr(pos + 9, 5);
    rej.sqlstate = state;
    rej.code = std::string{db_error_map::map_sqlstate(state)};
    return rej;
  }
  // Final fallback: pattern-match common syntax-error / undefined-X
  // messages so the test suite can prove the error-mapping pipeline
  // works against Drogon's PgBatchConnection (which loses SqlError
  // typing when batches abort). This is a pragmatic stopgap; proper
  // fix is a Drogon patch to preserve SqlError through batch aborts,
  // tracked as a follow-up in DEFERRED.md.
  if (rej.message.find("syntax error") != std::string::npos) {
    rej.code = "db.syntax_error";
    rej.sqlstate = std::string{"42601"};
  } else if (rej.message.find("does not exist") != std::string::npos &&
             rej.message.find("relation") != std::string::npos) {
    rej.code = "db.undefined_table";
    rej.sqlstate = std::string{"42P01"};
  } else if (rej.message.find("does not exist") != std::string::npos &&
             rej.message.find("column") != std::string::npos) {
    rej.code = "db.undefined_column";
    rej.sqlstate = std::string{"42703"};
  }
  return rej;
}

// ── Per-op dispatch helpers ───────────────────────────────────────
//
// Each AsyncOp::Type routes to a `_outcome` helper that returns an
// `OpOutcome` (= `std::expected<Json::Value, PromiseRejection>`).
// `dispatch_async_op_detached` below fans these into per-op detached
// coroutines and shuttles the outcome back to the main loop for
// resolve/reject. Splitting per-type keeps each arm below the
// cognitive-complexity threshold clang-tidy enforces.

auto db_connection_unavailable(BridgeContext& bc, int id) -> void {
  bc.reject(id, PromiseRejection{.code = "db.connection_error",
                                 .message = "no DbClient configured",
                                 .sqlstate = std::nullopt});
}

// SqlBinderAwaiter — non-blocking awaiter for the runtime-sized param
// path. Replaces the `std::promise/future` bridge retired in 0.3.3.1
// per docs/DEFERRED.md "runtime-sized param binding loop blocking".
//
// Inherits Drogon's CallbackAwaiter<Result> for result / exception
// storage + await_resume. await_suspend sets up the SqlBinder,
// registers row + exception callbacks that capture `this` + the
// suspended handle, and returns. The binder's destructor (at the end
// of await_suspend's scope) triggers exec; callbacks fire async on
// Drogon's IO thread and resume the coroutine there. The awaiter
// lives in the coroutine frame, so `this` outlives the binder.
struct SqlBinderAwaiter : public drogon::CallbackAwaiter<drogon::orm::Result> {
  SqlBinderAwaiter(std::shared_ptr<drogon::orm::DbClient> db_in,
                   std::string sql_in, std::vector<Json::Value> params_in)
      : db(std::move(db_in)), sql(std::move(sql_in)),
        params(std::move(params_in)) {}

  auto await_suspend(std::coroutine_handle<> handle) -> void {
    auto binder = *db << sql;
    for (const auto& p : params) {
      bind_param(binder, p);
    }
    binder >> [this, handle](const drogon::orm::Result& res) {
      setValue(res);
      handle.resume();
    };
    binder >> [this, handle](const std::exception_ptr& ep) {
      setException(ep);
      handle.resume();
    };
  } // binder dtor here → exec queued; callbacks fire later

 private:
  std::shared_ptr<drogon::orm::DbClient> db;
  std::string sql;
  std::vector<Json::Value> params;
};

// ── Outcome-producing per-type helpers ───────────────────────────────
//
// 0.3.3.1 refactor: the per-type arms no longer touch bc.resolve /
// bc.reject directly. Each returns an `OpOutcome` that the detached
// dispatcher shuttles back to the main loop via `queueInLoop`. This
// keeps the Critical Invariant 1 boundary explicit — only main-loop
// code paths touch bc.rt / bc.ctx / bc.callbacks — and unifies the
// db.* success / failure / unreachable-PG branches.

// ICD-0.5.3 §Per-Op SET search_path Isolation. Return shape for the
// wrapper prepare step below. Exactly one of `tx` / `early_reject` is
// populated (or both empty, meaning kernel-scope bypass / enforce
// disabled → run raw).
struct SearchPathWrap {
  std::shared_ptr<drogon::orm::Transaction> tx;
  std::optional<PromiseRejection> early_reject;
};

// For extension-scope bcs with `enforce=true`, allocate a fresh
// transaction and run `SET LOCAL search_path TO ext_<id>, plinth`
// before the user SQL. Return struct-by-value so this coroutine has
// no reference parameters (the coroutine-frame capture of references
// to caller locals is unsafe per
// cppcoreguidelines-avoid-reference-coroutine-parameters).
auto prepare_search_path_wrapper(std::shared_ptr<drogon::orm::DbClient> db,
                                 std::string extension_name)
    -> drogon::Task<SearchPathWrap> {
  SearchPathWrap out;
  if (!plinth::js::db::search_path_enforced() || extension_name.empty()) {
    co_return out;
  }
  if (!plinth::js::db::is_valid_extension_name(extension_name)) {
    plinth::js::db::audit_search_path_set_failed(extension_name, "");
    out.early_reject = PromiseRejection{
        .code = "db.search_path.set_failed",
        .message = "invalid extension identity for SET LOCAL search_path",
        .sqlstate = std::nullopt};
    co_return out;
  }
  try {
    out.tx = co_await db->newTransactionCoro();
    co_await out.tx->execSqlCoro("SET LOCAL search_path TO ext_" +
                                 extension_name + ", plinth");
  } catch (const drogon::orm::DrogonDbException& e) {
    auto rej = pg_exception_to_rejection(e);
    plinth::js::db::audit_search_path_set_failed(extension_name,
                                                 rej.sqlstate.value_or(""));
    rej.code = "db.search_path.set_failed";
    out.early_reject = rej;
    out.tx.reset();
  }
  co_return out;
}

auto run_db_query_outcome(AsyncOp op) -> drogon::Task<OpOutcome> {
  try {
    auto db = drogon::app().getDbClient();
    if (!db) {
      co_return std::unexpected(
          PromiseRejection{.code = "db.connection_error",
                           .message = "no DbClient configured",
                           .sqlstate = std::nullopt});
    }

    // ICD-0.5.3 §`db.batch()` §Connection pinning — in-batch ops
    // route through the pinned TransactionPtr snapshotted at
    // enqueue time. The SET LOCAL search_path ran at DB_BATCH_BEGIN,
    // so this arm SKIPS the per-op wrapper and its COMMIT.
    SearchPathWrap wrap;
    std::shared_ptr<drogon::orm::DbClient> exec_target;
    if (op.batch_pinned_conn) {
      exec_target = op.batch_pinned_conn;
    } else {
      wrap = co_await prepare_search_path_wrapper(db, op.bc_extension_name);
      if (wrap.early_reject.has_value()) {
        co_return std::unexpected(*std::move(wrap.early_reject));
      }
      exec_target =
          wrap.tx ? std::static_pointer_cast<drogon::orm::DbClient>(wrap.tx)
                  : db;
    }

    drogon::orm::Result r{nullptr};
    if (op.sql_params.empty()) {
      r = co_await exec_target->execSqlCoro(op.sql);
    } else {
      r = co_await SqlBinderAwaiter{exec_target, op.sql, op.sql_params};
    }
    // Per-op search_path wrapper explicit COMMIT (phase 3). Batch
    // ops skip this — DB_BATCH_COMMIT owns the single commit at
    // batch end.
    if (wrap.tx) {
      co_await wrap.tx->execSqlCoro("COMMIT");
    }
    co_return db::result_to_json(r);
  } catch (const drogon::orm::DrogonDbException& e) {
    co_return std::unexpected(pg_exception_to_rejection(e));
  } catch (const std::exception& e) {
    co_return std::unexpected(PromiseRejection{.code = "db.internal",
                                               .message = std::string{e.what()},
                                               .sqlstate = std::nullopt});
  }
}

auto run_db_exec_outcome(AsyncOp op) -> drogon::Task<OpOutcome> {
  try {
    auto db = drogon::app().getDbClient();
    if (!db) {
      co_return std::unexpected(
          PromiseRejection{.code = "db.connection_error",
                           .message = "no DbClient configured",
                           .sqlstate = std::nullopt});
    }

    // Batch path — pinned conn + search_path already set by BEGIN.
    SearchPathWrap wrap;
    std::shared_ptr<drogon::orm::DbClient> exec_target;
    if (op.batch_pinned_conn) {
      exec_target = op.batch_pinned_conn;
    } else {
      wrap = co_await prepare_search_path_wrapper(db, op.bc_extension_name);
      if (wrap.early_reject.has_value()) {
        co_return std::unexpected(*std::move(wrap.early_reject));
      }
      exec_target =
          wrap.tx ? std::static_pointer_cast<drogon::orm::DbClient>(wrap.tx)
                  : db;
    }

    drogon::orm::Result r{nullptr};
    if (op.sql_params.empty()) {
      r = co_await exec_target->execSqlCoro(op.sql);
    } else {
      r = co_await SqlBinderAwaiter{exec_target, op.sql, op.sql_params};
    }
    // Per-op wrapper's explicit COMMIT (phase 3). In-batch ops
    // skip — DB_BATCH_COMMIT owns the batch-scope commit.
    if (wrap.tx) {
      co_await wrap.tx->execSqlCoro("COMMIT");
    }
    Json::Value out{Json::objectValue};
    out["row_count"] = static_cast<Json::Int>(r.affectedRows());
    // ICD-0.5.1 §Registry + Integration Point. The classifier
    // skips SELECT / DDL / unparseable forms (std::nullopt);
    // record_write tolerates row_count==0 (no-op on empty bucket)
    // and honors op.silent (0.5.3 per-call opt-out).
    // ICD-0.5.3 §`db.batch()` §Coalescer interaction — writes
    // tagged with a non-zero batch_scope_id accumulate under the
    // scope bucket (no timer); DB_BATCH_COMMIT releases them.
    if (!op.silent) {
      if (auto sql_class =
              plinth::realtime::classify_sql(op.sql, op.bc_extension_name);
          sql_class.has_value()) {
        plinth::realtime::CoalescerRegistry::instance().record_write(
            sql_class->schema, sql_class->table, sql_class->op_kind,
            static_cast<std::size_t>(r.affectedRows()), op.bc_extension_name,
            op.batch_scope_id);
      }
    } else {
      // ICD-0.5.3 §silent Flag §Rate-limited audit — the fire-
      // and-forget observability hook that lets operators detect
      // an extension silencing every write without drowning the
      // audit log. Key'd by extension identity so kernel-scope
      // bcs (empty name) aggregate together.
      plinth::js::record_silent_use(op.bc_extension_name);
    }
    co_return out;
  } catch (const drogon::orm::DrogonDbException& e) {
    co_return std::unexpected(pg_exception_to_rejection(e));
  } catch (const std::exception& e) {
    co_return std::unexpected(PromiseRejection{.code = "db.internal",
                                               .message = std::string{e.what()},
                                               .sqlstate = std::nullopt});
  }
}

// Capability dispatch arm (ICD-0.3.4 §CAP_CALL Dispatch Arm). Awaits
// the resolver's `call_capability_async` coroutine; maps every
// `CapabilityError` variant to the corresponding `cap.*` rejection per
// ICD §Error Mapping (see `capability_error_to_rejection`).
//
// The caller's `user` + `call_depth` snapshot was captured on the main
// loop thread at cap.call enqueue time and lives on `op.cap_user` /
// `op.cap_call_depth`. Reading those off the op value inside this
// detached coroutine is free of cross-thread races — the op is moved
// into this function and owned here.
auto run_cap_call_outcome(AsyncOp op) -> drogon::Task<OpOutcome> {
  plinth::capabilities::CapabilityCall cc{.signature = op.cap_signature,
                                          .args = op.cap_args,
                                          .call_depth = op.cap_call_depth};
  // ICD-0.5.0.3 §Error taxonomy — capture the `cap.*` detail code +
  // capped message the resolver routes up from the extension
  // dispatcher when an extension handler rejects. Non-extension
  // dispatches leave both strings empty and the rejection mapper
  // falls back to the enum-keyed `cap.*` codes unchanged.
  std::string ext_detail_code;
  std::string ext_detail_message;
  auto res = co_await plinth::capabilities::call_capability_async(
      cc, op.cap_user, &ext_detail_code, &ext_detail_message);
  if (res.has_value()) {
    co_return res->data;
  }
  co_return std::unexpected(capability_error_to_rejection(
      res.error(), op.cap_signature, ext_detail_code, ext_detail_message));
}

// 0.3.5 result-size enforcement (ICD-0.3.5 §New Enforcement). Measures
// a serialized byte count of the success `Json::Value` and returns a
// `PromiseRejection{code: "async.result_size_exceeded"}` if the count
// exceeds `bc.async_result_size_limit_bytes`. Runs in the detached task
// BEFORE the main-loop `queueInLoop` resolve lands, so oversized
// results never reach the JS heap (defense in depth against runaway
// query / capability returns).
//
// `Json::FastWriter` emits a trailing newline; the size returned
// therefore includes that one byte. Documented here rather than
// stripped — FastWriter is the project-wide primitive for this
// measurement (see logging.cpp), and the per-op overhead of scanning
// for the trailing byte would outweigh the informational benefit.
auto check_result_size(const BridgeContext& bc, const Json::Value& v)
    -> std::optional<PromiseRejection> {
  auto serialized = Json::FastWriter{}.write(v);
  const std::size_t SIZE = serialized.size();
  if (SIZE > bc.async_result_size_limit_bytes) {
    return PromiseRejection{
        .code = "async.result_size_exceeded",
        .message = "result size " + std::to_string(SIZE) +
                   " bytes exceeds limit " +
                   std::to_string(bc.async_result_size_limit_bytes) + " bytes",
        .sqlstate = std::nullopt};
  }
  return std::nullopt;
}

// ICD-0.5.0 §`pubsub.*` JS Stdlib → Rejection codes. Maps emit-side
// NotifyError onto the JS-visible pubsub.* rejection taxonomy. The
// binding has already done the regex + extension-identity checks that
// produce pubsub.channel_invalid / pubsub.extension_mismatch inline;
// by the time this outcome runs, only the emit helper's own
// validation + PG paths can fail.
auto notify_error_to_rejection(plinth::realtime::NotifyError err)
    -> PromiseRejection {
  using plinth::realtime::NotifyError;
  switch (err) {
    case NotifyError::INVALID_CHANNEL:
    case NotifyError::LAYER_MISMATCH:
      return {.code = "pubsub.channel_invalid",
              .message = "channel failed validation",
              .sqlstate = std::nullopt};
    case NotifyError::PAYLOAD_TOO_LARGE:
      return {.code = "pubsub.payload_too_large",
              .message = "serialized envelope exceeds ceiling",
              .sqlstate = std::nullopt};
    case NotifyError::MISSING_LAYER:
      // Impossible — the binding always sets layer="extension".
      return {.code = "pubsub.internal",
              .message = "missing layer (binding-internal bug)",
              .sqlstate = std::nullopt};
    case NotifyError::PG_FAILURE:
    default:
      return {.code = "pubsub.pg_error",
              .message = "pg_notify failed",
              .sqlstate = std::nullopt};
  }
}

auto run_pubsub_publish_outcome(AsyncOp op) -> drogon::Task<OpOutcome> {
  auto db = drogon::app().getDbClient();
  if (!db) {
    co_return std::unexpected(
        PromiseRejection{.code = "pubsub.pg_error",
                         .message = "no DbClient configured",
                         .sqlstate = std::nullopt});
  }
  Json::Value envelope(Json::objectValue);
  envelope["layer"] = "extension";
  envelope["channel"] = op.pubsub_channel;
  envelope["payload"] = std::move(op.pubsub_payload);
  auto res =
      co_await plinth::realtime::emit_notify_async(db, std::move(envelope));
  if (res.has_value()) {
    co_return Json::Value{Json::nullValue};
  }
  co_return std::unexpected(notify_error_to_rejection(res.error()));
}

auto run_audit_write_outcome(const AsyncOp& op) -> OpOutcome {
  if (!plinth::log::is_audit_ready()) {
    return std::unexpected(
        PromiseRejection{.code = "audit.not_ready",
                         .message = "audit writer not initialized",
                         .sqlstate = std::nullopt});
  }
  try {
    plinth::log::AuditCtx actx{
        .user_id = op.audit_user_id,
        .session_id = op.audit_session_id,
        .ip_address = op.audit_ip_address,
    };
    plinth::log::audit(op.audit_event_type, op.audit_payload, actx);
    return Json::Value{};
  } catch (const std::exception& e) {
    return std::unexpected(PromiseRejection{.code = "audit.internal",
                                            .message = std::string{e.what()},
                                            .sqlstate = std::nullopt});
  }
}

// ── ICD-0.5.3 §`db.batch()` — batch dispatch arms ────────────────────
//
// All three arms follow the pattern: do DB work on the detached
// coro, then hop back to `main_loop` to mutate bc.batch_state +
// resolve/reject the promise. The bc mutation lives on the main loop
// per the same discipline as bc.resolve (main-loop-serialized state).
//
// Arm responsibilities:
//   BEGIN — open TransactionPtr, run SET LOCAL search_path on the
//           pinned conn, store the tx on bc.batch_state.pinned_conn,
//           resolve P_begin with {}.
//   COMMIT — run COMMIT on the pinned conn, flush the coalescer
//            scope (emits one envelope per accumulated schema/table
//            tuple with window_ms=0), fire db.batch.committed audit,
//            clear bc.batch_state, resolve P_outer with {}. The JS
//            orchestrator returns the user fn's resolved value via
//            its own .then chain, so DB_BATCH_COMMIT itself carries
//            no payload.
//   ROLLBACK — run ROLLBACK on the pinned conn, discard the coalescer
//              scope, fire db.batch.rolled_back audit, clear
//              bc.batch_state, resolve P_rollback with {} (the
//              orchestrator's chain still surfaces the user error;
//              this arm does NOT reject with op.rollback_error — the
//              outer promise's rejection path fires via JS-side
//              `throw e` after rollback resolves).

auto finalize_batch(BridgeContext& bc, AsyncOp::Type type, AsyncOp op,
                    trantor::EventLoop* main_loop,
                    std::shared_ptr<drogon::orm::Transaction> tx,
                    std::optional<PromiseRejection> err) -> void {
  int cb_id = op.callback_id;
  std::uint64_t scope_id = op.batch_scope_id;
  std::string ext_name = op.bc_extension_name;
  std::string rollback_reason = "db_error";
  std::string rollback_code;
  if (op.rollback_error.isObject() && op.rollback_error.isMember("code")) {
    rollback_code = op.rollback_error["code"].asString();
    // ICD-0.5.3 §B.06 — when the orchestrator's catch was
    // triggered by a wall-clock timeout reject (set by either
    // the in-batch enqueue path or the COMMIT path once
    // `bc.batch_state.timed_out` is observed), stamp the audit
    // reason as `timeout` rather than `user_callback_threw`.
    rollback_reason = (rollback_code == "db.batch.timeout")
                          ? "timeout"
                          : "user_callback_threw";
  }

  // `tx` is moved into the queueInLoop lambda so the main-loop
  // callback owns the sole reference; this avoids a shared_ptr
  // copy (and satisfies performance-unnecessary-value-param).
  main_loop->queueInLoop([&bc, cb_id, type, scope_id, ext_name, rollback_reason,
                          rollback_code, main_loop, tx = std::move(tx),
                          err = std::move(err)]() mutable {
    if (err.has_value()) {
      // BEGIN failure never enters the registry; COMMIT /
      // ROLLBACK failure already ran through the arm, so
      // clean up the registry defensively in either case.
      plinth::js::unregister_in_flight_batch(scope_id);
      bc.reject(cb_id, *err);
    } else {
      switch (type) {
        case AsyncOp::Type::DB_BATCH_BEGIN: {
          bc.batch_state.pinned_conn = tx;
          plinth::js::register_in_flight_batch(scope_id, ext_name);
          // ICD-0.5.3 §B.06 — arm the wall-clock
          // deadline now that BEGIN succeeded.
          // Lambda captures `&bc` + `scope_id` by
          // value; the scope-id match check ensures a
          // stale fire after a fast COMMIT/ROLLBACK +
          // re-batch on the same bc no-ops. Discard
          // paths (`discard_batches_for_extension`,
          // `discard_all_batches`) invalidate this
          // timer synchronously, so a destroyed bc can
          // never see a late fire.
          std::size_t to_ms = plinth::js::batch_timeout_ms();
          trantor::TimerId tid = main_loop->runAfter(
              static_cast<double>(to_ms) / 1000.0, [&bc, scope_id]() {
                if (bc.batch_state.scope_id == scope_id) {
                  bc.batch_state.timed_out = true;
                }
              });
          bc.batch_state.timer_id = tid;
          bc.batch_state.timer_loop = main_loop;
          plinth::js::set_batch_timer(scope_id, tid, main_loop);
          bc.resolve(cb_id, Json::Value{});
          break;
        }
        case AsyncOp::Type::DB_BATCH_COMMIT: {
          std::size_t ops = bc.batch_state.ops_in_batch;
          plinth::js::clear_batch_timer(scope_id);
          bc.batch_state = BridgeContext::BatchState{};
          plinth::js::unregister_in_flight_batch(scope_id);
          plinth::realtime::CoalescerRegistry::instance().flush_batch_scope(
              scope_id);
          plinth::js::audit_batch_committed(ext_name, ops);
          bc.resolve(cb_id, Json::Value{});
          break;
        }
        case AsyncOp::Type::DB_BATCH_ROLLBACK:
          plinth::js::clear_batch_timer(scope_id);
          bc.batch_state = BridgeContext::BatchState{};
          plinth::js::unregister_in_flight_batch(scope_id);
          plinth::realtime::CoalescerRegistry::instance().discard_batch_scope(
              scope_id);
          plinth::js::audit_batch_rolled_back(ext_name, rollback_reason,
                                              rollback_code);
          bc.resolve(cb_id, Json::Value{});
          break;
        default: break;
      }
    }
    bc.inflight_detached.fetch_sub(1, std::memory_order_acq_rel);
    bc.signal_completion();
  });
}

// outlives the coroutine.
auto handle_db_batch_begin(BridgeContext& bc, AsyncOp op,
                           trantor::EventLoop* main_loop) -> drogon::Task<> {
  try {
    auto db = drogon::app().getDbClient();
    if (!db) {
      finalize_batch(bc, AsyncOp::Type::DB_BATCH_BEGIN, std::move(op),
                     main_loop, nullptr,
                     PromiseRejection{.code = "db.connection_error",
                                      .message = "no DbClient configured",
                                      .sqlstate = std::nullopt});
      co_return;
    }
    // Phase 3's regex check defends against a synthetic
    // extension_name; BEGIN is also extension-scoped, so honor
    // the same gate. Kernel-scope batches are legal — they skip
    // the SET LOCAL step.
    if (!op.bc_extension_name.empty() &&
        !plinth::js::db::is_valid_extension_name(op.bc_extension_name)) {
      plinth::js::db::audit_search_path_set_failed(op.bc_extension_name, "");
      finalize_batch(bc, AsyncOp::Type::DB_BATCH_BEGIN, std::move(op),
                     main_loop, nullptr,
                     PromiseRejection{
                         .code = "db.search_path.set_failed",
                         .message = "invalid extension identity for SET LOCAL",
                         .sqlstate = std::nullopt});
      co_return;
    }
    auto tx = co_await db->newTransactionCoro();
    if (!op.bc_extension_name.empty() &&
        plinth::js::db::search_path_enforced()) {
      co_await tx->execSqlCoro("SET LOCAL search_path TO ext_" +
                               op.bc_extension_name + ", plinth");
    }
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_BEGIN, std::move(op), main_loop,
                   std::move(tx), std::nullopt);
  } catch (const drogon::orm::DrogonDbException& e) {
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_BEGIN, std::move(op), main_loop,
                   nullptr, pg_exception_to_rejection(e));
  } catch (const std::exception& e) {
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_BEGIN, std::move(op), main_loop,
                   nullptr,
                   PromiseRejection{.code = "db.internal",
                                    .message = std::string{e.what()},
                                    .sqlstate = std::nullopt});
  }
}

auto handle_db_batch_commit(BridgeContext& bc, AsyncOp op,
                            trantor::EventLoop* main_loop) -> drogon::Task<> {
  try {
    auto tx = bc.batch_state.pinned_conn;
    if (!tx) {
      finalize_batch(
          bc, AsyncOp::Type::DB_BATCH_COMMIT, std::move(op), main_loop, nullptr,
          PromiseRejection{.code = "db.batch.no_pinned_conn",
                           .message = "batch commit without a pinned conn",
                           .sqlstate = std::nullopt});
      co_return;
    }
    co_await tx->execSqlCoro("COMMIT");
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_COMMIT, std::move(op), main_loop,
                   tx, std::nullopt);
  } catch (const drogon::orm::DrogonDbException& e) {
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_COMMIT, std::move(op), main_loop,
                   nullptr, pg_exception_to_rejection(e));
  } catch (const std::exception& e) {
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_COMMIT, std::move(op), main_loop,
                   nullptr,
                   PromiseRejection{.code = "db.internal",
                                    .message = std::string{e.what()},
                                    .sqlstate = std::nullopt});
  }
}

auto handle_db_batch_rollback(BridgeContext& bc, AsyncOp op,
                              trantor::EventLoop* main_loop) -> drogon::Task<> {
  try {
    auto tx = bc.batch_state.pinned_conn;
    if (tx) {
      // Issue ROLLBACK explicitly; Drogon's tx destructor would
      // auto-commit otherwise (see phase 3 note). The rollback()
      // method flips isCommitedOrRolledback_ so the destructor
      // skips its commit.
      tx->rollback();
    }
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_ROLLBACK, std::move(op),
                   main_loop, tx, std::nullopt);
  } catch (const std::exception& e) {
    // Rollback is best-effort — swallow exceptions and still
    // resolve the orchestrator promise so the chain proceeds to
    // reject with the user's original error.
    finalize_batch(bc, AsyncOp::Type::DB_BATCH_ROLLBACK, std::move(op),
                   main_loop, nullptr,
                   PromiseRejection{.code = "db.internal",
                                    .message = std::string{e.what()},
                                    .sqlstate = std::nullopt});
  }
  co_return;
}

// Detached dispatch arm — called from `drogon::async_run` per op. Runs
// the type-specific coroutine (possibly resuming on a Drogon IO
// thread), then hops back to `main_loop` via `queueInLoop` to invoke
// `bc.resolve` / `bc.reject`, decrement `inflight_detached`, and fire
// `signal_completion` to wake the outer coroutine.
//
// The capture of `bc` is by reference — safe because
// `run_on_context`'s normal path loops while `inflight_detached > 0`
// and the cancellation cascade drains inflight with a bounded ceiling
// before returning. The `BridgeContext` outlives every spawned detached
// task; see DEFERRED.md "runtime-binder" / "parallel-fanout" entries
// resolved in 0.3.3.1.
// outlives the coroutine.
auto dispatch_async_op_detached(BridgeContext& bc, AsyncOp op,
                                trantor::EventLoop* main_loop)
    -> drogon::Task<> {
  // Capture callback id before the switch — the CAP_CALL case
  // std::move's `op` into run_cap_call_outcome.
  int cb_id = op.callback_id;
  OpOutcome outcome;
  switch (op.type) {
    case AsyncOp::Type::DB_QUERY:
      outcome = co_await run_db_query_outcome(op);
      break;
    case AsyncOp::Type::DB_EXEC:
      outcome = co_await run_db_exec_outcome(op);
      break;
    case AsyncOp::Type::AUDIT_WRITE:
      outcome = run_audit_write_outcome(op);
      break;
    case AsyncOp::Type::CAP_CALL:
      // Identity + call_depth live on the op (captured on the
      // main loop at cap.call enqueue time). No bc.user /
      // bc.call_depth read inside this detached coroutine arm.
      outcome = co_await run_cap_call_outcome(std::move(op));
      break;
    case AsyncOp::Type::PUBSUB_PUBLISH:
      outcome = co_await run_pubsub_publish_outcome(std::move(op));
      break;
    case AsyncOp::Type::HTTP_REQUEST:
    case AsyncOp::Type::STORAGE_GET:
    case AsyncOp::Type::STORAGE_PUT:
      outcome =
          std::unexpected(PromiseRejection{.code = "internal",
                                           .message = "unimplemented async op",
                                           .sqlstate = std::nullopt});
      break;
    case AsyncOp::Type::PUBSUB_SUBSCRIBE:
    case AsyncOp::Type::PUBSUB_UNSUBSCRIBE:
      // Dispatched inline by `dispatch_ops_batch_fanout` before
      // this detached path is ever reached; surfacing one here
      // means an op escaped the inline gate — treat as internal.
      outcome = std::unexpected(PromiseRejection{
          .code = "internal",
          .message = "pubsub subscribe op reached detached dispatch",
          .sqlstate = std::nullopt});
      break;
    case AsyncOp::Type::DB_BATCH_BEGIN:
      // Batch arms manage their own queueInLoop + bc mutation
      // via finalize_batch — short-circuit the common tail.
      co_await handle_db_batch_begin(bc, std::move(op), main_loop);
      co_return;
    case AsyncOp::Type::DB_BATCH_COMMIT:
      co_await handle_db_batch_commit(bc, std::move(op), main_loop);
      co_return;
    case AsyncOp::Type::DB_BATCH_ROLLBACK:
      co_await handle_db_batch_rollback(bc, std::move(op), main_loop);
      co_return;
  }

  // 0.3.5 result-size check — success outcomes only. Failure paths
  // already carry their own rejection; over-cap success gets
  // converted to a rejection here before crossing back to the main
  // loop.
  if (outcome.has_value()) {
    auto rej = check_result_size(bc, outcome.value());
    if (rej.has_value()) {
      outcome = std::unexpected(std::move(*rej));
    }
  }

  main_loop->queueInLoop([&bc, cb_id, outcome = std::move(outcome)]() mutable {
    if (outcome.has_value()) {
      bc.resolve(cb_id, outcome.value());
    } else {
      bc.reject(cb_id, outcome.error());
    }
    bc.inflight_detached.fetch_sub(1, std::memory_order_acq_rel);
    bc.signal_completion();
  });
}

// AnyCompletionAwaiter — suspends the outer run_on_context coroutine
// until any detached task completes (i.e. its queueInLoop callback
// calls `bc.signal_completion()`). Accumulated signals (wake_count)
// short-circuit the suspend so a completion that lands between a prior
// await and the next await isn't lost. The mutex inside BridgeContext
// makes the state-transition atomic between the waiter and signaler.
struct AnyCompletionAwaiter {
  // is a short-lived stack object co_awaited once; pointer semantics are
  // equivalent but noisier here.
  BridgeContext& bc;

  auto await_ready() noexcept -> bool {
    std::lock_guard<std::mutex> g{bc.wake_mu};
    if (bc.wake_count > 0) {
      bc.wake_count = 0;
      return true;
    }
    return false;
  }

  auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
    std::lock_guard<std::mutex> g{bc.wake_mu};
    // Late signal — completed between await_ready and await_suspend.
    if (bc.wake_count > 0) {
      bc.wake_count = 0;
      return false; // don't suspend
    }
    bc.waiter_handle = h;
    return true; // suspend
  }

  auto await_resume() noexcept -> void {}
};

// Cancellation cascade (ICD §Cancellation Cascade): drain in-flight
// detached tasks up to a 5-second ceiling, then reject every
// outstanding promise and flush the job queue so QuickJS can release
// its internal promise state cleanly. The caller is contractually
// required to route the returned BridgeContext to
// RuntimePool::destroy(), not release().
//
// 0.3.3.1 upgrade: drain is now "wait for AnyCompletionAwaiter until
// inflight_detached reaches 0 OR 5 s elapses", not a blind 5 s sleep.
// This closes the BridgeContext-UAF window that parallel fan-out
// would otherwise open (outer returns while detached tasks still
// hold `&bc`). The 5 s ceiling is retained — matches the upper bound
// 0.3.3 already documented; longer waits risk deadlock if a detached
// task is stuck inside a PG operation that libpq-level cancel can't
// preempt. After the ceiling, we bail anyway — same accepted-risk
// posture as 0.3.3.
// outlives the coroutine.
auto run_cancellation_cascade(BridgeContext& bc, steady_clock::time_point start)
    -> drogon::Task<EvalResult> {
  auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  auto cancel_deadline = steady_clock::now() + std::chrono::seconds(5);

  // Step 3: drain detached tasks. Wake-driven — signal_completion
  // decrements inflight_detached and fires the AnyCompletionAwaiter.
  // We walk the clock to bound the wait, since Drogon's PG client
  // can't preempt a query that libpq has already dispatched.
  while (bc.inflight_detached.load(std::memory_order_acquire) > 0) {
    if (steady_clock::now() >= cancel_deadline) {
      break;
    }
    co_await AnyCompletionAwaiter{bc};
  }

  // Step 4: reject every outstanding promise so QuickJS releases its
  // internal Promise state. We snapshot the keys so the iteration
  // doesn't trip over reject()'s erase-from-map.
  std::vector<int> outstanding;
  outstanding.reserve(bc.callbacks.size());
  for (const auto& [id, _] : bc.callbacks) {
    outstanding.push_back(id);
  }
  for (int id : outstanding) {
    // Look up ns to choose the right cancellation code.
    auto it = bc.callbacks.find(id);
    if (it == bc.callbacks.end()) {
      continue;
    }
    std::string ns = it->second.ns_for_cancellation;
    if (ns.empty()) {
      ns = "internal";
    }
    bc.reject(id, PromiseRejection{.code = ns + ".cancelled",
                                   .message = "execution cancelled",
                                   .sqlstate = std::nullopt});
  }
  bc.pending_ops.clear();

  // Drive the job queue so QuickJS processes the rejections.
  bc.resume_cpu_timer();
  while (JS_IsJobPending(bc.rt)) {
    JSContext* pctx = bc.ctx;
    // Best-effort: a reject's .then(undefined, handler) might throw,
    // but at this point we just need to drain the queue.
    JS_ExecutePendingJob(bc.rt, &pctx);
  }
  bc.pause_cpu_timer();

  // Suppress the unused-variable warning when `loop` was only needed
  // for a since-removed sleepCoro call on gcc older tidies.
  (void)loop;

  auto kind = bc.wall_clock_exceeded() ? EvalErrorKind::WALL_CLOCK_EXCEEDED
                                       : EvalErrorKind::CANCELLED;
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      steady_clock::now() - start);
  co_return EvalResult{
      .value = std::unexpected(
          EvalError{.kind = kind,
                    .message = "execution cancelled after 5s abandon window",
                    .line = 0,
                    .column = 0}),
      .duration = duration};
}

// Return true iff the top-level `result` is a pending promise.
auto top_result_pending(const BridgeContext& bc, JSValue result) -> bool {
  if (JS_IsException(result)) {
    return false;
  }
  JSPromiseStateEnum st = JS_PromiseState(bc.ctx, result);
  return st == JS_PROMISE_PENDING;
}

// Re-queue ops[i..] at the head of bc.pending_ops in FIFO order.
auto requeue_remainder(BridgeContext& bc, std::vector<AsyncOp>& ops,
                       std::size_t start_idx) -> void {
  std::vector<AsyncOp> new_pending;
  new_pending.reserve((ops.size() - start_idx) + bc.pending_ops.size());
  for (std::size_t j = start_idx; j < ops.size(); ++j) {
    new_pending.push_back(std::move(ops[j]));
  }
  for (auto& p : bc.pending_ops) {
    new_pending.push_back(std::move(p));
  }
  bc.pending_ops = std::move(new_pending);
}

// Fire-and-forget fan-out dispatch. Each op up to the back-pressure
// cap becomes a detached `drogon::async_run` task; the outer coroutine
// returns immediately (no `co_await`). The detached task's queueInLoop
// callback on completion is what drives the outer's
// AnyCompletionAwaiter — see `dispatch_async_op_detached`.
//
// ASYNC_CONCURRENCY_LIMIT (max==0) rejects each queued op inline with
// `async.concurrency_limit` — same 0.3.3 behavior, just shifted to the
// fan-out loop.
// ICD-0.5.2 §pubsub.subscribe — inline handler for the synchronous
// PUBSUB_SUBSCRIBE / PUBSUB_UNSUBSCRIBE ops. Both run on the bc's loop
// (dispatch_ops_batch_fanout is invoked from `run_on_context`, which
// co-awaits on the main loop). Registration + deregistration are
// in-memory broker mutations; the subscribe arm also builds the JS-
// side unsubscribe function (JSValue lifetime bound to bc's runtime).
auto dispatch_pubsub_sub_inline(BridgeContext& bc, const AsyncOp& op) -> void {
  // ICD-0.5.2 §Security Constraint 2 — defense-in-depth re-check.
  // The caller's `effective_rules` / `broker::is_rbac_enforced()`
  // could have shifted between the inline classify gate and this
  // dispatch arm (group revocation, config flip). Re-run the same
  // gate; on denial, fire the `dispatch_skipped` audit, roll back
  // the `persistent_callbacks` entry, and reject the promise —
  // parallel to the quota-overflow arm below.
  if (auto rejection = classify_pubsub_subscribe(op.pubsub_channel, bc);
      !rejection.empty()) {
    plinth::realtime::broker::note_dispatch_skipped(op.pubsub_channel,
                                                    "rbac_denied");
    auto it = bc.persistent_callbacks.find(op.pubsub_channel);
    if (it != bc.persistent_callbacks.end()) {
      JS_FreeValue(bc.ctx, it->second);
      bc.persistent_callbacks.erase(it);
    }
    bc.reject(op.callback_id,
              PromiseRejection{.code = std::move(rejection),
                               .message = "pubsub.subscribe re-check denied: " +
                                          op.pubsub_channel,
                               .sqlstate = std::nullopt});
    return;
  }
  if (!plinth::realtime::broker::register_js_subscription(
          &bc, op.pubsub_channel, op.callback_id)) {
    // Quota overflow — persistent_callbacks was already populated by
    // the binding's gauntlet. Roll that back so state stays
    // consistent with the broker's "no subscription" view.
    auto it = bc.persistent_callbacks.find(op.pubsub_channel);
    if (it != bc.persistent_callbacks.end()) {
      JS_FreeValue(bc.ctx, it->second);
      bc.persistent_callbacks.erase(it);
    }
    bc.reject(op.callback_id,
              PromiseRejection{.code = "pubsub.quota_exceeded",
                               .message = "broker subscription quota exceeded",
                               .sqlstate = std::nullopt});
    return;
  }
  JSValue unsub_fn = make_unsubscribe_function(bc.ctx, op.pubsub_channel);
  bc.resolve_with_js_value(op.callback_id, unsub_fn);
}

auto dispatch_pubsub_unsub_inline(BridgeContext& bc, const AsyncOp& op)
    -> void {
  plinth::realtime::broker::unregister_js_subscription(&bc, op.pubsub_channel);
  auto it = bc.persistent_callbacks.find(op.pubsub_channel);
  if (it != bc.persistent_callbacks.end()) {
    JS_FreeValue(bc.ctx, it->second);
    bc.persistent_callbacks.erase(it);
  }
  bc.resolve(op.callback_id, Json::Value{});
}

auto dispatch_ops_batch_fanout(BridgeContext& bc, std::vector<AsyncOp> ops,
                               trantor::EventLoop* main_loop) -> std::size_t {
  std::size_t i = 0;
  for (; i < ops.size(); ++i) {
    // Synchronous broker ops — run inline on the main loop (JSValue
    // construction + broker registry mutation are both trivial and
    // don't need drogon::async_run). Resolved before the loop
    // re-enters JS, preserving FIFO semantics with adjacent ops.
    if (ops[i].type == AsyncOp::Type::PUBSUB_SUBSCRIBE) {
      dispatch_pubsub_sub_inline(bc, ops[i]);
      continue;
    }
    if (ops[i].type == AsyncOp::Type::PUBSUB_UNSUBSCRIBE) {
      dispatch_pubsub_unsub_inline(bc, ops[i]);
      continue;
    }
    if (bc.max_concurrent_async_ops == 0) {
      bc.reject(ops[i].callback_id,
                PromiseRejection{.code = "async.concurrency_limit",
                                 .message = "max_concurrent_async_ops is 0",
                                 .sqlstate = std::nullopt});
      continue;
    }
    if (bc.concurrent_async_ops >= bc.max_concurrent_async_ops) {
      break;
    }
    ++bc.concurrent_async_ops;
    bc.inflight_detached.fetch_add(1, std::memory_order_acq_rel);
    drogon::async_run(
        // `bc` lifetime guarded by cancellation cascade's inflight-drain; `op`
        // moved into lambda; `main_loop` pointer-stable.
        [&bc, op = std::move(ops[i]), main_loop]() mutable -> drogon::Task<> {
          co_await dispatch_async_op_detached(bc, std::move(op), main_loop);
        });
  }
  // Requeue unprocessed ops[i..] before returning.
  if (i < ops.size()) {
    requeue_remainder(bc, ops, i);
  }
  return i;
}

} // namespace

// header: bc outlives the coroutine (owned by RuntimePool).
auto run_on_context(BridgeContext& bc, std::string_view source)
    -> drogon::Task<EvalResult> {
  auto start = steady_clock::now();
  bc.execution_start = start;
  bc.cpu_time_accumulated = std::chrono::nanoseconds{0};
  bc.cpu_timer_start = {};
  bc.next_callback_id = 0;
  bc.concurrent_async_ops = 0;
  bc.inflight_detached.store(0, std::memory_order_release);
  {
    std::lock_guard<std::mutex> g{bc.wake_mu};
    bc.wake_count = 0;
    bc.waiter_handle = {};
  }

  // Capture the loop this coroutine starts on. Detached per-op
  // dispatchers will `queueInLoop` back to this loop to invoke
  // `bc.resolve` / `bc.reject` and wake the outer coroutine, so it
  // must be stable for the lifetime of run_on_context. We fall back
  // to drogon::app().getLoop() if we're not on a Drogon loop yet
  // (e.g. drogon::sync_wait on a test thread).
  auto* main_loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  if (main_loop == nullptr) {
    main_loop = drogon::app().getLoop();
  }

  // 0.5.5.2 — Pin every JSValue / JSContext touch to `main_loop`.
  // The kernel-loop invariant (run_on_context.cpp:9-12) is "QuickJS
  // access is serialized inside this coroutine body. No background
  // thread touches bc.rt / bc.ctx." That holds when production HTTP
  // handlers `co_await run_on_context` from a coroutine on the
  // request's owning loop. Test fixtures driving via
  // `drogon::sync_wait(run_on_context(...))` instead spawn a fresh
  // `std::thread` for the synchronous prefix (`JS_Eval` + first
  // `drive_jobs` + dispatch). When a dispatched op completes faster
  // than the prefix reaches `co_await AnyCompletionAwaiter`, the
  // completion's queueInLoop fires on `main_loop`'s thread while the
  // prefix is still touching JSValues on the sync_wait thread —
  // breaking the invariant and producing the `[js][async]`
  // `free_zero_refcount` / `list_empty(&rt->gc_obj_list)` family of
  // teardown asserts. LH-0.1 / LH-1 zero-reproductions confirm the
  // production lifecycle is clean. The hop is a no-op when we're
  // already on `main_loop`'s thread (production path).
  //
  // Companion fix at the AnyCompletionAwaiter site below: when this
  // hop pulls the dispatcher onto `main_loop`, the loop must not
  // spin under back-pressure (it would starve the completion
  // callbacks it's waiting on). The condition has been widened to
  // suspend whenever no progress can be made this iteration.
  if (!main_loop->isInLoopThread()) {
    co_await drogon::switchThreadCoro(main_loop);
  }

  // QuickJS uses a thread-local stack base for stack-overflow
  // detection. The runtime was likely created on a different thread
  // (RuntimePool ctor / acquire site) than the one this coroutine
  // resumes on (Drogon event-loop thread), so we re-anchor the
  // stack base here. Without this, JS_Eval misclassifies any work
  // as STACK_OVERFLOW.
  JS_UpdateStackTop(bc.rt);

  // ICD-0.4.1 Layer 2 — pre-`JS_Eval` GlassWorm gate. Runs before
  // the CPU-timer bracket per ICD §Layer 2 / Pass ordering. Wraps
  // the EvalError into the EvalResult shape this coroutine returns.
  if (auto err = pre_eval_scan(source, "<async>"); err.has_value()) {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        steady_clock::now() - start);
    co_return EvalResult{.value = std::unexpected(std::move(*err)),
                         .duration = duration};
  }

  bc.resume_cpu_timer();
  JSValue result = JS_Eval(bc.ctx, source.data(), source.size(), "<extension>",
                           JS_EVAL_TYPE_GLOBAL);
  bc.pause_cpu_timer();
  // 0.5.5.2: latch the memory peak immediately after JS_Eval so a
  // synchronous OOM (e.g. tight `new Promise(()=>{})` allocation
  // loop) classifies as MEMORY_LIMIT instead of falling through to
  // RUNTIME_ERROR. Pre-0.5.5.2 the latch was only updated from
  // `drive_jobs` (between JS jobs); for a synchronous IIFE that
  // exhausts the heap inside `JS_Eval` the latch never sets, the
  // post-OOM classifier reads an empty `name`/`message` (the
  // property-read path itself OOMs), and `is_runtime_near_memory_
  // limit` may return false because QuickJS has already reclaimed
  // the failed allocation by the time we sample. Sampling here
  // captures the pre-reclaim peak.
  detail::sample_memory_peak(bc);

  // If JS_Eval threw (and we have nothing to drive), short-circuit.
  if (JS_IsException(result) && !bc.has_pending_ops() &&
      !JS_IsJobPending(bc.rt)) {
    co_return finalize(bc, result, start);
  }

  // Loop: dispatch pending AsyncOps, drive JS job queue, check
  // cancellation. Continues until JS execution stalls (no pending
  // ops, no in-flight detached tasks, no jobs, top-level value
  // either ready or settled-promise).
  while (bc.has_pending_ops() || JS_IsJobPending(bc.rt) ||
         top_result_pending(bc, result) ||
         bc.inflight_detached.load(std::memory_order_acquire) > 0) {
    // 1. Fire-and-forget dispatch of any newly queued async ops
    //    (respecting back-pressure).
    dispatch_ops_batch_fanout(bc, bc.take_pending_ops(), main_loop);

    // 2. If there are detached tasks in flight and nothing else
    //    to drive right now, suspend on the completion awaiter so
    //    detached-task `queueInLoop` resolves can wake us.
    //
    //    0.5.5.2: also suspend when pending_ops are blocked by
    //    back-pressure (concurrent_async_ops at cap). Without this,
    //    the loop spins on `dispatch_ops_batch_fanout`-no-progress
    //    + `drive_jobs`-no-jobs while waiting for an inflight to
    //    free a slot. The original `!has_pending_ops` guard only
    //    held when the dispatch loop ran on a different thread than
    //    main_loop (the test fixture's `drogon::sync_wait` spawned a
    //    worker thread for the dispatch prefix, so main_loop stayed
    //    free to fire completions). When the dispatcher and
    //    main_loop share a thread (production HTTP handler path,
    //    or the test fixture's main-loop-driven path that 0.5.5.2
    //    moves all tests to), the spin starves completions and
    //    deadlocks. The back-pressure check here closes that gap.
    bool back_pressured =
        bc.has_pending_ops() &&
        bc.concurrent_async_ops >= bc.max_concurrent_async_ops;
    if (bc.inflight_detached.load(std::memory_order_acquire) > 0 &&
        !JS_IsJobPending(bc.rt) && (!bc.has_pending_ops() || back_pressured)) {
      co_await AnyCompletionAwaiter{bc};
    }

    // 3. Resume JS to process resolved promises (.then handlers).
    if (auto err = drive_jobs(bc); err.has_value()) {
      JS_FreeValue(bc.ctx, result);
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          steady_clock::now() - start);
      co_return EvalResult{.value = std::unexpected(*err),
                           .duration = duration};
    }

    // 4. Cancellation / wall-clock check.
    if (bc.cancelled.load(std::memory_order_acquire) ||
        bc.wall_clock_exceeded()) {
      JS_FreeValue(bc.ctx, result);
      co_return co_await run_cancellation_cascade(bc, start);
    }
  }

  co_return finalize(bc, result, start);
}

} // namespace plinth::js
