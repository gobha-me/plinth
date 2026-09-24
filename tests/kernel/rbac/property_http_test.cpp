#include "property_graph.hpp"

#include "kernel/auth/csrf.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/rbac/enforcement.hpp"

#include "../packages/http_test_fixture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <json/reader.h>
#include <json/value.h>
#include <json/writer.h>
#include <libpq-fe.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace prop = plinth::rbac_property;

constexpr auto INSPECT_RULE = "property89.inspect";
constexpr auto ROUTE = "/api/packages";

using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto checked(plinth::ws_test::TestPg& pg, const std::string& sql,
             const std::vector<std::string>& params = {}) -> PgResult {
  auto result = pg.exec_params(sql, params);
  if (result == nullptr) {
    throw std::runtime_error("property RBAC query returned no result");
  }
  const auto status = PQresultStatus(result.get());
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    throw std::runtime_error("property RBAC SQL failed: " +
                             std::string{PQresultErrorMessage(result.get())});
  }
  return result;
}

auto first_value(plinth::ws_test::TestPg& pg, const std::string& sql,
                 const std::vector<std::string>& params = {}) -> std::string {
  auto result = checked(pg, sql, params);
  if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1) {
    throw std::runtime_error("property RBAC expected exactly one scalar row");
  }
  return PQgetvalue(result.get(), 0, 0);
}

auto parse_json(const drogon::HttpResponsePtr& response) -> Json::Value {
  Json::Value value;
  Json::CharReaderBuilder builder;
  std::string error;
  auto reader = std::unique_ptr<Json::CharReader>{builder.newCharReader()};
  const auto body = std::string{response->body()};
  if (!reader->parse(body.data(), body.data() + body.size(), &value, &error)) {
    throw std::runtime_error("property RBAC response is not JSON: " + error);
  }
  return value;
}

class PropertyHttpHarness {
 public:
  PropertyHttpHarness()
      : http_(), pg_(plinth::ws_test::test_config().db),
        user_id_(plinth::ws_test::insert_user(pg_, "property89-user", "fake")),
        original_requirement_(
            plinth::rbac::get_required_rules(drogon::Get, ROUTE)) {
    if (!original_requirement_) {
      throw std::runtime_error("property RBAC route requirement is absent");
    }
    plinth::ws_test::insert_session(pg_, user_id_, TOKEN);
    groups_[0] = first_value(
        pg_, "SELECT id::text FROM plinth.groups WHERE name='everyone'");
    groups_[1] = first_value(
        pg_, "SELECT id::text FROM plinth.groups WHERE name='admin'");
    for (std::size_t group = 2; group < prop::GROUP_COUNT; ++group) {
      groups_[group] = first_value(
          pg_, "INSERT INTO plinth.groups(name) VALUES ($1) RETURNING id::text",
          {"property89-group" + std::to_string(group)});
    }
    rules_[0] = first_value(
        pg_,
        "SELECT id::text FROM plinth.rbac_rules WHERE rule='kernel.admin'");
    for (std::size_t rule = 1; rule < prop::RULE_COUNT; ++rule) {
      rules_[rule] = first_value(
          pg_,
          "INSERT INTO plinth.rbac_rules"
          "(rule,namespace,description,extension_name) "
          "VALUES ($1,'property89','generated RBAC rule','property89') "
          "RETURNING id::text",
          {std::string{prop::RULE_NAMES[rule]}});
    }
    inspect_id_ = first_value(
        pg_,
        "INSERT INTO plinth.rbac_rules"
        "(rule,namespace,description,extension_name) "
        "VALUES ($1,'property89','test-only inspection rule','property89') "
        "RETURNING id::text",
        {INSPECT_RULE});
    checked(pg_,
            "INSERT INTO plinth.group_rules(group_id,rule_id) "
            "VALUES ($1::uuid,$2::uuid)",
            {groups_[0], inspect_id_});
    plinth::capabilities::clear_resolver_for_test();
    plinth::capabilities::register_tier1_handler(
        "property89:1:inspect", INSPECT_RULE,
        [](const Json::Value&, const plinth::capabilities::UserContext& context,
           int) -> plinth::capabilities::HandlerOutcome {
          Json::Value rules(Json::arrayValue);
          for (const auto& rule : context.effective_rules) {
            rules.append(rule);
          }
          return rules;
        });
  }

  ~PropertyHttpHarness() {
    plinth::rbac::register_rule_requirement(drogon::Get, ROUTE,
                                            *original_requirement_);
    plinth::capabilities::clear_resolver_for_test();
  }

