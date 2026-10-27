#include "kernel/extension_http/authentication.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/auth/csrf.hpp"
#include "kernel/db/operations.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {
using namespace plinth::extension_http;
using namespace std::chrono_literals;
namespace auth = plinth::auth;
constexpr auto USER = "11111111-1111-1111-1111-111111111111";
constexpr auto PAT = "22222222-2222-2222-2222-222222222222";
constexpr auto OTHER = "33333333-3333-3333-3333-333333333333";
constexpr std::uint64_t MS = 1000000;
static_assert(!std::is_default_constructible_v<AuthenticatedHttpPrincipal>);
static_assert(!std::is_copy_constructible_v<AuthenticatedHttpPrincipal>);

struct Probe {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<AuthenticationResult> result;
  std::size_t count = 0;
  auto store(AuthenticationResult value) -> void {
    {
      const std::lock_guard lock(mutex);
      ++count;
      result.emplace(std::move(value));
    }
    changed.notify_all();
  }
  auto wait() -> bool {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 5s, [this] { return count != 0; });
  }
  auto clear() -> void {
    const std::lock_guard lock(mutex);
    result.reset();
  }
};
auto completion(const std::shared_ptr<Probe>& probe)
    -> AuthenticationCompletion {
  return
      [probe](AuthenticationResult result) { probe->store(std::move(result)); };
}
auto request(drogon::HttpMethod method = drogon::Post)
    -> drogon::HttpRequestPtr {
  auto result = drogon::HttpRequest::newHttpRequest();
  result->setMethod(method);
  result->addHeader("Host", "example.test");
  return result;
}
auto pat_context(std::string user = USER, std::string pat = PAT)
    -> auth::TokenValidationResult {
  return {.ok = true,
          .context = {.user_id = std::move(user),
                      .username = "not-output",
                      .auth_type = "pat",
                      .session_id = "",
                      .pat_id = std::move(pat),
                      .token_hash = "fake-hash-not-output"},
          .error_code = ""};
}
auto pat_validator() -> auth::test_seam::TokenValidator {
  return [](const std::string&, auth::CredentialSource,
            auth::TokenValidationCallback cb) { cb(pat_context()); };
}
struct Fixture {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  std::shared_ptr<std::atomic<std::uint64_t>> now =
      std::make_shared<std::atomic<std::uint64_t>>(100 * MS);
  std::shared_ptr<AdmissionLease> lease =
      pool.try_acquire("authfixture", 0, process.try_acquire());
  std::shared_ptr<AuthenticationOwner> owner =
      AuthenticationOwner::create(lease, *DeadlineBounds::make(now->load()),
                                  [clock = now] { return clock->load(); });
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
};
auto principal(const std::shared_ptr<Probe>& probe) -> Principal {
  const std::lock_guard lock(probe->mutex);
  REQUIRE(probe->result.has_value());
  REQUIRE(probe->result->has_value());
  auto captured = probe->result->value().snapshot();
  REQUIRE(captured.has_value());
  return *captured;
}
auto require_error(const std::shared_ptr<Probe>& probe,
                   drogon::HttpStatusCode status, std::string_view code,
                   bool origin = false) -> void {
  const std::lock_guard lock(probe->mutex);
  REQUIRE(probe->result.has_value());
  REQUIRE_FALSE(probe->result->has_value());
  auto rejected = probe->result->error().response;
  REQUIRE(rejected);
  REQUIRE(rejected->statusCode() == status);
  REQUIRE(nlohmann::json::parse(rejected->body())["error"] == code);
  REQUIRE(rejected->getHeader("Cache-Control") == "no-store");
  REQUIRE(rejected->getHeader("Vary") ==
          (origin ? "Origin, Cookie, Authorization" : "Cookie, Authorization"));
}
auto require_retired(const std::shared_ptr<Probe>& probe) -> void {
  const std::lock_guard lock(probe->mutex);
  REQUIRE(probe->result.has_value());
  REQUIRE_FALSE(probe->result->has_value());
  REQUIRE(probe->result->error().code == AuthenticationFailureCode::RETIRED);
  REQUIRE_FALSE(probe->result->error().response);
}
} // namespace

TEST_CASE(
    "HTTP authentication actual SessionFilter refuses missing credentials",
    "[extension-http][http_authentication]") {
  Fixture f;
  REQUIRE(authenticate_http(request(), AuthenticationMode::PROTECTED, f.owner,
                            completion(f.probe)));
  require_error(f.probe, drogon::k401Unauthorized, "not_authenticated");
}

