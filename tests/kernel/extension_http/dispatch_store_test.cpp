// SPDX-License-Identifier: MIT
#include "../ws/ws_test_fixture.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/extension_http/dispatch_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace plinth::extension_http;
using namespace std::chrono_literals;
constexpr auto ID = "11111111-1111-4111-8111-111111111111";
constexpr auto EPOCH = "22222222-2222-4222-8222-222222222222";
constexpr auto OTHER = "33333333-3333-4333-8333-333333333333";

auto now_ns() -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
auto binding(std::string id = ID, std::string name = "notes",
             std::uint64_t start = 100) -> DispatchBinding {
  ContextData context{
      id,
      EPOCH,
      OTHER,
      EPOCH,
      start,
      start + 4500000000ULL,
      "/dav/*",
      {},
      {ID, std::move(name), "1.2.3", std::string(64, 'a'), OTHER}};
  return {std::move(context),
          start + 5000000000ULL,
          HttpAdapterMode::QUICKJS,
          OTHER,
          "POST",
          "",
          "127.0.0.1",
          "http-store-component-fixture"};
}
auto record() -> DispatchRecord {
  DispatchRecord value;
  value.binding = binding();
  value.fence = {EPOCH, 1};
  value.live_generation_receipt_id =
      value.binding.context.extension.generation_receipt_id;
  return value;
}

static_assert(!std::is_default_constructible_v<PrepareAck>);
static_assert(!std::is_default_constructible_v<HandoffObservation>);
static_assert(!std::is_default_constructible_v<ActualSettlementWitness>);
static_assert(!std::is_default_constructible_v<RecoveryAuthority>);

struct PgStore {
  plinth::Config config{plinth::ws_test::test_config()};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{nullptr, PQfinish};
  plinth::lifecycle::AsyncTaskRegistry tasks;
  CreditPool credits;
  std::shared_ptr<DispatchStoreRoot> root;
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + 30s;
  bool closed = false;
  unsigned sequence = 0;
  struct Generation {
    std::string name, package, receipt;
    bool seeded = false;
  };
  std::vector<Generation> generations;

