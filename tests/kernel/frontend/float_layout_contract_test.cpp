#include "kernel/frontend/float_layout_contract.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {
namespace contract = plinth::frontend::layout_contract;
using ContractJson = nlohmann::json;
constexpr std::string_view GENERATION = "10000000-0000-4000-8000-000000000046";

auto entry(std::string record = "fake-a") -> ContractJson {
  return {
      {"application_id", "notes"},
      {"panel_id", "preview"},
      {"capability", "notes:1:preview"},
      {"context", {{"record_id", std::move(record)}}},
      {"presentation", "shown"},
      {"maximized", false},
      {"geometry", {{"x", 0}, {"y", 65535}, {"width", 1}, {"height", 65535}}}};
}
auto binding(unsigned int index = 0) -> ContractJson {
  return {{"index", index},
          {"generation", GENERATION},
          {"artifact_digest", std::string(64, 'a')},
          {"schema_digest", std::string(64, 'b')},
          {"projection_contract", {{"id", "record-v1"}, {"version", 1}}},
          {"policy_revision", "18446744073709551615"}};
}
auto body(unsigned int count = 1) -> ContractJson {
  ContractJson layout{{"version", 1}, {"floats", ContractJson::array()}},
      rows = ContractJson::array();
  for (unsigned int index = 0; index < count; ++index) {
    layout["floats"].push_back(entry("fake-" + std::to_string(index)));
    rows.push_back(binding(index));
  }
  return {{"schema_version", 1}, {"layout", layout}, {"bindings", rows}};
}
auto refuse(const ContractJson& document) -> void {
  REQUIRE_FALSE(contract::parse_float_commit(document.dump()));
}
} // namespace

