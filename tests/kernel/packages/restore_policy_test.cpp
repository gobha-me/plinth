#include "kernel/packages/restore_policy.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace restore = plinth::packages::restore;
using Json = nlohmann::json;

auto policy_entry() -> Json {
  return {
      {"application_id", "notes"},
      {"panel_id", "preview"},
      {"capability", "notes:1:read"},
      {"projection_contract", {{"id", "record-v1"}, {"version", 1}}},
      {"artifact_digest", std::string(64, 'a')},
      {"schema_digest", std::string(64, 'b')},
      {"fields", {{{"key", "record_id"}, {"meaning", "Stable identifier"}}}}};
}

auto policy_document() -> Json {
  return {{"format_version", 1},
          {"policy_revision", "1"},
          {"review",
           {{"admin_user_id", "00000000-0000-0000-0000-000000000000"},
            {"reviewed_at", "2024-02-29T23:59:59Z"},
            {"review_ref", "fixture-review"},
            {"evidence_sha256", std::string(64, 'c')}}},
          {"entries", Json::array({policy_entry()})}};
}

auto reject_policy(std::string_view text,
                   restore::CodecError expected =
                       restore::CodecError::INVALID_POLICY) -> void {
  const auto parsed = restore::parse_policy(text);
  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error() == expected);
}

auto reject_policy(const Json& document,
                   restore::CodecError expected =
                       restore::CodecError::INVALID_POLICY) -> void {
  reject_policy(std::string_view{document.dump()}, expected);
}

auto accept_policy(const Json& document) -> restore::ParsedPolicy {
  auto parsed = restore::parse_policy(document.dump());
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

} // namespace