  PgStore() {
    plinth::ws_test::settle_fixture_work();
    plinth::ws_test::reset_schema(config.db);
    connection.reset(
        PQconnectdb(plinth::db::connection_info(config.db).c_str()));
    REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
    command("BEGIN");
    const auto schema = ensure_schema(*connection);
    command(schema ? "COMMIT" : "ROLLBACK");
    REQUIRE(schema);
    root = DispatchStoreRoot::create(tasks);
  }
  ~PgStore() {
    // No asynchronous callback or DbLoop may escape a failed assertion.
    if (!close()) std::terminate();
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining <= 0ms || !tasks.drain(remaining)) std::terminate();
  }
  auto close() -> bool {
    if (closed) return true;
    tasks.close_admission();
    bool rolled_back = true;
    if (connection &&
        PQtransactionStatus(connection.get()) == PQTRANS_INTRANS) {
      const std::unique_ptr<PGresult, decltype(&PQclear)> result{
          PQexec(connection.get(), "ROLLBACK"), PQclear};
      rolled_back = PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }
    const auto result = (!root || root->drain(deadline)) && rolled_back;
    if (result) closed = true;
    return result;
  }
  auto command(const std::string& sql) -> void {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto command(const std::string& sql,
               const std::vector<std::string>& parameters) -> void {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for (const auto& parameter : parameters)
      values.push_back(parameter.c_str());
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexecParams(connection.get(), sql.c_str(),
                     static_cast<int>(values.size()), nullptr, values.data(),
                     nullptr, nullptr, 0),
        PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  auto generation(const std::string& name, bool seed) -> Generation {
    auto found = std::ranges::find_if(
        generations, [&](const auto& value) { return value.name == name; });
    if (found == generations.end()) {
      auto package = std::string{"44444444-4444-4444-8444-444444444444"};
      auto receipt = std::string{"55555555-5555-4555-8555-555555555555"};
      const auto last = "0123456789abcdef"[generations.size() + 1];
      package.back() = last;
      receipt.back() = last;
      generations.push_back(
          {name, std::move(package), std::move(receipt), false});
      found = generations.end() - 1;
    }
    if (seed && !found->seeded) {
      // Synthetic stored metadata only: no real captured artifact, validated
      // install, callable readiness, claim publication or auth-filter proof.
      command("INSERT INTO "
              "plinth.packages(id,name,version,state,provenance,manifest_json,"
              "entry_point,manifest_checksum) "
              "VALUES($1::uuid,$2,'1.2.3','ACTIVATING','user','{}','server/"
              "main.js',$3)",
              {found->package, name, std::string(64, 'a')});
      command("INSERT INTO "
              "plinth.package_artifact_receipts(package_id,profile,artifact_"
              "digest,declarations_digest,record_count,content_bytes,metadata_"
              "bytes) VALUES($1::uuid,1,$2,$3,0,0,32)",
              {found->package, std::string(64, 'a'), std::string(64, 'b')});
      command(
          "INSERT INTO "
          "plinth.package_http_claim_receipts(generation_receipt_id,package_id,"
          "capture_profile,name,version,provenance,schema,artifact_digest,"
          "registered_declarations_digest,http_declarations_digest,adapter_"
          "descriptor_digest,handler_mode,http_prefixes,unauthenticated_"
          "prefixes,adapter_binding_key,module_entry) "
          "VALUES($1::uuid,$2::uuid,1,$3,'1.2.3','user',1,$4,$5,$6,$7,'quickjs'"
          ",ARRAY['/dav/*'],ARRAY['/dav/*'],'component-fixture','server/"
          "http.js')",
          {found->receipt, found->package, name, std::string(64, 'a'),
           std::string(64, 'b'), std::string(64, 'c'), std::string(64, 'd')});
      found->seeded = true;
    }
    return *found;
  }
  auto scalar(const std::string& sql) -> std::string {
    const std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(connection.get(), sql.c_str()), PQclear};
    REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
    REQUIRE(PQntuples(result.get()) == 1);
    return PQgetvalue(result.get(), 0, 0);
  }
  struct Session {
    std::shared_ptr<StoreDatabaseOwner> database;
    std::shared_ptr<DispatchStoreSession> store;
    std::string id;
  };
  auto session(std::string name = "notes", bool seed = true,
               bool wrong_artifact = false) -> Session {
    const auto stored = generation(name, seed);
    std::string id{ID};
    constexpr auto HEX = "0123456789abcdef";
    id.back() = HEX[++sequence];
    auto process = tasks.try_acquire();
    auto admission = credits.try_acquire(name, 0, process);
    REQUIRE(admission);
    const auto admitted = now_ns();
    auto database = root->open(config.db, admission, process);
    REQUIRE(database);
    auto data = binding(id, name, admitted);
    data.context.extension.package_id = stored.package;
    data.context.extension.generation_receipt_id = stored.receipt;
    if (wrong_artifact) data.context.extension.artifact_sha256.assign(64, 'e');
    auto store =
        DispatchStoreSession::create(database, std::move(data), now_ns);
    REQUIRE(store);
    return {std::move(database), std::move(store), std::move(id)};
  }
  template <typename T, typename Start>
  auto wait(Start start) -> StoreResult<T> {
    struct Completion {
      std::mutex mutex;
      std::condition_variable changed;
      std::optional<StoreResult<T>> result;
    };
    auto completion = std::make_shared<Completion>();
    start([completion](StoreResult<T> result) {
      std::lock_guard lock(completion->mutex);
      if (!completion->result) completion->result.emplace(std::move(result));
      completion->changed.notify_all();
    });
    std::unique_lock lock(completion->mutex);
    const bool ready = completion->changed.wait_until(
        lock, deadline - 3s, [&] { return completion->result.has_value(); });
    if (!ready) {
      lock.unlock();
      const bool drained = close();
      REQUIRE(drained);
      REQUIRE(ready);
      return std::unexpected(StoreError::UNKNOWN);
    }
    return std::move(*completion->result);
  }
};
} // namespace

TEST_CASE("HTTP obligation data keeps captured binding closed and private",
          "[extension_http][dispatch-store]") {
  auto data = binding();
  REQUIRE(valid_dispatch_binding(data));
  data.method = "post";
  REQUIRE_FALSE(valid_dispatch_binding(data));
  data = binding();
  data.node_id = std::string{"node\0private", 12};
  REQUIRE_FALSE(valid_dispatch_binding(data));
  data = binding();
  data.ip_address = std::string{"127.0.0.1\0x", 11};
  REQUIRE_FALSE(valid_dispatch_binding(data));
  data = binding();
  data.session_id = ID;
  REQUIRE_FALSE(valid_dispatch_binding(data));
  data = binding();
  data.context.principal = {Principal::Kind::PAT, ID, OTHER};
  REQUIRE(valid_dispatch_binding(data));
  data = binding();
  data.wire_expiry_ns = data.context.execution_cutoff_ns - 1;
  REQUIRE_FALSE(valid_dispatch_binding(data));
}

