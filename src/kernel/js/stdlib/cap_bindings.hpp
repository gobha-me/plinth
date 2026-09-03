// SPDX-License-Identifier: MIT
//
// `cap.*` host bindings — shared declarations consumed by the CAP_CALL
// dispatch arm in run_on_context.cpp. The two binding entrypoints
// (cap.call / cap.batch) live in cap_bindings.cpp's anonymous namespace;
// only the error-mapping helper is exported because the dispatch arm
// needs to translate `CapabilityError` → JS-visible `cap.*` rejection.
//
// See ICD-0.3.4-cap-call-from-js.md §Error Mapping for the contract.

#pragma once

#include "kernel/capabilities/types.hpp"
#include "kernel/js/async_op.hpp"

#include <string_view>

namespace plinth::js {

// Map a dispatch-time `CapabilityError` to the JS-visible
// `{code, message, sqlstate=nullopt}` rejection envelope. `signature`
// is appended to `message` for operator-debuggability — the JS surface
// sees `"cap.not_found: ns:v:fn"`. `sqlstate` is always `nullopt` for
// cap.* rejections (no PG origin). See ICD-0.3.4 §Error Mapping for the
// full variant → code table; the mapping is an explicit switch, not a
// concatenation with `error_string()`.
//
// ICD-0.5.0.3 §Error taxonomy — when `e == EXTENSION_DISPATCH_FAILED`
// the caller supplies `ext_detail_code` + `ext_detail_message` carried
// up from the `RuntimeRegistry::dispatch` PromiseRejection (through
// out-parameters on `call_capability_async`). An empty `ext_detail_code`
// on that variant falls back to `"cap.internal"`.
[[nodiscard]] auto capability_error_to_rejection(
    plinth::capabilities::CapabilityError e, std::string_view signature,
    std::string_view ext_detail_code = {},
    std::string_view ext_detail_message = {}) -> PromiseRejection;

} // namespace plinth::js
