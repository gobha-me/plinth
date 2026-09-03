#pragma once

// plinth::js — pre_eval_scan: GlassWorm Layer 2 pre-`JS_Eval` gate.
//
// Contract: ICD-0.4.1 §Layer 2 — QuickJS Source-Load Gate. Each `JS_Eval`
// call site invokes pre_eval_scan with the host-submitted source bytes
// before the runtime sees them; if the scan exceeds the configured
// threshold (or hits malformed UTF-8 with strict_utf8=true), the gate
// returns a populated EvalError and the call site MUST NOT call
// JS_Eval. Per §Security Constraint 5, a future refactor that moves
// the scan after JS_Eval (even for measurement) breaks the model.
//
// Scanner config (security.unicode_scanner.{enabled,threshold,...}) is
// process-global per DESIGN §5.1 — uniform system-wide policy. Set
// once at kernel boot via set_unicode_scanner_policy() from the
// loaded plinth::Config; tests that need to override the defaults
// call the same setter.

#include "kernel/js/eval.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace plinth::js {

// Returns std::nullopt on clean source; populated EvalError on threshold
// exceed or UTF-8 decode failure. The caller propagates the unexpected
// to its own return path.
//
// `source_label` identifies the call site for audit-event attribution
// and error-message formatting. Standard labels: "<eval>" (one-shot),
// "<pool>" (RuntimePool::eval_on_context), "<async>" (run_on_context
// coroutine).
[[nodiscard]] auto pre_eval_scan(std::string_view src,
                                 std::string_view source_label)
    -> std::optional<EvalError>;

// Set the process-global scanner policy. Called from kernel main on
// boot after `plinth::log::init(cfg)`; tests call this to override
// defaults (e.g., disable the gate for a test that wants to evaluate
// known-noisy sources). Threshold of 0 is clamped to 1 — a literal
// 0 would let a single legitimate emoji trip the gate, which is the
// opposite of the operator's intent when they set a low value.
auto set_unicode_scanner_policy(bool enabled, std::size_t threshold,
                                bool log_findings) -> void;

} // namespace plinth::js