TEST_CASE("HTTP obligation exact ARM readback rejects unchanged prepared state",
          "[extension_http][dispatch-store]") {
  auto prepared = record();
  ExpectedMutation prepare{StorePhase::PREPARE, 0, prepared};
  REQUIRE(exact_postcondition(prepared, prepare));
  auto armed = prepared;
  armed.revision = 2;
  armed.authorization = AuthorizationState::ARMED;
  ExpectedMutation arm{StorePhase::ARM, 1, armed};
  REQUIRE_FALSE(exact_postcondition(prepared, arm));
  REQUIRE(exact_postcondition(armed, arm));
  armed.authorization = AuthorizationState::PREPARED;
  REQUIRE_FALSE(
      exact_postcondition(armed, ExpectedMutation{StorePhase::ARM, 1, armed}));
}

TEST_CASE("HTTP obligation exact mutation includes fence revision and every "
          "observation",
          "[extension_http][dispatch-store]") {
  auto post = record();
  post.revision = 2;
  post.authorization = AuthorizationState::ARMED;
  const ExpectedMutation expected{StorePhase::ARM, 1, post};
  auto wrong = post;
  ++wrong.revision;
  REQUIRE_FALSE(exact_postcondition(wrong, expected));
  wrong = post;
  wrong.fence.owner_epoch = OTHER;
  REQUIRE_FALSE(exact_postcondition(wrong, expected));
  wrong = post;
  wrong.wire = WireObservation::UNKNOWN;
  REQUIRE_FALSE(exact_postcondition(wrong, expected));
  wrong = post;
  wrong.retained = true;
  REQUIRE_FALSE(exact_postcondition(wrong, expected));
  wrong = post;
  wrong.binding.context.extension.artifact_sha256[0] = 'b';
  REQUIRE_FALSE(exact_postcondition(wrong, expected));
  REQUIRE(exact_postcondition(post, expected));
}

TEST_CASE("HTTP obligation timeouts preserve execution wire and submillisecond "
          "bounds",
          "[extension_http][dispatch-store]") {
  REQUIRE(store_timeout_ms(0, 4500000000ULL) == 4500);
  REQUIRE(store_timeout_ms(4500000000ULL, 5000000000ULL) == 500);
  REQUIRE_FALSE(store_timeout_ms(4500000000ULL, 4500000000ULL));
  REQUIRE_FALSE(store_timeout_ms(5000000000ULL, 5000000000ULL));
  REQUIRE_FALSE(store_timeout_ms(5000000001ULL, 5000000000ULL));
  REQUIRE_FALSE(store_timeout_ms(0, 999999));
  REQUIRE(store_timeout_ms(0, 1999999) == 1);
}

TEST_CASE("HTTP obligation unresolved uncertainty remains pending until actual "
          "settlement",
          "[extension_http][dispatch-store]") {
  auto data = record();
  data.authorization = AuthorizationState::UNKNOWN;
  data.retained = true;
  REQUIRE(valid_dispatch_record(data));
  data.pending = false;
  REQUIRE_FALSE(valid_dispatch_record(data));
  data.settlement_resolved = true;
  REQUIRE(valid_dispatch_record(data));
  data.live_generation_receipt_id.reset();
  REQUIRE(valid_dispatch_record(data));
  REQUIRE(data.authorization == AuthorizationState::UNKNOWN);
  REQUIRE(data.entry == PhysicalEntry::UNKNOWN);
  data.retained = false;
  REQUIRE_FALSE(valid_dispatch_record(data));
}

TEST_CASE(
    "HTTP obligation entry and choice identity remain distinct from decisions",
    "[extension_http][dispatch-store]") {
  auto data = record();
  data.authorization = AuthorizationState::DISPATCHED;
  data.choice =
      ChoiceRecord{{ID, EPOCH, 1, OTHER}, 200, ChoiceReason::HANDLER_RESPONSE};
  REQUIRE(valid_dispatch_record(data));
  data.choice->identity.dispatch_id = OTHER;
  REQUIRE_FALSE(valid_dispatch_record(data));
  data.choice->identity.dispatch_id = ID;
  data.entry = PhysicalEntry::KNOWN_ENTERED;
  REQUIRE_FALSE(valid_dispatch_record(data));
  data.entered_at_ns = data.binding.context.admitted_at_ns;
  REQUIRE(valid_dispatch_record(data));
  data.entered_at_ns = data.binding.context.execution_cutoff_ns;
  REQUIRE_FALSE(valid_dispatch_record(data));
}

