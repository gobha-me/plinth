#pragma once

// plinth::capabilities — cap.batch() dispatch (ICD-0.2.2 §cap.batch()
// Behavioral Contract, milestone 0.2.5).
//
// batch_call_capability() fans a vector of CapabilityCall out through
// the standard resolver. Each element walks the full pipeline in
// resolution.cpp (signature parse → depth check → RBAC → tier lookup →
// dispatch) — no shortcuts, no pre-flighting. Behavioral guarantees:
//
//   1. Pipeline-per-call. Each element is resolved independently.
//   2. Order-preserving. On success, result[i] corresponds to call[i].
//   3. Fail-fast-discard-priors. The first element to return an error
//      aborts the batch; any successful results accumulated before it
//      are discarded (matches Promise.all rejection semantics).
//   4. Shared call depth. Every element dispatches at the same depth,
//      taken from calls[0].call_depth (or 0 for an empty batch). Per-
//      element call_depth fields are ignored — the ICD specifies that
//      the whole batch shares the caller's depth.
//
// ── Deviations from ICD-0.2.2 (continues the 0.2.0 / 0.2.2 / 0.2.4
// accepted-deviation precedent, tracked in CHANGELOG)
//
//   1. Sequential dispatch, not threaded. The ICD permits "concurrently
//      where possible" but also: "The initial implementation may simply
//      expand to Promise.all(calls.map(c => cap.call(...c)))", and
//      DESIGN-quickjs-bridge.md §7.3 classifies concurrency as "an
//      optimization opportunity, not a correctness requirement". The
//      kernel has no thread pool today and no caller demanding
//      parallelism — this will be revisited alongside 0.2.6 (async
//      wrapper) or 0.3.3 (first JS Promise.all user).
//
//   2. Tier-3 multiplexing is a no-op. DESIGN-quickjs-bridge.md §7.3
//      explicitly defers same-node Tier 3 bundling to milestone 0.8.

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"

#include <cstddef>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

namespace plinth::capabilities {

// Outcome of batch_call_capability(). Exactly one of `values` / `error`
// is engaged, mirroring ResolveResult. On error, `failed_index` points
// at the element of the input vector that aborted the batch (useful
// for downstream error reporting and audit debugging). On success
// `failed_index` is unspecified and should be ignored.
struct BatchResult {
  std::optional<std::vector<CapabilityResult>> values = std::nullopt;
  std::optional<CapabilityError> error = std::nullopt;
  std::size_t failed_index = 0;
};

// Batch dispatch entry point. Runs each CapabilityCall through
// call_capability() in input order, collecting results until one
// fails. See header comment for the full behavioral contract.
//
// Empty input → BatchResult{ .values = std::vector<CapabilityResult>{} }
// (successful zero-result batch), consistent with Promise.all([])
// resolving to [].
auto batch_call_capability(std::vector<CapabilityCall> calls,
                           const UserContext& ctx) -> BatchResult;

// Async (coroutine) wrapper around batch_call_capability — see
// ICD-0.2.6-async-dispatch.md. The body is `co_return
// batch_call_capability(calls, ctx)` today; semantic identity with the
// sync form is the contract. Lands alongside the QuickJS async bridge
// in 0.3.3 as the first coroutine-shaped caller.
//
// `ctx` outlives the coroutine; `calls` is taken by value and forwarded to the
// sync form per existing batch_call_capability shape.
auto batch_call_capability_async(std::vector<CapabilityCall> calls,
                                 const UserContext& ctx)
    -> drogon::Task<BatchResult>;

} // namespace plinth::capabilities
