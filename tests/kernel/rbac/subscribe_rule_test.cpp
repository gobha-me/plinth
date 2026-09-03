// SPDX-License-Identifier: MIT
//
// ICD-0.5.2-ws-broker §Subscription RBAC — unit coverage for the
// rule-derivation helper that the WS `on_subscribe` gate and the
// `pubsub.subscribe` JS binding both consult.

#include "kernel/rbac/subscribe_rule.hpp"

#include <catch2/catch_test_macros.hpp>

using plinth::rbac::derive_subscribe_rule;

TEST_CASE("derive_subscribe_rule: Layer 1 extension schema strips ext_",
          "[rbac][subscribe_rule]") {
  REQUIRE(derive_subscribe_rule("plinth:data:ext_notes.notes") ==
          "notes.realtime.subscribe");
  REQUIRE(derive_subscribe_rule("plinth:data:ext_terminal.sessions") ==
          "terminal.realtime.subscribe");
}

TEST_CASE("derive_subscribe_rule: Layer 1 kernel schema uses kernel.*",
          "[rbac][subscribe_rule]") {
  REQUIRE(derive_subscribe_rule("plinth:data:plinth.users") ==
          "kernel.realtime.subscribe.plinth.users");
}

TEST_CASE("derive_subscribe_rule: Layer 2 system channels use kernel.*",
          "[rbac][subscribe_rule]") {
  REQUIRE(derive_subscribe_rule("plinth:system:packages.installed") ==
          "kernel.realtime.subscribe.packages.installed");
  REQUIRE(derive_subscribe_rule("plinth:system:auth.session.expired") ==
          "kernel.realtime.subscribe.auth.session.expired");
}

TEST_CASE("derive_subscribe_rule: Layer 3 extension channels use <ext>.*",
          "[rbac][subscribe_rule]") {
  REQUIRE(derive_subscribe_rule("plinth:ext:notes:chat_typing") ==
          "notes.realtime.subscribe.chat_typing");
  REQUIRE(derive_subscribe_rule("plinth:ext:terminal:typing") ==
          "terminal.realtime.subscribe.typing");
}

TEST_CASE("derive_subscribe_rule: invalid channels return empty",
          "[rbac][subscribe_rule]") {
  REQUIRE(derive_subscribe_rule("").empty());
  REQUIRE(derive_subscribe_rule("not:a:channel").empty());
  REQUIRE(derive_subscribe_rule("plinth:data:no_dot").empty());
  REQUIRE(derive_subscribe_rule("plinth:ext:no-colon-body").empty());
}