TEST_CASE("HTTP obligation root denies missing accepted owner before opening "
          "native clients",
          "[extension_http][dispatch-store]") {
  plinth::lifecycle::AsyncTaskRegistry tasks;
  auto root = DispatchStoreRoot::create(tasks);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  REQUIRE_FALSE(root->open({}, {}, {}));
  REQUIRE_FALSE(root->open_startup({}, {}));
  REQUIRE(root->active_domains() == 0);
  tasks.close_admission();
  REQUIRE(root->drain(deadline));
  REQUIRE(tasks.drain(std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now())));
}

TEST_CASE("HTTP obligation PG acknowledges real prepare and ARM commits "
          "without execution projection",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  const auto session = fixture.session();
  auto prepared = fixture.wait<PrepareAck>(
      [&](auto cb) { session.store->prepare(std::move(cb)); });
  REQUIRE(prepared);
  CHECK(prepared->record().revision == 1);
  CHECK(fixture.scalar(
            "SELECT authorization_state FROM "
            "plinth.extension_http_dispatch_obligations WHERE dispatch_id='" +
            session.id + "'") == "prepared");
  auto armed = fixture.wait<ArmAck>(
      [&](auto cb) { session.store->arm(*prepared, std::move(cb)); });
  REQUIRE(armed);
  CHECK(armed->record().revision == 2);
  CHECK(fixture.credits.snapshot().tasks ==
        1); // ACK is not native-domain join.
  CHECK(fixture.scalar(
            "SELECT authorization_state FROM "
            "plinth.extension_http_dispatch_obligations WHERE dispatch_id='" +
            session.id + "'") == "armed");
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.audit_log WHERE "
                       "action='extension.http.dispatched'") == "0");
  const auto repeated = fixture.wait<ArmAck>(
      [&](auto cb) { session.store->arm(*prepared, std::move(cb)); });
  CHECK_FALSE(repeated);
  REQUIRE(fixture.close());
  CHECK(fixture.credits.snapshot().tasks == 0);
  CHECK(fixture.tasks.active_for_test() == 0);
}

TEST_CASE("HTTP obligation PG durable root retains dropped callers before "
          "first acquisition",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  auto session = fixture.session();
  // No store callback/self-cycle exists yet: only the fixed root can retain
  // this owner after both caller handles are dropped.
  std::weak_ptr<StoreDatabaseOwner> weak = session.database;
  session.store.reset();
  session.database.reset();
  CHECK_FALSE(weak.expired());
  CHECK(fixture.root->active_domains() == 1);
  CHECK(fixture.credits.snapshot().tasks == 1);
  REQUIRE(fixture.close());
  CHECK(weak.expired());
  CHECK(fixture.root->active_domains() == 0);
  CHECK(fixture.credits.snapshot().tasks == 0);
  CHECK(fixture.tasks.active_for_test() == 0);
}

TEST_CASE(
    "HTTP obligation PG pending capacity survives completed physical domains",
    "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  for (unsigned i = 0; i < 8; ++i) {
    const auto session = fixture.session(i < 4 ? "notes" : "tasks");
    const auto prepared = fixture.wait<PrepareAck>(
        [&](auto cb) { session.store->prepare(std::move(cb)); });
    REQUIRE(prepared);
    REQUIRE(session.database->close(fixture.deadline));
    if (i == 3) {
      const auto fifth = fixture.session("notes");
      const auto rejected = fixture.wait<PrepareAck>(
          [&](auto cb) { fifth.store->prepare(std::move(cb)); });
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error() == StoreError::BUSY);
      REQUIRE(fifth.database->close(fixture.deadline));
    }
  }
  const auto ninth = fixture.session("other");
  const auto rejected = fixture.wait<PrepareAck>(
      [&](auto cb) { ninth.store->prepare(std::move(cb)); });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == StoreError::BUSY);
  CHECK(fixture.scalar(
            "SELECT count(*) FROM plinth.extension_http_dispatch_obligations "
            "WHERE pending") == "8");
  REQUIRE(fixture.close());
  CHECK(fixture.root->active_domains() == 0);
}