TEST_CASE(
    "float layout contract: strict raw saved descriptors and complete bindings",
    "[frontend][float_layout_contract]") {
  SECTION("canonical error classification preserves operational failure "
          "without allocation injection") {
    using plinth::packages::restore::CodecError;
    for (const auto error :
         {CodecError::LIMIT_EXCEEDED, CodecError::HASHING_FAILED}) {
      REQUIRE(contract::detail::classify_canonical_error(error) ==
              contract::Error::LIMIT_EXCEEDED);
    }
    REQUIRE(contract::detail::classify_canonical_error(CodecError::TOO_LARGE) ==
            contract::Error::TOO_LARGE);
    for (const auto error :
         {CodecError::INVALID_JSON, CodecError::DUPLICATE_KEY,
          CodecError::INVALID_POLICY, CodecError::CONFLICTING_BINDING,
          CodecError::INVALID_INVENTORY, CodecError::INVALID_SCHEMA}) {
      REQUIRE(contract::detail::classify_canonical_error(error) ==
              contract::Error::INVALID);
    }
    // Explicit value mapping is the mechanism under test, not a claim that an
    // allocator/hash fault was physically induced in any production operation.
  }
  SECTION("semantic canonical byte excess and invalid input retain their "
          "classification through every public expected result") {
    auto value = entry();
    value["context"] = ContractJson::object();
    for (unsigned int index = 0; index < 7; ++index)
      value["context"][std::string(1, static_cast<char>('a' + index))] =
          std::string(512, 'x');
    value["context"]["h"] = std::string(456, 'x');
    REQUIRE(value["context"].dump().size() == 4097);
    auto normalized = contract::normalize_saved_float_entry(value);
    auto identity = contract::saved_float_identity(value);
    auto document = body();
    document["layout"]["floats"][0] = value;
    auto parsed = contract::parse_float_commit(document.dump());
    REQUIRE_FALSE(normalized);
    REQUIRE_FALSE(identity);
    REQUIRE_FALSE(parsed);
    REQUIRE(normalized.error() == contract::Error::TOO_LARGE);
    REQUIRE(identity.error() == contract::Error::TOO_LARGE);
    REQUIRE(parsed.error() == contract::Error::TOO_LARGE);
    value["context"] = {{"record_id", nullptr}};
    normalized = contract::normalize_saved_float_entry(value);
    identity = contract::saved_float_identity(value);
    document["layout"]["floats"][0] = value;
    parsed = contract::parse_float_commit(document.dump());
    REQUIRE_FALSE(normalized);
    REQUIRE_FALSE(identity);
    REQUIRE_FALSE(parsed);
    REQUIRE(normalized.error() == contract::Error::INVALID);
    REQUIRE(identity.error() == contract::Error::INVALID);
    REQUIRE(parsed.error() == contract::Error::INVALID);
  }
  SECTION("empty guarded reset and five entries normalize complete permuted "
          "bindings") {
    for (const auto count : {0U, 1U, 5U}) {
      const auto document = body(count);
      const auto parsed = contract::parse_float_commit(document.dump());
      REQUIRE(parsed);
      REQUIRE(parsed->layout == document["layout"]);
      REQUIRE(parsed->bindings.size() == count);
    }
    auto document = body(2);
    std::swap(document["bindings"][0], document["bindings"][1]);
    document["layout"]["floats"][0]["capability"] = "notes:0001:preview";
    const auto parsed = contract::parse_float_commit(document.dump());
    REQUIRE(parsed);
    REQUIRE(parsed->bindings[0].index == 0);
    REQUIRE(parsed->bindings[1].index == 1);
    REQUIRE(parsed->layout["floats"][0]["capability"] == "notes:1:preview");
    REQUIRE(parsed->bindings[0].policy_revision == "18446744073709551615");
    REQUIRE(parsed->bindings[0].projection_contract.version == 1);
    refuse(body(6));
  }
  SECTION("numeric integer semantics match browser without string boolean or "
          "fractional coercion") {
    auto document = body();
    document["schema_version"] = 1.0;
    document["layout"]["version"] = 1.0;
    document["bindings"][0]["index"] = -0.0;
    document["bindings"][0]["projection_contract"]["version"] =
        9007199254740991.0;
    document["layout"]["floats"][0]["geometry"] = {
        {"x", -0.0}, {"y", 65535.0}, {"width", 1.0}, {"height", 65535.0}};
    document["layout"]["floats"][0]["capability"] = "notes:0001:preview";
    const auto ephemeral =
        contract::normalize_saved_float_entry(document["layout"]["floats"][0]);
    REQUIRE(ephemeral);
    REQUIRE((*ephemeral)["geometry"]["x"].is_number_integer());
    REQUIRE((*ephemeral)["capability"] == "notes:1:preview");
    REQUIRE(document["layout"]["floats"][0]["geometry"]["x"].is_number_float());
    REQUIRE(document["layout"]["floats"][0]["capability"] ==
            "notes:0001:preview");
    const auto parsed = contract::parse_float_commit(document.dump());
    REQUIRE(parsed);
    REQUIRE(parsed->layout["version"].is_number_integer());
    REQUIRE(parsed->layout["floats"][0]["geometry"]["x"] == 0);
    REQUIRE(parsed->bindings[0].projection_contract.version ==
            9007199254740991ULL);
    for (const auto& invalid :
         {ContractJson("1"), ContractJson(true), ContractJson(nullptr),
          ContractJson(0.5), ContractJson(-1), ContractJson(65536)}) {
      document = body();
      document["layout"]["floats"][0]["geometry"]["x"] = invalid;
      refuse(document);
    }
    for (const auto& invalid :
         {ContractJson("1"), ContractJson(true), ContractJson(0),
          ContractJson(9007199254740992ULL), ContractJson(1.5)}) {
      document = body();
      document["bindings"][0]["projection_contract"]["version"] = invalid;
      refuse(document);
    }
    document = body();
    document["layout"]["floats"][0]["geometry"]["width"] = 0;
    refuse(document);
    document = body();
    document["schema_version"] = true;
    refuse(document);
    document = body();
    document["layout"]["version"] = "1";
    refuse(document);
  }
  SECTION("exact shapes forbid authority URL generation and component state "
          "persistence") {
    for (const auto key : {"user_id", "policy", "schema", "generation"}) {
      auto document = body();
      document[key] = true;
      refuse(document);
    }
    for (const auto key : {"generation", "context_key", "module_url", "dirty",
                           "title", "component_state"}) {
      auto document = body();
      document["layout"]["floats"][0][key] = "must not persist";
      refuse(document);
    }
    for (const auto key :
         {"application_id", "panel_id", "capability", "context", "presentation",
          "maximized", "geometry"}) {
      auto document = body();
      document["layout"]["floats"][0].erase(key);
      refuse(document);
    }
    for (const auto key : {"schema_version", "layout", "bindings"}) {
      auto document = body();
      document.erase(key);
      refuse(document);
    }
    auto document = body();
    document["layout"]["extra"] = true;
    refuse(document);
    document = body();
    document["layout"]["floats"][0]["geometry"]["extra"] = 0;
    refuse(document);
    document = body();
    document["layout"]["floats"][0]["maximized"] = 1;
    refuse(document);
    document = body();
    document["layout"]["floats"][0]["presentation"] = "hidden";
    refuse(document);
    document = body();
    document["layout"]["floats"][0]["application_id"] = "../notes";
    refuse(document);
    document = body();
    document["layout"]["floats"][0]["capability"] = "notes:0:preview";
    refuse(document);
  }
  SECTION("context flat strings exact UTF8 byte and escaped byte boundaries "
          "preserve allowed controls") {
    auto value = entry();
    value["context"] = ContractJson::object();
    for (unsigned int index = 0; index < 7; ++index)
      value["context"][std::string(1, static_cast<char>('a' + index))] =
          std::string(512, 'x');
    value["context"]["h"] = std::string(455, 'x');
    REQUIRE(value["context"].dump().size() == 4096);
    REQUIRE(contract::validate_saved_float_entry(value));
    value["context"]["h"] = std::string(456, 'x');
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    value["context"] = {{"a", std::string(512, '\x01')},
                        {"b", std::string(168, '\x01') + "x"}};
    REQUIRE(value["context"].dump().size() == 4096);
    REQUIRE(contract::validate_saved_float_entry(value));
    value["context"]["b"] = std::string(168, '\x01') + "xx";
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    std::string key, child;
    for (unsigned int index = 0; index < 32; ++index)
      key += "é";
    for (unsigned int index = 0; index < 128; ++index)
      child += "😀";
    value["context"] = {{key, child}};
    REQUIRE(contract::validate_saved_float_entry(value));
    value["context"] = {{key + "é", child}};
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    value["context"] = {{key, child + "😀"}};
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    for (const auto name : {"", "__proto__", "prototype", "constructor"}) {
      value["context"] = {{name, ""}};
      REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    }
    value["context"] = {{"empty", ""}, {"controls", "\b\t\n\f\r\x01"}};
    REQUIRE(contract::validate_saved_float_entry(value));
    value["context"] = {{"nul", std::string{"a\0b", 3}}};
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    value["context"] = {{std::string{"a\0b", 3}, "x"}};
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    value["context"] = {{"invalid", std::string(1, static_cast<char>(0xff))}};
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    for (const auto& invalid :
         {ContractJson(1), ContractJson(nullptr), ContractJson(true),
          ContractJson::array(), ContractJson::object()}) {
      value["context"] = {{"record_id", invalid}};
      REQUIRE_FALSE(contract::validate_saved_float_entry(value));
    }
    value["context"] = ContractJson::object();
    for (unsigned int index = 0; index < 17; ++index)
      value["context"]["key" + std::to_string(index)] = "x";
    REQUIRE_FALSE(contract::validate_saved_float_entry(value));
  }
  SECTION("independent identity bytes deduplicate normalized capability "
          "context not presentation") {
    auto first = entry();
    first["context"] = {{"é", "😀"}, {"a", "line\nquote\""}};
    first["capability"] = "notes:0001:preview";
    const auto identity = contract::saved_float_identity(first);
    REQUIRE(identity);
    // Independently computed with Python json.dumps UTF8 sorted keys + compact
    // array; no production canonical encoder produces expected bytes.
    REQUIRE(
        *identity ==
        R"(["notes","preview","notes:1:preview","{\"a\":\"line\\nquote\\\"\",\"é\":\"😀\"}"])");
    auto second = first;
    second["presentation"] = "minimized";
    second["maximized"] = true;
    second["geometry"]["x"] = 8;
    second["capability"] = "notes:1:preview";
    REQUIRE(contract::saved_float_identity(second) == identity);
    auto document = body(2);
    document["layout"]["floats"] = ContractJson::array({first, second});
    refuse(document);
    second["context"]["a"] = "different";
    REQUIRE(contract::saved_float_identity(second) != identity);
    document["layout"]["floats"] = ContractJson::array({first, second});
    REQUIRE(contract::parse_float_commit(document.dump()));
  }
  SECTION("bindings require exactly one complete bounded row per original "
          "descriptor index") {
    for (unsigned int variant = 0; variant < 13; ++variant) {
      auto document = body(2);
      if (variant == 0) document["bindings"].erase(1);
      if (variant == 1) document["bindings"].push_back(binding(2));
      if (variant == 2) document["bindings"][1]["index"] = 0;
      if (variant == 3) document["bindings"][1]["index"] = 2;
      if (variant == 4) document["bindings"][0]["index"] = "0";
      if (variant == 5)
        document["bindings"][0]["generation"] =
            "A0000000-0000-4000-8000-000000000046";
      if (variant == 6)
        document["bindings"][0]["artifact_digest"] = std::string(64, 'A');
      if (variant == 7) document["bindings"][0]["schema_digest"] = "short";
      if (variant == 8) document["bindings"][0]["policy_revision"] = "01";
      if (variant == 9)
        document["bindings"][0]["policy_revision"] = "18446744073709551616";
      if (variant == 10)
        document["bindings"][0]["projection_contract"]["id"] = "../record";
      if (variant == 11)
        document["bindings"][0]["projection_contract"]["extra"] = true;
      if (variant == 12) document["bindings"][0]["user_id"] = GENERATION;
      refuse(document);
    }
    for (const auto key :
         {"index", "generation", "artifact_digest", "schema_digest",
          "projection_contract", "policy_revision"}) {
      auto document = body();
      document["bindings"][0].erase(key);
      refuse(document);
    }
    auto reset = body(0);
    reset["bindings"].push_back(binding());
    refuse(reset);
  }
  SECTION("raw bytes aliases depth and nodes are refused before DOM erasure") {
    const auto raw = body().dump();
    std::string padded = raw + std::string(65536 - raw.size(), ' ');
    REQUIRE(contract::parse_float_commit(padded));
    padded.push_back(' ');
    const auto excess = contract::parse_float_commit(padded);
    REQUIRE_FALSE(excess);
    REQUIRE(excess.error() == contract::Error::TOO_LARGE);
    REQUIRE_FALSE(contract::parse_float_commit("\xef\xbb\xbf" + raw));
    REQUIRE_FALSE(contract::parse_float_commit(raw + "garbage"));
    REQUIRE_FALSE(contract::parse_float_commit(
        "{\"schema_version\":1,\"schema_\\u0076ersion\":1}"));
    for (const auto& pair :
         {std::pair{"\"version\":1", "\"version\":1,\"version\":1"},
          std::pair{"\"panel_id\":\"preview\"",
                    "\"panel_id\":\"preview\",\"panel_\\u0069d\":\"preview\""},
          std::pair{"\"record_id\":\"fake-0\"",
                    "\"record_id\":\"fake-0\",\"record_\\u0069d\":\"fake-0\""},
          std::pair{"\"x\":0", "\"x\":0,\"x\":0"},
          std::pair{"\"index\":0", "\"index\":0,\"index\":0"},
          std::pair{"\"id\":\"record-v1\"",
                    "\"id\":\"record-v1\",\"id\":\"record-v1\""}}) {
      auto duplicate = raw;
      const auto location = duplicate.find(pair.first);
      REQUIRE(location != std::string::npos);
      duplicate.replace(location, std::string_view{pair.first}.size(),
                        pair.second);
      const auto refused = contract::parse_float_commit(duplicate);
      REQUIRE_FALSE(refused);
      REQUIRE(refused.error() == contract::Error::INVALID);
    }
    const auto depth = contract::parse_float_commit("[[[[[[[0]]]]]]]");
    REQUIRE_FALSE(depth);
    REQUIRE(depth.error() == contract::Error::STRUCTURAL_LIMIT);
    std::string nodes = "[0";
    for (unsigned int index = 0; index < 16384; ++index)
      nodes += ",0";
    nodes += "]";
    const auto count = contract::parse_float_commit(nodes);
    REQUIRE_FALSE(count);
    REQUIRE(count.error() == contract::Error::STRUCTURAL_LIMIT);
    REQUIRE_FALSE(contract::parse_float_commit("{\"context\":\"\\ud800\"}"));
  }
}
