#include "kernel/realtime/channel.hpp"

#include <cctype>
#include <string_view>

namespace plinth::realtime {

namespace {

auto is_ident_start(char c) -> bool {
  return (c >= 'a' && c <= 'z');
}

auto is_ident_rest(char c) -> bool {
  return is_ident_start(c) || (c >= '0' && c <= '9') || c == '_';
}

// Consume `[a-z][a-z0-9_]*` starting at `pos`. On success advances
// `pos` past the identifier and returns true. On failure `pos` is
// unchanged.
auto consume_ident(std::string_view s, std::size_t& pos) -> bool {
  if (pos >= s.size() || !is_ident_start(s[pos])) {
    return false;
  }
  std::size_t start = pos;
  ++pos;
  while (pos < s.size() && is_ident_rest(s[pos])) {
    ++pos;
  }
  return pos > start;
}

// Layer 3 extension segment: `[a-z][a-z0-9]*` — no underscores, per
// ICD-0.4.1 package-name rules.
auto consume_ext_name(std::string_view s, std::size_t& pos) -> bool {
  if (pos >= s.size() || !is_ident_start(s[pos])) {
    return false;
  }
  ++pos;
  while (pos < s.size() && ((s[pos] >= 'a' && s[pos] <= 'z') ||
                            (s[pos] >= '0' && s[pos] <= '9'))) {
    ++pos;
  }
  return true;
}

// One or more dot-separated ident segments (`a_b.c.d`).
auto consume_dotted_idents(std::string_view s, std::size_t& pos) -> bool {
  if (!consume_ident(s, pos)) {
    return false;
  }
  while (pos < s.size() && s[pos] == '.') {
    ++pos;
    if (!consume_ident(s, pos)) {
      return false;
    }
  }
  return true;
}

} // namespace

auto validate_channel(std::string_view ch) -> bool {
  if (ch.empty() || ch.size() > 63) {
    return false;
  }
  if (!ch.starts_with("plinth:")) {
    return false;
  }
  std::size_t pos = std::string_view{"plinth:"}.size();

  if (ch.substr(pos).starts_with("data:")) {
    pos += 5;
    // <schema>.<table>
    if (!consume_ident(ch, pos)) {
      return false;
    }
    if (pos >= ch.size() || ch[pos] != '.') {
      return false;
    }
    ++pos;
    if (!consume_ident(ch, pos)) {
      return false;
    }
    return pos == ch.size();
  }
  if (ch.substr(pos).starts_with("system:")) {
    pos += 7;
    if (!consume_dotted_idents(ch, pos)) {
      return false;
    }
    return pos == ch.size();
  }
  if (ch.substr(pos).starts_with("ext:")) {
    pos += 4;
    // <extension>:<event_class>[.<segment>]*
    if (!consume_ext_name(ch, pos)) {
      return false;
    }
    if (pos >= ch.size() || ch[pos] != ':') {
      return false;
    }
    ++pos;
    if (!consume_dotted_idents(ch, pos)) {
      return false;
    }
    return pos == ch.size();
  }
  return false;
}

auto channel_layer(std::string_view ch) -> ChannelLayer {
  // Precondition: validate_channel(ch) == true.
  if (ch.starts_with("plinth:data:")) {
    return ChannelLayer::DATA;
  }
  if (ch.starts_with("plinth:system:")) {
    return ChannelLayer::SYSTEM;
  }
  return ChannelLayer::EXTENSION;
}

auto channel_extension(std::string_view ch) -> std::string_view {
  constexpr std::string_view PREFIX = "plinth:ext:";
  if (!ch.starts_with(PREFIX)) {
    return {};
  }
  auto rest = ch.substr(PREFIX.size());
  auto colon = rest.find(':');
  if (colon == std::string_view::npos) {
    return {};
  }
  return rest.substr(0, colon);
}

} // namespace plinth::realtime