TEST_CASE("HTTP authentication decision seam emits only live validated Bearer "
          "PAT identity",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  REQUIRE(test_seam::authenticate_http(req, AuthenticationMode::PROTECTED,
                                       f.owner, completion(f.probe),
                                       pat_validator()));
  REQUIRE(principal(f.probe) == Principal{Principal::Kind::PAT, USER, PAT});
  f.owner->cancel(CancelReason::GENERATION_RETIRED);
  const std::lock_guard lock(f.probe->mutex);
  REQUIRE_FALSE(f.probe->result->value().snapshot());
  // A hermetic validator is decision proof only; no actual token/DB claim.
}

TEST_CASE(
    "HTTP authentication preserves cookie precedence and never falls back",
    "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addCookie("plinth_session", "fake-cookie");
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  auto calls = std::make_shared<std::atomic<int>>(0);
  auto source =
      std::make_shared<auth::CredentialSource>(auth::CredentialSource::UNKNOWN);
  auto raw = std::make_shared<std::string>();
  auth::test_seam::TokenValidator validator =
      [calls, source, raw](const std::string& token,
                           auth::CredentialSource selected,
                           auth::TokenValidationCallback cb) {
        ++*calls;
        *source = selected;
        *raw = token;
        cb({.ok = true,
            .context = {.user_id = USER,
                        .username = "fixture",
                        .auth_type = "session",
                        .session_id = OTHER},
            .error_code = ""});
      };
  REQUIRE(test_seam::authenticate_http(req, AuthenticationMode::PROTECTED,
                                       f.owner, completion(f.probe),
                                       validator));
  REQUIRE(calls->load() == 1);
  REQUIRE(*source == auth::CredentialSource::COOKIE);
  REQUIRE(*raw == "fake-cookie");
  require_error(f.probe, drogon::k401Unauthorized, "pat_required");
}

TEST_CASE("HTTP authentication empty or unrelated cookie permits the existing "
          "Bearer selector",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  SECTION("empty selected cookie") {
    req->addCookie("plinth_session", "");
  }
  SECTION("unrelated cookie") {
    req->addCookie("unrelated", "fixture");
  }
  auto source =
      std::make_shared<auth::CredentialSource>(auth::CredentialSource::UNKNOWN);
  REQUIRE(test_seam::authenticate_http(
      req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
      [source](const std::string&, auth::CredentialSource selected,
               auth::TokenValidationCallback cb) {
        *source = selected;
        cb(pat_context());
      }));
  REQUIRE(*source == auth::CredentialSource::BEARER);
  REQUIRE(principal(f.probe).pat_id == PAT);
}

TEST_CASE("HTTP authentication rejects Bearer sessions invalid IDs and failed "
          "validators",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer fixture-token");
  SECTION("Bearer session") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback cb) {
          cb({.ok = true,
              .context = {.user_id = USER,
                          .auth_type = "session",
                          .session_id = OTHER},
              .error_code = ""});
        }));
    require_error(f.probe, drogon::k401Unauthorized, "pat_required");
  }
  SECTION("invalid successful identity") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback cb) {
          cb(pat_context("invalid", PAT));
        }));
    require_error(f.probe, drogon::k401Unauthorized, "not_authenticated");
  }
  SECTION("backend unavailable") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback cb) {
          cb({.ok = false, .context = {}, .error_code = "service_unavailable"});
        }));
    require_error(f.probe, drogon::k503ServiceUnavailable,
                  "service_unavailable");
  }
  SECTION("invalid credential") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback cb) {
          cb({.ok = false, .context = {}, .error_code = "not_authenticated"});
        }));
    require_error(f.probe, drogon::k401Unauthorized, "not_authenticated");
  }
}

