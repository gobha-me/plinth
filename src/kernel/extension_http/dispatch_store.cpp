// SPDX-License-Identifier: MIT
#include "kernel/extension_http/dispatch_store.hpp"

#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include <drogon/orm/DbClient.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace plinth::extension_http {
namespace {
using Json = nlohmann::ordered_json;
using Client = std::shared_ptr<drogon::orm::DbClient>;
using Transaction = std::shared_ptr<drogon::orm::Transaction>;
constexpr auto MAX_VERSION =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::size_t MAX_RECORD_BYTES = 16384;
constexpr std::size_t MAX_OPERATIONS = 16;
constexpr std::array AUTH{"prepared", "armed", "dispatched", "unknown",
                          "aborted"};
constexpr std::array ENTRY{"unknown", "known_not_entered", "known_entered"};
constexpr std::array WIRE{"not_attempted", "attempt_started", "write_completed",
                          "write_failed", "unknown"};
constexpr std::array MODES{"quickjs", "sidecar", "bundled_native"};
constexpr std::array REASONS{
    "handler_response",    "handler_failed",     "response_invalid",
    "adapter_unavailable", "resource_exhausted", "deadline",
    "client_gone",         "shutdown",           "generation_retired"};

template <typename E, std::size_t N>
auto enum_text(E value, const std::array<const char*, N>& names)
    -> std::string_view {
  const auto index = static_cast<std::size_t>(value);
  return index < names.size() ? names[index] : std::string_view{};
}
template <typename E, std::size_t N>
auto read_enum(const Json& value, const std::array<const char*, N>& names)
    -> std::optional<E> {
  if (!value.is_string()) return {};
  for (std::size_t i = 0; i < N; ++i)
    if (value == names[i]) return static_cast<E>(i);
  return {};
}
auto keys(const Json& value, std::initializer_list<std::string_view> fields)
    -> bool {
  return value.is_object() && value.size() == fields.size() &&
         std::ranges::all_of(fields,
                             [&](auto key) { return value.contains(key); });
}
auto text(const Json& value) -> std::optional<std::string> {
  if (!value.is_string()) return {};
  return value.get<std::string>();
}
auto ns(const Json& value) -> std::optional<std::uint64_t> {
  if (!value.is_string()) return {};
  auto parsed = parse_ns(value.get_ref<const std::string&>());
  return parsed ? std::optional{*parsed} : std::nullopt;
}
auto binding_json(const DispatchBinding& binding) -> Json {
  return Json{{"schema", 1},
              {"context", Json::parse(*serialize_context(binding.context))},
              {"wire_expiry_ns", std::to_string(binding.wire_expiry_ns)},
              {"mode", enum_text(binding.mode, MODES)},
              {"adapter_binding_id", binding.adapter_binding_id},
              {"method", binding.method},
              {"session_id", binding.session_id},
              {"ip_address", binding.ip_address},
              {"node_id", binding.node_id},
              {"package_name", binding.context.extension.name}};
}
auto read_binding(const Json& value) -> std::optional<DispatchBinding> {
  if (!keys(value, {"schema", "context", "wire_expiry_ns", "mode",
                    "adapter_binding_id", "method", "session_id", "ip_address",
                    "node_id", "package_name"}) ||
      !value["schema"].is_number_integer() || value["schema"] != 1)
    return {};
  auto context = parse_context(value["context"].dump());
  auto wire = ns(value["wire_expiry_ns"]);
  auto mode = read_enum<HttpAdapterMode>(value["mode"], MODES);
  auto adapter = text(value["adapter_binding_id"]),
       method = text(value["method"]), session = text(value["session_id"]),
       ip = text(value["ip_address"]), node = text(value["node_id"]);
  if (!context || !wire || !mode || !adapter || !method || !session || !ip ||
      !node || value["package_name"] != context->extension.name)
    return {};
  DispatchBinding result{std::move(*context),
                         *wire,
                         *mode,
                         std::move(*adapter),
                         std::move(*method),
                         std::move(*session),
                         std::move(*ip),
                         std::move(*node)};
  return valid_dispatch_binding(result) ? std::optional{std::move(result)}
                                        : std::nullopt;
}
auto choice_json(const ChoiceRecord& choice) -> Json {
  return {{"dispatch_id", choice.identity.dispatch_id},
          {"owner_epoch", choice.identity.owner_epoch},
          {"version", std::to_string(choice.identity.version)},
          {"choice_id", choice.identity.choice_id},
          {"status", choice.status ? Json(*choice.status) : Json(nullptr)},
          {"reason", enum_text(choice.reason, REASONS)}};
}
auto read_choice(const Json& value) -> std::optional<ChoiceRecord> {
  if (!keys(value, {"dispatch_id", "owner_epoch", "version", "choice_id",
                    "status", "reason"}))
    return {};
  auto dispatch = text(value["dispatch_id"]),
       epoch = text(value["owner_epoch"]), id = text(value["choice_id"]);
  auto version = ns(value["version"]);
  auto reason = read_enum<ChoiceReason>(value["reason"], REASONS);
  if (!dispatch || !epoch || !id || !version || !reason) return {};
  std::optional<int> status;
  if (!value["status"].is_null()) {
    if (!value["status"].is_number_integer() || value["status"] < 200 ||
        value["status"] > 599)
      return {};
    status = value["status"].get<int>();
  }
  return ChoiceRecord{
      {std::move(*dispatch), std::move(*epoch), *version, std::move(*id)},
      status,
      *reason};
}
auto record_json(const DispatchRecord& record) -> Json {
  return {
      {"schema", 1},
      {"binding", binding_json(record.binding)},
      {"owner_epoch", record.fence.owner_epoch},
      {"fence_version", std::to_string(record.fence.version)},
      {"revision", std::to_string(record.revision)},
      {"authorization", enum_text(record.authorization, AUTH)},
      {"choice", record.choice ? choice_json(*record.choice) : Json(nullptr)},
      {"wire", enum_text(record.wire, WIRE)},
      {"physical_entry", enum_text(record.entry, ENTRY)},
      {"entered_at_ns", record.entered_at_ns
                            ? Json(std::to_string(*record.entered_at_ns))
                            : Json(nullptr)},
      {"live_generation_receipt_id",
       record.live_generation_receipt_id
           ? Json(*record.live_generation_receipt_id)
           : Json(nullptr)},
      {"projection_confirmed", record.projection_confirmed},
      {"pending", record.pending},
      {"retained", record.retained},
      {"settlement_resolved", record.settlement_resolved}};
}
auto read_record(std::string_view raw) -> std::optional<DispatchRecord> try {
  if (raw.size() > MAX_RECORD_BYTES) return {};
  bool excessive = false;
  unsigned nodes = 0;
  auto value = Json::parse(
      raw,
      [&](int depth, Json::parse_event_t, Json&) {
        if (depth > 8 || ++nodes > 256) excessive = true;
        return !excessive;
      },
      false);
  if (excessive) return {};
  if (!keys(value, {"schema", "binding", "owner_epoch", "fence_version",
                    "revision", "authorization", "choice", "wire",
                    "physical_entry", "entered_at_ns",
                    "live_generation_receipt_id", "projection_confirmed",
                    "pending", "retained", "settlement_resolved"}) ||
      !value["schema"].is_number_integer() || value["schema"] != 1)
    return {};
  auto binding = read_binding(value["binding"]);
  auto epoch = text(value["owner_epoch"]);
  auto fence = ns(value["fence_version"]), revision = ns(value["revision"]);
  auto authorization =
      read_enum<AuthorizationState>(value["authorization"], AUTH);
  auto wire = read_enum<WireObservation>(value["wire"], WIRE);
  auto entry = read_enum<PhysicalEntry>(value["physical_entry"], ENTRY);
  if (!binding || !epoch || !fence || !revision || !authorization || !wire ||
      !entry)
    return {};
  DispatchRecord record;
  record.binding = std::move(*binding);
  record.fence = {std::move(*epoch), *fence};
  record.revision = *revision;
  record.authorization = *authorization;
  record.wire = *wire;
  record.entry = *entry;
  if (!value["choice"].is_null()) {
    record.choice = read_choice(value["choice"]);
    if (!record.choice) return {};
  }
  if (!value["entered_at_ns"].is_null()) {
    record.entered_at_ns = ns(value["entered_at_ns"]);
    if (!record.entered_at_ns) return {};
  }
  if (!value["live_generation_receipt_id"].is_null()) {
    record.live_generation_receipt_id =
        text(value["live_generation_receipt_id"]);
    if (!record.live_generation_receipt_id) return {};
  }
  for (auto field :
       {"projection_confirmed", "pending", "retained", "settlement_resolved"})
    if (!value[field].is_boolean()) return {};
  record.projection_confirmed = value["projection_confirmed"].get<bool>();
  record.pending = value["pending"].get<bool>();
  record.retained = value["retained"].get<bool>();
  record.settlement_resolved = value["settlement_resolved"].get<bool>();
  return valid_dispatch_record(record) ? std::optional{std::move(record)}
                                       : std::nullopt;
} catch (const Json::exception&) {
  return {};
}

constexpr std::string_view GATE_COLUMNS =
    "singleton BOOLEAN PRIMARY KEY DEFAULT true CHECK(singleton)";
constexpr std::string_view OBLIGATION_COLUMNS = R"sql(
dispatch_id UUID PRIMARY KEY,
binding JSONB NOT NULL CHECK(jsonb_typeof(binding)='object' AND octet_length(binding::text)<=8192),
owner_epoch UUID NOT NULL,
fence_version BIGINT NOT NULL CHECK(fence_version>0),
revision BIGINT NOT NULL DEFAULT 1 CHECK(revision>0),
authorization_state TEXT NOT NULL DEFAULT 'prepared' CHECK(authorization_state IN ('prepared','armed','dispatched','unknown','aborted')),
choice JSONB CHECK(choice IS NULL OR (jsonb_typeof(choice)='object' AND octet_length(choice::text)<=1024)),
wire TEXT NOT NULL DEFAULT 'not_attempted' CHECK(wire IN ('not_attempted','attempt_started','write_completed','write_failed','unknown')),
physical_entry TEXT NOT NULL DEFAULT 'unknown' CHECK(physical_entry IN ('known_entered','known_not_entered','unknown')),
entered_at_ns NUMERIC(20,0) CHECK(entered_at_ns IS NULL OR entered_at_ns BETWEEN 0 AND 18446744073709551615),
live_generation_receipt_id UUID REFERENCES plinth.package_http_claim_receipts(generation_receipt_id) ON DELETE RESTRICT,
projection_confirmed BOOLEAN NOT NULL DEFAULT false,
pending BOOLEAN NOT NULL DEFAULT true,
retained BOOLEAN NOT NULL DEFAULT false,
settlement_resolved BOOLEAN NOT NULL DEFAULT false,
created_at TIMESTAMPTZ NOT NULL DEFAULT now(),updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
CHECK((physical_entry='known_entered')=(entered_at_ns IS NOT NULL)),
CHECK(NOT projection_confirmed OR authorization_state='dispatched'),
CHECK(live_generation_receipt_id IS NULL OR (live_generation_receipt_id::text IS NOT DISTINCT FROM (binding#>>'{context,extension,generation_receipt_id}'))),
CHECK(NOT pending OR live_generation_receipt_id IS NOT NULL),
CHECK(live_generation_receipt_id IS NOT NULL OR (NOT pending AND retained AND settlement_resolved)),
CHECK(pending OR (retained AND settlement_resolved))
)sql";
constexpr std::string_view ROW_SQL = R"sql(
SELECT jsonb_build_object('schema',1,'binding',binding,'owner_epoch',owner_epoch::text,
'fence_version',fence_version::text,'revision',revision::text,'authorization',authorization_state,
'choice',choice,'wire',wire,'physical_entry',physical_entry,'entered_at_ns',entered_at_ns::text,
'projection_confirmed',projection_confirmed,'pending',pending,'retained',retained,
'settlement_resolved',settlement_resolved,'live_generation_receipt_id',live_generation_receipt_id::text)::text AS record
FROM plinth.extension_http_dispatch_obligations WHERE dispatch_id=$1::uuid
)sql";

using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
auto pg_query(PGconn& connection, const std::string& sql,
              const std::vector<std::string>& parameters = {}) -> PgResult {
  std::vector<const char*> values;
  values.reserve(parameters.size());
  for (const auto& value : parameters)
    values.push_back(value.c_str());
  return {db::exec_params(&connection, sql.c_str(),
                          static_cast<int>(values.size()), nullptr,
                          values.data(), nullptr, nullptr, 0),
          PQclear};
}
auto pg_command(PGconn& connection, const std::string& sql) -> bool {
  auto result = pg_query(connection, sql);
  return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
}
auto checked_table(PGconn& connection, std::string_view table) -> bool {
  const std::vector<std::string> names{"plinth." + std::string{table},
                                       "pg_temp.http_dispatch_expected_" +
                                           std::string{table}};
  auto result = pg_query(connection, R"sql(
SELECT (actual.relkind='r' AND actual.relpersistence='p' AND actual.relowner=packages.relowner
AND NOT actual.relrowsecurity AND NOT actual.relforcerowsecurity AND NOT actual.relispartition
AND NOT EXISTS(SELECT 1 FROM pg_inherits WHERE inhrelid=actual.oid OR inhparent=actual.oid)
AND NOT EXISTS(SELECT 1 FROM pg_rewrite WHERE ev_class=actual.oid)
AND NOT EXISTS(SELECT 1 FROM pg_trigger WHERE tgrelid=actual.oid AND NOT tgisinternal)
AND NOT EXISTS(SELECT 1 FROM pg_constraint c WHERE c.conrelid=actual.oid AND c.contype='f' AND
 ((SELECT count(*) FROM pg_trigger t WHERE t.tgconstraint=c.oid)<>4 OR EXISTS
 (SELECT 1 FROM pg_trigger t WHERE t.tgconstraint=c.oid AND (NOT t.tgisinternal OR t.tgenabled NOT IN ('O','A')))))
AND NOT EXISTS(SELECT 1 FROM aclexplode(COALESCE(actual.relacl,acldefault('r',actual.relowner))) a WHERE a.grantee<>actual.relowner)
AND NOT EXISTS(SELECT 1 FROM pg_constraint c JOIN pg_index i ON i.indexrelid=c.conindid
WHERE c.conrelid=actual.oid AND (NOT i.indisvalid OR NOT i.indisready OR NOT i.indisunique OR NOT i.indimmediate)))::text
FROM pg_class actual JOIN pg_class packages ON packages.oid='plinth.packages'::regclass
WHERE actual.oid=$1::regclass
)sql",
                         {names[0]});
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1 ||
      std::string_view{PQgetvalue(result.get(), 0, 0)} != "true")
    return false;
  result = pg_query(connection, R"sql(
WITH c AS(SELECT a.attrelid,a.attnum,a.attname,format_type(a.atttypid,a.atttypmod) AS type,
a.attcollation,a.attnotnull,a.attgenerated,a.attidentity,a.attisdropped,a.attacl,
pg_get_expr(d.adbin,d.adrelid) AS default_value FROM pg_attribute a
LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
WHERE a.attnum>0 AND a.attrelid IN($1::regclass,$2::regclass)),
a AS(SELECT attnum,attname,type,attcollation,attnotnull,attgenerated,attidentity,attisdropped,attacl,default_value FROM c WHERE attrelid=$1::regclass),
e AS(SELECT attnum,attname,type,attcollation,attnotnull,attgenerated,attidentity,attisdropped,attacl,default_value FROM c WHERE attrelid=$2::regclass)
(SELECT * FROM a EXCEPT SELECT * FROM e) UNION ALL(SELECT * FROM e EXCEPT SELECT * FROM a)
)sql",
                    names);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 0)
    return false;
  result = pg_query(connection, R"sql(
WITH c AS(SELECT conrelid,contype,convalidated,condeferrable,condeferred,
CASE WHEN contype='f' THEN jsonb_build_object('keys',conkey,'foreign_keys',confkey,
'delete',confdeltype,'update',confupdtype,'match',confmatchtype,'relation',
CASE WHEN confrelid='pg_temp.http_dispatch_expected_receipts'::regclass
THEN 'plinth.package_http_claim_receipts'::regclass ELSE confrelid END)::text
ELSE pg_get_constraintdef(oid,true) END AS definition
FROM pg_constraint WHERE conrelid IN($1::regclass,$2::regclass)),
a AS(SELECT contype,convalidated,condeferrable,condeferred,definition FROM c WHERE conrelid=$1::regclass),
e AS(SELECT contype,convalidated,condeferrable,condeferred,definition FROM c WHERE conrelid=$2::regclass)
(SELECT * FROM a EXCEPT SELECT * FROM e) UNION ALL(SELECT * FROM e EXCEPT SELECT * FROM a)
)sql",
                    names);
  return result && PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
         PQntuples(result.get()) == 0;
}
} // namespace

