// SPDX-License-Identifier: MIT
#include "kernel/extension_http/caller_resolution.hpp"
#include "kernel/extension_http/caller_resolution_detail.hpp"

#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/operations.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace plinth::extension_http;
using namespace std::chrono_literals;
namespace auth = plinth::auth;
constexpr auto USER = "11111111-1111-4111-8111-111111111111";
constexpr auto PAT = "22222222-2222-4222-8222-222222222222";
constexpr std::uint64_t MS = 1000000;
constexpr std::size_t PRINCIPAL_CHARGE = 1024 + sizeof(ExtraMemoryCharge);
constexpr std::size_t CALLER_CHARGE =
    4ULL * 1024 * 1024 + sizeof(ExtraMemoryCharge);
using NativeResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
using NativeRow = std::array<std::optional<std::string>, 3>;

static_assert(!std::is_default_constructible_v<ResolvedHttpCaller>);
static_assert(!std::is_copy_constructible_v<ResolvedHttpCaller>);
static_assert(!std::is_copy_assignable_v<ResolvedHttpCaller>);
static_assert(std::is_nothrow_move_constructible_v<ResolvedHttpCaller>);
static_assert(!std::is_constructible_v<ResolvedHttpCaller, HttpCallerSnapshot,
                                       std::shared_ptr<AuthenticationOwner>,
                                       ExtraMemoryCharge>);
static_assert(!std::is_constructible_v<ResolvedHttpCaller,
                                       plinth::capabilities::UserContext>);
static_assert(!std::is_constructible_v<AuthenticatedHttpPrincipal, Principal,
                                       std::shared_ptr<AuthenticationOwner>,
                                       ExtraMemoryCharge>);
static_assert(std::is_same_v<decltype(detail::decode_http_caller(
                                 std::declval<const PGresult&>(),
                                 std::declval<const Principal&>())),
                             std::expected<plinth::capabilities::UserContext,
                                           CallerResolutionFailure>>);
static_assert(noexcept(
    std::declval<const AuthenticationOwner&>().remaining_execution_ms()));

auto pat_data() -> Principal {
  return {Principal::Kind::PAT, USER, PAT};
}

// Actual libpq-owned PGresults exercise the native decoder. These Principal
// inputs and decoded UserContexts are DATA, never sealed authentication proof.
struct NativeTable {
  ExecStatusType status = PGRES_TUPLES_OK;
  std::vector<std::optional<std::string>> names = {"username", "rule",
                                                   "has_rule"};
  std::array<int, 3> formats{};
  std::vector<NativeRow> rows = {{{"fixture-user", "caller.read", "t"}}};

  auto build() -> NativeResult {
    NativeResult result{PQmakeEmptyPGresult(nullptr, status), PQclear};
    REQUIRE(result);
    std::vector<PGresAttDesc> attributes;
    attributes.reserve(names.size());
    for (std::size_t column = 0; column < names.size(); ++column) {
      attributes.push_back(
          {.name = names[column] ? names[column]->data() : nullptr,
           .tableid = 0,
           .columnid = 0,
           .format = column < formats.size() ? formats[column] : 0,
           .typid = column == 2 ? 16U : 25U,
           .typlen = -1,
           .atttypmod = -1});
    }
    if (!attributes.empty())
      REQUIRE(PQsetResultAttrs(result.get(),
                               static_cast<int>(attributes.size()),
                               attributes.data()) == 1);
    for (std::size_t row = 0; row < rows.size(); ++row) {
      for (std::size_t column = 0; column < names.size(); ++column) {
        auto* cell = column < rows[row].size() ? &rows[row][column] : nullptr;
        REQUIRE(PQsetvalue(result.get(), static_cast<int>(row),
                           static_cast<int>(column),
                           cell && *cell ? (*cell)->data() : nullptr,
                           cell && *cell ? static_cast<int>((*cell)->size())
                                         : 0) == 1);
      }
    }
    return result;
  }
};

auto require_decode_failure(
    NativeTable table, const Principal& principal = pat_data(),
    CallerResolutionFailure failure = CallerResolutionFailure::UNAVAILABLE)
    -> void {
  const auto native = table.build();
  const auto decoded = detail::decode_http_caller(*native, principal);
  REQUIRE_FALSE(decoded.has_value());
  REQUIRE(decoded.error() == failure);
}

auto require_context(const plinth::capabilities::UserContext& user,
                     const std::string& id, const std::string& username,
                     const std::string& type,
                     const std::vector<std::string>& rules) -> void {
  CHECK(user.user_id == id);
  CHECK(user.username == username);
  CHECK(user.auth_type == type);
  CHECK(user.effective_rules == rules);
  CHECK(user.session_id.empty());
  CHECK(user.ip_address.empty());
}

struct AuthenticationProbe {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<AuthenticationResult> result;
  std::size_t calls = 0;

  auto store(AuthenticationResult value) -> void {
    {
      const std::lock_guard lock(mutex);
      ++calls;
      result.emplace(std::move(value));
    }
    changed.notify_all();
  }
  auto take() -> AuthenticatedHttpPrincipal {
    std::unique_lock lock(mutex);
    REQUIRE(changed.wait_for(lock, 5s, [this] { return result.has_value(); }));
    REQUIRE(calls == 1);
    REQUIRE(result->has_value());
    auto value = std::move(result->value());
    result.reset();
    calls = 0;
    return value;
  }
  auto clear() -> void {
    const std::lock_guard lock(mutex);
    result.reset();
    calls = 0;
  }
};

struct LiveOwner {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  std::shared_ptr<std::atomic<std::uint64_t>> now =
      std::make_shared<std::atomic<std::uint64_t>>(100 * MS);
  std::shared_ptr<std::atomic_bool> throwing_clock =
      std::make_shared<std::atomic_bool>(false);
  std::stop_source stop;
  std::shared_ptr<AdmissionLease> lease =
      pool.try_acquire("callerfixture", 23, process.try_acquire());
  std::shared_ptr<AuthenticationOwner> owner;
  std::shared_ptr<AuthenticationProbe> probe =
      std::make_shared<AuthenticationProbe>();

  LiveOwner() { reset_owner(*DeadlineBounds::make(now->load())); }
  auto reset_owner(DeadlineBounds bounds) -> void {
    REQUIRE(lease);
    owner = AuthenticationOwner::create(
        lease, bounds,
        [clock = now, fail = throwing_clock] {
          if (fail->load())
            throw std::runtime_error("controlled owner clock failure");
          return clock->load();
        },
        stop.get_token());
    REQUIRE(owner);
  }
  auto completion() const -> AuthenticationCompletion {
    return [captured = probe](AuthenticationResult result) {
      captured->store(std::move(result));
    };
  }
  auto release() -> void {
    probe->clear();
    owner.reset();
    lease.reset();
  }
  auto require_drained() -> void {
    REQUIRE(pool.drain(std::chrono::steady_clock::now() + 5s));
    REQUIRE(process.drain(5s));
    CHECK(pool.snapshot() == CreditSnapshot{});
    CHECK(process.active_for_test() == 0);
  }
};