TEST_CASE("HTTP public authentication is anonymous with the exact safe unsafe "
          "origin truth table",
          "[extension-http][http_authentication]") {
  // Uses existing request-derived trusted origin; no global origin change.
  for (auto method : {drogon::Get, drogon::Head, drogon::Options, drogon::Post,
                      drogon::Put, drogon::Delete}) {
    for (auto origin :
         {"", "http://example.test", "https://foreign.test", "malformed"}) {
      for (int browser_marker : {0, 1, 2}) {
        Fixture f;
        auto req = request(method);
        req->addHeader("Origin", origin);
        req->addHeader("Authorization", "Bearer ambient-pat");
        if (browser_marker == 1)
          req->addCookie("plinth_session", "ambient-cookie");
        if (browser_marker == 2)
          req->addHeader("Sec-Fetch-Site", "same-origin");
        auto calls = std::make_shared<std::atomic<int>>(0);
        REQUIRE(test_seam::authenticate_http(
            req, AuthenticationMode::PUBLIC, f.owner, completion(f.probe),
            [calls](const std::string&, auth::CredentialSource,
                    auth::TokenValidationCallback) { ++*calls; }));
        REQUIRE(calls->load() == 0);
        const bool safe = method == drogon::Get || method == drogon::Head ||
                          method == drogon::Options;
        const bool same = std::string_view{origin} == "http://example.test";
        if (safe || same ||
            (std::string_view{origin}.empty() && browser_marker == 0)) {
          REQUIRE(principal(f.probe) == Principal{});
        } else
          require_error(f.probe, drogon::k403Forbidden, "csrf_failed", true);
      }
    }
  }
}

TEST_CASE("HTTP authentication late callbacks cannot survive cancellation or "
          "exact cutoff",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  auto held = std::make_shared<auth::TokenValidationCallback>();
  REQUIRE(test_seam::authenticate_http(
      req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
      [held](const std::string&, auth::CredentialSource,
             auth::TokenValidationCallback cb) { *held = std::move(cb); }));
  REQUIRE(f.probe->count == 0);
  SECTION("deadline without a timer turn") {
    f.now->store(4600 * MS);
  }
  SECTION("client gone") {
    f.owner->cancel(CancelReason::CLIENT_GONE);
  }
  SECTION("shutdown") {
    f.owner->cancel(CancelReason::SHUTDOWN);
  }
  SECTION("generation retired") {
    f.owner->cancel(CancelReason::GENERATION_RETIRED);
  }
  (*held)(pat_context());
  require_retired(f.probe);
  REQUIRE(f.probe->count == 1);
  *held = {};
}

TEST_CASE("HTTP authentication duplicate and throwing completions do not "
          "create new authority",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  SECTION("duplicate validator") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback cb) {
          cb(pat_context());
          cb(pat_context(OTHER, OTHER));
        }));
    REQUIRE(f.probe->count == 1);
    REQUIRE(principal(f.probe).user_id == USER);
  }
  SECTION("throwing completion") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner,
        [](AuthenticationResult) {
          throw std::runtime_error("controlled completion failure");
        },
        pat_validator()));
    REQUIRE_FALSE(f.owner->current());
  }
  SECTION("throwing validator") {
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [](const std::string&, auth::CredentialSource,
           auth::TokenValidationCallback) {
          throw std::runtime_error("controlled validator failure");
        }));
    require_error(f.probe, drogon::k503ServiceUnavailable,
                  "service_unavailable");
  }
}

TEST_CASE("HTTP authentication callback descendants retain feature and process "
          "credits",
          "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  auto held = std::make_shared<auth::TokenValidationCallback>();
  REQUIRE(test_seam::authenticate_http(
      req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
      [held](const std::string&, auth::CredentialSource,
             auth::TokenValidationCallback cb) { *held = std::move(cb); }));
  (*held)(pat_context());
  REQUIRE(principal(f.probe).pat_id == PAT);
  f.probe->clear();
  f.owner.reset();
  f.lease.reset();
  REQUIRE(f.pool.snapshot().tasks == 1);
  REQUIRE(f.process.active_for_test() == 1);
  REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
  // Controlled descendant closure, not proof that a real DB query ran here.
  *held = {};
  REQUIRE(f.pool.drain(std::chrono::steady_clock::now()));
  REQUIRE(f.process.active_for_test() == 0);
}