auto valid_dispatch_binding(const DispatchBinding& binding) -> bool {
  if (!validate_context(binding.context) ||
      !valid_uuid(binding.adapter_binding_id) ||
      !valid_method(binding.method) || enum_text(binding.mode, MODES).empty() ||
      binding.context.execution_cutoff_ns > binding.wire_expiry_ns ||
      binding.wire_expiry_ns - binding.context.admitted_at_ns > 5000000000ULL ||
      binding.context.execution_cutoff_ns - binding.context.admitted_at_ns >
          4500000000ULL ||
      binding.node_id.empty() || binding.node_id.size() > 256 ||
      !valid_utf8(binding.node_id) ||
      std::ranges::any_of(binding.node_id, [](unsigned char c) {
        return c < 0x20 || c == 0x7f;
      }))
    return false;
  if ((!binding.session_id.empty() && !valid_uuid(binding.session_id)) ||
      (binding.context.principal.kind == Principal::Kind::ANONYMOUS &&
       !binding.session_id.empty()))
    return false;
  if (!binding.ip_address.empty()) {
    std::array<unsigned char, 16> bytes{};
    if (binding.ip_address.size() > 45 || binding.ip_address.contains('\0') ||
        (inet_pton(AF_INET, binding.ip_address.c_str(), bytes.data()) != 1 &&
         inet_pton(AF_INET6, binding.ip_address.c_str(), bytes.data()) != 1))
      return false;
  }
  return true;
}
auto valid_dispatch_record(const DispatchRecord& record) -> bool {
  if (!valid_dispatch_binding(record.binding) ||
      !valid_uuid(record.fence.owner_epoch) || record.fence.version == 0 ||
      record.fence.version > MAX_VERSION || record.revision == 0 ||
      record.revision > MAX_VERSION ||
      enum_text(record.authorization, AUTH).empty() ||
      enum_text(record.entry, ENTRY).empty() ||
      enum_text(record.wire, WIRE).empty() ||
      (record.projection_confirmed &&
       record.authorization != AuthorizationState::DISPATCHED) ||
      (!record.pending && (!record.retained || !record.settlement_resolved)) ||
      (record.live_generation_receipt_id &&
       *record.live_generation_receipt_id !=
           record.binding.context.extension.generation_receipt_id) ||
      (!record.live_generation_receipt_id &&
       (record.pending || !record.retained || !record.settlement_resolved)) ||
      ((record.entry == PhysicalEntry::KNOWN_ENTERED) !=
       record.entered_at_ns.has_value()))
    return false;
  if (record.entered_at_ns &&
      (*record.entered_at_ns < record.binding.context.admitted_at_ns ||
       *record.entered_at_ns >= record.binding.context.execution_cutoff_ns))
    return false;
  if (record.choice) {
    const auto& choice = *record.choice;
    if (choice.identity.dispatch_id != record.binding.context.dispatch_id ||
        choice.identity.owner_epoch != record.binding.context.owner_epoch ||
        choice.identity.version == 0 || choice.identity.version > MAX_VERSION ||
        !valid_uuid(choice.identity.choice_id) ||
        enum_text(choice.reason, REASONS).empty() ||
        (choice.status && (*choice.status < 200 || *choice.status > 599)))
      return false;
  }
  return true;
}
auto exact_postcondition(const DispatchRecord& row,
                         const ExpectedMutation& expected) -> bool {
  if (!valid_dispatch_record(row) || !valid_dispatch_record(expected.post) ||
      expected.pre_revision >= MAX_VERSION ||
      expected.post.revision != expected.pre_revision + 1 ||
      row != expected.post)
    return false;
  switch (expected.phase) {
    case StorePhase::PREPARE:
      return expected.pre_revision == 0 &&
             row.authorization == AuthorizationState::PREPARED && !row.choice &&
             row.wire == WireObservation::NOT_ATTEMPTED &&
             row.entry == PhysicalEntry::UNKNOWN && !row.projection_confirmed &&
             row.pending && !row.retained && !row.settlement_resolved;
    case StorePhase::ARM:
      return expected.pre_revision > 0 &&
             row.authorization == AuthorizationState::ARMED && !row.choice &&
             row.pending && !row.projection_confirmed;
    case StorePhase::AUTHORIZATION:
      return expected.pre_revision > 0 &&
             (row.authorization == AuthorizationState::DISPATCHED ||
              row.authorization == AuthorizationState::UNKNOWN ||
              row.authorization == AuthorizationState::ABORTED);
    case StorePhase::CHOICE:
      return expected.pre_revision > 0 &&
             row.authorization == AuthorizationState::DISPATCHED &&
             row.choice.has_value();
    case StorePhase::OBSERVATION: return expected.pre_revision > 0;
    case StorePhase::TERMINAL:
      return expected.pre_revision > 0 && !row.pending && row.retained &&
             row.settlement_resolved;
  }
  return false;
}
auto store_timeout_ms(std::uint64_t now, std::uint64_t deadline)
    -> std::optional<std::uint32_t> {
  if (now >= deadline || deadline - now < 1000000) return {};
  const auto remaining = (deadline - now) / 1000000;
  if (remaining >
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
    return {};
  return static_cast<std::uint32_t>(remaining);
}
auto schema_sql() -> std::string_view {
  static const std::string sql =
      "CREATE TABLE IF NOT EXISTS plinth.extension_http_pending_gate(" +
      std::string{GATE_COLUMNS} +
      ");CREATE TABLE IF NOT EXISTS "
      "plinth.extension_http_dispatch_obligations(" +
      std::string{OBLIGATION_COLUMNS} +
      ");INSERT INTO plinth.extension_http_pending_gate VALUES(true) ON "
      "CONFLICT DO NOTHING;"
      "CREATE INDEX IF NOT EXISTS extension_http_pending_package ON "
      "plinth.extension_http_dispatch_obligations((binding->>'package_name')) "
      "WHERE pending;";
  return sql;
}
auto ensure_schema(PGconn& connection) -> std::expected<void, StoreError> try {
  if (PQtransactionStatus(&connection) != PQTRANS_INTRANS)
    return std::unexpected(StoreError::INVALID);
  // Commands are separate: parameterized libpq execution never assumes that
  // a driver accepts multiple SQL commands as one prepared statement.
  const std::array tables{
      std::pair{"extension_http_pending_gate", GATE_COLUMNS},
      std::pair{"extension_http_dispatch_obligations", OBLIGATION_COLUMNS}};
  // PostgreSQL forbids TEMP foreign keys to permanent relations. Declare an
  // independent temporary parent and compare the complete FK identity after
  // normalizing only that parent's OID; keep the production FK restrictive.
  if (!pg_command(connection,
                  "CREATE TEMP TABLE http_dispatch_expected_receipts("
                  "generation_receipt_id UUID PRIMARY KEY) ON COMMIT DROP"))
    return std::unexpected(StoreError::INVALID);
  for (const auto& [name, columns] : tables) {
    auto temporary_columns = std::string{columns};
    constexpr std::string_view ACTUAL_PARENT =
        "REFERENCES plinth.package_http_claim_receipts(generation_receipt_id)";
    if (const auto parent = temporary_columns.find(ACTUAL_PARENT);
        parent != std::string::npos)
      temporary_columns.replace(
          parent, ACTUAL_PARENT.size(),
          "REFERENCES pg_temp.http_dispatch_expected_receipts("
          "generation_receipt_id)");
    if (!pg_command(connection, "CREATE TABLE IF NOT EXISTS plinth." +
                                    std::string{name} + "(" +
                                    std::string{columns} + ")") ||
        !pg_command(connection, "CREATE TEMP TABLE http_dispatch_expected_" +
                                    std::string{name} + "(" +
                                    temporary_columns + ") ON COMMIT DROP") ||
        !checked_table(connection, name))
      return std::unexpected(StoreError::INVALID);
  }
  if (!pg_command(connection, "INSERT INTO plinth.extension_http_pending_gate "
                              "VALUES(true) ON CONFLICT DO NOTHING") ||
      !pg_command(
          connection,
          "CREATE INDEX IF NOT EXISTS extension_http_pending_package ON "
          "plinth.extension_http_dispatch_obligations((binding->>'package_name'"
          ")) WHERE pending") ||
      !pg_command(connection,
                  "CREATE INDEX http_dispatch_expected_pending_package ON "
                  "pg_temp.http_dispatch_expected_extension_http_dispatch_"
                  "obligations((binding->>'package_name')) WHERE pending"))
    return std::unexpected(StoreError::INVALID);
  auto index = pg_query(connection, R"sql(
SELECT (a.indisvalid AND a.indisready AND a.indislive AND a.indimmediate
AND a.indisunique=e.indisunique AND a.indkey=e.indkey AND a.indclass=e.indclass
AND a.indcollation=e.indcollation AND a.indoption=e.indoption
AND a.indnatts=e.indnatts AND a.indnkeyatts=e.indnkeyatts
AND pg_get_expr(a.indexprs,a.indrelid,true) IS NOT DISTINCT FROM pg_get_expr(e.indexprs,e.indrelid,true)
AND pg_get_expr(a.indpred,a.indrelid,true) IS NOT DISTINCT FROM pg_get_expr(e.indpred,e.indrelid,true))::text
FROM pg_index a,pg_index e WHERE a.indexrelid='plinth.extension_http_pending_package'::regclass
AND a.indrelid='plinth.extension_http_dispatch_obligations'::regclass
AND e.indexrelid='pg_temp.http_dispatch_expected_pending_package'::regclass
)sql");
  if (!index || PQresultStatus(index.get()) != PGRES_TUPLES_OK ||
      PQntuples(index.get()) != 1 ||
      std::string_view{PQgetvalue(index.get(), 0, 0)} != "true" ||
      !pg_command(
          connection,
          "DROP TABLE "
          "pg_temp.http_dispatch_expected_extension_http_pending_gate,pg_temp."
          "http_dispatch_expected_extension_http_dispatch_obligations,"
          "pg_temp.http_dispatch_expected_receipts"))
    return std::unexpected(StoreError::INVALID);
  return {};
} catch (...) {
  return std::unexpected(StoreError::UNAVAILABLE);
}

namespace {
class NativeStoreOperation {
 public:
  virtual ~NativeStoreOperation() = default;
  virtual auto domain_closed() noexcept -> void = 0;
};
} // namespace

struct DispatchStoreRoot::State {
  std::mutex mutex;
  std::array<std::shared_ptr<StoreDatabaseOwner>, 9> owners;
  bool accepting = true;
  bool failed = false;
  bool notify_global = false;
};
struct StoreDatabaseOwner::State {
  std::shared_ptr<AdmissionLease> admission;
  std::shared_ptr<lifecycle::AsyncTaskLease> process;
  std::optional<ExtraMemoryCharge> allocation;
  std::weak_ptr<DispatchStoreRoot> root;
  std::size_t slot = 0;
  std::timed_mutex close_mutex;
  std::recursive_mutex mutex;
  Client writer;
  Client reader;
  std::array<std::shared_ptr<NativeStoreOperation>, MAX_OPERATIONS> operations;
  std::array<Transaction, MAX_OPERATIONS> rollback_transactions;
  bool closing = false;
  bool closed = false;
  bool callback_failure = false;
  bool session_created = false;
};
DispatchStoreRoot::DispatchStoreRoot() : state(std::make_shared<State>()) {
}
DispatchStoreRoot::~DispatchStoreRoot() = default;
auto DispatchStoreRoot::create() -> std::shared_ptr<DispatchStoreRoot> {
  auto root = create(lifecycle::async_tasks());
  std::lock_guard lock(root->state->mutex);
  root->state->notify_global = true;
  return root;
}
auto DispatchStoreRoot::create(lifecycle::AsyncTaskRegistry& registry)
    -> std::shared_ptr<DispatchStoreRoot> {
  auto root = std::shared_ptr<DispatchStoreRoot>(new DispatchStoreRoot);
  // Intrusive registration allocates no failure-path cleanup node. It exists
  // before even the first native client/connection or asynchronous operation.
  registry.retain_cleanup(root);
  return root;
}
auto DispatchStoreRoot::open_impl(
    const Config::Database& config, std::shared_ptr<AdmissionLease> admission,
    std::shared_ptr<lifecycle::AsyncTaskLease> process, bool startup,
    bool with_readback) -> std::shared_ptr<StoreDatabaseOwner> {
  if (!process || (!startup && !admission)) return {};
  std::optional<ExtraMemoryCharge> allocation;
  if (admission) {
    allocation = admission->reserve_extra(std::size_t{128} * 1024);
    if (!allocation) return {};
  }
  auto local = std::make_shared<StoreDatabaseOwner::State>();
  local->admission = std::move(admission);
  local->process = std::move(process);
  local->allocation = std::move(allocation);
  local->root = shared_from_this();
  auto owner =
      std::shared_ptr<StoreDatabaseOwner>(new StoreDatabaseOwner(local));
  {
    std::lock_guard lock(state->mutex);
    if (!state->accepting) return {};
    const auto first = startup ? 8U : 0U, last = startup ? 9U : 8U;
    auto index = first;
    while (index < last && state->owners[index])
      ++index;
    if (index == last) return {};
    local->slot = index;
    state->owners[index] = owner;
  }
  // The fixed root slot is durable BEFORE any native resources are created.
  // A constructor/setup exception leaves the registered domain available for
  // the same real close protocol, instead of final destruction on a DbLoop.
  try {
    const auto info = db::connection_info(config);
    std::lock_guard lock(local->mutex);
    local->writer = drogon::orm::DbClient::newPgClient(info, 1);
    if (with_readback)
      local->reader = drogon::orm::DbClient::newPgClient(info, 1);
  } catch (...) {
    std::lock_guard lock(local->mutex);
    local->closing = true;
    return {};
  }
  return owner;
}
auto DispatchStoreRoot::open(const Config::Database& config,
                             std::shared_ptr<AdmissionLease> admission,
                             std::shared_ptr<lifecycle::AsyncTaskLease> process,
                             bool with_readback)
    -> std::shared_ptr<StoreDatabaseOwner> {
  return open_impl(config, std::move(admission), std::move(process), false,
                   with_readback);
}
auto DispatchStoreRoot::open_startup(
    const Config::Database& config,
    std::shared_ptr<lifecycle::AsyncTaskLease> process)
    -> std::shared_ptr<StoreDatabaseOwner> {
  return open_impl(config, {}, std::move(process), true, false);
}
auto DispatchStoreRoot::close_admission() -> void {
  std::lock_guard lock(state->mutex);
  state->accepting = false;
}
auto DispatchStoreRoot::active_domains() const -> std::size_t {
  std::lock_guard lock(state->mutex);
  return static_cast<std::size_t>(std::ranges::count_if(
      state->owners, [](const auto& p) { return bool(p); }));
}
auto DispatchStoreRoot::drain(std::chrono::steady_clock::time_point deadline)
    -> bool {
  close_admission();
  std::array<std::shared_ptr<StoreDatabaseOwner>, 9> owners;
  {
    std::lock_guard lock(state->mutex);
    owners = state->owners;
  }
  bool settled = true;
  for (const auto& owner : owners)
    if (owner && !owner->close(deadline)) settled = false;
  std::lock_guard lock(state->mutex);
  return settled && !state->failed &&
         std::ranges::none_of(state->owners,
                              [](const auto& owner) { return bool(owner); });
}
auto DispatchStoreRoot::retry_cleanup(
    std::chrono::steady_clock::time_point deadline) noexcept -> bool {
  {
    std::lock_guard lock(state->mutex);
    if (state->accepting) return false;
  }
  try {
    return drain(deadline);
  } catch (...) {
    return false;
  }
}
StoreDatabaseOwner::StoreDatabaseOwner(std::shared_ptr<State> value)
    : state(std::move(value)) {
}
StoreDatabaseOwner::~StoreDatabaseOwner() = default;
auto StoreDatabaseOwner::close(std::chrono::steady_clock::time_point deadline)
    -> bool {
  {
    std::lock_guard lock(state->mutex);
    if (state->closed) return !state->callback_failure;
  }
  std::unique_lock close_lock(state->close_mutex, std::defer_lock);
  if (!close_lock.try_lock_for(deadline - std::chrono::steady_clock::now()))
    return false;
  std::array<Client, 2> clients;
  {
    std::lock_guard lock(state->mutex);
    if (state->closed) return !state->callback_failure;
    state->closing = true;
    clients = {state->writer, state->reader};
  }
  for (const auto& client : clients) {
    if (!client) continue;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0 || !client->closeAllFor(remaining)) return false;
  }
  // All native loops have really closed. These durable client owners are
  // released only here, on the caller's lifecycle/worker thread.
  std::array<std::shared_ptr<NativeStoreOperation>, MAX_OPERATIONS> completed;
  std::array<Transaction, MAX_OPERATIONS> retired_transactions;
  {
    std::lock_guard lock(state->mutex);
    state->writer.reset();
    state->reader.reset();
    completed = state->operations;
    retired_transactions = state->rollback_transactions;
    state->operations.fill({});
    state->rollback_transactions.fill({});
    state->closed = true;
  }
  // Retained failed/rollback transactions drop only after verified native
  // shutdown, while the durable client owners still exist on this thread.
  retired_transactions.fill({});
  clients.fill({});
  // A driver close may discard an unacknowledged commit callback. Complete
  // its logical waiter only after native close, while retaining all leases.
  for (const auto& operation : completed)
    if (operation) operation->domain_closed();
  completed.fill({});
  {
    std::lock_guard lock(state->mutex);
    state->allocation.reset();
    state->admission.reset();
    state->process.reset();
  }
  bool notify_global = false;
  if (auto root = state->root.lock()) {
    std::lock_guard lock(root->state->mutex);
    root->state->failed = root->state->failed || state->callback_failure;
    root->state->owners[state->slot].reset();
    notify_global = root->state->notify_global;
  }
  // Explicit registries are drained synchronously AFTER this physical close;
  // they need no borrowed asynchronous wake callback. Production preserves its
  // process-registry notification through the no-argument factory.
  if (notify_global) lifecycle::async_tasks().notify_cleanup_progress();
  std::lock_guard lock(state->mutex);
  return !state->callback_failure;
}
auto StoreDatabaseOwner::retry_cleanup(
    std::chrono::steady_clock::time_point deadline) noexcept -> bool {
  {
    std::lock_guard lock(state->mutex);
    if (!state->closing) return false;
  }
  try {
    return close(deadline);
  } catch (...) {
    return false;
  }
}