auto request() -> drogon::HttpRequestPtr {
  auto result = drogon::HttpRequest::newHttpRequest();
  result->setMethod(drogon::Get);
  result->addHeader("Host", "example.test");
  return result;
}
auto public_principal(LiveOwner& live) -> AuthenticatedHttpPrincipal {
  REQUIRE(authenticate_http(request(), AuthenticationMode::PUBLIC, live.owner,
                            live.completion()));
  return live.probe->take();
}

// Existing SessionFilter validator seam: decision proof only. This uses the
// real authentication wrapper to seal its output, without claiming token/PG
// validation or fabricating AuthenticatedHttpPrincipal/ResolvedHttpCaller.
auto decision_pat_principal(LiveOwner& live) -> AuthenticatedHttpPrincipal {
  auto req = request();
  req->addHeader("Authorization", "Bearer plinth_fake-caller-decision-token");
  REQUIRE(test_seam::authenticate_http(
      req, AuthenticationMode::PROTECTED, live.owner, live.completion(),
      [](const std::string&, auth::CredentialSource,
         auth::TokenValidationCallback callback) {
        callback({.ok = true,
                  .context = {.user_id = USER,
                              .username = "decision-only",
                              .auth_type = "pat",
                              .session_id = "",
                              .pat_id = PAT,
                              .token_hash = "fake-decision-hash"},
                  .error_code = ""});
      }));
  return live.probe->take();
}

auto unconnected() -> std::unique_ptr<PGconn, decltype(&PQfinish)> {
  // A nonexistent local Unix-socket directory cannot contact a service. No
  // directory is created and connect_timeout also bounds libpq's connect path.
  const auto missing = std::filesystem::temp_directory_path() /
                       "plinth-caller-resolution-nonexistent-socket";
  REQUIRE_FALSE(std::filesystem::exists(missing));
  const auto path = missing.string();
  constexpr std::array KEYS = {"host",
                               "port",
                               "user",
                               "dbname",
                               "password",
                               "connect_timeout",
                               static_cast<const char*>(nullptr)};
  const std::array VALUES = {path.c_str(),
                             "1",
                             "caller-fixture",
                             "caller-fixture",
                             "fake-unused-password",
                             "1",
                             static_cast<const char*>(nullptr)};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{
      PQconnectdbParams(KEYS.data(), VALUES.data(), 0), PQfinish};
  REQUIRE(connection);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_BAD);
  REQUIRE(PQbackendPID(connection.get()) == 0);
  return connection;
}

auto require_resolution_failure(const CallerResolutionResult& result,
                                CallerResolutionFailure failure) -> void {
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == failure);
}
} // namespace

TEST_CASE("HTTP caller decoder preserves PAT and anonymous rule data without "
          "authority",
          "[extension-http][http_caller_decoder]") {
  NativeTable table;
  table.rows = {{{"fixture-\xC3\xA9", "caller.group", "t"}},
                {{"fixture-\xC3\xA9", "caller.public", "t"}}};
  const auto native = table.build();
  const auto user = detail::decode_http_caller(*native, pat_data());
  REQUIRE(user);
  require_context(*user, USER, "fixture-\xC3\xA9", "pat",
                  {"caller.group", "caller.public"});
  for (auto& row : table.rows)
    row[0] = "";
  const auto public_native = table.build();
  const auto anonymous =
      detail::decode_http_caller(*public_native, Principal{});
  REQUIRE(anonymous);
  require_context(*anonymous, "", "", "anonymous",
                  {"caller.group", "caller.public"});
  // Later mutation/free of the native fixture cannot mutate the decoded DATA.
  REQUIRE(PQsetvalue(native.get(), 0, 1, nullptr, 0) == 1);
  CHECK(user->effective_rules ==
        std::vector<std::string>{"caller.group", "caller.public"});
}

TEST_CASE("HTTP caller decoder accepts exactly one null no-rule row",
          "[extension-http][http_caller_decoder]") {
  for (const bool anonymous : {false, true}) {
    CAPTURE(anonymous);
    NativeTable table;
    table.rows = {{{anonymous ? "" : "fixture-user", std::nullopt, "f"}}};
    const auto native = table.build();
    const auto user = detail::decode_http_caller(
        *native, anonymous ? Principal{} : pat_data());
    REQUIRE(user);
    require_context(*user, anonymous ? "" : USER,
                    anonymous ? "" : "fixture-user",
                    anonymous ? "anonymous" : "pat", {});
    table.rows.clear();
    require_decode_failure(table, anonymous ? Principal{} : pat_data(),
                           CallerResolutionFailure::REJECTED);
  }
}

TEST_CASE("HTTP caller decoder enforces native row and UTF8 byte limits",
          "[extension-http][http_caller_decoder]") {
  NativeTable table;
  std::string username;
  for (int i = 0; i < 128; ++i)
    username += "\xC3\xA9";
  REQUIRE(username.size() == 256);
  table.rows.clear();
  std::vector<std::string> expected;
  table.rows.reserve(1024);
  expected.reserve(1024);
  for (int i = 0; i < 1024; ++i) {
    auto rule = std::string(252, 'r') + std::to_string(1000 + i);
    REQUIRE(rule.size() == 256);
    expected.push_back(rule);
    table.rows.push_back({username, std::move(rule), "t"});
  }
  const auto native = table.build();
  const auto user = detail::decode_http_caller(*native, pat_data());
  REQUIRE(user);
  require_context(*user, USER, username, "pat", expected);
  table.rows.push_back(table.rows.front());
  require_decode_failure(table);
  table.rows.resize(1);
  table.rows[0][0] = username + "x";
  require_decode_failure(table);
  table.rows[0][0] = username;
  table.rows[0][1] = std::string(257, 'r');
  require_decode_failure(table);
}

TEST_CASE("HTTP caller decoder rejects invalid and unknown principal data",
          "[extension-http][http_caller_decoder]") {
  const std::array principals = {
      Principal{Principal::Kind::ANONYMOUS, USER, ""},
      Principal{Principal::Kind::ANONYMOUS, "", PAT},
      Principal{Principal::Kind::PAT, "", PAT},
      Principal{Principal::Kind::PAT, USER, ""},
      Principal{Principal::Kind::PAT, "invalid", PAT},
      Principal{Principal::Kind::PAT, USER, "invalid"},
      Principal{static_cast<Principal::Kind>(99), USER, PAT}};
  for (const auto& principal : principals) {
    CAPTURE(static_cast<int>(principal.kind), principal.user_id,
            principal.pat_id);
    require_decode_failure({}, principal);
  }
}

