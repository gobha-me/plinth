#include "kernel/packages/detail/restore_json.hpp"
#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/restore_digest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace restore = plinth::packages::restore;
using plinth::packages::PanelContextField;
using plinth::packages::PanelContextSchema;
using restore::ArtifactKind;
using restore::ArtifactRecord;
using Json = nlohmann::json;

// Independent vectors: tests/tools/restore_codec_vectors.py uses only Python
// struct/hashlib/json, imports no Plinth encoder and prints exact preimage hex.
constexpr std::string_view EMPTY_ARTIFACT =
    "a161ab513d8f7b37477fbace66e49aeb3e6d8b825ecd08093ea5f619364ef506";
constexpr std::string_view BINARY_ARTIFACT =
    "2980df2ad79e3c7a334cbea99bc241c784f57bafdee55a8c9fb2aecb4a38411b";
constexpr std::string_view INVENTORY_ARTIFACT =
    "845c6f95e504b9ca25e2aee16d0c1a8871cbffa8147d87ad8ea4b8adb1d98f51";
constexpr std::string_view EMPTY_SCHEMA =
    "0f2d1ca55a6f3011606caeb5aba93b578f48ffcc7c6391de75e277e8ea0c40b2";
constexpr std::string_view UNICODE_SCHEMA =
    "07f43635254a14c16ff4f15acea8b891ca9817996a642052d3b2278068be3cbd";

auto from_hex(std::string_view hex) -> std::string {
  REQUIRE(hex.size() % 2 == 0);
  const auto digit = [](char byte) -> unsigned int {
    if (byte >= '0' && byte <= '9')
      return static_cast<unsigned int>(byte - '0');
    REQUIRE(byte >= 'a');
    REQUIRE(byte <= 'f');
    return static_cast<unsigned int>(byte - 'a' + 10);
  };
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i != hex.size(); i += 2)
    bytes.push_back(static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1])));
  return bytes;
}

auto unicode_schema() -> PanelContextSchema {
  return {.version = 1,
          .fields = {{"z",
                      {.required = false,
                       .max_bytes = 0,
                       .meaning = std::string{"a\0b", 3}}},
                     {"é",
                      {.required = true,
                       .max_bytes = 512,
                       .meaning = "café/\"\\\b\t\n\f\r\1\37\u2028\u2029"}},
                     {"😀",
                      {.required = true, .max_bytes = 1, .meaning = "emoji"}}},
          .persist_keys = {"😀", "z"}};
}

auto digest_of(std::span<const ArtifactRecord> inventory) -> std::string {
  auto result = restore::artifact_digest(inventory);
  REQUIRE(result.has_value());
  return std::move(*result);
}

auto schema_hash(const PanelContextSchema& schema) -> std::string {
  auto result = restore::schema_digest(schema);
  REQUIRE(result.has_value());
  return std::move(*result);
}

auto reject_inventory(
    std::span<const ArtifactRecord> inventory,
    restore::CodecError expected = restore::CodecError::INVALID_INVENTORY,
    std::size_t cap = restore::INVENTORY_CONTENT_MAX_BYTES) -> void {
  const auto result = restore::artifact_digest(inventory, cap);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == expected);
}

auto reject_schema(const PanelContextSchema& schema) -> void {
  const auto canonical = restore::canonical_schema(schema);
  REQUIRE_FALSE(canonical.has_value());
  REQUIRE(canonical.error() == restore::CodecError::INVALID_SCHEMA);
  const auto digest = restore::schema_digest(schema);
  REQUIRE_FALSE(digest.has_value());
  REQUIRE(digest.error() == restore::CodecError::INVALID_SCHEMA);
}

} // namespace

