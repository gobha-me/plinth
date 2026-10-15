// SPDX-License-Identifier: MIT

#include "kernel/audit/detail_encoding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

TEST_CASE("audit detail encoding preserves NUL values and names losslessly",
          "[audit][encoding]") {
  Json::Value detail{Json::objectValue};
  detail[std::string{"key\0suffix", 10}] = std::string{"a\0b\0", 4};
  detail["nested"][0]["literal"] = R"(\u0000)";
  detail["nested"][0]["escaped_then_nul"] = std::string{"\\\0", 2};
  detail["number"] = Json::Int64{-9007199254740991LL};
  detail["bool"] = true;
  detail["null"] = Json::nullValue;
  // Deliberately own an independent snapshot, not an alias to the input.
  const auto before = Json::Value{detail};
  const auto stored = plinth::audit::encode_detail(detail);
  REQUIRE(stored.encoding == 1);
  const auto decoded =
      plinth::audit::decode_detail(stored.json, stored.encoding);
  REQUIRE(decoded.has_value());
  REQUIRE(*decoded == detail);
  REQUIRE(detail == before);
}

TEST_CASE("audit detail encoding keeps ordinary and marker-shaped JSON plain",
          "[audit][encoding]") {
  std::array<Json::Value, 8> values{Json::Value{},
                                    Json::Value{false},
                                    Json::Value{42},
                                    Json::Value{1.25},
                                    Json::Value{Json::arrayValue},
                                    Json::Value{Json::objectValue},
                                    Json::Value{R"(\u0000)"},
                                    Json::Value{R"(\\u0000)"}};
  values[4].append("ordinary");
  values[5]["detail_encoding"] = 1;
  values[5]["json"] = R"({"channel":"\u0000"})";
  values[5]["literal_key\\u0000"] = "unchanged";
  for (const auto& value : values) {
    const auto stored = plinth::audit::encode_detail(value);
    REQUIRE(stored.encoding == 0);
    REQUIRE(stored.json == Json::FastWriter{}.write(value));
    const auto decoded = plinth::audit::decode_detail(stored.json, 0);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == value);
  }
}

TEST_CASE("audit detail encoding distinguishes escape parity and scalar NUL",
          "[audit][encoding]") {
  for (std::size_t count = 0; count < 8; ++count) {
    const auto prefix = std::string(count, '\\');
    const Json::Value nul{prefix + std::string(1, '\0')};
    const Json::Value literal{prefix + "u0000"};
    const auto encoded = plinth::audit::encode_detail(nul);
    REQUIRE(encoded.encoding == 1);
    REQUIRE(plinth::audit::decode_detail(encoded.json, 1) == nul);
    REQUIRE(plinth::audit::encode_detail(literal).encoding == 0);
  }
}

TEST_CASE("audit detail decoding rejects corrupt or unknown encodings",
          "[audit][encoding]") {
  REQUIRE_FALSE(plinth::audit::decode_detail({}, 0));
  REQUIRE_FALSE(plinth::audit::decode_detail("{}", -1));
  REQUIRE_FALSE(plinth::audit::decode_detail("{}", 2));
  REQUIRE_FALSE(plinth::audit::decode_detail("{}", 1));
  REQUIRE_FALSE(plinth::audit::decode_detail("\"not JSON\"", 1));
  REQUIRE_FALSE(plinth::audit::decode_detail("{} trailing", 0));
  REQUIRE_FALSE(plinth::audit::decode_detail("\"{} trailing\"", 1));
  REQUIRE_FALSE(plinth::audit::decode_detail("\"/* comment */{}\"", 1));
  REQUIRE_FALSE(plinth::audit::decode_detail("\"[1,]\"", 1));
  REQUIRE_FALSE(
      plinth::audit::decode_detail("\"{\\\"x\\\":1,\\\"x\\\":2}\"", 1));
  const auto deep = std::string(1100, '[') + "0" + std::string(1100, ']');
  REQUIRE_FALSE(plinth::audit::decode_detail(
      Json::FastWriter{}.write(Json::Value{deep}), 1));
}
