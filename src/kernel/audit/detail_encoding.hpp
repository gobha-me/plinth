// SPDX-License-Identifier: MIT
#pragma once

#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::audit {

struct StoredDetail {
  std::string json;
  int encoding = 0;
};

// Walk JSON escape pairs, not substrings: a caller's literal backslash-u0000
// must remain ordinary JSONB. FastWriter emits actual NUL as \u0000 in both
// string values and object names. No C-string operation may inspect them.
inline auto encode_detail(const Json::Value& detail) -> StoredDetail {
  auto json = Json::FastWriter{}.write(detail);
  for (std::size_t index = 0; index < json.size(); ++index) {
    if (json[index] != '\\') {
      continue;
    }
    ++index;
    if (json.compare(index, 5, "u0000") == 0) {
      // PostgreSQL JSONB now sees a string of serialized JSON, whose NUL
      // escape is literal ASCII text. Decode exactly once, only with the
      // out-of-band column; no caller-supplied sentinel is authoritative.
      return {Json::FastWriter{}.write(Json::Value{json}), 1};
    }
  }
  return {std::move(json), 0};
}

inline auto parse_detail_json(std::string_view stored)
    -> std::optional<Json::Value> {
  if (stored.empty()) {
    return std::nullopt;
  }
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  // Detail can be any JSON value, not just a container.
  builder["strictRoot"] = false;
  builder["collectComments"] = false;
  std::unique_ptr<Json::CharReader> reader{builder.newCharReader()};
  Json::Value detail;
  std::string errors;
  try {
    if (!reader->parse(stored.data(), stored.data() + stored.size(), &detail,
                       &errors)) {
      return std::nullopt;
    }
  } catch (const Json::Exception&) {
    // Depth-limit failures are reported by JsonCpp as exceptions. They must
    // use the same sanitized query-error path as other corrupt encoded rows.
    return std::nullopt;
  }
  return detail;
}

inline auto decode_detail(std::string_view stored, int encoding)
    -> std::optional<Json::Value> {
  if (encoding != 0 && encoding != 1) {
    return std::nullopt;
  }
  auto detail = parse_detail_json(stored);
  if (!detail) {
    return std::nullopt;
  }
  if (encoding == 0) {
    return detail;
  }
  if (!detail->isString()) {
    return std::nullopt;
  }
  return parse_detail_json(detail->asString());
}

} // namespace plinth::audit
