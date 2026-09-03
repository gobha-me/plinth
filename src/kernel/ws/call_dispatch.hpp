#pragma once

// WS `call` dispatch per ICD-LH-0 §WS Protocol Addition. Migrated in
// 0.5.0.4 (ICD-0.5.0.3 §Sync vs async) to `call_capability_async` so
// `provider_type == "extension"` entries dispatch through the
// `plinth::extensions::RuntimeRegistry`. Kernel-Tier-1 caps still
// resolve in-memory — the coroutine adds only a schedule hop for them.
//
// Surfaces the `plinth::capabilities::call_capability_async` entry
// through the /ws/events transport. The harness (and future
// kernel clients that speak WS) send
//
//     {"type":"call","id":"<client-id>","signature":"ns:v:fn","args":[...]}
//
// and receive either
//
//     {"type":"call_result","id":"<client-id>","value":<handler-payload>,
//      "resolved_tier":"tier1|tier2","provider_type":"kernel|extension|sidecar"}
//
// or
//
//     {"type":"call_error","id":"<client-id>","code":"<error_code>",
//      "message":"<human-readable>"}
//
// RBAC model: the `rbac_rule` stored on the Tier 1/Tier 2 entry governs
// access. ConnState only carries `is_admin` (populated in auth_flow),
// so for LH-0 the effective_rules vector is synthesized as
// `{"kernel.admin"}` for admin users and empty for everyone else. The
// resolver's `kernel.admin` universal-match (ICD-0.2.4) then gates
// admin access; non-admin WS callers cannot invoke any RBAC-gated
// capability. Widening the ConnState rule set is tracked as a future
// extension, not LH-0 scope.

#include <drogon/WebSocketConnection.h>
#include <json/value.h>

namespace plinth::ws {

// Handle a `{type:"call", ...}` frame. Spawns an owned coroutine whose
// process-lifecycle lease is drained before runtime/database teardown. It
// awaits plinth::capabilities::call_capability_async and writes the response
// frame on the caller's own WS connection. No-op if the connection is
// not authenticated (silent per the "error frames only where
// meaningful" convention of the ICD-0.1.6 protocol).
auto on_call(const drogon::WebSocketConnectionPtr& conn, const Json::Value& msg)
    -> void;

} // namespace plinth::ws