  PropertyHttpHarness(const PropertyHttpHarness&) = delete;
  auto operator=(const PropertyHttpHarness&) -> PropertyHttpHarness& = delete;
  PropertyHttpHarness(PropertyHttpHarness&&) = delete;
  auto operator=(PropertyHttpHarness&&) -> PropertyHttpHarness& = delete;

  auto install(const prop::Graph& graph) -> void {
    // The filter and cap-dispatch loader use other PG connections: commit the
    // complete generated graph before observing it through real HTTP.
    checked(pg_, "BEGIN");
    try {
      checked(pg_, "DELETE FROM plinth.group_members WHERE user_id=$1::uuid",
              {user_id_});
      for (const auto& group : groups_) {
        checked(pg_,
                "DELETE FROM plinth.group_rules WHERE group_id=$1::uuid "
                "AND rule_id IN ($2::uuid,$3::uuid,$4::uuid,$5::uuid,$6::uuid) "
                "AND NOT (group_id=$7::uuid AND rule_id=$2::uuid)",
                {group, rules_[0], rules_[1], rules_[2], rules_[3], rules_[4],
                 groups_[1]});
      }
      for (std::size_t rule = 1; rule < prop::RULE_COUNT; ++rule) {
        checked(pg_,
                "UPDATE plinth.rbac_rules SET orphaned_at=NULL "
                "WHERE id=$1::uuid",
                {rules_[rule]});
      }
      for (std::size_t group = 1; group < prop::GROUP_COUNT; ++group) {
        if (graph.membership[group]) {
          checked(pg_,
                  "INSERT INTO plinth.group_members(group_id,user_id) "
                  "VALUES ($1::uuid,$2::uuid)",
                  {groups_[group], user_id_});
        }
      }
      for (std::size_t group = 0; group < prop::GROUP_COUNT; ++group) {
        for (std::size_t rule = 0; rule < prop::RULE_COUNT; ++rule) {
          if (graph.grants[group][rule] && !(group == 1 && rule == 0)) {
            checked(pg_,
                    "INSERT INTO plinth.group_rules(group_id,rule_id) "
                    "VALUES ($1::uuid,$2::uuid)",
                    {groups_[group], rules_[rule]});
          }
        }
      }
      for (std::size_t rule = 1; rule < prop::RULE_COUNT; ++rule) {
        if (graph.orphan[rule]) {
          checked(pg_,
                  "UPDATE plinth.rbac_rules SET orphaned_at=NOW() "
                  "WHERE id=$1::uuid",
                  {rules_[rule]});
        }
      }
      checked(pg_, "COMMIT");
    } catch (...) {
      checked(pg_, "ROLLBACK");
      throw;
    }
  }

  auto route_allows(const prop::Graph& graph) -> bool {
    std::vector<std::string> required;
    for (std::size_t rule = 0; rule < prop::RULE_COUNT; ++rule) {
      if (graph.required[rule]) {
        required.emplace_back(prop::RULE_NAMES[rule]);
      }
    }
    plinth::rbac::register_rule_requirement(drogon::Get, ROUTE,
                                            std::move(required));
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(ROUTE);
    request->addCookie("plinth_session", TOKEN);
    const auto response = http_.dispatch(request);
    if (response->statusCode() == drogon::k200OK) {
      return true;
    }
    if (response->statusCode() == drogon::k403Forbidden) {
      return false;
    }
    throw std::runtime_error("property RBAC GET returned unexpected HTTP " +
                             std::to_string(response->statusCode()));
  }

  auto missing_rule_response() -> drogon::HttpResponsePtr {
    plinth::rbac::register_rule_requirement(drogon::Get, ROUTE,
                                            {"property89.absent"});
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Get);
    request->setPath(ROUTE);
    request->addCookie("plinth_session", TOKEN);
    return http_.dispatch(request);
  }

  auto capability_rules() -> std::vector<std::string> {
    auto request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath("/api/cap/property89.inspect");
    request->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    request->setBody(R"({"args":null})");
    request->addCookie("plinth_session", TOKEN);
    const auto csrf = plinth::auth::csrf_token_for_session(TOKEN);
    request->addCookie(std::string{plinth::auth::CSRF_COOKIE}, csrf);
    request->addHeader(std::string{plinth::auth::CSRF_HEADER}, csrf);
    request->addHeader("Origin",
                       "http://127.0.0.1:" +
                           std::to_string(plinth::ws_test::test_server_port()));
    const auto response = http_.dispatch(request);
    if (response->statusCode() != drogon::k200OK) {
      throw std::runtime_error("property RBAC inspection capability failed: " +
                               std::string{response->body()});
    }
    const auto body = parse_json(response);
    if (!body["ok"].asBool() || !body["value"].isArray()) {
      throw std::runtime_error("property RBAC inspection response malformed");
    }
    std::vector<std::string> rules;
    for (const auto& rule : body["value"]) {
      auto name = rule.asString();
      // The bootstrap also grants unrelated built-in application events to
      // everyone. Compare the generated graph's managed rule projection.
      if (name == INSPECT_RULE ||
          std::ranges::find(prop::RULE_NAMES, name) != prop::RULE_NAMES.end()) {
        rules.push_back(std::move(name));
      }
    }
    std::ranges::sort(rules);
    return rules;
  }

 private:
  static constexpr auto TOKEN = "property89-fake-session";
  plinth::http_test::HttpTestFixture http_;
  plinth::ws_test::TestPg pg_;
  std::string user_id_;
  std::array<std::string, prop::GROUP_COUNT> groups_;
  std::array<std::string, prop::RULE_COUNT> rules_;
  std::string inspect_id_;
  std::optional<std::vector<std::string>> original_requirement_;
};