TEST_CASE("HTTP caller decoder rejects native result status columns and binary "
          "formats",
          "[extension-http][http_caller_decoder]") {
  for (const auto status :
       {PGRES_EMPTY_QUERY, PGRES_COMMAND_OK, PGRES_FATAL_ERROR,
        PGRES_NONFATAL_ERROR, PGRES_BAD_RESPONSE, PGRES_COPY_IN, PGRES_COPY_OUT,
        PGRES_SINGLE_TUPLE}) {
    CAPTURE(static_cast<int>(status));
    NativeTable table;
    table.status = status;
    require_decode_failure(table);
  }
  for (const std::size_t columns : {0U, 1U, 2U, 4U}) {
    CAPTURE(columns);
    NativeTable table;
    table.names.resize(columns, "extra");
    require_decode_failure(table);
  }
  for (std::size_t column = 0; column < 3; ++column) {
    CAPTURE(column);
    NativeTable renamed;
    renamed.names[column] = "wrong";
    require_decode_failure(renamed);
    renamed.names[column] = std::nullopt;
    require_decode_failure(renamed);
    NativeTable binary;
    binary.formats[column] = 1;
    require_decode_failure(binary);
  }
  NativeTable duplicate;
  duplicate.names[1] = duplicate.names[0];
  require_decode_failure(duplicate);
  NativeTable reordered;
  std::swap(reordered.names[0], reordered.names[1]);
  require_decode_failure(reordered);
}

TEST_CASE("HTTP caller decoder rejects malformed text at every native field",
          "[extension-http][http_caller_decoder]") {
  const std::vector<std::optional<std::string>> malformed = {
      std::nullopt,
      "",
      std::string(257, 'x'),
      std::string(1, '\xFF'),
      std::string{"\xC0\xAF"},
      std::string{"\xED\xA0\x80"},
      std::string{"\xF4\x90\x80\x80"},
      std::string{"\xE2\x82"},
      std::string{'a', '\0', 'b'}};
  for (std::size_t column = 0; column < 3; ++column) {
    for (std::size_t index = 0; index < malformed.size(); ++index) {
      CAPTURE(column, index);
      NativeTable table;
      table.rows[0][column] = malformed[index];
      require_decode_failure(table);
    }
  }
  for (const auto* flag : {"true", "false", "T", "F", "1", "0", " t", "f "}) {
    CAPTURE(flag);
    NativeTable table;
    table.rows[0][2] = flag;
    require_decode_failure(table);
  }
}

TEST_CASE(
    "HTTP caller decoder rejects inconsistent rule flags and mixed identities",
    "[extension-http][http_caller_decoder]") {
  SECTION(
      "false flag requires native NULL rather than empty or populated text") {
    for (const auto* rule : {"", "caller.read"}) {
      NativeTable table;
      table.rows[0] = {"fixture-user", rule, "f"};
      require_decode_failure(table);
    }
  }
  SECTION("no-rule sentinel must be the sole row") {
    NativeTable table;
    table.rows.push_back({"fixture-user", std::nullopt, "f"});
    require_decode_failure(table);
    table.rows[0] = table.rows[1];
    require_decode_failure(table);
  }
  SECTION("all usernames must equal the first valid username") {
    for (const std::optional<std::string>& name :
         std::vector<std::optional<std::string>>{"different", "", std::nullopt,
                                                 std::string(257, 'u')}) {
      NativeTable table;
      table.rows.push_back({name, "caller.write", "t"});
      require_decode_failure(table);
    }
  }
  SECTION("anonymous username cannot carry an ambient identity") {
    require_decode_failure({}, Principal{});
  }
}

TEST_CASE("HTTP authentication remaining budget preserves the cutoff and "
          "floors milliseconds",
          "[extension-http][http_caller_budget]") {
  LiveOwner live;
  CHECK(live.owner->remaining_execution_ms() == 4500);
  live.now->store(101 * MS + 1);
  CHECK(live.owner->remaining_execution_ms() == 4498);
  live.now->store(4599 * MS);
  CHECK(live.owner->remaining_execution_ms() == 1);
  live.now->store(4599 * MS + 1);
  CHECK(live.owner->current());
  CHECK_FALSE(live.owner->remaining_execution_ms());
  live.now->store(4600 * MS);
  CHECK_FALSE(live.owner->remaining_execution_ms());
  live.now->store(100 * MS);
  CHECK_FALSE(live.owner->current());
  CHECK_FALSE(live.owner->remaining_execution_ms());
  live.release();
  live.require_drained();
}

TEST_CASE("HTTP authentication remaining budget honors shortened and "
          "submillisecond bounds",
          "[extension-http][http_caller_budget]") {
  LiveOwner live;
  auto bounds = *DeadlineBounds::make(100 * MS);
  SECTION("enclosing wire deadline shortens the original execution cutoff") {
    bounds = *DeadlineBounds::make(100 * MS, 223 * MS);
    live.reset_owner(bounds);
    CHECK(live.owner->remaining_execution_ms() == 123);
    live.now->store(150 * MS);
    CHECK(live.owner->remaining_execution_ms() == 73);
  }
  SECTION("a separately shortened execution bound is retained") {
    bounds.execution_cutoff_ns = 117 * MS;
    live.reset_owner(bounds);
    CHECK(live.owner->remaining_execution_ms() == 17);
    live.now->store(116 * MS + 1);
    CHECK_FALSE(live.owner->remaining_execution_ms());
    live.now->store(117 * MS);
    CHECK_FALSE(live.owner->current());
  }
  SECTION("positive submillisecond bounds never yield a zero timeout") {
    bounds.execution_cutoff_ns = 100 * MS + MS - 1;
    live.reset_owner(bounds);
    CHECK(live.owner->current());
    CHECK_FALSE(live.owner->remaining_execution_ms());
  }
}

TEST_CASE("HTTP authentication remaining budget retires cancellation stop "
          "regression and throwing clocks",
          "[extension-http][http_caller_budget]") {
  LiveOwner live;
  REQUIRE(live.owner->remaining_execution_ms());
  SECTION("explicit cancellation") {
    live.owner->cancel(CancelReason::CLIENT_GONE);
  }
  SECTION("stop token") {
    REQUIRE(live.stop.request_stop());
  }
  SECTION("expiry") {
    live.now->store(4600 * MS);
    live.owner->expire();
    live.now->store(100 * MS);
  }
  SECTION("clock regresses before admission") {
    live.now->store(100 * MS - 1);
    REQUIRE_FALSE(live.owner->remaining_execution_ms());
    live.now->store(100 * MS);
  }
  SECTION("throwing clock is caught and remains broken") {
    auto broken = std::make_shared<std::atomic_bool>(true);
    live.owner = AuthenticationOwner::create(
        live.lease, *DeadlineBounds::make(100 * MS),
        [broken]() -> std::uint64_t {
          if (broken->load())
            throw std::runtime_error("controlled caller clock failure");
          return 100 * MS;
        });
    REQUIRE(live.owner);
    REQUIRE_FALSE(live.owner->remaining_execution_ms());
    broken->store(false);
  }
  CHECK_FALSE(live.owner->remaining_execution_ms());
  CHECK_FALSE(live.owner->current());
}