namespace {
auto audit_json(const DispatchRecord& record) -> Json {
  const auto& c = record.binding.context;
  Json principal{{"kind", "anonymous"}};
  if (c.principal.kind == Principal::Kind::PAT)
    principal = {{"kind", "pat"},
                 {"user_id", c.principal.user_id},
                 {"pat_id", c.principal.pat_id}};
  return {
      {"schema", 1},
      {"dispatch_id", c.dispatch_id},
      {"initial_owner_epoch", c.owner_epoch},
      {"owner_epoch", record.fence.owner_epoch},
      {"fence_version", std::to_string(record.fence.version)},
      {"adapter_epoch", c.adapter_epoch},
      {"adapter_binding_id", record.binding.adapter_binding_id},
      {"package_id", c.extension.package_id},
      {"package_name", c.extension.name},
      {"package_version", c.extension.version},
      {"artifact_sha256", c.extension.artifact_sha256},
      {"generation_receipt_id", c.extension.generation_receipt_id},
      {"method", record.binding.method},
      {"matched_prefix", c.matched_prefix},
      {"principal", std::move(principal)},
      {"authorization", enum_text(record.authorization, AUTH)},
      {"choice", record.choice ? choice_json(*record.choice) : Json(nullptr)},
      {"wire", enum_text(record.wire, WIRE)},
      {"physical_entry", enum_text(record.entry, ENTRY)},
      {"entered_at_ns", record.entered_at_ns
                            ? Json(std::to_string(*record.entered_at_ns))
                            : Json(nullptr)}};
}
constexpr std::string_view INSERT_SQL = R"sql(
INSERT INTO plinth.extension_http_dispatch_obligations
(dispatch_id,binding,owner_epoch,fence_version,revision,authorization_state,choice,wire,
physical_entry,entered_at_ns,projection_confirmed,pending,retained,settlement_resolved,live_generation_receipt_id)
SELECT $1::uuid,j->'binding',(j->>'owner_epoch')::uuid,(j->>'fence_version')::bigint,
(j->>'revision')::bigint,j->>'authorization',NULLIF(j->'choice','null'::jsonb),j->>'wire',
j->>'physical_entry',(j->>'entered_at_ns')::numeric(20,0),(j->>'projection_confirmed')::boolean,
(j->>'pending')::boolean,(j->>'retained')::boolean,(j->>'settlement_resolved')::boolean,(j->>'live_generation_receipt_id')::uuid
FROM (SELECT $2::jsonb AS j) p RETURNING dispatch_id::text
)sql";
constexpr std::string_view UPDATE_SQL = R"sql(
UPDATE plinth.extension_http_dispatch_obligations SET
binding=j->'binding',revision=(j->>'revision')::bigint,authorization_state=j->>'authorization',
choice=NULLIF(j->'choice','null'::jsonb),wire=j->>'wire',physical_entry=j->>'physical_entry',
entered_at_ns=(j->>'entered_at_ns')::numeric(20,0),projection_confirmed=(j->>'projection_confirmed')::boolean,
pending=(j->>'pending')::boolean,retained=(j->>'retained')::boolean,
settlement_resolved=(j->>'settlement_resolved')::boolean,
live_generation_receipt_id=(j->>'live_generation_receipt_id')::uuid,updated_at=now()
FROM (SELECT $2::jsonb AS j) p
WHERE dispatch_id=$1::uuid AND owner_epoch=$3::uuid AND fence_version=$4::bigint
AND revision=$5::bigint RETURNING dispatch_id::text
)sql";

