// SPDX-License-Identifier: MIT
//
// AsyncOp / PromiseCallbacks / PromiseRejection — the per-AsyncOp
// state types consumed by the QuickJS coroutine bridge.
// See ICD-0.3.3-async-bridge.md §AsyncOp Contract.

#pragma once

#include "kernel/capabilities/resolution.hpp"

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <memory>
#include <optional>
#include <quickjs.h>
#include <string>
#include <vector>

namespace plinth::js {

// Owning pair of QuickJS callable references obtained from
// JS_NewPromiseCapability. Each JSValue carries refcount 1 on
// construction (no JS_DupValue at registration time — ICD-0.3.3
// §BridgeContext Async Activation). The bridge MUST JS_FreeValue both
// after the resolve/reject path's JS_Call returns, regardless of
// JS_Call's exception state.
//
// `ns_for_cancellation` lets the cancellation cascade reject every
// outstanding promise with a per-namespace error code (e.g.
// "db.cancelled" vs "audit.cancelled" — see ICD §Cancellation Cascade).
// Bindings populate it at register_pending() time.
struct PromiseCallbacks {
  JSValue resolve{};
  JSValue reject{};
  std::string ns_for_cancellation; // "db" | "audit" | "cap"
};

// Promise rejection envelope passed to BridgeContext::reject. The
// JS-visible shape is `{code, message[, sqlstate]}` — see ICD-0.3.3
// §Promise Rejection Shape (db.*) / §Rejection Shape (audit.*). The
// optional sqlstate is populated only for driver-origin db.* errors.
struct PromiseRejection {
  std::string code;
  std::string message;
  std::optional<std::string> sqlstate;
};

// AsyncOp — the work item enqueued by a JS binding and dispatched by
// the coroutine loop in run_on_context.
//
// 0.3.3 implements DB_QUERY / DB_EXEC / AUDIT_WRITE. The remaining
// enum variants are reserved for future milestones (see comments) so
// the enum layout is stable, but the dispatch switch in 0.3.3 rejects
// any non-implemented variant with EvalErrorKind::INTERNAL_ASYNC.
struct AsyncOp {
  enum class Type : std::uint8_t {
    DB_QUERY,
    DB_EXEC,
    AUDIT_WRITE,
    // Reserved (no dispatch arm in 0.3.3):
    HTTP_REQUEST,   // 0.10.3
    CAP_CALL,       // 0.3.4
    STORAGE_GET,    // 0.10.x
    STORAGE_PUT,    // 0.10.x
    PUBSUB_PUBLISH, // 0.5.x
    // ICD-0.5.2 §pubsub.subscribe JS Binding — synchronous (no
    // DB work); dispatched inline on the bc's loop rather than
    // via drogon::async_run. PUBSUB_SUBSCRIBE registers the
    // (bc, channel) subscription with the broker + resolves the
    // promise with a JS-side unsubscribe function. PUBSUB_UNSUBSCRIBE
    // drops the subscription + resolves with undefined.
    PUBSUB_SUBSCRIBE,
    PUBSUB_UNSUBSCRIBE,
    // ICD-0.5.3 §`db.batch()` Transactional Wrapper — three-arm
    // orchestration for the transactional batch wrapper. BEGIN
    // allocates a pinned TransactionPtr + runs SET LOCAL
    // search_path on the pinned connection + resolves the
    // begin-promise. COMMIT issues COMMIT on the pinned conn,
    // flushes the coalescer scope, fires the committed audit,
    // resolves the outer promise. ROLLBACK issues ROLLBACK,
    // discards the coalescer scope, fires the rolled_back audit,
    // rejects the outer promise with the carried error.
    DB_BATCH_BEGIN,
    DB_BATCH_COMMIT,
    DB_BATCH_ROLLBACK,
  };

  Type type;
  int callback_id = 0;

  // Operation-specific payload — populated per `type`. Unused fields
  // remain default-initialised. See ICD-0.3.3 §AsyncOp Contract.
  std::string sql;                     // DB_QUERY, DB_EXEC
  std::vector<Json::Value> sql_params; // DB_QUERY, DB_EXEC (optional)
  bool silent = false;          // DB_EXEC; carried for 0.5.x realtime hook
  std::string audit_event_type; // AUDIT_WRITE
  Json::Value audit_payload;    // AUDIT_WRITE
  // AUDIT_WRITE identity snapshot. The binding captures these on the owning
  // JS loop before the operation moves to a detached worker, so audit rows
  // cannot lose or substitute the authenticated caller identity.
  std::string audit_user_id;
  std::string audit_session_id;
  std::string audit_ip_address;
  std::string
      cap_signature; // CAP_CALL — "ns:v:fn" forwarded verbatim to the resolver
  Json::Value
      cap_args; // CAP_CALL — forwarded verbatim; any JSON shape permitted
  // CAP_CALL identity + depth snapshot — captured at enqueue time on
  // the main loop thread where bc is authoritative. The detached
  // task reads these off the op (value-copy) rather than reaching
  // back into bc, which removes a worker-thread data dependency on
  // bc.user / bc.call_depth. Security Constraint 1 (caller's depth is
  // the single source of truth) and Constraint 3 (UserContext is
  // immutable across dispatch) are preserved — the op carries a
  // snapshot of bc's values at the moment cap.call was invoked.
  plinth::capabilities::UserContext cap_user{
      plinth::capabilities::UserContext::anonymous()};
  int cap_call_depth = 0;

  // ICD-0.5.0 PUBSUB_PUBLISH payload — `pubsub_channel` is the
  // full logical channel (Layer-3, `plinth:ext:<extension>:...`);
  // `pubsub_payload` is the caller's extension-supplied JSON value.
  // The dispatch arm composes the final envelope
  // {layer: "extension", channel, payload} at run time.
  std::string pubsub_channel;
  Json::Value pubsub_payload;

  // ICD-0.5.1 §Registry + Integration Point — extension identity
  // snapshotted at enqueue time for the coalescer's (schema, table)
  // window ownership. Mirrors the 0.3.4 `cap_user`/`cap_call_depth`
  // snapshot pattern: the detached task reads the value off the op
  // rather than reaching back into `bc`. Empty for kernel-scope
  // DB_EXEC (no coalescence intended there).
  std::string bc_extension_name;

  // ICD-0.5.3 §`db.batch()` §AsyncOp additions — zero means "not
  // in a batch". Populated by the db.batch binding's scope
  // allocator (monotonic uint64). DB_QUERY / DB_EXEC enqueued while
  // `bc.batch_state.depth > 0` carry the scope id so:
  //   - the dispatch arm routes execution through the pinned conn
  //   - the coalescer tags accumulated writes under the scope
  //     (no timer flush; released at COMMIT)
  std::uint64_t batch_scope_id = 0;

  // ICD-0.5.3 §`db.batch()` §ROLLBACK orchestration — the error to
  // propagate out of the outer batch promise when the user callback
  // rejects or a db-level failure triggers rollback. Empty for
  // successful batches.
  Json::Value rollback_error;

  // ICD-0.5.3 §`db.batch()` §Connection pinning — shared_ptr
  // snapshot of the pinned Drogon transaction created by the
  // DB_BATCH_BEGIN dispatch arm. Captured by the db.exec / db.query
  // bindings at enqueue time (when bc.batch_state.depth > 0) so the
  // outcome helpers can route through this client without reaching
  // back into bc. Mirrors the existing bc_extension_name /
  // cap_user snapshot pattern. Non-null iff `batch_scope_id != 0`.
  std::shared_ptr<drogon::orm::DbClient> batch_pinned_conn;
};

} // namespace plinth::js