TEST_CASE(
    "HTTP authentication requires preadmission credits and captured bounds",
    "[extension-http][http_authentication]") {
  Fixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-pat");
  REQUIRE_FALSE(
      AuthenticationOwner::create({}, *DeadlineBounds::make(100 * MS),
                                  [clock = f.now] { return clock->load(); }));
  auto invalid = *DeadlineBounds::make(100 * MS);
  invalid.execution_cutoff_ns = invalid.wire_expiry_ns;
  REQUIRE_FALSE(AuthenticationOwner::create(
      f.lease, invalid, [clock = f.now] { return clock->load(); }));
  auto calls = std::make_shared<std::atomic<int>>(0);
  SECTION("resource exhaustion before filter") {
    auto charge =
        f.lease->reserve_extra(EXTRA_BYTES_PER_LEASE - f.lease->extra_used() -
                               sizeof(ExtraMemoryCharge));
    REQUIRE(charge);
    REQUIRE_FALSE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [calls](const std::string&, auth::CredentialSource,
                auth::TokenValidationCallback) { ++*calls; }));
    REQUIRE(calls->load() == 0);
    REQUIRE(f.probe->count == 0);
  }
  SECTION("already cancelled before filter") {
    f.owner->cancel(CancelReason::CLIENT_GONE);
    REQUIRE(test_seam::authenticate_http(
        req, AuthenticationMode::PROTECTED, f.owner, completion(f.probe),
        [calls](const std::string&, auth::CredentialSource,
                auth::TokenValidationCallback) { ++*calls; }));
    REQUIRE(calls->load() == 0);
    require_retired(f.probe);
  }
  SECTION("stop signal invalidates a previously selected principal") {
    std::stop_source stop;
    auto owner = AuthenticationOwner::create(
        f.lease, *DeadlineBounds::make(100 * MS),
        [clock = f.now] { return clock->load(); }, stop.get_token());
    REQUIRE(test_seam::authenticate_http(req, AuthenticationMode::PROTECTED,
                                         owner, completion(f.probe),
                                         pat_validator()));
    REQUIRE(principal(f.probe).pat_id == PAT);
    stop.request_stop();
    const std::lock_guard lock(f.probe->mutex);
    REQUIRE_FALSE(f.probe->result->value().snapshot());
  }
}

