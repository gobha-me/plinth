#include "kernel/frontend/typed_requests.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <expected>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
namespace frontend = plinth::frontend;

auto fresh(std::string_view panel = "preview") -> Json {
  return {{"application_id", "notes"},
          {"panel_id", panel},
          {"kind", "float"},
          {"expected_generation", nullptr},
          {"content_type", "text/plain"}};
}
auto envelope(Json targets) -> Json {
  return {{"schema_version", 1}, {"targets", std::move(targets)}};
}
auto reject(const Json& value) -> void {
  const auto result = frontend::parse_surface_admission(value.dump());
  REQUIRE_FALSE(result);
  REQUIRE(result.error() == frontend::TypedRequestError::INVALID);
}
} // namespace

TEST_CASE("typed admission: exact bounded raw lookup variants",
          "[frontend][typed_requests]") {
  SECTION("actual parser boundary preserves operational unknown without "
          "global allocator injection or retry") {
    using Result = std::expected<std::vector<frontend::SurfaceLookup>,
                                 frontend::TypedRequestError>;
    unsigned int calls = 0;
    const auto allocation =
        frontend::typed_request_detail::operational_boundary([&]() -> Result {
          ++calls;
          throw std::bad_alloc{};
        });
    REQUIRE_FALSE(allocation);
    REQUIRE(allocation.error() == frontend::TypedRequestError::UNAVAILABLE);
    REQUIRE(calls == 1);
    const auto length =
        frontend::typed_request_detail::operational_boundary([&]() -> Result {
          ++calls;
          throw std::length_error{"controlled parser length failure"};
        });
    REQUIRE_FALSE(length);
    REQUIRE(length.error() == frontend::TypedRequestError::UNAVAILABLE);
    REQUIRE(calls == 2);
    for (const auto error : {frontend::TypedRequestError::INVALID,
                             frontend::TypedRequestError::TOO_LARGE}) {
      const auto semantic =
          frontend::typed_request_detail::operational_boundary(
              [error]() -> Result { return std::unexpected(error); });
      REQUIRE_FALSE(semantic);
      REQUIRE(semantic.error() == error);
    }
    REQUIRE(frontend::typed_request_detail::operational_boundary(
        []() -> Result { return std::vector<frontend::SurfaceLookup>{}; }));
    REQUIRE_THROWS_AS(
        frontend::typed_request_detail::operational_boundary([]() -> Result {
          throw std::runtime_error{"unrelated exception"};
        }),
        std::runtime_error);
    // These exercise the SAME actual parser boundary with explicit throwing
    // operations, not physical OOM or an authority/token construction seam.
  }
  SECTION("fresh float and pinned canonical capability are distinct") {
    auto input = fresh();
    auto result = frontend::parse_surface_admission(
        envelope(Json::array({input})).dump());
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    REQUIRE((*result)[0].kind == frontend::SurfaceKind::FLOAT);
    REQUIRE_FALSE((*result)[0].expected_generation);
    REQUIRE((*result)[0].content_type == "text/plain");
    REQUIRE_FALSE((*result)[0].capability);
    input.erase("content_type");
    input["expected_generation"] = "generation-1";
    input["capability"] = "notes:1:read";
    result = frontend::parse_surface_admission(
        envelope(Json::array({input})).dump());
    REQUIRE(result.has_value());
    REQUIRE((*result)[0].expected_generation == "generation-1");
    REQUIRE((*result)[0].capability == "notes:1:read");
    REQUIRE_FALSE((*result)[0].content_type);
    input["capability"] = "notes:01:read";
    reject(envelope(Json::array({input})));
  }
  SECTION("primary and tray omit all float-only fields") {
    for (const auto kind : {"primary", "tray"}) {
      auto input = fresh();
      input["kind"] = kind;
      input.erase("content_type");
      REQUIRE(frontend::parse_surface_admission(
          envelope(Json::array({input})).dump()));
      input["expected_generation"] = "generation-1";
      REQUIRE(frontend::parse_surface_admission(
          envelope(Json::array({input})).dump()));
      input["capability"] = "notes:1:read";
      reject(envelope(Json::array({input})));
    }
  }
  SECTION("whole-envelope bounds and duplicate logical identities") {
    auto targets = Json::array();
    REQUIRE(frontend::parse_surface_admission(envelope(targets).dump()));
    for (int index = 0; index < 38; ++index)
      targets.push_back(fresh("p" + std::to_string(index)));
    REQUIRE(
        frontend::parse_surface_admission(envelope(targets).dump())->size() ==
        38);
    targets.push_back(fresh("p38"));
    reject(envelope(targets));
    auto first = fresh();
    auto pinned = first;
    pinned.erase("content_type");
    pinned["expected_generation"] = "other";
    pinned["capability"] = "notes:1:read";
    reject(envelope(Json::array({first, pinned})));
    reject(envelope(Json::array({first, first})));
    reject({{"schema_version", 1.0}, {"targets", Json::array()}});
    reject({{"schema_version", true}, {"targets", Json::array()}});
    reject({{"schema_version", 2}, {"targets", Json::array()}});
    reject({{"schema_version", 1}, {"targets", Json::object()}});
    reject({{"schema_version", 1}, {"targets", Json::array()}, {"extra", 1}});
  }
  SECTION("unknown missing mixed malformed and overlong target fields") {
    for (const auto key : {"application_id", "panel_id", "kind",
                           "expected_generation", "content_type"}) {
      auto input = fresh();
      input.erase(key);
      reject(envelope(Json::array({input})));
    }
    for (const auto& value :
         {Json(nullptr), Json(true), Json(0), Json::object()}) {
      auto input = fresh();
      input["expected_generation"] = value;
      if (!value.is_null()) reject(envelope(Json::array({input})));
    }
    for (const auto value : {"", "Notes", "a", "notes_thing", "notes\n"}) {
      auto input = fresh();
      input["application_id"] = value;
      reject(envelope(Json::array({input})));
    }
    auto input = fresh();
    input["capability"] = "notes:1:read";
    reject(envelope(Json::array({input})));
    input = fresh();
    input["expected_generation"] = "not-fresh";
    reject(envelope(Json::array({input})));
    input.erase("content_type");
    input["capability"] = "notes:1:read";
    for (const auto& generation : {std::string{}, std::string(257, 'a')}) {
      input["expected_generation"] = generation;
      reject(envelope(Json::array({input})));
    }
  }
  SECTION(
      "decoded duplicate keys UTF8 nesting and raw bytes fail before DOM use") {
    for (
        const auto raw :
        {R"({"schema_version":1,"schema_versio\u006e":1,"targets":[]})",
         R"({"schema_version":1,"targets":[{"application_id":"notes","panel_id":"p","kind":"float","expected_generation":null,"content_type":"text/plain","content_\u0074ype":"text/plain"}]})",
         R"({"schema_version":1,"targets":[],"extra":[[[[[[0]]]]]]})",
         R"({"schema_version":1,"targets":[])"}) {
      const auto result = frontend::parse_surface_admission(raw);
      REQUIRE_FALSE(result);
      REQUIRE(result.error() == frontend::TypedRequestError::INVALID);
    }
    std::string invalid_utf8 = envelope(Json::array({fresh()})).dump();
    invalid_utf8.insert(1, "\xff");
    const auto invalid = frontend::parse_surface_admission(invalid_utf8);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error() == frontend::TypedRequestError::INVALID);
    auto padded = envelope(Json::array()).dump();
    padded.append(32768 - padded.size(), ' ');
    REQUIRE(frontend::parse_surface_admission(padded));
    const auto oversized =
        frontend::parse_surface_admission(std::string(32769, ' '));
    REQUIRE_FALSE(oversized);
    REQUIRE(oversized.error() == frontend::TypedRequestError::TOO_LARGE);
  }
  SECTION("canonical literal MIME rejects normalization parameters wildcards "
          "and separators") {
    for (const auto value : {"text/plain", "application/vnd.notes+json", "x/0"})
      REQUIRE(frontend::is_canonical_content_type(value));
    for (const auto value :
         {"", "TEXT/plain", "text/PLAIN", "text/plain;charset=utf8", "text/*",
          " text/plain", "text/plain\n", "text//plain", "/plain", "text/",
          "a/.b"})
      REQUIRE_FALSE(frontend::is_canonical_content_type(value));
    REQUIRE(frontend::is_canonical_content_type("a/" + std::string(126, 'b')));
    REQUIRE_FALSE(
        frontend::is_canonical_content_type("a/" + std::string(127, 'b')));
  }
}