// Guard the supported binder's destructor auto-exec on partial setup. This
// cancels only an unexecuted binder, never pretends to cancel native SQL.
class BinderSetup {
 public:
  explicit BinderSetup(drogon::orm::internal::SqlBinder& value)
      : binder(value) {}
  ~BinderSetup() { binder.cancel(); }

 private:
  drogon::orm::internal::SqlBinder& binder;
};
} // namespace

struct DispatchStoreSession::State : std::enable_shared_from_this<State> {
  struct Operation;
  std::shared_ptr<StoreDatabaseOwner> database;
  DispatchBinding binding;
  Clock clock;
  mutable std::recursive_mutex mutex;
  std::optional<DispatchRecord> acknowledged;
  std::optional<ExpectedMutation> expected;
  std::weak_ptr<Operation> current;
  bool busy = false;
  bool sealed = false;
  std::uint64_t enclosing_deadline = std::numeric_limits<std::uint64_t>::max();

  struct Operation : NativeStoreOperation,
                     std::enable_shared_from_this<Operation> {
    struct QueryPayload {
      std::string sql;
      std::vector<std::string> parameters;
    };
    std::shared_ptr<State> session;
    ExpectedMutation expected;
    std::optional<DispatchRecord> previous;
    std::optional<ExtraMemoryCharge> allocation;
    std::function<void(StoreResult<DispatchRecord>)> callback;
    std::uint64_t deadline = 0;
    bool done = false;
    bool ready_to_commit = false;
    bool readback_used = false;
    std::size_t domain_slot = 0;
    std::array<std::shared_ptr<QueryPayload>, 24> query_payloads;
    std::size_t query_count = 0;
    std::size_t query_bytes = 0;

    auto domain_closed() noexcept -> void override {
      try {
        finish(std::unexpected(StoreError::UNAVAILABLE));
      } catch (...) {
        std::lock_guard lock(session->database->state->mutex);
        session->database->state->callback_failure = true;
      }
    }

    auto timeout() -> std::optional<std::uint32_t> {
      std::lock_guard lock(session->mutex);
      return store_timeout_ms(session->clock(),
                              std::min(deadline, session->enclosing_deadline));
    }
    auto finish(StoreResult<DispatchRecord> result) -> void {
      std::function<void(StoreResult<DispatchRecord>)> notify;
      // Allocate/copy before sealing logical completion. If this fails the
      // outer callback guard can still report UNKNOWN exactly once.
      std::optional<DispatchRecord> captured;
      if (result) captured = *result;
      {
        std::lock_guard lock(session->mutex);
        if (done) return;
        done = true;
        session->busy = false;
        if (!result) session->sealed = true;
        if (captured) session->acknowledged = std::move(captured);
        notify = std::move(callback);
      }
      // Database-domain root ownership remains even after this callback. The
      // actual domain close is what joins native callbacks and drops leases.
      try {
        notify(std::move(result));
      } catch (...) {
        {
          std::lock_guard lock(session->mutex);
          session->sealed = true;
        }
        std::lock_guard lock(session->database->state->mutex);
        session->database->state->callback_failure = true;
      }
    }
    auto rollback(const Transaction& tx, StoreError error) -> void {
      {
        std::lock_guard lock(session->database->state->mutex);
        session->database->state->rollback_transactions[domain_slot] = tx;
      }
      // Retain BEFORE requesting rollback: if its setup throws, dropping the
      // last transaction owner must not accidentally request auto-COMMIT.
      try {
        tx->rollback();
      } catch (...) {
        std::lock_guard lock(session->database->state->mutex);
        session->database->state->callback_failure = true;
      }
      finish(std::unexpected(error));
    }
    using Next = std::function<void(const drogon::orm::Result&)>;
    auto raw(const Client& client, std::string sql,
             std::vector<std::string> parameters, Next next,
             const Transaction& rollback_tx = {}) -> void {
      if (!timeout()) {
        if (rollback_tx)
          rollback(rollback_tx, StoreError::UNKNOWN);
        else
          finish(std::unexpected(StoreError::UNKNOWN));
        return;
      }
      auto self = shared_from_this();
      auto owner = session->database;
      try {
        std::shared_ptr<QueryPayload> payload;
        {
          std::lock_guard lock(session->mutex);
          if (done) return;
          std::size_t bytes = sql.size();
          for (const auto& parameter : parameters)
            bytes += parameter.size();
          if (parameters.size() > 8 || query_count == query_payloads.size() ||
              bytes > 65536 - query_bytes) {
            if (rollback_tx)
              rollback(rollback_tx, StoreError::BUSY);
            else
              finish(std::unexpected(StoreError::BUSY));
            return;
          }
          payload = std::make_shared<QueryPayload>(
              QueryPayload{std::move(sql), std::move(parameters)});
          query_payloads[query_count++] = payload;
          query_bytes += bytes;
        }
        bool closing = false;
        {
          std::lock_guard lock(owner->state->mutex);
          closing = owner->state->closing || owner->state->closed;
        }
        if (closing) {
          if (rollback_tx)
            rollback(rollback_tx, StoreError::UNAVAILABLE);
          else
            finish(std::unexpected(StoreError::UNAVAILABLE));
          return;
        }
        // Do not hold the domain mutex through a possibly synchronous driver
        // callback. Patched native admission serializes with actual close.
        auto binder = (*client) << payload->sql;
        BinderSetup guard{binder};
        // The operation root owns every SQL/parameter byte through ACTUAL
        // native-domain close, independently of logical timeout callbacks.
        for (const auto& parameter : payload->parameters)
          binder << parameter;
        binder >> [self, next = std::move(next),
                   rollback_tx](const drogon::orm::Result& result) {
          {
            std::lock_guard lock(self->session->mutex);
            if (self->done) return;
          }
          try {
            next(result);
          } catch (...) {
            if (rollback_tx)
              self->rollback(rollback_tx, StoreError::UNKNOWN);
            else
              self->finish(std::unexpected(StoreError::UNKNOWN));
          }
        };
        binder >> [self, rollback_tx](const std::exception_ptr&) {
          if (rollback_tx)
            self->rollback(rollback_tx, StoreError::UNKNOWN);
          else
            self->finish(std::unexpected(StoreError::UNKNOWN));
        };
        binder.exec();
      } catch (...) {
        if (rollback_tx)
          rollback(rollback_tx, StoreError::UNKNOWN);
        else
          finish(std::unexpected(StoreError::UNKNOWN));
      }
    }
    auto query(const Transaction& tx, std::string sql,
               std::vector<std::string> parameters, Next next) -> void {
      const auto remaining = timeout();
      if (!remaining) {
        rollback(tx, StoreError::UNKNOWN);
        return;
      }
      auto self = shared_from_this();
      // Both limits are positive, floored from the original phase deadline.
      raw(
          tx,
          "SELECT "
          "set_config('statement_timeout',$1,true),set_config('lock_timeout',$"
          "1,true),set_config('synchronous_commit','on',true)",
          {std::to_string(*remaining)},
          [self, tx, sql = std::move(sql), parameters = std::move(parameters),
           next = std::move(next)](const auto&) mutable {
            self->raw(tx, std::move(sql), std::move(parameters),
                      std::move(next), tx);
          },
          tx);
    }
    auto readback() -> void {
      auto owner = session->database;
      Client reader;
      {
        std::lock_guard lock(owner->state->mutex);
        if (!readback_used && !owner->state->closing && !owner->state->closed)
          reader = owner->state->reader;
        if (reader) readback_used = true;
      }
      if (!reader) {
        finish(std::unexpected(StoreError::UNKNOWN));
        return;
      }
      const auto remaining = timeout();
      if (!remaining) {
        finish(std::unexpected(StoreError::UNKNOWN));
        return;
      }
      auto self = shared_from_this();
      // This is the one independent read-only child client. It never acquires
      // a write transaction, recreates a row, or changes the expected state.
      reader->setTimeout(static_cast<double>(*remaining) / 1000.0);
      raw(reader,
          "SELECT "
          "set_config('default_transaction_read_only','on',false),"
          "set_config('statement_timeout',$1,false),set_config('lock_timeout',$"
          "1,false)",
          {std::to_string(*remaining)}, [self, reader](const auto&) {
            self->raw(
                reader, std::string{ROW_SQL} + " AND NOT pg_is_in_recovery()",
                {self->expected.post.binding.context.dispatch_id},
                [self](const auto& result) {
                  if (result.size() != 1) {
                    self->finish(std::unexpected(StoreError::UNKNOWN));
                    return;
                  }
                  const auto row = read_record(
                      result[0]["record"].template as<std::string>());
                  if (!row || !exact_postcondition(*row, self->expected)) {
                    self->finish(std::unexpected(StoreError::UNKNOWN));
                    return;
                  }
                  if (!self->timeout()) {
                    self->finish(std::unexpected(StoreError::UNKNOWN));
                    return;
                  }
                  self->finish(*row);
                });
          });
    }
    auto committed(bool committed) -> void {
      {
        std::lock_guard lock(session->mutex);
        if (done) return;
      }
      if (!ready_to_commit) {
        finish(std::unexpected(StoreError::UNKNOWN));
        return;
      }
      if (!committed) {
        readback();
        return;
      }
      if (!timeout()) {
        finish(std::unexpected(StoreError::UNKNOWN));
        return;
      }
      finish(expected.post);
    }
    auto verify_written(const Transaction& tx) -> void {
      auto self = shared_from_this();
      query(tx, std::string{ROW_SQL},
            {expected.post.binding.context.dispatch_id},
            [self, tx](const auto& result) {
              if (result.size() != 1) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              const auto row =
                  read_record(result[0]["record"].template as<std::string>());
              if (!row || !exact_postcondition(*row, self->expected)) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              self->ready_to_commit = true;
              // The last query owner releases tx after returning. The commit
              // callback installed at acquisition owns self but NEVER owns tx.
            });
    }
    auto project(const Transaction& tx) -> void {
      if (!expected.post.projection_confirmed) {
        verify_written(tx);
        return;
      }
      auto self = shared_from_this();
      query(tx,
            "SELECT action,detail::text AS detail FROM plinth.audit_log WHERE "
            "id=$1::uuid FOR UPDATE",
            {expected.post.binding.context.dispatch_id},
            [self, tx](const auto& result) {
              const auto wanted = audit_json(self->expected.post);
              if (result.size() > 1) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              if (!result.empty()) {
                const auto raw_detail =
                    result[0]["detail"].template as<std::string>();
                if (raw_detail.size() > MAX_RECORD_BYTES ||
                    result[0]["action"].template as<std::string>() !=
                        "extension.http.dispatched") {
                  self->rollback(tx, StoreError::CONFLICT);
                  return;
                }
                const auto old = Json::parse(raw_detail, nullptr, false);
                if (old != wanted &&
                    (!self->previous || !self->previous->projection_confirmed ||
                     old != audit_json(*self->previous))) {
                  self->rollback(tx, StoreError::CONFLICT);
                  return;
                }
                self->query(
                    tx,
                    "UPDATE plinth.audit_log SET detail=$2::jsonb WHERE "
                    "id=$1::uuid AND action='extension.http.dispatched' "
                    "RETURNING id::text",
                    {self->expected.post.binding.context.dispatch_id,
                     wanted.dump()},
                    [self, tx](const auto& updated) {
                      if (updated.size() != 1)
                        self->rollback(tx, StoreError::CONFLICT);
                      else
                        self->verify_written(tx);
                    });
              } else {
                const auto& binding = self->expected.post.binding;
                self->query(tx,
                            "INSERT INTO "
                            "plinth.audit_log(id,action,user_id,session_id,"
                            "detail,detail_encoding,ip_address,node_id) "
                            "VALUES($1::uuid,'extension.http.dispatched',"
                            "NULLIF($2,'')::uuid,NULLIF($3,'')::uuid,$4::jsonb,"
                            "0,NULLIF($5,'')::inet,$6) RETURNING id::text",
                            {binding.context.dispatch_id,
                             binding.context.principal.user_id,
                             binding.session_id, wanted.dump(),
                             binding.ip_address, binding.node_id},
                            [self, tx](const auto& inserted) {
                              if (inserted.size() != 1)
                                self->rollback(tx, StoreError::CONFLICT);
                              else
                                self->verify_written(tx);
                            });
              }
            });
    }
    auto write(const Transaction& tx) -> void {
      auto self = shared_from_this();
      std::vector<std::string> parameters{
          expected.post.binding.context.dispatch_id,
          record_json(expected.post).dump()};
      if (previous) {
        parameters.push_back(previous->fence.owner_epoch);
        parameters.push_back(std::to_string(previous->fence.version));
        parameters.push_back(std::to_string(previous->revision));
      }
      query(tx, std::string{previous ? UPDATE_SQL : INSERT_SQL},
            std::move(parameters), [self, tx](const auto& result) {
              if (result.size() != 1)
                self->rollback(tx, StoreError::CONFLICT);
              else
                self->project(tx);
            });
    }
    auto check_receipt(const Transaction& tx) -> void {
      auto self = shared_from_this();
      const auto& b = expected.post.binding;
      const auto& x = b.context.extension;
      query(tx, R"sql(
SELECT generation_receipt_id::text FROM plinth.package_http_claim_receipts
WHERE generation_receipt_id=$1::uuid AND package_id=$2::uuid AND name=$3 AND version=$4
AND artifact_digest=$5 AND handler_mode=$6 AND $7=ANY(http_prefixes)
AND (($8='anonymous' AND $7=ANY(unauthenticated_prefixes)) OR
     ($8='pat' AND NOT($7=ANY(unauthenticated_prefixes))))
)sql",
            {x.generation_receipt_id, x.package_id, x.name, x.version,
             x.artifact_sha256, std::string{enum_text(b.mode, MODES)},
             b.context.matched_prefix,
             b.context.principal.kind == Principal::Kind::ANONYMOUS
                 ? "anonymous"
                 : "pat"},
            [self, tx](const auto& result) {
              if (result.size() != 1) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              self->write(tx);
            });
    }
    auto inspect(const Transaction& tx) -> void {
      auto self = shared_from_this();
      query(tx, std::string{ROW_SQL} + " FOR UPDATE",
            {expected.post.binding.context.dispatch_id},
            [self, tx](const auto& result) {
              if (!self->previous) {
                if (!result.empty()) {
                  self->rollback(tx, StoreError::EXISTING_STATE);
                  return;
                }
                self->query(
                    tx,
                    "SELECT count(*)::text AS total,count(*) FILTER(WHERE "
                    "binding->>'package_name'=$1)::text AS package FROM "
                    "plinth.extension_http_dispatch_obligations WHERE pending",
                    {self->expected.post.binding.context.extension.name},
                    [self, tx](const auto& counts) {
                      if (counts.size() != 1 ||
                          counts[0]["total"].template as<std::uint64_t>() >=
                              8 ||
                          counts[0]["package"].template as<std::uint64_t>() >=
                              4) {
                        self->rollback(tx, StoreError::BUSY);
                        return;
                      }
                      self->check_receipt(tx);
                    });
                return;
              }
              if (result.size() != 1) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              const auto row =
                  read_record(result[0]["record"].template as<std::string>());
              if (!row) {
                self->rollback(tx, StoreError::INVALID);
                return;
              }
              if (row->fence != self->previous->fence) {
                self->rollback(tx, StoreError::FENCED);
                return;
              }
              if (*row != *self->previous) {
                self->rollback(tx, StoreError::CONFLICT);
                return;
              }
              self->write(tx);
            });
    }
    auto start() -> void {
      Client client;
      {
        std::lock_guard lock(session->database->state->mutex);
        client = session->database->state->writer;
      }
      const auto remaining = timeout();
      if (!client || !remaining) {
        finish(std::unexpected(StoreError::UNAVAILABLE));
        return;
      }
      auto self = shared_from_this();
      try {
        bool closing = false;
        {
          std::lock_guard lock(session->database->state->mutex);
          closing = session->database->state->closing;
        }
        if (closing) {
          finish(std::unexpected(StoreError::UNAVAILABLE));
          return;
        }
        client->setTimeout(static_cast<double>(*remaining) / 1000.0);
        client->newTransactionAsync([self](const Transaction& tx) {
          try {
            if (!tx) {
              self->finish(std::unexpected(StoreError::UNAVAILABLE));
              return;
            }
            {
              std::lock_guard lock(self->session->mutex);
              if (self->done) {
                self->rollback(tx, StoreError::UNAVAILABLE);
                return;
              }
            }
            const auto remaining = self->timeout();
            if (!remaining) {
              self->rollback(tx, StoreError::UNKNOWN);
              return;
            }
            tx->setTimeout(static_cast<double>(*remaining) / 1000.0);
            // No transaction capture here: release of the actual final query
            // owner requests COMMIT; only its real callback can acknowledge it.
            tx->setCommitCallback([self](bool committed) {
              try {
                self->committed(committed);
              } catch (...) {
                self->finish(std::unexpected(StoreError::UNKNOWN));
              }
            });
            // Pin the supported snapshot policy BEFORE the first SELECT,
            // including query()'s timeout setup. Database defaults must not
            // turn the global gate/count protocol into a stale RR snapshot.
            self->raw(
                tx,
                "SET TRANSACTION ISOLATION LEVEL READ COMMITTED, READ WRITE",
                {},
                [self, tx](const auto&) {
                  self->query(
                      tx,
                      "SELECT singleton FROM "
                      "plinth.extension_http_pending_gate "
                      "WHERE singleton AND NOT pg_is_in_recovery() FOR UPDATE",
                      {}, [self, tx](const auto& gate) {
                        if (gate.size() != 1)
                          self->rollback(tx, StoreError::INVALID);
                        else
                          self->inspect(tx);
                      });
                },
                tx);
          } catch (...) {
            if (tx)
              self->rollback(tx, StoreError::UNKNOWN);
            else
              self->finish(std::unexpected(StoreError::UNKNOWN));
          }
        });
      } catch (...) {
        finish(std::unexpected(StoreError::UNKNOWN));
      }
    }
  };
};

