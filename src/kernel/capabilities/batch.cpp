#include "kernel/capabilities/batch.hpp"
#include "kernel/capabilities/resolution.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace plinth::capabilities {

auto batch_call_capability(std::vector<CapabilityCall> calls,
                           const UserContext& ctx) -> BatchResult {
  // Empty batch → success with an empty result vector. Matches
  // Promise.all([]) which resolves to []. Keeps downstream JS
  // code from needing a special-case branch in 0.3.x.
  if (calls.empty()) {
    return BatchResult{
        .values = std::vector<CapabilityResult>{},
        .error = std::nullopt,
        .failed_index = 0,
    };
  }

  // Per ICD-0.2.2 §cap.batch() "Call depth is shared across the
  // batch — all calls in a batch inherit the same call_depth from
  // the caller." We normalize to calls[0].call_depth rather than
  // trusting per-element values, which keeps the "batch is one hop
  // from the caller" semantics honest even if the caller set
  // calls[i].call_depth inconsistently.
  auto shared_depth = calls.front().call_depth;

  std::vector<CapabilityResult> values;
  values.reserve(calls.size());

  for (std::size_t i = 0; i < calls.size(); ++i) {
    calls[i].call_depth = shared_depth;
    auto out = call_capability(calls[i], ctx);
    if (!out.has_value()) {
      // Fail-fast: discard any priors, report the failing index.
      // ResolveResult is std::expected<CapabilityResult,
      // CapabilityError>, so a missing value guarantees
      // `out.error()` is engaged.
      return BatchResult{
          .values = std::nullopt,
          .error = out.error(),
          .failed_index = i,
      };
    }
    values.push_back(std::move(*out));
  }

  return BatchResult{
      .values = std::move(values),
      .error = std::nullopt,
      .failed_index = 0,
  };
}

// header: ctx outlives the coroutine.
auto batch_call_capability_async(std::vector<CapabilityCall> calls,
                                 const UserContext& ctx)
    -> drogon::Task<BatchResult> {
  // Per ICD-0.2.6: thin coroutine wrapper. No suspension point yet —
  // batch_call_capability iterates synchronously through the resolver.
  co_return batch_call_capability(std::move(calls), ctx);
}

} // namespace plinth::capabilities