TEST_CASE(
    "HTTP obligation PG stale fence refuses ARM and does not fabricate handoff",
    "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  const auto session = fixture.session();
  const auto prepared = fixture.wait<PrepareAck>(
      [&](auto cb) { session.store->prepare(std::move(cb)); });
  REQUIRE(prepared);
  // Controlled component metadata fixture, NOT a real coordinator takeover.
  fixture.command(
      "UPDATE plinth.extension_http_dispatch_obligations SET owner_epoch='" +
      std::string{OTHER} + "',fence_version=2,revision=2 WHERE dispatch_id='" +
      session.id + "'");
  const auto rejected = fixture.wait<ArmAck>(
      [&](auto cb) { session.store->arm(*prepared, std::move(cb)); });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == StoreError::FENCED);
  CHECK(fixture.scalar(
            "SELECT authorization_state FROM "
            "plinth.extension_http_dispatch_obligations WHERE dispatch_id='" +
            session.id + "'") == "prepared");
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.audit_log WHERE "
                       "action='extension.http.dispatched'") == "0");
  REQUIRE(fixture.close());
}

TEST_CASE("HTTP obligation PG schema check refuses altered constraints",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  fixture.command("BEGIN");
  fixture.command("ALTER TABLE plinth.extension_http_dispatch_obligations DROP "
                  "CONSTRAINT extension_http_dispatch_obligations_pkey");
  const auto mismatch = ensure_schema(*fixture.connection);
  fixture.command("ROLLBACK");
  REQUIRE_FALSE(mismatch);
  CHECK(mismatch.error() == StoreError::INVALID);
  REQUIRE(fixture.close());
}

TEST_CASE("HTTP obligation PG pending live pin restricts receipt deletion",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  const auto session = fixture.session();
  const auto prepared = fixture.wait<PrepareAck>(
      [&](auto cb) { session.store->prepare(std::move(cb)); });
  REQUIRE(prepared);
  REQUIRE(prepared->record().live_generation_receipt_id);
  const auto id = *prepared->record().live_generation_receipt_id;
  CHECK(fixture.scalar(
            "SELECT live_generation_receipt_id::text FROM "
            "plinth.extension_http_dispatch_obligations WHERE dispatch_id='" +
            session.id + "'") == id);
  fixture.command("BEGIN");
  const std::unique_ptr<PGresult, decltype(&PQclear)> result{
      PQexec(fixture.connection.get(),
             ("DELETE FROM plinth.package_http_claim_receipts WHERE "
              "generation_receipt_id='" +
              id + "'")
                 .c_str()),
      PQclear};
  const bool restricted =
      PQresultStatus(result.get()) == PGRES_FATAL_ERROR &&
      PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) != nullptr &&
      std::string_view{PQresultErrorField(result.get(), PG_DIAG_SQLSTATE)} ==
          "23503";
  fixture.command("ROLLBACK");
  REQUIRE(restricted);
  REQUIRE(fixture.close());
  CHECK(fixture.scalar(
            "SELECT count(*) FROM plinth.extension_http_dispatch_obligations "
            "WHERE pending AND live_generation_receipt_id IS NOT NULL") == "1");
}

TEST_CASE("HTTP obligation PG missing and mismatched receipt cannot prepare",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  const auto missing = fixture.session("missing", false);
  const auto absent = fixture.wait<PrepareAck>(
      [&](auto cb) { missing.store->prepare(std::move(cb)); });
  REQUIRE_FALSE(absent);
  REQUIRE(missing.database->close(fixture.deadline));
  const auto mismatch = fixture.session("notes", true, true);
  const auto wrong = fixture.wait<PrepareAck>(
      [&](auto cb) { mismatch.store->prepare(std::move(cb)); });
  REQUIRE_FALSE(wrong);
  CHECK(
      fixture.scalar(
          "SELECT count(*) FROM plinth.extension_http_dispatch_obligations") ==
      "0");
  CHECK(fixture.scalar("SELECT count(*) FROM plinth.audit_log WHERE "
                       "action='extension.http.dispatched'") == "0");
  REQUIRE(fixture.close());
}

TEST_CASE("HTTP obligation PG shortened deadline never resets or admits "
          "expired prepare",
          "[extension_http][dispatch-store][integration]") {
  if (!plinth::ws_test::pg_available()) SKIP("PG unavailable");
  PgStore fixture;
  const auto session = fixture.session();
  session.store->shorten_deadline(0);
  session.store->shorten_deadline(std::numeric_limits<std::uint64_t>::max());
  const auto rejected = fixture.wait<PrepareAck>(
      [&](auto cb) { session.store->prepare(std::move(cb)); });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == StoreError::UNAVAILABLE);
  CHECK(
      fixture.scalar(
          "SELECT count(*) FROM plinth.extension_http_dispatch_obligations") ==
      "0");
  CHECK(fixture.root->active_domains() == 1);
  CHECK(fixture.credits.snapshot().tasks == 1);
  REQUIRE(fixture.close());
  CHECK(fixture.credits.snapshot().tasks == 0);
}
