#include "kernel/frontend/persistence_projection.hpp"
#include "kernel/packages/restore_digest.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
namespace frontend = plinth::frontend;
namespace detail = frontend::projection_detail;
using CodecError = plinth::packages::restore::CodecError;

auto schema() -> plinth::packages::PanelContextSchema {
  return {.version = 1,
          .fields = {{"record_id",
                      {.required = true,
                       .max_bytes = 128,
                       .meaning = "Stable fake record identifier"}},
                     {"query",
                      {.required = false,
                       .max_bytes = 512,
                       .meaning = "Transient query text"}}},
          .persist_keys = {"record_id"}};
}
} // namespace

TEST_CASE(
    "persistence projection: operational failure is not semantic ineligibility",
    "[frontend][persistence_projection]") {
  SECTION("actual codec-result boundary distinguishes operational and semantic "
          "errors") {
    const auto success = detail::checked_codec(std::string{"unchanged-digest"});
    REQUIRE(success);
    REQUIRE(*success == "unchanged-digest");
    for (const auto error :
         {CodecError::TOO_LARGE, CodecError::INVALID_JSON,
          CodecError::DUPLICATE_KEY, CodecError::INVALID_POLICY,
          CodecError::CONFLICTING_BINDING, CodecError::INVALID_INVENTORY,
          CodecError::INVALID_SCHEMA}) {
      REQUIRE_FALSE(detail::checked_codec(std::unexpected(error)));
    }
    for (const auto error :
         {CodecError::LIMIT_EXCEEDED, CodecError::HASHING_FAILED}) {
      REQUIRE_THROWS_AS(detail::checked_codec(std::unexpected(error)),
                        frontend::ProjectionUnavailable);
    }
    const frontend::ProjectionUnavailable unavailable;
    REQUIRE(std::string_view{unavailable.what()} ==
            "persistence projection unavailable");
  }
  SECTION("same production allocation boundary throws fixed error once without "
          "allocator injection") {
    unsigned calls = 0;
    REQUIRE_THROWS_AS(detail::operational_boundary([&]() -> bool {
                        ++calls;
                        throw std::bad_alloc{};
                      }),
                      frontend::ProjectionUnavailable);
    REQUIRE(calls == 1);
    REQUIRE_THROWS_AS(detail::operational_boundary([&]() -> bool {
                        ++calls;
                        throw std::length_error{"controlled length failure"};
                      }),
                      frontend::ProjectionUnavailable);
    REQUIRE(calls == 2);
    REQUIRE_FALSE(detail::operational_boundary([&] {
      ++calls;
      return false;
    }));
    REQUIRE(calls == 3);
    REQUIRE(detail::operational_boundary([] { return 17; }) == 17);
    REQUIRE_THROWS_AS(detail::operational_boundary([]() -> bool {
                        throw std::runtime_error{"not an allocation error"};
                      }),
                      std::runtime_error);
    REQUIRE_THROWS_AS(detail::operational_boundary([] {
                        return detail::checked_codec(
                            std::unexpected(CodecError::HASHING_FAILED));
                      }),
                      frontend::ProjectionUnavailable);
  }
  SECTION("actual context and projection APIs retain semantic invalid mismatch "
          "and required-key refusal") {
    const auto contract = schema();
    const auto digest = plinth::packages::restore::schema_digest(contract);
    REQUIRE(digest);
    // Pure projection metadata is not ProtectedPolicy or an admission token.
    // Protected-policy matching itself remains the actual deployment fixture.
    const frontend::PersistenceProjection metadata{
        .projection_contract = {.id = "record-v1", .version = 1},
        .artifact_digest = std::string(64, 'a'),
        .schema_digest = *digest,
        .policy_revision = "1",
        .approved_keys = {"record_id"}};
    const nlohmann::json context = {{"record_id", "fake-record"},
                                    {"query", "unsaved"}};
    REQUIRE(frontend::validate_float_context(contract, context));
    const auto projected =
        frontend::project_float_context(metadata, contract, context);
    REQUIRE(projected);
    REQUIRE(*projected == nlohmann::json{{"record_id", "fake-record"}});
    REQUIRE_FALSE(frontend::validate_float_context(
        contract, nlohmann::json{{"query", "missing record"}}));
    REQUIRE_FALSE(frontend::project_float_context(
        metadata, contract, nlohmann::json{{"query", "missing record"}}));
    REQUIRE_FALSE(frontend::project_float_context(
        metadata, contract, nlohmann::json{{"record_id", 1}}));
    REQUIRE_FALSE(frontend::project_float_context(
        metadata, contract,
        nlohmann::json{{"record_id", "fake-record"}, {"extra", "unknown"}}));
    auto changed = metadata;
    changed.schema_digest = std::string(64, 'b');
    REQUIRE_FALSE(frontend::project_float_context(changed, contract, context));
    changed = metadata;
    changed.approved_keys.clear();
    REQUIRE_FALSE(frontend::project_float_context(changed, contract, context));
    auto invalid = contract;
    invalid.version = 0;
    REQUIRE_FALSE(frontend::validate_float_context(invalid, context));
    REQUIRE_FALSE(frontend::project_float_context(metadata, invalid, context));
  }
}
