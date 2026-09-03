// ICD-0.6.3 §12.1 — `POST /api/cap/{capability}` HTTP cap-dispatch route
// (B.* family). Library-level coverage of the kernel's HTTP cap-dispatch
// substrate that browser-side `plinth.call` consumes.
//
// Library-level posture: tests register a Tier 1 echo handler gated by
// `kernel.admin`, dispatch through the HttpTestFixture (real HTTP +
// SessionFilter), and assert on the JSON envelope shape per §A.3.
//
// The C.* / S.* / U.* tests (per-call result assertions across QuickJS
// dispatch + WebSocket subscribe + Preact hooks) require the JS-dispatch
// fixture and remain deferred per ICD §12.10 — same posture as v0.6.1's
// P.* / I.* and v0.6.2's T.* / S.* / I.* (blocked on the `init_registry`
// teardown bug from test-fixture-buildout session 9).

#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"

#include "../packages/http_test_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <json/reader.h>
#include <json/value.h>

#include <string>

namespace {

// Set up a per-test scratch: clear the resolver state from prior tests,
// then register a Tier 1 echo handler gated by kernel.admin so admin
// users can dispatch it (universal-match rule per ICD-0.2.4) and
// non-admin users cannot. The handler echoes the args back so B.01
// can assert round-trip integrity.
struct CapHandlerScratch {
  CapHandlerScratch() {
    plinth::capabilities::clear_resolver_for_test();
    plinth::capabilities::register_tier1_handler(
        "test:1:echo", "kernel.admin",
        [](const Json::Value& args,
           const plinth::capabilities::UserContext& /*ctx*/,
           int /*depth*/) -> plinth::capabilities::HandlerOutcome {
          Json::Value v(Json::objectValue);
          v["echoed"] = args;
          return v;
        });
  }
  ~CapHandlerScratch() { plinth::capabilities::clear_resolver_for_test(); }
  CapHandlerScratch(const CapHandlerScratch&) = delete;
  auto operator=(const CapHandlerScratch&) -> CapHandlerScratch& = delete;
  CapHandlerScratch(CapHandlerScratch&&) = delete;
  auto operator=(CapHandlerScratch&&) -> CapHandlerScratch& = delete;
};

// Build a `POST /api/cap/{capability}` request matching the production
// shape `plinth.call` will use: JSON body `{"args": [...]}` with the
// session cookie wired so SessionFilter accepts it.
auto build_cap_post(std::string_view capability, const Json::Value& args,
                    std::string_view session_token) -> drogon::HttpRequestPtr {
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(drogon::Post);
  req->setPath("/api/cap/" + std::string{capability});
  req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  Json::Value body(Json::objectValue);
  body["args"] = args;
  Json::StreamWriterBuilder w;
  w["indentation"] = "";
  req->setBody(Json::writeString(w, body));
  req->addCookie("plinth_session", std::string{session_token});
  return req;
}

auto parse_body(const drogon::HttpResponsePtr& resp) -> Json::Value {
  Json::Value out;
  Json::CharReaderBuilder b;
  auto body = std::string{resp->body()};
  std::string err;
  std::unique_ptr<Json::CharReader> reader{b.newCharReader()};
  const auto* begin = body.data();
  // Json::CharReader::parse demands (begin, end) char* pair;
  // std::string::data() is canonical, end pointer is begin+size().
  reader->parse(begin, begin + body.size(), &out, &err);
  return out;
}

} // namespace

// ── B.01 — happy path: admin dispatches kernel.admin-gated Tier 1 ────

TEST_CASE("B.01: POST /api/cap/test.echo as admin → 200 + ok envelope",
          "[ws][api_cap][B.01]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  plinth::http_test::HttpTestFixture fix;
  CapHandlerScratch handlers;

  auto token = fix.seed_admin();
  Json::Value args(Json::arrayValue);
  args.append("hello");
  args.append(42);
  auto req = build_cap_post("test.echo", args, token);
  auto resp = fix.dispatch(req);

  REQUIRE(resp->statusCode() == drogon::k200OK);
  auto body = parse_body(resp);
  REQUIRE(body["ok"].asBool() == true);
  REQUIRE(body["resolved_tier"].asString() == "tier1");
  REQUIRE(body["provider_type"].asString() == "kernel");
  REQUIRE(body["value"]["echoed"].size() == 2);
  REQUIRE(body["value"]["echoed"][0].asString() == "hello");
  REQUIRE(body["value"]["echoed"][1].asInt() == 42);
}

// ── B.02 — RBAC denial: non-admin lacks kernel.admin → 403 rbac_denied

TEST_CASE("B.02: POST /api/cap/test.echo as non-admin → 403 rbac_denied",
          "[ws][api_cap][B.02]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  plinth::http_test::HttpTestFixture fix;
  CapHandlerScratch handlers;

  auto token = fix.seed_non_admin("alice");
  auto req = build_cap_post("test.echo", Json::Value(Json::arrayValue), token);
  auto resp = fix.dispatch(req);

  REQUIRE(resp->statusCode() == drogon::k403Forbidden);
  auto body = parse_body(resp);
  REQUIRE(body["ok"].asBool() == false);
  REQUIRE(body["error"]["code"].asString() == "rbac_denied");
  REQUIRE(body["error"]["message"].isString());
}

// ── B.03 — capability not found ───────────────────────────────────

TEST_CASE("B.03: POST /api/cap/bogus.fake → 404 not_found",
          "[ws][api_cap][B.03]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PLINTH_PG_HOST not set");
  }
  plinth::http_test::HttpTestFixture fix;
  CapHandlerScratch handlers;

  auto token = fix.seed_admin();
  auto req = build_cap_post("bogus.fake", Json::Value(Json::arrayValue), token);
  auto resp = fix.dispatch(req);

  REQUIRE(resp->statusCode() == drogon::k404NotFound);
  auto body = parse_body(resp);
  REQUIRE(body["ok"].asBool() == false);
  REQUIRE(body["error"]["code"].asString() == "not_found");
}
