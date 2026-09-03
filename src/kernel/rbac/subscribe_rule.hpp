// SPDX-License-Identifier: MIT
//
// ICD-0.5.2-ws-broker §Subscription RBAC — Layer-derived rule-name
// derivation. The broker and the WS `on_subscribe` gate consult the
// same rule-name computation so extension `rbac.json` declarations
// line up 1:1 with the RBAC tokens the subscribe path checks.

#pragma once

#include <string>
#include <string_view>

namespace plinth::rbac {

// Compute the RBAC rule token a caller must hold to subscribe to
// `channel`. Returns empty string when `channel` fails
// `plinth::realtime::validate_channel` (silent-omission path).
//
// Layer mapping (ICD §Subscription RBAC):
//   Layer 1 `plinth:data:ext_<e>.<tbl>`     -> `<e>.realtime.subscribe`
//   Layer 1 `plinth:data:<kernel>.<tbl>`    ->
//   `kernel.realtime.subscribe.<kernel>.<tbl>` Layer 2
//   `plinth:system:<event_class>`   ->
//   `kernel.realtime.subscribe.<event_class>` Layer 3
//   `plinth:ext:<e>:<event_class>`  -> `<e>.realtime.subscribe.<event_class>`
auto derive_subscribe_rule(std::string_view channel) -> std::string;

} // namespace plinth::rbac
