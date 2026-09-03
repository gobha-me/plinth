#include "kernel/rbac/subscribe_rule.hpp"

#include "kernel/realtime/channel.hpp"

#include <string>
#include <string_view>

namespace plinth::rbac {

namespace {

constexpr std::string_view DATA_PREFIX = "plinth:data:";
constexpr std::string_view SYSTEM_PREFIX = "plinth:system:";
constexpr std::string_view EXT_PREFIX = "plinth:ext:";

} // namespace

auto derive_subscribe_rule(std::string_view channel) -> std::string {
  if (!plinth::realtime::validate_channel(channel)) {
    return {};
  }
  using plinth::realtime::ChannelLayer;
  switch (plinth::realtime::channel_layer(channel)) {
    case ChannelLayer::DATA: {
      auto body = channel.substr(DATA_PREFIX.size());
      auto dot = body.find('.');
      auto schema = body.substr(0, dot);
      auto table = body.substr(dot + 1);
      if (schema.starts_with("ext_") && schema.size() > 4) {
        auto ext = schema.substr(4);
        std::string rule{ext};
        rule += ".realtime.subscribe";
        return rule;
      }
      std::string rule = "kernel.realtime.subscribe.";
      rule += schema;
      rule += '.';
      rule += table;
      return rule;
    }
    case ChannelLayer::SYSTEM: {
      auto body = channel.substr(SYSTEM_PREFIX.size());
      std::string rule = "kernel.realtime.subscribe.";
      rule += body;
      return rule;
    }
    case ChannelLayer::EXTENSION: {
      auto ext = plinth::realtime::channel_extension(channel);
      auto tail = channel.substr(EXT_PREFIX.size() + ext.size() + 1);
      std::string rule{ext};
      rule += ".realtime.subscribe.";
      rule += tail;
      return rule;
    }
  }
  return {};
}

} // namespace plinth::rbac
