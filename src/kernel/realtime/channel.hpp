// SPDX-License-Identifier: MIT
//
// ICD-0.5.0 §Channel Naming Scheme — pure-function validator for the
// three-layer logical channel namespace (plinth:data:*, plinth:system:*,
// plinth:ext:*). Hand-rolled rather than std::regex per plan §Risks
// gotcha 6 (simpler rejection-reason reporting, no ICU pull-in, ~40 LOC).
//
// The PG wire channel is ALWAYS the literal string "plinth:realtime"
// (see §Channel Subscription / Decision: single-channel fan-in). This
// module validates the LOGICAL channel that rides inside the envelope.

#pragma once

#include <cstdint>
#include <string_view>

namespace plinth::realtime {

enum class ChannelLayer : std::uint8_t {
  DATA,      // plinth:data:<schema>.<table>
  SYSTEM,    // plinth:system:<event_class>(.<segment>)*
  EXTENSION, // plinth:ext:<extension>:<event_class>(.<segment>)*
};

// True if `ch` matches the ICD §Channel Naming Scheme regex. Rejects
// channels with forbidden characters, length > 63, bad prefixes, or
// malformed layer-specific bodies.
auto validate_channel(std::string_view ch) -> bool;

// Extract the layer from a validated channel. UB if ch has not passed
// validate_channel. Used by emit layer↔channel consistency check and
// by listener dispatch.
auto channel_layer(std::string_view ch) -> ChannelLayer;

// Extract the `<extension>` segment from an ext-layer channel.
// Returns empty string_view if ch is not a valid ext-layer channel or
// empty. Used by the pubsub.publish extension-identity gate (Slice 6).
auto channel_extension(std::string_view ch) -> std::string_view;

} // namespace plinth::realtime