TEST_CASE("restore policy: strict bounded raw contract",
          "[packages][restore_policy]") {
  SECTION("owned values preserve spelling without conveying authority") {
    auto document = policy_document();
    document["policy_revision"] = "18446744073709551615";
    document["review"]["reviewed_at"] = "2024-02-29t23:59:59-00:00";
    document["review"]["review_ref"] = "";
    document["entries"][0]["capability"] = "notes:0001:read";
    document["entries"][0]["projection_contract"]["version"] =
        9007199254740991ULL;
    document["entries"][0]["fields"][0]["meaning"] = std::string{"a\0b\n", 4};
    auto raw = document.dump();
    auto parsed = restore::parse_policy(raw);
    REQUIRE(parsed.has_value());
    raw.assign(raw.size(),
               'x'); // No parsed view may borrow this backing store.
    REQUIRE(parsed->policy_revision == "18446744073709551615");
    REQUIRE(parsed->review.admin_user_id ==
            "00000000-0000-0000-0000-000000000000");
    REQUIRE(parsed->review.reviewed_at == "2024-02-29t23:59:59-00:00");
    REQUIRE(parsed->review.review_ref.empty());
    REQUIRE(parsed->review.evidence_sha256 == std::string(64, 'c'));
    REQUIRE(parsed->entries.size() == 1);
    const auto& entry = parsed->entries.front();
    REQUIRE(entry.application_id == "notes");
    REQUIRE(entry.panel_id == "preview");
    REQUIRE(entry.capability == "notes:1:read");
    REQUIRE(entry.projection_contract.id == "record-v1");
    REQUIRE(entry.projection_contract.version == 9007199254740991ULL);
    REQUIRE(entry.artifact_digest == std::string(64, 'a'));
    REQUIRE(entry.schema_digest == std::string(64, 'b'));
    REQUIRE(entry.fields.size() == 1);
    REQUIRE(entry.fields.front().key == "record_id");
    REQUIRE(entry.fields.front().meaning == std::string{"a\0b\n", 4});
    document["review"]["admin_user_id"] =
        "ffffffff-ffff-ffff-ffff-ffffffffffff";
    document["entries"][0]["fields"] = Json::array();
    REQUIRE(accept_policy(document).entries.front().fields.empty());
    document["entries"] = Json::array();
    REQUIRE(accept_policy(document).entries.empty());
  }

  SECTION("every exact object shape rejects missing and unknown members") {
    for (const auto* key :
         {"format_version", "policy_revision", "review", "entries"}) {
      auto document = policy_document();
      document.erase(key);
      reject_policy(document);
    }
    for (const auto* key :
         {"admin_user_id", "reviewed_at", "review_ref", "evidence_sha256"}) {
      auto document = policy_document();
      document["review"].erase(key);
      reject_policy(document);
    }
    for (const auto* key :
         {"application_id", "panel_id", "capability", "projection_contract",
          "artifact_digest", "schema_digest", "fields"}) {
      auto document = policy_document();
      document["entries"][0].erase(key);
      reject_policy(document);
    }
    for (const auto* key : {"id", "version"}) {
      auto document = policy_document();
      document["entries"][0]["projection_contract"].erase(key);
      reject_policy(document);
    }
    for (const auto* key : {"key", "meaning"}) {
      auto document = policy_document();
      document["entries"][0]["fields"][0].erase(key);
      reject_policy(document);
    }
    for (int level = 0; level != 5; ++level) {
      auto document = policy_document();
      if (level == 0) document["unexpected"] = true;
      if (level == 1) document["review"]["unexpected"] = true;
      if (level == 2) document["entries"][0]["unexpected"] = true;
      if (level == 3)
        document["entries"][0]["projection_contract"]["unexpected"] = true;
      if (level == 4) document["entries"][0]["fields"][0]["unexpected"] = true;
      reject_policy(document);
    }
    for (const auto& value :
         std::vector<Json>{nullptr, false, 1, "object", Json::array()}) {
      reject_policy(value);
      auto document = policy_document();
      document["review"] = value;
      reject_policy(document);
      document = policy_document();
      document["entries"][0]["projection_contract"] = value;
      reject_policy(document);
      document = policy_document();
      document["entries"][0]["fields"][0] = value;
      reject_policy(document);
    }
    for (const auto& value :
         std::vector<Json>{nullptr, false, 1, "array", Json::object()}) {
      auto document = policy_document();
      document["entries"] = value;
      reject_policy(document);
      document = policy_document();
      document["entries"][0]["fields"] = value;
      reject_policy(document);
    }
  }

  SECTION("literal numbers and canonical revisions retain exact upper bounds") {
    for (const auto& value : std::vector<Json>{nullptr, true, 0, 2, 1.0, "1"}) {
      auto document = policy_document();
      document["format_version"] = value;
      reject_policy(document);
    }
    for (const auto& value : std::vector<Json>{
             nullptr, true, 1, 1.0, "", "0", "01", "+1", "-1", " 1", "1 ",
             "1e1", "18446744073709551616", "100000000000000000000"}) {
      auto document = policy_document();
      document["policy_revision"] = value;
      reject_policy(document);
    }
    for (const auto& value : std::vector<Json>{nullptr, false, 0, -1, 1.0, "1",
                                               9007199254740992ULL}) {
      auto document = policy_document();
      document["entries"][0]["projection_contract"]["version"] = value;
      reject_policy(document);
    }
    for (const auto* revision : {"1", "9", "18446744073709551615"}) {
      auto document = policy_document();
      document["policy_revision"] = revision;
      REQUIRE(accept_policy(document).policy_revision == revision);
    }
  }

  SECTION("identity digest signature and UTF8 byte bounds are not coercions") {
    for (const auto* uuid : {"", "00000000000000000000000000000000",
                             "AAAAAAAA-0000-0000-0000-000000000000",
                             "00000000-0000-0000-0000-00000000000g",
                             " 00000000-0000-0000-0000-000000000000"}) {
      auto document = policy_document();
      document["review"]["admin_user_id"] = uuid;
      reject_policy(document);
    }
    for (const auto* field : {"artifact_digest", "schema_digest"}) {
      for (const auto& digest : std::vector<Json>{
               nullptr, false, std::string(63, 'a'), std::string(65, 'a'),
               std::string(64, 'A'), std::string(64, 'g')}) {
        auto document = policy_document();
        document["entries"][0][field] = digest;
        reject_policy(document);
      }
    }
    auto document = policy_document();
    document["review"]["evidence_sha256"] = std::string(64, 'A');
    reject_policy(document);
    document = policy_document();
    document["review"]["review_ref"] = std::string(256, 'x');
    REQUIRE(accept_policy(document).review.review_ref.size() == 256);
    document["review"]["review_ref"] = std::string(257, 'x');
    reject_policy(document);
    std::string unicode_reference;
    for (int i = 0; i != 128; ++i)
      unicode_reference += "é";
    document["review"]["review_ref"] = unicode_reference;
    REQUIRE(accept_policy(document).review.review_ref.size() == 256);
    document["review"]["review_ref"] = unicode_reference + "é";
    reject_policy(document);
    document = policy_document();
    document["entries"][0]["application_id"] = std::string(64, 'a');
    document["entries"][0]["panel_id"] = std::string(64, 'a');
    document["entries"][0]["projection_contract"]["id"] = std::string(64, 'a');
    REQUIRE(accept_policy(document).entries.size() == 1);
    for (const auto* value : {"a", "Notes", "bad_name", "bad.name", "-notes"}) {
      document = policy_document();
      document["entries"][0]["application_id"] = value;
      reject_policy(document);
    }
    document = policy_document();
    document["entries"][0]["application_id"] = std::string(65, 'a');
    reject_policy(document);
    for (const auto& value : std::vector<Json>{nullptr, false, "", "A", "-a",
                                               std::string(65, 'a')}) {
      document = policy_document();
      document["entries"][0]["panel_id"] = value;
      reject_policy(document);
      document = policy_document();
      document["entries"][0]["projection_contract"]["id"] = value;
      reject_policy(document);
    }
    document = policy_document();
    document["entries"][0]["panel_id"] = "a_b-1";
    document["entries"][0]["capability"] =
        "other_namespace:2147483647:read.part";
    REQUIRE(accept_policy(document).entries.front().capability ==
            "other_namespace:2147483647:read.part");
    for (const auto* signature :
         {"notes:0:read", "notes:2147483648:read", "notes:+1:read",
          "notes:1.0:read", "notes:1:read\n", "notes:1:..read",
          "notes:1:read:", "notes:*:read"}) {
      document = policy_document();
      document["entries"][0]["capability"] = signature;
      reject_policy(document);
    }
    document = policy_document();
    document["entries"][0]["capability"] =
        "notes:" + std::string(244, '0') + "1:read";
    REQUIRE(document["entries"][0]["capability"].get<std::string>().size() ==
            256);
    REQUIRE(accept_policy(document).entries.front().capability ==
            "notes:1:read");
    document["entries"][0]["capability"] =
        "notes:" + std::string(245, '0') + "1:read";
    reject_policy(document);
  }

  SECTION("review timestamps validate real calendar offset and potential leap "
          "position") {
    for (const auto* timestamp :
         {"0000-02-29t00:00:00z", "2000-02-29T23:59:59Z",
          "9999-12-31T23:59:59Z", "2024-01-01T00:00:00+23:59",
          "2024-01-01T00:00:00-00:00", "2016-12-31T23:59:60Z",
          "2024-02-29T23:59:60Z", "1900-02-28T23:59:60Z",
          "2024-03-01T00:59:60+01:00", "2024-02-29T22:59:60-01:00",
          "2024-03-01T23:58:60+23:59", "2024-02-29T00:00:60-23:59",
          "2024-02-29T23:59:60-00:00", "2024-01-31T23:59:60.12345678901Z",
          "0000-01-01T00:59:60+01:00", "9999-12-31T22:59:60-01:00"}) {
      auto document = policy_document();
      document["review"]["reviewed_at"] = timestamp;
      REQUIRE(accept_policy(document).review.reviewed_at == timestamp);
    }
    for (const auto* timestamp : {"1900-02-29T00:00:00Z",
                                  "2023-02-29T00:00:00Z",
                                  "2024-04-31T00:00:00Z",
                                  "2024-00-01T00:00:00Z",
                                  "2024-01-00T00:00:00Z",
                                  "2024-01-01T24:00:00Z",
                                  "2024-01-01T00:60:00Z",
                                  "2024-01-01T00:00:61Z",
                                  "2024-01-01 00:00:00Z",
                                  "2024-01-01T00:00:00",
                                  "2024-01-01T00:00:00+24:00",
                                  "2024-01-01T00:00:00+00:60",
                                  "2024-01-01T00:00:00+0000",
                                  "2024-01-01T00:00:00+00:00:00",
                                  "2024-01-01T00:00:00.Z",
                                  "2024-01-01T00:00:00,1Z",
                                  "2024-01-01T00:00:00Z ",
                                  "2024-01-30T23:59:60Z",
                                  "2024-02-29T23:59:60+01:00",
                                  "2024-03-01T00:00:60Z",
                                  "2024-01-31T23:59:60.123456789012Z",
                                  "0000-01-01T01:59:60+01:00",
                                  "9999-12-31T21:59:60-01:00"}) {
      auto document = policy_document();
      document["review"]["reviewed_at"] = timestamp;
      reject_policy(document);
    }
  }

  SECTION("raw duplicates invalid Unicode and malformed JSON fail before DOM "
          "erasure") {
    for (
        const auto* raw :
        {R"({"format_version":1,"\u0066ormat_version":1})",
         R"({"review":{"admin_user_id":"a","admin_\u0075ser_id":"b"}})",
         R"({"entries":[{"capability":"notes:1:read","capabilit\u0079":"notes:1:read"}]})",
         R"({"entries":[{"projection_contract":{"id":"x","\u0069d":"y"}}]})",
         R"({"entries":[{"fields":[{"key":"x","\u006bey":"y"}]}]})",
         R"({"format_version":1,"opaque":{"a":1,"\u0061":2}})"}) {
      reject_policy(std::string_view{raw}, restore::CodecError::DUPLICATE_KEY);
    }
    for (const auto* raw :
         {"", "{", "[1,]", R"({"x":"\ud800"})", R"({"x":"\udc00"})",
          R"({"format_version":1e10000})"}) {
      reject_policy(std::string_view{raw}, restore::CodecError::INVALID_JSON);
    }
    auto raw = policy_document().dump();
    const auto position = raw.find("fixture-review");
    REQUIRE(position != std::string::npos);
    raw.replace(position, 14, std::string{"\xc0\xaf", 2});
    reject_policy(std::string_view{raw}, restore::CodecError::INVALID_JSON);
    raw = policy_document().dump();
    raw.insert(raw.find("fixture-review"), 1, '\0');
    reject_policy(std::string_view{raw}, restore::CodecError::INVALID_JSON);
    raw = policy_document().dump() + " trailing";
    reject_policy(std::string_view{raw}, restore::CodecError::INVALID_JSON);
    raw = "{\"" + std::string{"\xc0\xaf", 2} + "\":0}";
    reject_policy(std::string_view{raw}, restore::CodecError::INVALID_JSON);
  }

  SECTION("field bounds duplicate bindings and normalized capability aliases "
          "fail whole policy") {
    auto document = policy_document();
    document["entries"][0]["fields"] = Json::array();
    for (int i = 0; i != 16; ++i)
      document["entries"][0]["fields"].push_back(
          {{"key", "k" + std::to_string(i)},
           {"meaning", std::string(256, 'x')}});
    REQUIRE(accept_policy(document).entries.front().fields.size() == 16);
    document["entries"][0]["fields"].push_back(
        {{"key", "extra"}, {"meaning", "x"}});
    reject_policy(document);
    for (const auto& key : std::vector<Json>{
             nullptr, false, "", "__proto__", "prototype", "constructor",
             std::string(65, 'a'), std::string{"a\0b", 3}}) {
      document = policy_document();
      document["entries"][0]["fields"][0]["key"] = key;
      reject_policy(document);
    }
    for (const auto& meaning :
         std::vector<Json>{nullptr, false, "", std::string(257, 'a')}) {
      document = policy_document();
      document["entries"][0]["fields"][0]["meaning"] = meaning;
      reject_policy(document);
    }
    document = policy_document();
    document["entries"][0]["fields"][0]["key"] = std::string(64, 'a');
    REQUIRE(accept_policy(document).entries.front().fields.front().key.size() ==
            64);
    document["entries"][0]["fields"].push_back(
        document["entries"][0]["fields"][0]);
    reject_policy(document);
    std::string unicode_key;
    for (int i = 0; i != 32; ++i)
      unicode_key += "é";
    std::string unicode_meaning;
    for (int i = 0; i != 128; ++i)
      unicode_meaning += "é";
    document = policy_document();
    document["entries"][0]["fields"][0]["key"] = unicode_key;
    document["entries"][0]["fields"][0]["meaning"] = unicode_meaning;
    REQUIRE(accept_policy(document).entries.front().fields.front().key.size() ==
            64);
    REQUIRE(
        accept_policy(document).entries.front().fields.front().meaning.size() ==
        256);
    document["entries"][0]["fields"][0]["key"] = unicode_key + "é";
    reject_policy(document);
    document["entries"][0]["fields"][0]["key"] = unicode_key;
    document["entries"][0]["fields"][0]["meaning"] = unicode_meaning + "é";
    reject_policy(document);
    for (int variant = 0; variant != 3; ++variant) {
      document = policy_document();
      auto second = document["entries"][0];
      if (variant == 1) second["projection_contract"]["version"] = 2;
      if (variant == 2) second["capability"] = "notes:0001:read";
      document["entries"].push_back(second);
      reject_policy(document, restore::CodecError::CONFLICTING_BINDING);
    }
    document = policy_document();
    auto escaped_alias = document["entries"][0];
    escaped_alias["capability"] = "notes:01:read";
    document["entries"].push_back(escaped_alias);
    auto escaped_raw = document.dump();
    const auto alias_position = escaped_raw.find("notes:01:read");
    REQUIRE(alias_position != std::string::npos);
    escaped_raw.replace(alias_position,
                        std::string_view{"notes:01:read"}.size(),
                        R"(notes:\u0030\u0031:read)");
    reject_policy(std::string_view{escaped_raw},
                  restore::CodecError::CONFLICTING_BINDING);
    document = policy_document();
    auto second = document["entries"][0];
    second["artifact_digest"] = std::string(64, 'd');
    document["entries"].push_back(second);
    second = document["entries"][0];
    second["schema_digest"] = std::string(64, 'e');
    document["entries"].push_back(second);
    REQUIRE(accept_policy(document).entries.size() == 3);
    document["entries"][2]["panel_id"] = nullptr;
    reject_policy(document); // No valid subset is salvaged.
  }

  SECTION("raw bytes depth nodes entries and compact original-entry bounds are "
          "exact") {
    const auto base = policy_document().dump();
    auto padded =
        std::string(restore::POLICY_MAX_BYTES - base.size(), ' ') + base;
    REQUIRE(restore::parse_policy(padded).has_value());
    padded.insert(padded.begin(), ' ');
    reject_policy(std::string_view{padded}, restore::CodecError::TOO_LARGE);
    reject_policy(std::string_view{
        "[[[[[0]]]]]"}); // Exactly depth6, invalid policy shape.
    reject_policy(std::string_view{"[[[[[[0]]]]]]"},
                  restore::CodecError::LIMIT_EXCEEDED);
    auto nodes = Json::array();
    for (std::size_t i = 1; i < restore::JSON_MAX_NODES; ++i)
      nodes.push_back(nullptr);
    reject_policy(nodes); // Root plus16383scalars is exactly16384nodes.
    nodes.push_back(nullptr);
    reject_policy(nodes, restore::CodecError::LIMIT_EXCEEDED);
    auto document = policy_document();
    document["entries"] = Json::array();
    for (std::size_t i = 0; i != restore::POLICY_MAX_ENTRIES; ++i) {
      auto entry = policy_entry();
      entry["panel_id"] = "p" + std::to_string(i);
      document["entries"].push_back(std::move(entry));
    }
    REQUIRE(accept_policy(document).entries.size() == 256);
    auto extra = policy_entry();
    extra["panel_id"] = "extra";
    document["entries"].push_back(extra);
    reject_policy(document);

    document = policy_document();
    auto& entry = document["entries"][0];
    entry["capability"] = "notes:0001:read";
    entry["fields"] = Json::array();
    for (int i = 0; i != 16; ++i)
      entry["fields"].push_back(
          {{"key", "k" + std::to_string(i)}, {"meaning", "x"}});
    auto remaining = restore::POLICY_ENTRY_MAX_BYTES - entry.dump().size();
    for (auto& field : entry["fields"]) {
      auto& meaning = field["meaning"].get_ref<std::string&>();
      while (remaining >= 6 && meaning.size() < 256) {
        meaning.push_back('\1');
        remaining -= 6;
      }
      while (remaining != 0 && meaning.size() < 256) {
        meaning.push_back('x');
        --remaining;
      }
    }
    REQUIRE(remaining == 0);
    REQUIRE(entry.dump().size() == restore::POLICY_ENTRY_MAX_BYTES);
    REQUIRE(accept_policy(document).entries.front().capability ==
            "notes:1:read");
    entry["fields"][15]["meaning"].get_ref<std::string&>().push_back('x');
    REQUIRE(entry.dump().size() == restore::POLICY_ENTRY_MAX_BYTES + 1);
    reject_policy(document, restore::CodecError::TOO_LARGE);
  }
}