namespace {
// Reuses the existing production-owned singleton client/process fixture. No
// per-test ORM client, driver setting, global origin or network route change.
struct PgFixture {
  plinth::Config config = plinth::ws_test::test_config();
  std::unique_ptr<plinth::ws_test::TestPg> database;
  CreditPool pool;
  std::shared_ptr<std::atomic<std::uint64_t>> now =
      std::make_shared<std::atomic<std::uint64_t>>(100 * MS);
  std::shared_ptr<AdmissionLease> lease;
  std::shared_ptr<AuthenticationOwner> owner;
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  std::string user, pat;
  PgFixture() {
    static_cast<void>(plinth::ws_test::test_server_port());
    plinth::ws_test::reset_schema(config.db);
    database = std::make_unique<plinth::ws_test::TestPg>(config.db);
    auto limits = database->exec(
        "SET statement_timeout='1000ms'; SET lock_timeout='1000ms'");
    REQUIRE(PQresultStatus(limits.get()) == PGRES_COMMAND_OK);
    user = plinth::ws_test::insert_user(*database, "http-auth-owner",
                                        "fake-http-auth-password");
    pat = plinth::ws_test::insert_pat(*database, user, "owned-auth-random");
    lease = pool.try_acquire("authfixture", 0,
                             plinth::lifecycle::async_tasks().try_acquire());
    REQUIRE(lease);
    owner =
        AuthenticationOwner::create(lease, *DeadlineBounds::make(now->load()),
                                    [clock = now] { return clock->load(); });
    REQUIRE(owner);
  }
  auto release_logical() -> void {
    probe->clear();
    owner.reset();
    lease.reset();
  }
};
struct PatRowLock {
  plinth::ws_test::TestPg connection;
  bool held = false;
  explicit PatRowLock(PgFixture& fixture) : connection(fixture.config.db) {
    auto limits = connection.exec(
        "SET statement_timeout='1000ms'; SET lock_timeout='1000ms'");
    REQUIRE(PQresultStatus(limits.get()) == PGRES_COMMAND_OK);
    auto begin = connection.exec("BEGIN");
    REQUIRE(PQresultStatus(begin.get()) == PGRES_COMMAND_OK);
    held = true;
    auto locked = connection.exec_params(
        "SELECT id FROM plinth.pats WHERE id=$1::uuid FOR UPDATE",
        {fixture.pat});
    REQUIRE(PQresultStatus(locked.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(locked.get()) == 1);
  }
  ~PatRowLock() {
    if (held) {
      // No throwing C++ allocation during assertion unwind. The owned
      // connection closes next even if this bounded rollback cannot settle.
      auto* result = PQexec(connection.conn, "ROLLBACK");
      if (result) PQclear(result);
    }
  }
  auto release() -> void {
    auto rollback = connection.exec("ROLLBACK");
    REQUIRE(PQresultStatus(rollback.get()) == PGRES_COMMAND_OK);
    held = false;
  }
};
} // namespace

TEST_CASE("HTTP authentication actual PG filter validates PAT state and "
          "refuses sessions",
          "[extension-http][http_authentication_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgFixture f;
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_owned-auth-random");
  bool allowed = false;
  std::string expected = "not_authenticated";
  SECTION("actual PAT") {
    allowed = true;
  }
  SECTION("revoked") {
    auto changed = f.database->exec("UPDATE plinth.pats SET revoked_at=now()");
    REQUIRE(PQresultStatus(changed.get()) == PGRES_COMMAND_OK);
  }
  SECTION("expired") {
    auto changed = f.database->exec(
        "UPDATE plinth.pats SET expires_at=now()-interval '1 second'");
    REQUIRE(PQresultStatus(changed.get()) == PGRES_COMMAND_OK);
  }
  SECTION("disabled user") {
    auto changed =
        f.database->exec("UPDATE plinth.users SET disabled_at=now()");
    REQUIRE(PQresultStatus(changed.get()) == PGRES_COMMAND_OK);
  }
  SECTION("actual Bearer session") {
    static_cast<void>(
        plinth::ws_test::insert_session(*f.database, f.user, "owned-session"));
    req->addHeader("Authorization", "Bearer owned-session");
    expected = "pat_required";
  }
  SECTION("actual cookie session before valid PAT") {
    static_cast<void>(
        plinth::ws_test::insert_session(*f.database, f.user, "owned-session"));
    req->addCookie("plinth_session", "owned-session");
    expected = "pat_required";
  }
  REQUIRE(authenticate_http(req, AuthenticationMode::PROTECTED, f.owner,
                            completion(f.probe)));
  REQUIRE(f.probe->wait());
  if (allowed)
    REQUIRE(principal(f.probe) ==
            Principal{Principal::Kind::PAT, f.user, f.pat});
  else
    require_error(f.probe, drogon::k401Unauthorized, expected);
  f.release_logical();
  REQUIRE(f.pool.drain(std::chrono::steady_clock::now() + 5s));
  // This joins this feature's callback lineage, not the process DbClient
  // itself.
}

TEST_CASE("HTTP authentication actual PAT bookkeeping retains ownership after "
          "success",
          "[extension-http][http_authentication_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgFixture f;
  // Declared last: rollback happens before fixture/lease cleanup on ANY
  // failure.
  PatRowLock lock{f};
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_owned-auth-random");
  REQUIRE(authenticate_http(req, AuthenticationMode::PROTECTED, f.owner,
                            completion(f.probe)));
  REQUIRE(f.probe->wait());
  REQUIRE(principal(f.probe).pat_id == f.pat);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool blocked = false;
  while (!blocked && std::chrono::steady_clock::now() < deadline) {
    auto observed = f.database->exec_params(
        "SELECT EXISTS(SELECT 1 FROM pg_stat_activity a WHERE "
        "a.datname=current_database() "
        "AND $1::integer=ANY(pg_blocking_pids(a.pid)) "
        "AND a.query LIKE 'UPDATE plinth.pats SET last_used_at = "
        "NOW()%')::text",
        {std::to_string(PQbackendPID(lock.connection.conn))});
    REQUIRE(PQresultStatus(observed.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(observed.get()) == 1);
    blocked = std::string_view{PQgetvalue(observed.get(), 0, 0)} == "true";
    if (!blocked) std::this_thread::sleep_for(5ms);
  }
  REQUIRE(blocked);
  f.release_logical();
  REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
  REQUIRE(f.pool.snapshot().tasks == 1);
  lock.release();
  REQUIRE(f.pool.drain(std::chrono::steady_clock::now() + 5s));
  auto updated = f.database->exec(
      "SELECT count(*)::text FROM plinth.pats WHERE last_used_at IS NOT NULL");
  REQUIRE(PQresultStatus(updated.get()) == PGRES_TUPLES_OK);
  REQUIRE(std::string_view{PQgetvalue(updated.get(), 0, 0)} == "1");
  // Actual UPDATE callbacks settled; auth success alone did not prove this and
  // this local pool drain is NOT a claim that the process client has joined.
}