DispatchStoreSession::DispatchStoreSession(std::shared_ptr<State> value)
    : state(std::move(value)) {
}
DispatchStoreSession::~DispatchStoreSession() = default;
auto DispatchStoreSession::shorten_deadline(std::uint64_t deadline) -> void {
  std::lock_guard lock(state->mutex);
  state->enclosing_deadline = std::min(state->enclosing_deadline, deadline);
}
auto DispatchStoreSession::create(std::shared_ptr<StoreDatabaseOwner> database,
                                  DispatchBinding binding, Clock clock)
    -> std::shared_ptr<DispatchStoreSession> {
  if (!database || !clock || !valid_dispatch_binding(binding)) return {};
  {
    std::lock_guard lock(database->state->mutex);
    if (database->state->closing || database->state->closed ||
        !database->state->admission || database->state->session_created)
      return {};
    database->state->session_created = true;
  }
  auto value = std::make_shared<State>();
  value->database = std::move(database);
  value->binding = std::move(binding);
  value->clock = std::move(clock);
  return std::shared_ptr<DispatchStoreSession>(
      new DispatchStoreSession(std::move(value)));
}
auto DispatchStoreSession::expected_mutation() const
    -> std::optional<ExpectedMutation> {
  std::lock_guard lock(state->mutex);
  return state->expected;
}
auto DispatchStoreSession::mutate(
    ExpectedMutation expected,
    std::function<void(StoreResult<DispatchRecord>)> callback) -> void {
  if (!callback) return;
  auto notify = [&](StoreError error) { callback(std::unexpected(error)); };
  std::shared_ptr<State::Operation> op;
  {
    std::lock_guard lock(state->mutex);
    if (state->sealed) {
      notify(StoreError::UNAVAILABLE);
      return;
    }
    if (state->busy) {
      notify(StoreError::BUSY);
      return;
    }
    if (expected.post.binding != state->binding ||
        !exact_postcondition(expected.post, expected) ||
        (expected.pre_revision == 0) != !state->acknowledged ||
        (state->acknowledged &&
         expected.pre_revision != state->acknowledged->revision)) {
      notify(StoreError::INVALID);
      return;
    }
    std::lock_guard database_lock(state->database->state->mutex);
    auto& owner = *state->database->state;
    if (owner.closing || owner.closed) {
      notify(StoreError::UNAVAILABLE);
      return;
    }
    const auto slot = std::ranges::find(
        owner.operations, std::shared_ptr<NativeStoreOperation>{});
    if (slot == owner.operations.end()) {
      notify(StoreError::BUSY);
      return;
    }
    auto allocation = owner.admission->reserve_extra(std::size_t{128} * 1024);
    if (!allocation) {
      notify(StoreError::BUSY);
      return;
    }
    op = std::make_shared<State::Operation>();
    op->session = state;
    op->expected = expected;
    op->previous = state->acknowledged;
    op->domain_slot = static_cast<std::size_t>(slot - owner.operations.begin());
    op->allocation = std::move(allocation);
    op->callback = std::move(callback);
    op->deadline = expected.phase == StorePhase::PREPARE ||
                           expected.phase == StorePhase::ARM
                       ? state->binding.context.execution_cutoff_ns
                       : state->binding.wire_expiry_ns;
    state->expected = std::move(expected);
    state->current = op;
    owner
        .operations[static_cast<std::size_t>(slot - owner.operations.begin())] =
        op;
    state->busy = true;
  }
  op->start();
}
auto DispatchStoreSession::prepare(StoreCallback<PrepareAck> callback) -> void {
  if (!callback) return;
  DispatchRecord record;
  record.binding = state->binding;
  record.fence = {record.binding.context.owner_epoch, 1};
  record.live_generation_receipt_id =
      record.binding.context.extension.generation_receipt_id;
  auto self = shared_from_this();
  mutate({StorePhase::PREPARE, 0, std::move(record)},
         [self, callback = std::move(callback)](auto result) mutable {
           if (result)
             callback(PrepareAck{std::move(*result), self});
           else
             callback(std::unexpected(result.error()));
         });
}
auto DispatchStoreSession::arm(const PrepareAck& ack,
                               StoreCallback<ArmAck> callback) -> void {
  if (!callback) return;
  if (ack.owner.lock().get() != this ||
      ack.record().authorization != AuthorizationState::PREPARED ||
      ack.record().revision == MAX_VERSION) {
    callback(std::unexpected(StoreError::INVALID));
    return;
  }
  auto record = ack.record();
  record.authorization = AuthorizationState::ARMED;
  ++record.revision;
  auto self = shared_from_this();
  mutate({StorePhase::ARM, ack.record().revision, std::move(record)},
         [self, callback = std::move(callback)](auto result) mutable {
           if (result)
             callback(ArmAck{std::move(*result), self});
           else
             callback(std::unexpected(result.error()));
         });
}
auto DispatchStoreSession::acknowledge_handoff(
    const ArmAck&, const HandoffObservation&,
    StoreCallback<AuthorizationAck> callback) -> void {
  // TODO(common owner): actual one-time recipient observation, never metadata.
  if (callback) callback(std::unexpected(StoreError::PROOF_REQUIRED));
}
auto DispatchStoreSession::commit_choice(const AuthorizationAck& ack,
                                         const ChoiceRecord& choice,
                                         StoreCallback<ChoiceAck> callback)
    -> void {
  if (!callback) return;
  if (ack.owner.lock().get() != this ||
      ack.record().authorization != AuthorizationState::DISPATCHED ||
      ack.record().revision == MAX_VERSION ||
      (ack.record().choice && *ack.record().choice != choice)) {
    callback(std::unexpected(StoreError::INVALID));
    return;
  }
  auto record = ack.record();
  {
    std::lock_guard lock(state->mutex);
    if (state->acknowledged && state->acknowledged->choice) {
      const auto& current = *state->acknowledged;
      if (*current.choice != choice || current.fence != record.fence ||
          current.binding != record.binding) {
        callback(std::unexpected(StoreError::CONFLICT));
        return;
      }
      if (!current.pending || current.revision == MAX_VERSION) {
        callback(std::unexpected(StoreError::EXISTING_STATE));
        return;
      }
      // Same immutable choice is an idempotent refinement, not a second
      // decision/event. Still check the actual current DB fence/revision.
      record = current;
    }
  }
  const auto previous_revision = record.revision;
  record.choice = choice;
  ++record.revision;
  auto self = shared_from_this();
  mutate({StorePhase::CHOICE, previous_revision, std::move(record)},
         [self, callback = std::move(callback)](auto result) mutable {
           if (result)
             callback(ChoiceAck{std::move(*result), self});
           else
             callback(std::unexpected(result.error()));
         });
}
auto DispatchStoreSession::observe(const ChoiceAck&, WireObservation,
                                   PhysicalEntry, std::optional<std::uint64_t>,
                                   StoreCallback<ObservationAck> callback)
    -> void {
  // TODO(common owner): bind the observation to its actual wire/entry attempt.
  if (callback) callback(std::unexpected(StoreError::PROOF_REQUIRED));
}
auto DispatchStoreSession::retain_terminal(const ObservationAck&,
                                           const ActualSettlementWitness&,
                                           StoreCallback<TerminalAck> callback)
    -> void {
  if (callback) callback(std::unexpected(StoreError::PROOF_REQUIRED));
}
auto DispatchStoreSession::recover(const RecoveryAuthority&,
                                   StoreCallback<DispatchRecord> callback)
    -> void {
  if (callback) callback(std::unexpected(StoreError::PROOF_REQUIRED));
}
auto DispatchStoreSession::readback(StoreCallback<DispatchRecord> callback)
    -> void {
  // Uncertain mutation readback is the one owned child of the ORIGINAL
  // operation. A later public load cannot mint a phase ACK or another ticket.
  if (callback) callback(std::unexpected(StoreError::PROOF_REQUIRED));
}

} // namespace plinth::extension_http