auto expected_rules(const prop::Graph& graph) -> std::vector<std::string> {
  const auto bits = prop::effective(graph);
  std::vector<std::string> rules{INSPECT_RULE};
  for (std::size_t rule = 0; rule < prop::RULE_COUNT; ++rule) {
    if (bits[rule]) {
      rules.emplace_back(prop::RULE_NAMES[rule]);
    }
  }
  std::ranges::sort(rules);
  return rules;
}

struct Failure {
  std::size_t stage;
  std::string detail;
};

auto find_failure(PropertyHttpHarness& harness, const prop::Case& sample)
    -> std::optional<Failure> {
  auto graph = sample.initial;
  for (std::size_t stage = 0; stage <= sample.operations.size(); ++stage) {
    harness.install(graph);
    const auto granted = harness.route_allows(graph);
    const auto expected_grant = prop::allows(graph);
    if (granted != expected_grant) {
      return Failure{stage,
                     "real RbacFilter decision disagrees with graph oracle"};
    }
    const auto actual_rules = harness.capability_rules();
    if (actual_rules != expected_rules(graph)) {
      return Failure{
          stage, "real capability effective rules disagree with graph oracle"};
    }
    if (stage < sample.operations.size()) {
      prop::apply(graph, sample.operations[stage]);
    }
  }
  return std::nullopt;
}

} // namespace

TEST_CASE("RBAC virtual everyone, orphan recovery, and admin stay live",
          "[ws][integration][rbac][property]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  PropertyHttpHarness harness;
  prop::Graph graph;
  graph.required[2] = true;
  graph.grants[prop::EVERYONE][2] = true;
  harness.install(graph);
  REQUIRE(harness.route_allows(graph));
  REQUIRE(harness.capability_rules() == expected_rules(graph));

  graph.orphan[2] = true;
  harness.install(graph);
  REQUIRE_FALSE(harness.route_allows(graph));
  REQUIRE(harness.capability_rules() == expected_rules(graph));

  graph.membership[prop::ADMIN] = true;
  harness.install(graph);
  REQUIRE(harness.route_allows(graph)); // kernel.admin overrides absence

  graph.membership[prop::ADMIN] = false;
  graph.orphan[2] = false;
  harness.install(graph);
  REQUIRE(harness.route_allows(graph)); // preserved grant reactivates
}

TEST_CASE("RBAC denies absent registered rule through the real filter",
          "[ws][integration][rbac][property]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  PropertyHttpHarness harness;
  prop::Graph graph;
  harness.install(graph);
  const auto response = harness.missing_rule_response();
  REQUIRE(response->statusCode() == drogon::k403Forbidden);
  const auto body = parse_json(response);
  CHECK(body["error"].asString() == "permission_denied");
  CHECK(body["rule"].asString() == "property89.absent");
  CHECK(body["message"].asString().find("not registered") != std::string::npos);
}

TEST_CASE("Generated RBAC graphs match real filter and capability authority",
          "[ws][integration][rbac][property]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  PropertyHttpHarness harness;
  for (const auto seed : prop::SEEDS) {
    const auto sample = prop::generate(seed);
    if (const auto failure = find_failure(harness, sample); failure) {
      const auto minimized = prop::shrink(
          sample,
          [&](const prop::Case& candidate) {
            return find_failure(harness, candidate).has_value();
          },
          48);
      FAIL("seed=" << seed << " stage=" << failure->stage << " "
                   << failure->detail
                   << " minimized=" << prop::describe(minimized.minimized));
    }
  }
}