TEST_CASE("restore digests: canonical bounded byte formats",
          "[packages][restore_digest]") {
  SECTION("independent artifact goldens include headers modes empty hidden and "
          "binary files") {
    REQUIRE(digest_of({}) == EMPTY_ARTIFACT);
    const std::string payload{"A\0B\n", 4};
    const std::array<ArtifactRecord, 1> binary{
        {{ArtifactKind::REGULAR, "hello.txt", 0644, payload}}};
    REQUIRE(digest_of(binary) == BINARY_ARTIFACT);
    const std::string script{"x\0\xff", 3};
    std::array<ArtifactRecord, 7> records{{
        {ArtifactKind::REGULAR, "é/δ", 0600, "z\n"},
        {ArtifactKind::DIRECTORY, "z-empty", 0555, {}},
        {ArtifactKind::REGULAR, "app/main.js", 0644, script},
        {ArtifactKind::DIRECTORY, ".hidden", 0700, {}},
        {ArtifactKind::DIRECTORY, "é", 0750, {}},
        {ArtifactKind::REGULAR, ".hidden/empty", 0000, {}},
        {ArtifactKind::DIRECTORY, "app", 0755, {}},
    }};
    for (std::size_t i = 0; i != records.size(); ++i) {
      REQUIRE(digest_of(records) == INVENTORY_ARTIFACT);
      std::rotate(records.begin(), records.begin() + 1, records.end());
    }
    std::reverse(records.begin(), records.end());
    REQUIRE(digest_of(records) == INVENTORY_ARTIFACT);
    auto changed = binary;
    changed[0].content = std::string_view{"A\0C\n", 4};
    REQUIRE(digest_of(changed) != BINARY_ARTIFACT);
    changed = binary;
    changed[0].mode = 0600;
    REQUIRE(digest_of(changed) != BINARY_ARTIFACT);
    changed = binary;
    changed[0].path = "hello2.txt";
    REQUIRE(digest_of(changed) != BINARY_ARTIFACT);
    changed[0].content = {};
    const auto empty_file = digest_of(changed);
    changed[0].kind = ArtifactKind::DIRECTORY;
    REQUIRE(digest_of(changed) != empty_file);
    REQUIRE(restore::artifact_digest(changed, 0).has_value());
  }

  SECTION("path tree kind and mode validation never silently normalize") {
    for (const auto& path : std::vector<std::string>{
             "", ".", "..", "/a", "a/", "a//b", "./a", "a/./b", "a/../b",
             "a\\b", "a\nb", "a\tb", std::string{"a\0b", 3},
             std::string{"a\x7f", 2}, std::string{"\xc0\xaf", 2},
             std::string{"\xed\xa0\x80", 3}}) {
      const std::array<ArtifactRecord, 1> record{
          {{ArtifactKind::REGULAR, path, 0644, {}}}};
      reject_inventory(record);
    }
    std::array<ArtifactRecord, 1> record{
        {{ArtifactKind::REGULAR, "a", 0777, {}}}};
    REQUIRE(restore::artifact_digest(record).has_value());
    for (auto mode :
         {04000U, 0100644U, std::numeric_limits<std::uint32_t>::max()}) {
      record[0].mode = mode;
      reject_inventory(record);
    }
    record[0].mode = 0;
    record[0].kind = static_cast<ArtifactKind>(0);
    reject_inventory(record);
    record[0].kind = static_cast<ArtifactKind>(3);
    reject_inventory(record);
    record[0].kind = ArtifactKind::DIRECTORY;
    record[0].content = "not empty";
    reject_inventory(record);
    const std::array<ArtifactRecord, 1> missing{
        {{ArtifactKind::REGULAR, "a/b", 0, {}}}};
    reject_inventory(missing);
    const std::array<ArtifactRecord, 2> file_parent{
        {{ArtifactKind::REGULAR, "a", 0, {}},
         {ArtifactKind::REGULAR, "a/b", 0, {}}}};
    reject_inventory(file_parent);
    const std::array<ArtifactRecord, 2> duplicate{
        {{ArtifactKind::REGULAR, "a", 0, {}},
         {ArtifactKind::REGULAR, "a", 0, {}}}};
    reject_inventory(duplicate);
    auto conflict = duplicate;
    conflict[0].kind = ArtifactKind::DIRECTORY;
    reject_inventory(conflict);
    const std::array<ArtifactRecord, 3> reversed_parents{
        {{ArtifactKind::REGULAR, "a/b/c", 0, {}},
         {ArtifactKind::DIRECTORY, "a/b", 0, {}},
         {ArtifactKind::DIRECTORY, "a", 0, {}}}};
    REQUIRE(restore::artifact_digest(reversed_parents).has_value());
    const std::array<ArtifactRecord, 2> distinct_unicode{
        {{ArtifactKind::REGULAR, "é", 0, {}},
         {ArtifactKind::REGULAR, "e\u0301", 0, {}}}};
    REQUIRE(restore::artifact_digest(distinct_unicode).has_value());
  }

  SECTION("record path component and encoded metadata profile bounds are "
          "checked exactly") {
    std::string path(1024, 'a');
    std::array<ArtifactRecord, 1> one{{{ArtifactKind::REGULAR, path, 0, {}}}};
    REQUIRE(restore::artifact_digest(one).has_value());
    path.push_back('a');
    one[0].path = path;
    reject_inventory(one, restore::CodecError::LIMIT_EXCEEDED);
    std::string utf8_path;
    for (int i = 0; i != 512; ++i)
      utf8_path += "é";
    REQUIRE(utf8_path.size() == 1024);
    one[0].path = utf8_path;
    REQUIRE(restore::artifact_digest(one).has_value());
    utf8_path.push_back('a');
    one[0].path = utf8_path;
    reject_inventory(one, restore::CodecError::LIMIT_EXCEEDED);

    std::vector<std::string> chain;
    chain.reserve(33);
    for (int depth = 0; depth != 33; ++depth)
      chain.push_back(depth == 0 ? "a" : chain.back() + "/a");
    std::vector<ArtifactRecord> records;
    for (std::size_t i = 0; i != 32; ++i)
      records.push_back(
          {i == 31 ? ArtifactKind::REGULAR : ArtifactKind::DIRECTORY,
           chain[i],
           0,
           {}});
    REQUIRE(restore::artifact_digest(records).has_value());
    records.back().kind = ArtifactKind::DIRECTORY;
    records.push_back({ArtifactKind::REGULAR, chain[32], 0, {}});
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED);

    std::vector<std::string> short_names;
    short_names.reserve(4097);
    for (int i = 0; i != 4097; ++i)
      short_names.push_back("p" + std::to_string(i));
    records.clear();
    for (std::size_t i = 0; i != 4096; ++i)
      records.push_back({ArtifactKind::REGULAR, short_names[i], 0, {}});
    REQUIRE(restore::artifact_digest(records).has_value());
    records.push_back({ArtifactKind::REGULAR, short_names[4096], 0, {}});
    // Tiny paths keep metadata well below1MiB: this isolates record count.
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED);

    // Independent metadata arithmetic: domain/NUL/version/count is32bytes;
    // each flat regular record is17+pathbytes, excluding empty file payloads.
    std::vector<std::string> paths;
    paths.reserve(4097);
    for (std::size_t i = 0; i != 4096; ++i) {
      auto name = "p" + std::to_string(i);
      name.append((i == 0 ? 207U : 239U) - name.size(), 'a');
      paths.push_back(std::move(name));
    }
    records.clear();
    for (const auto& name : paths)
      records.push_back({ArtifactKind::REGULAR, name, 0, {}});
    REQUIRE(records.size() == 4096);
    REQUIRE(32 + 4096 * 17 + 207 + 4095 * 239 ==
            restore::INVENTORY_METADATA_MAX_BYTES);
    REQUIRE(restore::artifact_digest(records).has_value());
    paths[0].push_back('a');
    records[0].path = paths[0];
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED);
    paths[0].pop_back();
    records[0].path = paths[0];
    paths.push_back("extra");
    records.push_back({ArtifactKind::REGULAR, paths.back(), 0, {}});
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED);
  }

  SECTION("payload cap is exact cumulative and cannot raise the50MiB ceiling") {
    const std::string bytes(restore::INVENTORY_CONTENT_MAX_BYTES + 1, 'x');
    std::array<ArtifactRecord, 1> records{
        {{ArtifactKind::REGULAR, "file", 0600,
          std::string_view{bytes}.substr(
              0, restore::INVENTORY_CONTENT_MAX_BYTES)}}};
    REQUIRE(restore::artifact_digest(records).has_value());
    records[0].content = bytes;
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED);
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED,
                     std::numeric_limits<std::size_t>::max());
    std::array<ArtifactRecord, 2> global_cumulative{{
        {ArtifactKind::REGULAR, "a", 0,
         std::string_view{bytes}.substr(
             0, restore::INVENTORY_CONTENT_MAX_BYTES - 1)},
        {ArtifactKind::REGULAR, "b", 0, "x"},
    }};
    REQUIRE(restore::artifact_digest(global_cumulative).has_value());
    global_cumulative[0].content =
        std::string_view{bytes}.substr(0, restore::INVENTORY_CONTENT_MAX_BYTES);
    reject_inventory(global_cumulative, restore::CodecError::LIMIT_EXCEEDED,
                     std::numeric_limits<std::size_t>::max());
    records[0].content = std::string_view{bytes}.substr(0, 3);
    REQUIRE(restore::artifact_digest(records, 3).has_value());
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED, 2);
    reject_inventory(records, restore::CodecError::LIMIT_EXCEEDED, 0);
    const std::array<ArtifactRecord, 2> cumulative{
        {{ArtifactKind::REGULAR, "a", 0, "ab"},
         {ArtifactKind::REGULAR, "b", 0, "cd"}}};
    REQUIRE(restore::artifact_digest(cumulative, 4).has_value());
    reject_inventory(cumulative, restore::CodecError::LIMIT_EXCEEDED, 3);
  }

  SECTION("schema canonical bytes and hashes match independent Unicode and "
          "control vectors") {
    const PanelContextSchema empty;
    auto encoded = restore::canonical_schema(empty);
    REQUIRE(encoded.has_value());
    REQUIRE(*encoded == R"({"fields":{},"persist_keys":[],"version":1})");
    REQUIRE(schema_hash(empty) == EMPTY_SCHEMA);
    auto schema = unicode_schema();
    encoded = restore::canonical_schema(schema);
    REQUIRE(encoded.has_value());
    const auto exact = from_hex(
        "7b226669656c6473223a7b227a223a7b226d61785f6279746573223a302c226d65616e"
        "696e67223a22615c753030303062222c227265717569726564223a66616c73657d2c22"
        "c3a9223a7b226d61785f6279746573223a3531322c226d65616e696e67223a22636166"
        "c3a92f5c225c5c5c625c745c6e5c665c725c75303030315c7530303166e280a8e280a9"
        "222c227265717569726564223a747275657d2c22f09f9880223a7b226d61785f627974"
        "6573223a312c226d65616e696e67223a22656d6f6a69222c227265717569726564223a"
        "747275657d7d2c22706572736973745f6b657973223a5b227a222c22f09f9880225d2c"
        "2276657273696f6e223a317d");
    REQUIRE(*encoded == exact);
    REQUIRE(schema_hash(schema) == UNICODE_SCHEMA);
    std::reverse(schema.persist_keys.begin(), schema.persist_keys.end());
    REQUIRE(schema_hash(schema) == UNICODE_SCHEMA);
    schema.fields.at("z").required = true;
    REQUIRE(schema_hash(schema) != UNICODE_SCHEMA);
    schema = unicode_schema();
    schema.fields.at("z").max_bytes = 1;
    REQUIRE(schema_hash(schema) != UNICODE_SCHEMA);
    schema = unicode_schema();
    schema.fields.at("z").meaning = "different";
    REQUIRE(schema_hash(schema) != UNICODE_SCHEMA);
    schema = unicode_schema();
    schema.persist_keys.pop_back();
    REQUIRE(schema_hash(schema) != UNICODE_SCHEMA);
    schema = unicode_schema();
    auto field = schema.fields.at("z");
    schema.fields.erase("z");
    schema.fields.emplace("a", std::move(field));
    schema.persist_keys = {"a", "😀"};
    REQUIRE(schema_hash(schema) != UNICODE_SCHEMA);
    schema = {
        .version = 1,
        .fields = {{"id", {.required = false, .max_bytes = 2, .meaning = "é"}}},
        .persist_keys = {}};
    const auto composed = schema_hash(schema);
    schema.fields.at("id").meaning = "e\u0301";
    REQUIRE(schema_hash(schema) != composed); // Never normalize Unicode.
  }

  SECTION("forged typed schema values fail defensively before serialization") {
    for (auto version : {0U, 2U, std::numeric_limits<std::uint32_t>::max()}) {
      auto schema = unicode_schema();
      schema.version = version;
      reject_schema(schema);
    }
    for (const auto& name : std::vector<std::string>{
             "", "__proto__", "prototype", "constructor", std::string(65, 'a'),
             std::string{"a\0b", 3}, std::string{"\xc0\xaf", 2}}) {
      auto schema = unicode_schema();
      schema.fields.emplace(name, PanelContextField{false, 0, "x"});
      reject_schema(schema);
    }
    for (const auto& meaning : std::vector<std::string>{
             "", std::string(257, 'a'), std::string{"\xed\xa0\x80", 3}}) {
      auto schema = unicode_schema();
      schema.fields.at("z").meaning = meaning;
      reject_schema(schema);
    }
    auto schema = unicode_schema();
    schema.fields.at("z").max_bytes = 513;
    reject_schema(schema);
    schema = unicode_schema();
    schema.persist_keys.push_back("z");
    reject_schema(schema);
    schema = unicode_schema();
    schema.persist_keys.push_back("undeclared");
    reject_schema(schema);
    schema = PanelContextSchema{};
    for (int i = 0; i != 16; ++i)
      schema.fields.emplace(
          "k" + std::to_string(i),
          PanelContextField{true, 512, std::string(256, 'x')});
    REQUIRE(restore::schema_digest(schema).has_value());
    schema.fields.emplace("extra", PanelContextField{false, 0, "x"});
    reject_schema(schema);
    schema = {.version = 1,
              .fields = {{std::string(64, 'a'), {false, 0, "x"}}},
              .persist_keys = {}};
    REQUIRE(restore::schema_digest(schema).has_value());
  }

  SECTION("existing typed declaration parser retains NUL meaning and "
          "persistence request semantics") {
    auto schema = unicode_schema();
    auto fields = Json::object();
    for (const auto& [name, field] : schema.fields)
      fields[name] = {{"required", field.required},
                      {"max_bytes", field.max_bytes},
                      {"meaning", field.meaning}};
    const Json context{{"version", 1},
                       {"fields", fields},
                       {"persist_keys", schema.persist_keys}};
    const Json document{{"format_version", 2},
                        {"panels",
                         {{{"id", "preview"},
                           {"type", "float"},
                           {"client_path", "preview.js"},
                           {"rbac_rule", "notes.read"},
                           {"capability", "notes:1:read"},
                           {"content_types", {"text/plain"}},
                           {"context_schema", context}}}}};
    const auto parsed =
        plinth::packages::PanelsManifest::parse(document.dump(), "panels.json");
    REQUIRE(parsed.value.has_value());
    REQUIRE(parsed.value->panels.size() == 1);
    REQUIRE(parsed.value->panels.front().context_schema.has_value());
    REQUIRE(*parsed.value->panels.front().context_schema == schema);
    REQUIRE(schema_hash(*parsed.value->panels.front().context_schema) ==
            UNICODE_SCHEMA);
  }

  SECTION("shared canonical JSON obeys exact byte ordering limits and no float "
          "encoding") {
    const Json value{{"z", std::numeric_limits<std::uint64_t>::max()},
                     {"a", -1},
                     {"é", Json::array({nullptr, true, false, "/"})}};
    const auto encoded = restore::detail::canonical_json(value, 1024);
    REQUIRE(encoded.has_value());
    REQUIRE(*encoded ==
            R"({"a":-1,"z":18446744073709551615,"é":[null,true,false,"/"]})");
    REQUIRE(
        restore::detail::canonical_json(value, encoded->size()).has_value());
    auto rejected = restore::detail::canonical_json(value, encoded->size() - 1);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == restore::CodecError::TOO_LARGE);
    for (const auto& invalid : std::vector<Json>{
             1.0, Json::binary({1, 2}), Json(std::string{"\xc0\xaf", 2})}) {
      rejected = restore::detail::canonical_json(invalid, 1024);
      REQUIRE_FALSE(rejected.has_value());
      REQUIRE(rejected.error() == restore::CodecError::INVALID_JSON);
    }
    Json nested = 0;
    for (int i = 1; i != 6; ++i)
      nested = Json::array({nested});
    REQUIRE(restore::detail::canonical_json(nested, 1024).has_value());
    nested = Json::array({nested});
    rejected = restore::detail::canonical_json(nested, 1024);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == restore::CodecError::LIMIT_EXCEEDED);
    auto nodes = Json::array();
    for (std::size_t i = 1; i < restore::JSON_MAX_NODES; ++i)
      nodes.push_back(nullptr);
    REQUIRE(restore::detail::canonical_json(nodes, restore::POLICY_MAX_BYTES)
                .has_value());
    nodes.push_back(nullptr);
    rejected =
        restore::detail::canonical_json(nodes, restore::POLICY_MAX_BYTES);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == restore::CodecError::LIMIT_EXCEEDED);
  }
}