TEST_CASE("HTTP authentication remaining budget rechecks changing clocks "
          "without renewal",
          "[extension-http][http_caller_budget]") {
  LiveOwner live;
  for (const auto second : {100 * MS - 1, 4600 * MS}) {
    CAPTURE(second);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto owner = AuthenticationOwner::create(
        live.lease, *DeadlineBounds::make(100 * MS), [calls, second] {
          return calls->fetch_add(1) == 1 ? second : 100 * MS;
        });
    REQUIRE(owner);
    REQUIRE_FALSE(owner->remaining_execution_ms());
    CHECK(calls->load() == 2);
    CHECK_FALSE(owner->remaining_execution_ms());
    CHECK_FALSE(owner->current());
  }
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto owner = AuthenticationOwner::create(
      live.lease, *DeadlineBounds::make(100 * MS), [calls]() -> std::uint64_t {
        if (calls->fetch_add(1) == 1)
          throw std::runtime_error("second clock read fails");
        return 100 * MS;
      });
  REQUIRE(owner);
  REQUIRE_FALSE(owner->remaining_execution_ms());
  CHECK_FALSE(owner->current());
}

TEST_CASE("HTTP caller preflight uses real public authentication and "
          "decision-only PAT sealing",
          "[extension-http][http_caller_preflight]") {
  LiveOwner live;
  auto connection = unconnected();
  const auto before_owner = live.lease->extra_used();
  const auto counters = live.pool.snapshot();
  bool decision_only = false;
  SECTION("actual public wrapper") {
  }
  SECTION("existing PAT validator seam is decision proof only") {
    decision_only = true;
  }
  auto principal =
      decision_only ? decision_pat_principal(live) : public_principal(live);
  REQUIRE(principal.snapshot());
  REQUIRE(live.lease->extra_used() == before_owner + PRINCIPAL_CHARGE);
  const std::string native_error{PQerrorMessage(connection.get())};
  const auto resolved = resolve_http_caller(std::move(principal), *connection);
  require_resolution_failure(resolved, CallerResolutionFailure::UNAVAILABLE);
  CHECK_FALSE(principal.snapshot());
  CHECK(live.lease->extra_used() == before_owner);
  CHECK(live.pool.snapshot() == counters);
  CHECK(live.process.active_for_test() == 1);
  // A query attempt on this bad PGconn replaces its original connect error.
  CHECK(std::string{PQerrorMessage(connection.get())} == native_error);
  CHECK(PQisnonblocking(connection.get()) == 0);
  live.release();
  live.require_drained();
}

TEST_CASE("HTTP caller preflight retires sealed inputs before any native query",
          "[extension-http][http_caller_preflight]") {
  LiveOwner live;
  auto connection = unconnected();
  const auto before_owner = live.lease->extra_used();
  auto principal = public_principal(live);
  std::optional<AuthenticatedHttpPrincipal> retained;
  SECTION("client cancellation") {
    live.owner->cancel(CancelReason::CLIENT_GONE);
  }
  SECTION("generation retirement") {
    live.owner->cancel(CancelReason::GENERATION_RETIRED);
  }
  SECTION("shutdown") {
    live.owner->cancel(CancelReason::SHUTDOWN);
  }
  SECTION("stop requested") {
    REQUIRE(live.stop.request_stop());
  }
  SECTION("original cutoff expired") {
    live.now->store(4600 * MS);
  }
  SECTION("positive but exhausted whole-millisecond budget") {
    live.now->store(4600 * MS - 1);
  }
  SECTION("clock regression") {
    live.now->store(100 * MS - 1);
  }
  SECTION("throwing clock") {
    live.throwing_clock->store(true);
  }
  SECTION("moved-from input") {
    retained.emplace(std::move(principal));
  }
  const std::string native_error{PQerrorMessage(connection.get())};
  const auto resolved = resolve_http_caller(std::move(principal), *connection);
  require_resolution_failure(resolved, CallerResolutionFailure::RETIRED);
  CHECK(std::string{PQerrorMessage(connection.get())} == native_error);
  CHECK(PQisnonblocking(connection.get()) == 0);
  CHECK(live.lease->extra_used() ==
        before_owner + (retained ? PRINCIPAL_CHARGE : 0));
  retained.reset();
  CHECK(live.lease->extra_used() == before_owner);
  live.release();
  live.require_drained();
}

TEST_CASE("HTTP caller preflight refuses exhausted credit and releases every "
          "temporary charge",
          "[extension-http][http_caller_preflight]") {
  LiveOwner live;
  auto connection = unconnected();
  const auto before_owner = live.lease->extra_used();
  auto principal = decision_pat_principal(live);
  auto held = live.lease->reserve_extra(EXTRA_BYTES_PER_LEASE -
                                        live.lease->extra_used() -
                                        sizeof(ExtraMemoryCharge));
  REQUIRE(held);
  REQUIRE(live.lease->extra_used() == EXTRA_BYTES_PER_LEASE);
  auto expected = CallerResolutionFailure::BUSY;
  SECTION("live sealed decision-only PAT") {
  }
  SECTION("retirement takes precedence over credit exhaustion") {
    live.owner->cancel(CancelReason::GENERATION_RETIRED);
    expected = CallerResolutionFailure::RETIRED;
  }
  const std::string native_error{PQerrorMessage(connection.get())};
  const auto resolved = resolve_http_caller(std::move(principal), *connection);
  require_resolution_failure(resolved, expected);
  CHECK(std::string{PQerrorMessage(connection.get())} == native_error);
  CHECK(live.lease->extra_used() == EXTRA_BYTES_PER_LEASE - PRINCIPAL_CHARGE);
  held.reset();
  CHECK(live.lease->extra_used() == before_owner);
  live.release();
  live.require_drained();
}

namespace {
// Existing disposable PLINTH_PG_* schema fixture and production-owned singleton
// auth client. No private DB client outlives its enclosing test. All live
// worker PGconn access is exclusive.
struct PgCaller {
  plinth::Config config = plinth::ws_test::test_config();
  LiveOwner live;
  std::unique_ptr<plinth::ws_test::TestPg> seed;
  std::shared_ptr<plinth::ws_test::TestPg> worker;
  std::string user, pat;

