#pragma once

// plinth::capabilities — kernel capability bootstrap.
//
// Runs once during startup, after bootstrap_schema() and bootstrap_groups()
// but before Drogon begins accepting connections. Seeds the minimum set of
// kernel-owned RBAC rules and the corresponding kernel capabilities defined
// by ICD-0.2.0 §Bootstrap: Kernel Capabilities. Idempotent — safe to call
// on every startup.

#include "kernel/config.hpp"

namespace plinth::capabilities {

// Insert kernel RBAC rules (idempotent) and kernel capabilities
// (idempotent) via direct libpq, then emit rbac.rule_registered +
// capability.registered audit events for each NEW row. Reentry is a
// no-op — audit events are guarded by a probe read (same pattern
// groups::bootstrap_groups uses for kernel.admin).
//
// NOTIFY is not emitted during bootstrap. At this point no node has yet
// attached a listener connection (the listener is implemented in 0.2.3)
// so the notification would be dropped anyway, and on first startup the
// table is empty so there is no cache to invalidate.
auto bootstrap_kernel_capabilities(const Config::Database& db_cfg) -> void;

} // namespace plinth::capabilities
