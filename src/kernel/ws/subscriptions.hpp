#pragma once

// Channel subscription handling per ICD-0.1.6 §Subscribe / Unsubscribe.
//
// For 0.1.6, subscribe is admin-only: non-admin users receive an empty
// `subscribed[]` list (silent omission per ICD). A per-channel rule
// naming convention is deferred to a later ICD.

#include <cstddef>
#include <cstdint>
#include <drogon/WebSocketConnection.h>
#include <json/value.h>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::ws {

// Handle a `{type:"subscribe", channels:[...]}` message.
auto on_subscribe(const drogon::WebSocketConnectionPtr& conn,
                  const Json::Value& msg) -> void;

// Handle a `{type:"unsubscribe", channels:[...]}` message.
auto on_unsubscribe(const drogon::WebSocketConnectionPtr& conn,
                    const Json::Value& msg) -> void;

// ICD-0.5.5 §7 — client `{type:"debounce_renegotiate", channel:..,
// debounce_ms:..}` frame. Kernel does not enforce client debounce
// (advisory + audit per OQ5); this handler emits a rate-limited
// `realtime.debounce.advisory_overridden` audit and returns.
auto on_debounce_renegotiate(const drogon::WebSocketConnectionPtr& conn,
                             const Json::Value& msg) -> void;

// Test seam — reset the per-(user_id, channel) audit-window state
// for `realtime.debounce.advisory_overridden`. Catch2 test cases
// that fire repeated renegotiate frames inside a single window
// would otherwise see only the first audit.
auto reset_debounce_audit_state_for_test() -> void;

// Test seam — count of `realtime.debounce.advisory_overridden`
// audit emissions since the last reset. D.04 verifies the
// per-(user, channel) sliding-window dedup by sending many
// renegotiate frames and asserting only one audit fired.
[[nodiscard]] auto debounce_audit_emit_count_for_test() -> std::uint64_t;

// Test seam — run the `on_debounce_renegotiate` audit pipeline
// without a real WebSocketConnectionPtr. Used by D.03 / D.04 to
// drive the rate-limit window from a unit-test TU. Returns the
// `count_in_window` value the production handler would emit; the
// boolean reports whether the audit fired (true on the first call
// of a window, false on subsequent in-window calls).
auto run_debounce_renegotiate_for_test(std::string_view user_id,
                                       std::string_view channel,
                                       std::int64_t override_ms)
    -> std::pair<bool, std::uint64_t>;

// Test seam — synthesize a `subscribed` ack without a real WS conn.
// Returns the JSON the production `make_ack("subscribed", ...)`
// path would emit. D.01 / D.02 / J.01–J.03 use this to assert the
// advisory fields without a full Drogon WS round-trip.
[[nodiscard]] auto subscribed_ack_for_test(
    const std::vector<std::string>& channels) -> Json::Value;

namespace test_seam {

// ICD-0.5.5 §8 — override `cfg.events.live_buffer_cap_per_subscription`
// for tests that need to drive the live-buffer overflow path (L.05)
// without seeding hundreds of events. Returns nullopt when no
// override is set; `fire_replay` consults this BEFORE defaulting to
// `cfg.events.live_buffer_cap_per_subscription` at replay-start.
[[nodiscard]] auto live_buffer_cap_override() -> std::optional<std::size_t>;

// Set / clear the override above. Process-static — tests must clear
// before the next TEST_CASE to avoid cross-test leakage.
auto set_live_buffer_cap_override(std::size_t cap) -> void;
auto clear_live_buffer_cap_override() -> void;

} // namespace test_seam

} // namespace plinth::ws