  PgCaller() {
    static_cast<void>(plinth::ws_test::test_server_port());
    plinth::ws_test::reset_schema(config.db);
    seed = std::make_unique<plinth::ws_test::TestPg>(config.db);
    worker = std::make_shared<plinth::ws_test::TestPg>(config.db);
    REQUIRE(PQstatus(seed->conn) == CONNECTION_OK);
    REQUIRE(PQstatus(worker->conn) == CONNECTION_OK);
    command("SET statement_timeout='5000ms'");
    command("SET lock_timeout='5000ms'");
    auto limits = worker->exec(
        "SET statement_timeout='5000ms'; SET lock_timeout='5000ms'");
    REQUIRE(PQresultStatus(limits.get()) == PGRES_COMMAND_OK);
    user = plinth::ws_test::insert_user(*seed, "http-caller-user",
                                        "fake-caller-password");
    pat = plinth::ws_test::insert_pat(*seed, user, "caller-resolution-token");
    REQUIRE(valid_uuid(user));
    REQUIRE(valid_uuid(pat));
    command("DELETE FROM plinth.group_rules");
  }
  ~PgCaller() {
    live.release();
    // A failed assertion still joins the real auth callback lineage before
    // schema reset/connection teardown. This is a local feature drain, not a
    // claim that the process-owned Drogon database client has joined.
    if (!live.pool.drain(std::chrono::steady_clock::now() + 5s) ||
        !live.process.drain(5s))
      std::terminate();
  }
  auto command(const std::string& sql,
               const std::vector<std::string>& values = {}) -> void {
    auto result = seed->exec_params(sql, values);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto scalar(const std::string& sql,
              const std::vector<std::string>& values = {}) -> std::string {
    auto result = seed->exec_params(sql, values);
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    REQUIRE(PQnfields(result.get()) == 1);
    REQUIRE(PQgetisnull(result.get(), 0, 0) == 0);
    return PQgetvalue(result.get(), 0, 0);
  }
  auto group(const std::string& name) -> void {
    command("INSERT INTO plinth.groups(name) VALUES($1)", {name});
  }
  auto member(const std::string& group) -> void {
    command("INSERT INTO plinth.group_members(group_id,user_id) "
            "SELECT id,$2::uuid FROM plinth.groups WHERE name=$1",
            {group, user});
  }
  auto grant(const std::string& rule, const std::string& group,
             bool orphaned = false) -> void {
    command("INSERT INTO "
            "plinth.rbac_rules(rule,namespace,description,extension_name) "
            "VALUES($1,'callerfixture','Caller resolution fixture','kernel') "
            "ON CONFLICT(rule) DO NOTHING",
            {rule});
    command(
        "INSERT INTO plinth.group_rules(group_id,rule_id) "
        "SELECT g.id,r.id FROM plinth.groups g CROSS JOIN plinth.rbac_rules r "
        "WHERE g.name=$1 AND r.rule=$2",
        {group, rule});
    if (orphaned)
      command("UPDATE plinth.rbac_rules SET orphaned_at=now() WHERE rule=$1",
              {rule});
  }
  auto authenticate(bool anonymous = false, bool ambient = false)
      -> AuthenticatedHttpPrincipal {
    auto req = request();
    if (!anonymous || ambient)
      req->addHeader("Authorization", "Bearer plinth_caller-resolution-token");
    if (ambient) {
      static_cast<void>(plinth::ws_test::insert_session(
          *seed, user, "caller-ambient-session"));
      req->addCookie("plinth_session", "caller-ambient-session");
    }
    REQUIRE(authenticate_http(req,
                              anonymous ? AuthenticationMode::PUBLIC
                                        : AuthenticationMode::PROTECTED,
                              live.owner, live.completion()));
    return live.probe->take();
  }
  auto resolve(bool anonymous = false) -> CallerResolutionResult {
    return resolve_http_caller(authenticate(anonymous), *worker->conn);
  }
  auto mark_worker() -> std::string {
    constexpr auto SQL = "SELECT 'caller-preflight-sentinel'::text";
    auto marker = worker->exec(SQL);
    REQUIRE(PQresultStatus(marker.get()) == PGRES_TUPLES_OK);
    return SQL;
  }
  auto worker_query() -> std::string {
    return scalar("SELECT query FROM pg_stat_activity WHERE pid=$1::integer",
                  {std::to_string(PQbackendPID(worker->conn))});
  }
  auto blocked_by(int worker_pid, int locker_pid) -> bool {
    return scalar("SELECT EXISTS(SELECT 1 FROM pg_stat_activity a WHERE "
                  "a.datname=current_database() AND a.pid=$1::integer "
                  "AND $2::integer=ANY(pg_blocking_pids(a.pid)))::text",
                  {std::to_string(worker_pid), std::to_string(locker_pid)}) ==
           "true";
  }
};

struct PgTransaction {
  std::shared_ptr<plinth::ws_test::TestPg> connection;
  bool held = false;
  explicit PgTransaction(std::shared_ptr<plinth::ws_test::TestPg> connection,
                         const std::string& begin = "BEGIN")
      : connection(std::move(connection)) {
    auto limits = this->connection->exec(
        "SET statement_timeout='5000ms'; SET lock_timeout='5000ms'");
    REQUIRE(PQresultStatus(limits.get()) == PGRES_COMMAND_OK);
    auto result = this->connection->exec(begin);
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
    held = true;
  }
  ~PgTransaction() {
    if (held) {
      auto* result = PQexec(connection->conn, "ROLLBACK");
      if (result) PQclear(result);
    }
  }
  auto release() -> void {
    auto result = connection->exec("ROLLBACK");
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
    held = false;
  }
};

auto snapshot(const CallerResolutionResult& result) -> HttpCallerSnapshot {
  REQUIRE(result.has_value());
  auto value = result->snapshot();
  REQUIRE(value);
  return *value;
}
} // namespace

TEST_CASE("HTTP caller PG public resolution uses only everyone despite ambient "
          "credentials",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  fixture.group("caller-members");
  fixture.member("caller-members");
  fixture.member("admin");
  fixture.grant("caller.public", "everyone");
  fixture.grant("caller.shared", "everyone");
  fixture.grant("caller.shared", "caller-members");
  fixture.grant("caller.private", "caller-members");
  fixture.grant("caller.orphaned", "everyone", true);
  fixture.grant("kernel.admin", "admin");
  {
    auto validated = fixture.authenticate();
    REQUIRE(validated.snapshot() ==
            Principal{Principal::Kind::PAT, fixture.user, fixture.pat});
  }
  auto principal = fixture.authenticate(true, true);
  REQUIRE(principal.snapshot() == Principal{});
  auto resolved =
      resolve_http_caller(std::move(principal), *fixture.worker->conn);
  const auto caller = snapshot(resolved);
  CHECK(caller.principal == Principal{});
  require_context(caller.user, "", "", "anonymous",
                  {"caller.public", "caller.shared"});
}

TEST_CASE("HTTP caller PG PAT rechecks ownership and current identity state",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto principal = fixture.authenticate();
  REQUIRE(principal.snapshot() ==
          Principal{Principal::Kind::PAT, fixture.user, fixture.pat});
  bool accepted = false;
  SECTION("active PAT without rules is a valid caller") {
    accepted = true;
  }
  SECTION("PAT belongs to another user after authentication") {
    const auto other = plinth::ws_test::insert_user(
        *fixture.seed, "other-caller", "fake-other-password");
    fixture.command("UPDATE plinth.pats SET user_id=$1::uuid WHERE id=$2::uuid",
                    {other, fixture.pat});
  }
  SECTION("disabled user") {
    fixture.command("UPDATE plinth.users SET disabled_at=now()");
  }
  SECTION("revoked PAT") {
    fixture.command("UPDATE plinth.pats SET revoked_at=now()");
  }
  SECTION("expired PAT") {
    fixture.command("UPDATE plinth.pats SET expires_at=statement_timestamp()");
  }
  SECTION("deleted PAT") {
    fixture.command("DELETE FROM plinth.pats");
  }
  SECTION("deleted user and identity") {
    fixture.command("DELETE FROM plinth.pats");
    fixture.command("DELETE FROM plinth.group_members");
    fixture.command("DELETE FROM plinth.users");
  }
  auto resolved =
      resolve_http_caller(std::move(principal), *fixture.worker->conn);
  if (accepted) {
    const auto caller = snapshot(resolved);
    CHECK(caller.principal ==
          Principal{Principal::Kind::PAT, fixture.user, fixture.pat});
    require_context(caller.user, fixture.user, "http-caller-user", "pat", {});
  } else {
    require_resolution_failure(resolved, CallerResolutionFailure::REJECTED);
  }
}

TEST_CASE("HTTP caller PG PAT rules are a deduplicated fresh union without "
          "implicit admin",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  for (const auto* name : {"caller-one", "caller-two", "caller-foreign"})
    fixture.group(name);
  fixture.member("caller-one");
  fixture.member("caller-two");
  fixture.member("everyone");
  fixture.grant("caller.public", "everyone");
  fixture.grant("caller.shared", "everyone");
  fixture.grant("caller.shared", "caller-one");
  fixture.grant("caller.shared", "caller-two");
  fixture.grant("caller.alpha", "caller-one");
  fixture.grant("caller.beta", "caller-two");
  fixture.grant("caller.foreign", "caller-foreign");
  fixture.grant("caller.orphaned", "caller-one", true);
  fixture.grant("caller.orphaned", "everyone", true);
  auto first_principal = fixture.authenticate();
  auto next_principal = fixture.authenticate();
  auto first =
      resolve_http_caller(std::move(first_principal), *fixture.worker->conn);
  const std::vector<std::string> original = {"caller.alpha", "caller.beta",
                                             "caller.public", "caller.shared"};
  require_context(snapshot(first).user, fixture.user, "http-caller-user", "pat",
                  original);
  fixture.command(
      "DELETE FROM plinth.group_rules WHERE rule_id="
      "(SELECT id FROM plinth.rbac_rules WHERE rule='caller.alpha')");
  fixture.command("UPDATE plinth.rbac_rules SET orphaned_at=now() WHERE "
                  "rule='caller.beta'");
  fixture.grant("caller.fresh", "caller-one");
  fixture.command(
      "UPDATE plinth.users SET username='renamed-caller' WHERE id=$1::uuid",
      {fixture.user});
  auto next =
      resolve_http_caller(std::move(next_principal), *fixture.worker->conn);
  require_context(snapshot(next).user, fixture.user, "renamed-caller", "pat",
                  {"caller.fresh", "caller.public", "caller.shared"});
  // Returned rule data is one request snapshot; a later lookup must be fresh.
  require_context(snapshot(first).user, fixture.user, "http-caller-user", "pat",
                  original);
}

TEST_CASE("HTTP caller PG rejects existing transaction snapshots without "
          "issuing lookup",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto principal = fixture.authenticate();
  PgTransaction transaction{fixture.worker,
                            "BEGIN ISOLATION LEVEL REPEATABLE READ"};
  auto expected_status = PQTRANS_INTRANS;
  std::string marker;
  SECTION("repeatable read snapshot") {
    marker = fixture.mark_worker();
  }
  SECTION("failed transaction") {
    marker = "SELECT 1/0";
    auto failed = fixture.worker->exec(marker);
    REQUIRE(PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    expected_status = PQTRANS_INERROR;
  }
  const auto result =
      resolve_http_caller(std::move(principal), *fixture.worker->conn);
  require_resolution_failure(result, CallerResolutionFailure::UNAVAILABLE);
  CHECK(PQtransactionStatus(fixture.worker->conn) == expected_status);
  CHECK(fixture.worker_query() == marker);
  transaction.release();
}

TEST_CASE("HTTP caller PG retired and busy principals never send a query",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto principal = fixture.authenticate();
  const auto marker = fixture.mark_worker();
  std::optional<ExtraMemoryCharge> held;
  std::optional<AuthenticatedHttpPrincipal> retained;
  auto expected = CallerResolutionFailure::RETIRED;
  SECTION("cancelled") {
    fixture.live.owner->cancel(CancelReason::CLIENT_GONE);
  }
  SECTION("generation retired") {
    fixture.live.owner->cancel(CancelReason::GENERATION_RETIRED);
  }
  SECTION("stopped") {
    REQUIRE(fixture.live.stop.request_stop());
  }
  SECTION("expired") {
    fixture.live.now->store(4600 * MS);
  }
  SECTION("submillisecond budget") {
    fixture.live.now->store(4600 * MS - 1);
  }
  SECTION("moved-from") {
    retained.emplace(std::move(principal));
  }
  SECTION("no caller payload credit") {
    held = fixture.live.lease->reserve_extra(EXTRA_BYTES_PER_LEASE -
                                             fixture.live.lease->extra_used() -
                                             sizeof(ExtraMemoryCharge));
    REQUIRE(held);
    expected = CallerResolutionFailure::BUSY;
  }
  auto result =
      resolve_http_caller(std::move(principal), *fixture.worker->conn);
  require_resolution_failure(result, expected);
  CHECK(fixture.worker_query() == marker);
  CHECK(PQtransactionStatus(fixture.worker->conn) == PQTRANS_IDLE);
}

TEST_CASE("HTTP caller PG sealed snapshots gate retirement moves and retain "
          "exact leases",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  fixture.grant("caller.public", "everyone");
  const auto owner_bytes = fixture.live.lease->extra_used();
  std::optional<ResolvedHttpCaller> caller;
  {
    auto result = fixture.resolve(true);
    REQUIRE(result);
    caller.emplace(std::move(*result));
    CHECK_FALSE(result->snapshot());
  }
  REQUIRE(caller->snapshot());
  CHECK(fixture.live.lease->extra_used() == owner_bytes + CALLER_CHARGE);
  SECTION("live output and moved-from snapshot") {
    ResolvedHttpCaller moved{std::move(*caller)};
    CHECK_FALSE(caller->snapshot());
    CHECK(moved.snapshot());
    *caller = std::move(moved);
    CHECK_FALSE(moved.snapshot());
    CHECK(caller->snapshot());
  }
  SECTION("cancellation") {
    fixture.live.owner->cancel(CancelReason::CLIENT_GONE);
    CHECK_FALSE(caller->snapshot());
  }
  SECTION("expiry") {
    fixture.live.now->store(4600 * MS);
    CHECK_FALSE(caller->snapshot());
  }
  SECTION("stop") {
    REQUIRE(fixture.live.stop.request_stop());
    CHECK_FALSE(caller->snapshot());
  }
  fixture.live.release();
  CHECK(fixture.live.pool.snapshot() ==
        CreditSnapshot{1, 23, EXTRA_BYTES_PER_LEASE});
  CHECK(fixture.live.process.active_for_test() == 1);
  CHECK_FALSE(fixture.live.pool.drain(std::chrono::steady_clock::now()));
  caller.reset();
  fixture.live.require_drained();
}

TEST_CASE("HTTP caller PG retained auth bookkeeping outlives resolved caller "
          "destruction",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto locker = std::make_shared<plinth::ws_test::TestPg>(fixture.config.db);
  PgTransaction transaction{locker};
  auto locked = locker->exec_params(
      "SELECT id FROM plinth.pats WHERE id=$1::uuid FOR UPDATE", {fixture.pat});
  REQUIRE(PQresultStatus(locked.get()) == PGRES_TUPLES_OK);
  REQUIRE(PQntuples(locked.get()) == 1);
  std::optional<ResolvedHttpCaller> caller;
  {
    auto result = fixture.resolve();
    REQUIRE(result);
    caller.emplace(std::move(*result));
  }
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool blocked = false;
  while (!blocked && std::chrono::steady_clock::now() < deadline) {
    blocked =
        fixture.scalar("SELECT EXISTS(SELECT 1 FROM pg_stat_activity a WHERE "
                       "a.datname=current_database() "
                       "AND $1::integer=ANY(pg_blocking_pids(a.pid)) "
                       "AND a.query LIKE 'UPDATE plinth.pats SET last_used_at "
                       "= NOW()%')::text",
                       {std::to_string(PQbackendPID(locker->conn))}) == "true";
  }
  REQUIRE(blocked);
  REQUIRE(caller->snapshot());
  caller.reset();
  fixture.live.release();
  CHECK(fixture.live.pool.snapshot() ==
        CreditSnapshot{1, 23, EXTRA_BYTES_PER_LEASE});
  CHECK(fixture.live.process.active_for_test() == 1);
  CHECK_FALSE(fixture.live.pool.drain(std::chrono::steady_clock::now()));
  transaction.release();
  fixture.live.require_drained();
  CHECK(fixture.scalar("SELECT count(*)::text FROM plinth.pats WHERE "
                       "last_used_at IS NOT NULL") == "1");
}

TEST_CASE("HTTP caller PG inherits enclosing operation expiry and stop",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto principal = fixture.authenticate(true);
  const auto marker = fixture.mark_worker();
  std::stop_source enclosing;
  auto deadline = std::chrono::steady_clock::now() + 5s;
  SECTION("expired enclosing operation") {
    deadline = std::chrono::steady_clock::now() - 1ns;
  }
  SECTION("stopped enclosing operation") {
    REQUIRE(enclosing.request_stop());
  }
  const auto before = fixture.live.lease->extra_used();
  {
    plinth::db::OperationScope operation{enclosing.get_token(), deadline};
    const auto result =
        resolve_http_caller(std::move(principal), *fixture.worker->conn);
    require_resolution_failure(result, CallerResolutionFailure::UNAVAILABLE);
  }
  CHECK(fixture.live.owner->current());
  CHECK(fixture.worker_query() == marker);
  CHECK(fixture.live.lease->extra_used() == before - PRINCIPAL_CHARGE);
}

TEST_CASE(
    "HTTP caller PG rejects rule and username overflow without partial grants",
    "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  SECTION("public rule width is bounded before native copying") {
    fixture.grant(std::string(256, 'r'), "everyone");
    const auto legal = fixture.resolve(true);
    CHECK(snapshot(legal).user.effective_rules ==
          std::vector<std::string>{std::string(256, 'r')});
    fixture.grant(std::string(257, 'r'), "everyone");
    require_resolution_failure(fixture.resolve(true),
                               CallerResolutionFailure::UNAVAILABLE);
  }
  SECTION("PAT username width is rechecked after authentication") {
    auto principal = fixture.authenticate();
    fixture.command("UPDATE plinth.users SET username=$1",
                    {std::string(257, 'u')});
    require_resolution_failure(
        resolve_http_caller(std::move(principal), *fixture.worker->conn),
        CallerResolutionFailure::UNAVAILABLE);
  }
  SECTION("public and PAT row limits reject the overflow witness") {
    fixture.command(
        "INSERT INTO "
        "plinth.rbac_rules(rule,namespace,description,extension_name) "
        "SELECT 'caller.bound.'||lpad(n::text,4,'0'),'callerfixture','Bounded "
        "fixture','kernel' "
        "FROM generate_series(1,1024) n");
    fixture.command(
        "INSERT INTO plinth.group_rules(group_id,rule_id) "
        "SELECT g.id,r.id FROM plinth.groups g CROSS JOIN plinth.rbac_rules r "
        "WHERE g.name='everyone' AND r.rule LIKE 'caller.bound.%'");
    for (const bool anonymous : {true, false}) {
      CAPTURE(anonymous);
      const auto legal = fixture.resolve(anonymous);
      const auto rules = snapshot(legal).user.effective_rules;
      REQUIRE(rules.size() == 1024);
      CHECK(rules.front() == "caller.bound.0001");
      CHECK(rules.back() == "caller.bound.1024");
    }
    fixture.grant("caller.bound.1025", "everyone");
    for (const bool anonymous : {true, false}) {
      CAPTURE(anonymous);
      require_resolution_failure(fixture.resolve(anonymous),
                                 CallerResolutionFailure::UNAVAILABLE);
    }
  }
}

TEST_CASE("HTTP caller PG bounded database waits honor deadline and stop with "
          "owned cleanup",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  auto bounds = *DeadlineBounds::make(100 * MS, 2100 * MS);
  fixture.live.reset_owner(bounds);
  auto principal = fixture.authenticate(true);
  const auto baseline = fixture.live.lease->extra_used() - PRINCIPAL_CHARGE;
  bool stop_query = false;
  SECTION("absolute database deadline") {
  }
  SECTION("authentication stop token") {
    stop_query = true;
  }
  auto locker = std::make_shared<plinth::ws_test::TestPg>(fixture.config.db);
  PgTransaction transaction{locker};
  auto locked =
      locker->exec("LOCK TABLE plinth.group_rules IN ACCESS EXCLUSIVE MODE");
  REQUIRE(PQresultStatus(locked.get()) == PGRES_COMMAND_OK);
  const auto worker_pid = PQbackendPID(fixture.worker->conn);
  const auto locker_pid = PQbackendPID(locker->conn);
  struct Completion {
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<CallerResolutionResult> result;
  };
  auto completion = std::make_shared<Completion>();
  const auto started = std::chrono::steady_clock::now();
  // The thread owns both the sealed principal and the PGconn's shared owner.
  // jthread joins even on assertion unwind; the query has its own 2s deadline
  // and bounded native cancellation cleanup while the table lock stays held.
  std::jthread worker{[connection = fixture.worker, completion,
                       principal = std::move(principal)]() mutable {
    auto result = resolve_http_caller(std::move(principal), *connection->conn);
    {
      const std::lock_guard lock(completion->mutex);
      completion->result.emplace(std::move(result));
    }
    completion->changed.notify_all();
  }};
  bool blocked = false;
  while (!blocked && std::chrono::steady_clock::now() < started + 2s) {
    blocked = fixture.blocked_by(worker_pid, locker_pid);
    if (!blocked) {
      const std::lock_guard lock(completion->mutex);
      if (completion->result) break;
    }
  }
  REQUIRE(blocked);
  CHECK(fixture.live.process.active_for_test() == 1);
  CHECK(fixture.live.lease->extra_used() ==
        baseline + PRINCIPAL_CHARGE + CALLER_CHARGE);
  if (stop_query) REQUIRE(fixture.live.stop.request_stop());
  {
    std::unique_lock lock(completion->mutex);
    REQUIRE(completion->changed.wait_until(lock, started + 5s, [&completion] {
      return completion->result.has_value();
    }));
    require_resolution_failure(
        *completion->result, stop_query ? CallerResolutionFailure::RETIRED
                                        : CallerResolutionFailure::UNAVAILABLE);
  }
  worker.join();
  CHECK(std::chrono::steady_clock::now() < started + 5s);
  CHECK(fixture.live.lease->extra_used() == baseline);
  CHECK_FALSE(fixture.blocked_by(worker_pid, locker_pid));
  transaction.release();
  fixture.live.release();
  fixture.live.require_drained();
}

TEST_CASE("HTTP caller PG enclosing stop cancels blocked lookup while "
          "authentication stays live",
          "[extension-http][http_caller_pg][ws][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgCaller fixture;
  fixture.grant("caller.public", "everyone");
  const auto baseline = fixture.live.lease->extra_used();
  auto principal = fixture.authenticate(true);
  REQUIRE(fixture.live.owner->current());
  REQUIRE_FALSE(fixture.live.stop.stop_requested());
  REQUIRE(fixture.live.lease->extra_used() == baseline + PRINCIPAL_CHARGE);

  auto locker = std::make_shared<plinth::ws_test::TestPg>(fixture.config.db);
  PgTransaction transaction{locker};
  auto locked =
      locker->exec("LOCK TABLE plinth.group_rules IN ACCESS EXCLUSIVE MODE");
  REQUIRE(PQresultStatus(locked.get()) == PGRES_COMMAND_OK);
  const auto worker_pid = PQbackendPID(fixture.worker->conn);
  const auto locker_pid = PQbackendPID(locker->conn);
  std::stop_source enclosing;
  struct Completion {
    std::mutex mutex;
    std::optional<CallerResolutionResult> result;
    std::chrono::steady_clock::time_point finished_at{};
  };
  auto completion = std::make_shared<Completion>();
  const auto started = std::chrono::steady_clock::now();
  const auto execution_deadline = started + 4500ms;
  const auto completion_deadline = started + 4s;
  // This deadline precedes both the inherited/authentication query deadline
  // and PG timeouts. Missing the enclosing-token relay must fail, even if the
  // query eventually times out. The jthread owns and joins all native work.
  std::jthread worker{[connection = fixture.worker, completion,
                       principal = std::move(principal),
                       stop = enclosing.get_token(),
                       execution_deadline]() mutable {
    plinth::db::OperationScope operation{stop, execution_deadline};
    auto result = resolve_http_caller(std::move(principal), *connection->conn);
    const auto finished_at = std::chrono::steady_clock::now();
    const std::lock_guard lock(completion->mutex);
    completion->finished_at = finished_at;
    completion->result.emplace(std::move(result));
  }};
  bool blocked = false;
  while (!blocked && std::chrono::steady_clock::now() < started + 2s) {
    blocked = fixture.blocked_by(worker_pid, locker_pid);
    if (!blocked) {
      const std::lock_guard lock(completion->mutex);
      if (completion->result) break;
    }
  }
  REQUIRE(blocked);
  REQUIRE(fixture.scalar("SELECT wait_event_type FROM pg_stat_activity WHERE "
                         "pid=$1::integer",
                         {std::to_string(worker_pid)}) == "Lock");
  REQUIRE(fixture.live.lease->extra_used() ==
          baseline + PRINCIPAL_CHARGE + CALLER_CHARGE);
  CHECK(fixture.live.pool.snapshot() ==
        CreditSnapshot{1, 23, EXTRA_BYTES_PER_LEASE});
  CHECK(fixture.live.process.active_for_test() == 1);
  {
    const std::lock_guard lock(completion->mutex);
    REQUIRE_FALSE(completion->result.has_value());
  }
  REQUIRE(enclosing.request_stop());
  CHECK(fixture.live.owner->current());
  CHECK_FALSE(fixture.live.stop.stop_requested());

  bool completed = false;
  while (!completed && std::chrono::steady_clock::now() < completion_deadline) {
    // Sample charges BEFORE observing PG. If this same lookup is still
    // physically blocked afterwards, neither temporary charge may be gone.
    const auto charged = fixture.live.lease->extra_used();
    if (fixture.blocked_by(worker_pid, locker_pid)) {
      CHECK(charged == baseline + PRINCIPAL_CHARGE + CALLER_CHARGE);
    }
    const std::lock_guard lock(completion->mutex);
    completed = completion->result.has_value();
  }
  {
    const std::lock_guard lock(completion->mutex);
    REQUIRE(completion->result.has_value());
    require_resolution_failure(*completion->result,
                               CallerResolutionFailure::UNAVAILABLE);
    CHECK(completion->finished_at < completion_deadline);
  }
  worker.join();
  CHECK(fixture.live.owner->current());
  CHECK_FALSE(fixture.live.stop.stop_requested());
  CHECK(fixture.live.lease->extra_used() == baseline);
  // Cleanup is confirmed while the original exclusive lock remains held.
  CHECK(fixture.scalar(
            "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE pid=$1::integer "
            "AND relation='plinth.group_rules'::regclass "
            "AND mode='AccessExclusiveLock' AND granted)::text",
            {std::to_string(locker_pid)}) == "true");
  CHECK_FALSE(fixture.blocked_by(worker_pid, locker_pid));
  CHECK(fixture.scalar("SELECT count(*)::text FROM pg_stat_activity WHERE "
                       "pid=$1::integer AND state='active'",
                       {std::to_string(worker_pid)}) == "0");
  CHECK(fixture.scalar("SELECT count(*)::text FROM pg_locks WHERE "
                       "pid=$1::integer AND NOT granted",
                       {std::to_string(worker_pid)}) == "0");
  CHECK(fixture.live.pool.snapshot() ==
        CreditSnapshot{1, 23, EXTRA_BYTES_PER_LEASE});
  CHECK(fixture.live.process.active_for_test() == 1);
  CHECK_FALSE(fixture.live.pool.drain(std::chrono::steady_clock::now()));
  transaction.release();
  fixture.live.release();
  fixture.live.require_drained();
}
