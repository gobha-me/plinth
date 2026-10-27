#include "kernel/packages/http_claims.hpp"

#include "kernel/db/operations.hpp"
#include "kernel/extension_http/codec.hpp"
#include "kernel/packages/generation_receipts.hpp"
#include "kernel/packages/http_install_authorization.hpp"
#include "kernel/packages/install_lifecycle.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <set>
#include <utility>

namespace plinth::packages::http_claims {
namespace {
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Json = nlohmann::ordered_json;
namespace codec = plinth::extension_http;
constexpr std::size_t PAGE_ROWS = 32;
constexpr std::size_t MAX_ARRAY_JSON = 100000;

auto failure(Error error) -> std::unexpected<Error> {
  return std::unexpected(error);
}
auto query(PGconn* connection, const std::string& sql,
           const std::vector<std::string>& parameters = {}) -> PgResult {
  std::vector<const char*> values;
  values.reserve(parameters.size());
  for (const auto& value : parameters)
    values.push_back(value.c_str());
  return {db::exec_params(connection, sql.c_str(),
                          static_cast<int>(values.size()), nullptr,
                          values.data(), nullptr, nullptr, 0),
          PQclear};
}
auto tuples(const PgResult& result, int columns) -> bool {
  return result && PQresultStatus(result.get()) == PGRES_TUPLES_OK &&
         PQnfields(result.get()) == columns;
}
auto command(PGconn* connection, const std::string& sql) -> bool {
  const PgResult result{db::exec(connection, sql.c_str()), PQclear};
  return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
}
auto cell(const PgResult& result, int row, int column, std::size_t maximum)
    -> std::optional<std::string_view> {
  if (PQgetisnull(result.get(), row, column) ||
      static_cast<std::size_t>(PQgetlength(result.get(), row, column)) >
          maximum)
    return std::nullopt;
  return std::string_view{
      PQgetvalue(result.get(), row, column),
      static_cast<std::size_t>(PQgetlength(result.get(), row, column))};
}
auto mode_name(HttpHandlerMode mode) -> std::string_view {
  switch (mode) {
    case HttpHandlerMode::QUICKJS: return "quickjs";
    case HttpHandlerMode::SIDECAR: return "sidecar";
    case HttpHandlerMode::BUNDLED_NATIVE: return "bundled_native";
  }
  return {};
}
auto provenance_name(Provenance provenance) -> std::string_view {
  switch (provenance) {
    case Provenance::USER: return "user";
    case Provenance::BUNDLED: return "bundled";
  }
  return {};
}
auto parse_array(std::string_view raw) -> Result<std::vector<std::string>> {
  if (raw.size() > MAX_ARRAY_JSON) return failure(Error::INVALID);
  auto array = Json::parse(raw, nullptr, false);
  if (!array.is_array() || array.size() > 64) return failure(Error::INVALID);
  std::vector<std::string> result;
  result.reserve(array.size());
  for (const auto& value : array) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > 256)
      return failure(Error::INVALID);
    result.push_back(value.get<std::string>());
  }
  return result;
}
auto declaration_valid(const HttpDeclaration& declaration) -> bool {
  if (declaration.http_prefixes.empty() || !declaration.handler_mode ||
      declaration.http_prefixes.size() > 64 ||
      declaration.unauthenticated_prefixes.size() > 64 ||
      mode_name(*declaration.handler_mode).empty())
    return false;
  for (const auto& values :
       {&declaration.http_prefixes, &declaration.unauthenticated_prefixes})
    for (const auto& value : *values)
      if (value.size() > 256) return false;
  Json object{
      {"http_prefixes", declaration.http_prefixes},
      {"unauthenticated_prefixes", declaration.unauthenticated_prefixes},
      {"handler_mode", mode_name(*declaration.handler_mode)}};
  auto parsed = HttpDeclaration::parse(object.dump());
  return parsed.value && *parsed.value == declaration;
}
auto receipt_valid(const Receipt& receipt, bool allow_empty_id = false)
    -> bool {
  return ((allow_empty_id && receipt.generation_receipt_id.empty()) ||
          codec::valid_uuid(receipt.generation_receipt_id)) &&
         codec::valid_uuid(receipt.package_id) &&
         codec::valid_name(receipt.name) &&
         codec::valid_version(receipt.version) &&
         (receipt.provenance == "user" || receipt.provenance == "bundled") &&
         codec::valid_hex256(receipt.artifact_digest) &&
         codec::valid_hex256(receipt.registered_declarations_digest) &&
         codec::valid_hex256(receipt.http_declarations_digest) &&
         codec::valid_hex256(receipt.adapter_descriptor_digest) &&
         declaration_valid(receipt.declaration) &&
         receipt.http_declarations_digest ==
             http_install_declarations_sha256(receipt.declaration) &&
         !receipt.adapter_binding_key.empty() &&
         receipt.adapter_binding_key.size() <= 256 &&
         receipt.adapter_binding_key.find('\0') == std::string::npos &&
         codec::valid_utf8(receipt.adapter_binding_key) &&
         (receipt.declaration.handler_mode == HttpHandlerMode::QUICKJS
              ? receipt.module_entry == "server/http.js"
              : !receipt.module_entry);
}
// Shared pure decoder for the same fixed16-column receipt projection. Neither
// this decoder nor the row itself proves transaction/publication/readiness.
auto decode_receipt(const PgResult& rows, int offset) -> Result<Receipt> {
  constexpr std::array<std::size_t, 16> limits{
      36,  36, 64, 256, 7, 64, 64, 64, 64, 14, MAX_ARRAY_JSON, MAX_ARRAY_JSON,
      256, 14, 1,  1};
  std::array<std::string, 16> f;
  for (int col = 0; col < 16; ++col) {
    if (col == 13 && PQgetisnull(rows.get(), 0, offset + col)) continue;
    auto value =
        cell(rows, 0, offset + col, limits[static_cast<std::size_t>(col)]);
    if (!value) return failure(Error::INVALID);
    f[static_cast<std::size_t>(col)] = *value;
  }
  if (f[14] != "1" || f[15] != "1") return failure(Error::INVALID);
  auto prefixes = parse_array(f[10]);
  auto public_prefixes = parse_array(f[11]);
  if (!prefixes || !public_prefixes) return failure(Error::INVALID);
  HttpHandlerMode mode;
  if (f[9] == "quickjs")
    mode = HttpHandlerMode::QUICKJS;
  else if (f[9] == "sidecar")
    mode = HttpHandlerMode::SIDECAR;
  else if (f[9] == "bundled_native")
    mode = HttpHandlerMode::BUNDLED_NATIVE;
  else
    return failure(Error::INVALID);
  Receipt receipt{
      f[0],  f[1],
      f[2],  f[3],
      f[4],  f[5],
      f[6],  f[7],
      f[8],  {std::move(*prefixes), std::move(*public_prefixes), mode},
      f[12], {}};
  if (!PQgetisnull(rows.get(), 0, offset + 13)) receipt.module_entry = f[13];
  if (!receipt_valid(receipt)) return failure(Error::GENERATION_MISMATCH);
  return receipt;
}
auto reservation_valid(const Reservation& reservation) -> bool {
  if (!codec::valid_uuid(reservation.package_id) ||
      !codec::valid_hex256(reservation.http_declarations_digest) ||
      (reservation.generation_receipt_id &&
       !codec::valid_uuid(*reservation.generation_receipt_id)) ||
      (reservation.phase != ReservationPhase::HELD &&
       reservation.phase != ReservationPhase::RETIRING) ||
      reservation.http_prefixes.empty() ||
      reservation.http_prefixes.size() > 64)
    return false;
  for (std::size_t i = 0; i < reservation.http_prefixes.size(); ++i) {
    const auto& prefix = reservation.http_prefixes[i];
    if (!is_valid_http_claim(prefix) ||
        (i != 0 && reservation.http_prefixes[i - 1] >= prefix))
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (http_claims_intersect(prefix, reservation.http_prefixes[j]))
        return false;
  }
  return true;
}
auto binding_valid(const AdapterBinding& binding) -> bool {
  return codec::valid_uuid(binding.adapter_epoch) &&
         codec::valid_uuid(binding.generation_receipt_id) &&
         codec::valid_uuid(binding.owner_epoch) &&
         codec::valid_uuid(binding.clock_domain_id) &&
         codec::valid_hex256(binding.adapter_descriptor_digest);
}
struct Package {
  std::string id, name, version, provenance, checksum, actor, predecessor,
      state;
};
auto read_package(const Transaction& transaction, std::string_view id)
    -> Result<Package> {
  if (!codec::valid_uuid(id)) return failure(Error::INVALID);
  auto result =
      query(transaction.connection(),
            "SELECT id::text,CASE WHEN octet_length(name)<=64 THEN name END,"
            "CASE WHEN octet_length(version)<=256 THEN version END,provenance,"
            "CASE WHEN octet_length(manifest_checksum)<=64 THEN "
            "manifest_checksum END,"
            "COALESCE(installed_by_user_id::text,''),COALESCE(supersedes_id::"
            "text,''),state "
            "FROM plinth.packages WHERE id=$1::uuid",
            {std::string{id}});
  if (!tuples(result, 8)) return failure(Error::UNAVAILABLE);
  if (PQntuples(result.get()) != 1) return failure(Error::GENERATION_MISMATCH);
  constexpr std::array<std::size_t, 8> limits{36, 64, 256, 7, 64, 36, 36, 16};
  std::array<std::string, 8> fields;
  for (int column = 0; column < 8; ++column) {
    auto value =
        cell(result, 0, column, limits[static_cast<std::size_t>(column)]);
    if (!value) return failure(Error::INVALID);
    fields[static_cast<std::size_t>(column)] = *value;
  }
  Package package{fields[0], fields[1], fields[2], fields[3],
                  fields[4], fields[5], fields[6], fields[7]};
  if (package.name != transaction.name())
    return failure(Error::GENERATION_MISMATCH);
  return package;
}
auto candidate_state(std::string_view state) -> bool {
  return state == "VALIDATING" || state == "MIGRATING" ||
         state == "REGISTERING" || state == "EXTRACTING" ||
         state == "ACTIVATING";
}
auto check_authorization(const Transaction& transaction,
                         const AuthorizedHttpSubmission& auth,
                         std::string_view id) -> Result<Package> {
  if (auth.package_name() != transaction.name() ||
      !declaration_valid(auth.declaration()) ||
      !codec::valid_hex256(auth.zip_sha256()) ||
      !codec::valid_hex256(auth.raw_manifest_sha256()) ||
      auth.declarations_sha256() !=
          http_install_declarations_sha256(auth.declaration()))
    return failure(Error::AUTHORIZATION_MISMATCH);
  auto package = read_package(transaction, id);
  if (!package) return failure(package.error());
  if (package->name != auth.package_name() ||
      package->version != auth.package_version() ||
      package->provenance != provenance_name(auth.provenance()) ||
      package->checksum != auth.raw_manifest_sha256() ||
      package->actor != auth.actor_user_id() ||
      (auth.kernel_selected_bundle()
           ? auth.provenance() != Provenance::BUNDLED
           : !codec::valid_uuid(auth.actor_user_id())))
    return failure(Error::AUTHORIZATION_MISMATCH);
  if (auth.operation() == HttpInstallOperation::INSTALL) {
    if (!auth.predecessor_id().empty() || !package->predecessor.empty())
      return failure(Error::AUTHORIZATION_MISMATCH);
  } else if (auth.operation() == HttpInstallOperation::UPGRADE) {
    if (!codec::valid_uuid(auth.predecessor_id()) ||
        auth.predecessor_id() == id ||
        auth.predecessor_id() != package->predecessor)
      return failure(Error::AUTHORIZATION_MISMATCH);
    auto predecessor = read_package(transaction, auth.predecessor_id());
    if (!predecessor || predecessor->name != package->name ||
        (predecessor->state != "ACTIVE" &&
         predecessor->state != "ACTIVE_FLAGGED" &&
         predecessor->state != "SUPERSEDED"))
      return failure(Error::AUTHORIZATION_MISMATCH);
  } else
    return failure(Error::AUTHORIZATION_MISMATCH);
  return package;
}
auto matches_authorization(const Receipt& receipt,
                           const AuthorizedHttpSubmission& auth) -> bool {
  return receipt.name == auth.package_name() &&
         receipt.version == auth.package_version() &&
         receipt.provenance == provenance_name(auth.provenance()) &&
         receipt.declaration == auth.declaration() &&
         receipt.http_declarations_digest == auth.declarations_sha256();
}
auto mounted_intersection(std::string_view claim, std::string_view mount)
    -> Result<bool> {
  // A conservative literal subset of ACTUAL regex registrations. '.' and other
  // regex operators are intentionally not assumed literal just because valid
  // in an HTTP claim. Legacy installation semantics are left untouched.
  if (mount.size() > 8192 || mount.empty() || mount.front() != '/')
    return failure(Error::UNAVAILABLE);
  if (mount.ends_with("//")) return failure(Error::UNAVAILABLE);
  if (mount.size() > 1 && mount.back() == '/') mount.remove_suffix(1);
  if (mount == "/") return failure(Error::UNAVAILABLE);
  for (std::size_t i = 1; i < mount.size(); ++i) {
    const auto c = mount[i];
    if (c == '/' && mount[i - 1] != '/') continue;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')
      continue;
    return failure(Error::UNAVAILABLE);
  }
  auto base = claim;
  const bool subtree = base.ends_with("/*");
  if (subtree) base.remove_suffix(2);
  const auto descendant = [](std::string_view child, std::string_view parent) {
    return child == parent ||
           (child.starts_with(parent) && child.size() > parent.size() &&
            child[parent.size()] == '/');
  };
  return descendant(base, mount) || (subtree && descendant(mount, base));
}
} // namespace

Transaction::Transaction(name_operation::NameOperation& operation_in,
                         Deadline deadline_in, std::stop_token stop_in,
                         int backend_in, std::string transaction_in)
    : operation(&operation_in), deadline(deadline_in), stop(stop_in),
      backend(backend_in), transaction(std::move(transaction_in)) {
}
auto Transaction::connection() const noexcept -> PGconn* {
  return operation->connection();
}
auto Transaction::name() const noexcept -> std::string_view {
  return operation->name();
}
auto Transaction::acquire(name_operation::NameOperation& operation,
                          Deadline deadline, std::stop_token stop)
    -> Result<Transaction> try {
  if (stop.stop_requested()) return failure(Error::CANCELLED);
  if (std::chrono::steady_clock::now() >= deadline)
    return failure(Error::DEADLINE);
  db::OperationScope scope{stop, deadline};
  if (!operation.assert_owned()) return failure(Error::LOCK_NOT_OWNED);
  if (PQtransactionStatus(operation.connection()) != PQTRANS_INTRANS)
    return failure(Error::TRANSACTION_REQUIRED);
  auto settings = query(operation.connection(),
                        "SELECT "
                        "current_setting('transaction_isolation'),current_"
                        "setting('transaction_read_only'),"
                        "pg_backend_pid()::text,pg_current_xact_id()::text");
  if (!tuples(settings, 4) || PQntuples(settings.get()) != 1)
    return failure(Error::UNAVAILABLE);
  if (cell(settings, 0, 0, 32) != "read committed" ||
      cell(settings, 0, 1, 8) != "off")
    return failure(Error::TRANSACTION_REQUIRED);
  auto pid = cell(settings, 0, 2, 16);
  auto xid = cell(settings, 0, 3, 32);
  int backend = 0;
  if (!pid || !xid) return failure(Error::UNAVAILABLE);
  auto parsed =
      std::from_chars(pid->data(), pid->data() + pid->size(), backend);
  if (parsed.ec != std::errc{} || parsed.ptr != pid->data() + pid->size())
    return failure(Error::UNAVAILABLE);
  auto acquired = query(
      operation.connection(),
      "SELECT pg_try_advisory_xact_lock($1::integer,$2::integer)::text",
      {std::to_string(GLOBAL_LOCK_CLASS), std::to_string(GLOBAL_LOCK_OBJECT)});
  if (!tuples(acquired, 1) || PQntuples(acquired.get()) != 1)
    return failure(Error::UNAVAILABLE);
  if (cell(acquired, 0, 0, 5) != "true") return failure(Error::BUSY);
  Transaction transaction{operation, deadline, stop, backend,
                          std::string{*xid}};
  if (auto current = transaction.assert_current(); !current)
    return failure(current.error());
  return transaction;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::assert_current() const -> Result<void> try {
  if (stop.stop_requested()) return failure(Error::CANCELLED);
  if (std::chrono::steady_clock::now() >= deadline)
    return failure(Error::DEADLINE);
  db::OperationScope scope{stop, deadline};
  if (!operation || !operation->assert_owned())
    return failure(Error::LOCK_NOT_OWNED);
  if (PQtransactionStatus(connection()) != PQTRANS_INTRANS)
    return failure(Error::TRANSACTION_REQUIRED);
  auto current = query(
      connection(),
      "SELECT pg_backend_pid()::text,pg_current_xact_id()::text,"
      "current_setting('transaction_isolation'),current_setting('transaction_"
      "read_only'),"
      "EXISTS(SELECT 1 FROM pg_locks WHERE locktype='advisory' AND granted "
      "AND mode='ExclusiveLock' AND pid=pg_backend_pid() AND objsubid=2 "
      "AND database=(SELECT oid FROM pg_database WHERE "
      "datname=current_database()) "
      "AND classid=$1::oid AND objid=$2::oid)::text",
      {std::to_string(GLOBAL_LOCK_CLASS), std::to_string(GLOBAL_LOCK_OBJECT)});
  if (!tuples(current, 5) || PQntuples(current.get()) != 1)
    return failure(Error::UNAVAILABLE);
  if (cell(current, 0, 0, 16) != std::to_string(backend) ||
      cell(current, 0, 1, 32) != transaction)
    return failure(Error::STALE_TRANSACTION);
  if (cell(current, 0, 2, 32) != "read committed" ||
      cell(current, 0, 3, 8) != "off")
    return failure(Error::TRANSACTION_REQUIRED);
  if (cell(current, 0, 4, 5) != "true") return failure(Error::LOCK_NOT_OWNED);
  return {};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::read_reservation(std::string_view id) const
    -> Result<std::optional<Reservation>> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  if (auto package = read_package(*this, id); !package)
    return failure(package.error());
  auto result = query(
      connection(),
      "SELECT "
      "package_id::text,to_json(http_prefixes)::text,http_declarations_digest,"
      "generation_receipt_id::text,phase FROM "
      "plinth.package_http_claim_reservations WHERE package_id=$1::uuid",
      {std::string{id}});
  if (!tuples(result, 5)) return failure(Error::UNAVAILABLE);
  if (PQntuples(result.get()) == 0) return std::nullopt;
  if (PQntuples(result.get()) != 1) return failure(Error::INVALID);
  auto pid = cell(result, 0, 0, 36);
  auto prefixes = cell(result, 0, 1, MAX_ARRAY_JSON);
  auto digest = cell(result, 0, 2, 64);
  auto phase = cell(result, 0, 4, 8);
  if (!pid || !prefixes || !digest || !phase) return failure(Error::INVALID);
  auto parsed = parse_array(*prefixes);
  if (!parsed) return failure(parsed.error());
  Reservation reservation{std::string{*pid},
                          std::move(*parsed),
                          std::string{*digest},
                          {},
                          ReservationPhase::HELD};
  if (*phase == "RETIRING")
    reservation.phase = ReservationPhase::RETIRING;
  else if (*phase != "HELD")
    return failure(Error::INVALID);
  if (!PQgetisnull(result.get(), 0, 3)) {
    auto receipt = cell(result, 0, 3, 36);
    if (!receipt) return failure(Error::INVALID);
    reservation.generation_receipt_id = std::string{*receipt};
  }
  if (!reservation_valid(reservation)) return failure(Error::INVALID);
  if (reservation.generation_receipt_id) {
    auto receipt = read_receipt(id);
    if (!receipt || !*receipt ||
        (**receipt).generation_receipt_id !=
            *reservation.generation_receipt_id ||
        (**receipt).declaration.http_prefixes != reservation.http_prefixes ||
        (**receipt).http_declarations_digest !=
            reservation.http_declarations_digest)
      return failure(Error::GENERATION_MISMATCH);
  }
  return std::optional<Reservation>{std::move(reservation)};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::conflicts(const AuthorizedHttpSubmission& auth,
                            std::string_view id,
                            std::span<const MountedFrontendRoot> roots) const
    -> Result<std::vector<std::string>> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  auto package = check_authorization(*this, auth, id);
  if (!package) return failure(package.error());
  std::set<std::string> conflicts;
  const auto exempt = [&](std::string_view owner) {
    return owner == id || (auth.operation() == HttpInstallOperation::UPGRADE &&
                           owner == auth.predecessor_id());
  };
  std::string cursor;
  while (true) {
    auto rows =
        query(connection(),
              "SELECT "
              "r.package_id::text,to_json(r.http_prefixes)::text,r.http_"
              "declarations_digest,r.phase,"
              "(r.generation_receipt_id IS NULL OR EXISTS(SELECT 1 FROM "
              "plinth.package_http_claim_receipts h "
              "WHERE h.package_id=r.package_id AND "
              "h.generation_receipt_id=r.generation_receipt_id "
              "AND h.http_prefixes=r.http_prefixes AND "
              "h.http_declarations_digest=r.http_declarations_digest))::text "
              "FROM plinth.package_http_claim_reservations r WHERE ($1='' OR "
              "r.package_id>NULLIF($1,'')::uuid) "
              "ORDER BY r.package_id LIMIT 32",
              {cursor});
    if (!tuples(rows, 5) || PQntuples(rows.get()) > static_cast<int>(PAGE_ROWS))
      return failure(Error::UNAVAILABLE);
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
      scope.checkpoint();
      auto owner = cell(rows, row, 0, 36);
      auto raw = cell(rows, row, 1, MAX_ARRAY_JSON);
      auto digest = cell(rows, row, 2, 64);
      auto phase = cell(rows, row, 3, 8);
      if (!owner || !raw || !digest || !phase || !codec::valid_uuid(*owner))
        return failure(Error::INVALID);
      if (cell(rows, row, 4, 5) != "true")
        return failure(Error::GENERATION_MISMATCH);
      auto prefixes = parse_array(*raw);
      if (!prefixes) return failure(prefixes.error());
      Reservation stored{std::string{*owner},
                         std::move(*prefixes),
                         std::string{*digest},
                         {},
                         ReservationPhase::HELD};
      if (*phase == "RETIRING")
        stored.phase = ReservationPhase::RETIRING;
      else if (*phase != "HELD")
        return failure(Error::INVALID);
      if (!reservation_valid(stored)) return failure(Error::INVALID);
      cursor = *owner;
      if (exempt(*owner)) continue;
      for (const auto& submitted : auth.declaration().http_prefixes)
        for (const auto& held : stored.http_prefixes)
          if (http_claims_intersect(submitted, held))
            conflicts.insert(submitted);
    }
    if (PQntuples(rows.get()) < static_cast<int>(PAGE_ROWS)) break;
  }
  cursor.clear();
  while (true) {
    auto rows = query(
        connection(),
        "SELECT id::text,CASE WHEN octet_length(frontend_mount)<=8192 THEN "
        "frontend_mount END "
        "FROM plinth.packages WHERE frontend_mount IS NOT NULL AND state IN "
        "('UPLOADING','VALIDATING','MIGRATING','REGISTERING','EXTRACTING','"
        "ACTIVATING',"
        "'ACTIVE','ACTIVE_FLAGGED','DISABLED','UNINSTALLING') "
        "AND ($1='' OR id>NULLIF($1,'')::uuid) ORDER BY id LIMIT 32",
        {cursor});
    if (!tuples(rows, 2) || PQntuples(rows.get()) > static_cast<int>(PAGE_ROWS))
      return failure(Error::UNAVAILABLE);
    for (int row = 0; row < PQntuples(rows.get()); ++row) {
      scope.checkpoint();
      auto owner = cell(rows, row, 0, 36);
      auto mount = cell(rows, row, 1, 8192);
      if (!owner || !mount || !codec::valid_uuid(*owner))
        return failure(Error::UNAVAILABLE);
      cursor = *owner;
      // The candidate's OWN frontend remains an independent reserved surface.
      if (auth.operation() == HttpInstallOperation::UPGRADE &&
          *owner == auth.predecessor_id())
        continue;
      for (const auto& submitted : auth.declaration().http_prefixes) {
        auto intersects = mounted_intersection(submitted, *mount);
        if (!intersects) return failure(intersects.error());
        if (*intersects) conflicts.insert(submitted);
      }
    }
    if (PQntuples(rows.get()) < static_cast<int>(PAGE_ROWS)) break;
  }
  for (const auto& root : roots) {
    scope.checkpoint();
    if (root.package_id && !codec::valid_uuid(*root.package_id))
      return failure(Error::INVALID);
    if (root.package_id && auth.operation() == HttpInstallOperation::UPGRADE &&
        *root.package_id == auth.predecessor_id())
      continue;
    for (const auto& submitted : auth.declaration().http_prefixes) {
      auto intersects = mounted_intersection(submitted, root.mount);
      if (!intersects) return failure(intersects.error());
      if (*intersects) conflicts.insert(submitted);
    }
  }
  return std::vector<std::string>{conflicts.begin(), conflicts.end()};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::reserve(const AuthorizedHttpSubmission& auth,
                          std::string_view id,
                          std::span<const MountedFrontendRoot> roots)
    -> Result<Reservation> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  auto package = check_authorization(*this, auth, id);
  if (!package) return failure(package.error());
  if (!candidate_state(package->state))
    return failure(Error::GENERATION_MISMATCH);
  auto collisions = conflicts(auth, id, roots);
  if (!collisions) return failure(collisions.error());
  if (!collisions->empty()) return failure(Error::CONFLICT);
  auto existing = read_reservation(id);
  if (!existing) return failure(existing.error());
  Reservation proposed{std::string{id},
                       auth.declaration().http_prefixes,
                       auth.declarations_sha256(),
                       {},
                       ReservationPhase::HELD};
  if (*existing) {
    auto matched = **existing;
    matched.generation_receipt_id.reset();
    if (matched != proposed) return failure(Error::CONFLICT);
    return **existing;
  }
  auto inserted =
      query(connection(),
            "INSERT INTO "
            "plinth.package_http_claim_reservations(package_id,http_prefixes,"
            "http_declarations_digest,phase) "
            "VALUES($1::uuid,ARRAY(SELECT "
            "jsonb_array_elements_text($2::jsonb)),$3,'HELD') RETURNING "
            "package_id::text",
            {std::string{id}, Json(auth.declaration().http_prefixes).dump(),
             auth.declarations_sha256()});
  if (!tuples(inserted, 1) || PQntuples(inserted.get()) != 1)
    return failure(Error::UNAVAILABLE);
  return proposed;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::read_receipt(std::string_view id) const
    -> Result<std::optional<Receipt>> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  auto package = read_package(*this, id);
  if (!package) return failure(package.error());
  auto rows =
      query(connection(),
            "SELECT "
            "generation_receipt_id::text,package_id::text,name,version,"
            "provenance,artifact_digest,"
            "registered_declarations_digest,http_declarations_digest,adapter_"
            "descriptor_digest,handler_mode,"
            "to_json(http_prefixes)::text,to_json(unauthenticated_prefixes)::"
            "text,adapter_binding_key,module_entry,"
            "capture_profile::text,schema::text FROM "
            "plinth.package_http_claim_receipts WHERE package_id=$1::uuid",
            {std::string{id}});
  if (!tuples(rows, 16)) return failure(Error::UNAVAILABLE);
  if (PQntuples(rows.get()) == 0) return std::nullopt;
  if (PQntuples(rows.get()) != 1) return failure(Error::INVALID);
  auto decoded = decode_receipt(rows, 0);
  if (!decoded) return failure(decoded.error());
  auto receipt = std::move(*decoded);
  if (receipt.name != package->name || receipt.version != package->version ||
      receipt.provenance != package->provenance)
    return failure(Error::GENERATION_MISMATCH);
  const generation::GenerationKey key{std::string{id}, package->name,
                                      package->version, 1};
  auto artifact = generation::read_receipt(*operation, key);
  if (!artifact || !*artifact ||
      (**artifact).artifact_digest != receipt.artifact_digest ||
      (**artifact).declarations_digest !=
          receipt.registered_declarations_digest)
    return failure(Error::GENERATION_MISMATCH);
  return std::optional<Receipt>{std::move(receipt)};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto read_committed_receipt(name_operation::NameOperation& operation,
                            const generation::GenerationKey& expected,
                            std::chrono::steady_clock::time_point deadline,
                            std::stop_token stop)
    -> Result<std::optional<Receipt>> try {
  if (!codec::valid_uuid(expected.package_id) ||
      !codec::valid_name(expected.name) ||
      !codec::valid_version(expected.version) || expected.profile != 1 ||
      expected.name != operation.name())
    return failure(Error::INVALID);
  db::OperationScope scope{stop, deadline};
  const auto idle_owned = [&]() -> Result<void> {
    if (stop.stop_requested()) return failure(Error::CANCELLED);
    if (std::chrono::steady_clock::now() >= deadline)
      return failure(Error::DEADLINE);
    if (!operation.connection()) return failure(Error::LOCK_NOT_OWNED);
    if (PQstatus(operation.connection()) != CONNECTION_OK)
      return failure(Error::UNAVAILABLE);
    if (PQtransactionStatus(operation.connection()) != PQTRANS_IDLE)
      return failure(Error::TRANSACTION_REQUIRED);
    if (auto owned = operation.assert_owned(); !owned)
      return failure(owned.error() == name_operation::NameError::NOT_OWNED
                         ? Error::LOCK_NOT_OWNED
                         : Error::UNAVAILABLE);
    if (PQtransactionStatus(operation.connection()) != PQTRANS_IDLE)
      return failure(Error::TRANSACTION_REQUIRED);
    return {};
  };
  if (auto owned = idle_owned(); !owned) return failure(owned.error());
  // This ONE fresh statement sees the package, artifact receipt and HTTP
  // attachment in the same snapshot. Name ownership prevents a cooperating
  // same-package transition; the global mutation guard is not borrowed here.
  auto rows = query(
      operation.connection(),
      "SELECT p.id::text,CASE WHEN octet_length(p.name)<=64 THEN p.name END,"
      "CASE WHEN octet_length(p.version)<=256 THEN p.version "
      "END,p.provenance,p.state,"
      "h.generation_receipt_id::text,h.package_id::text,h.name,h.version,h."
      "provenance,h.artifact_digest,"
      "h.registered_declarations_digest,h.http_declarations_digest,h.adapter_"
      "descriptor_digest,h.handler_mode,"
      "to_json(h.http_prefixes)::text,to_json(h.unauthenticated_prefixes)::"
      "text,h.adapter_binding_key,h.module_entry,"
      "h.capture_profile::text,h.schema::text,a.profile::text,a.artifact_"
      "digest,a.declarations_digest,"
      "r.package_id::text,to_json(r.http_prefixes)::text,r.http_declarations_"
      "digest,r.generation_receipt_id::text,r.phase "
      "FROM plinth.packages p LEFT JOIN plinth.package_artifact_receipts a ON "
      "a.package_id=p.id AND a.profile=1 "
      "LEFT JOIN plinth.package_http_claim_receipts h ON h.package_id=p.id "
      "LEFT JOIN plinth.package_http_claim_reservations r ON r.package_id=p.id "
      "WHERE p.id=$1::uuid LIMIT 2",
      {expected.package_id});
  const auto decode_snapshot = [&]() -> Result<std::optional<Receipt>> {
    if (!tuples(rows, 29)) return failure(Error::UNAVAILABLE);
    if (PQntuples(rows.get()) != 1) return failure(Error::GENERATION_MISMATCH);
    const auto id = cell(rows, 0, 0, 36);
    const auto name = cell(rows, 0, 1, 64);
    const auto version = cell(rows, 0, 2, 256);
    const auto provenance = cell(rows, 0, 3, 7);
    const auto state = cell(rows, 0, 4, 16);
    if (id != expected.package_id || name != expected.name ||
        version != expected.version || !provenance ||
        (*provenance != "user" && *provenance != "bundled") || !state ||
        (*state != "ACTIVE" && *state != "ACTIVE_FLAGGED"))
      return failure(Error::GENERATION_MISMATCH);
    if (PQgetisnull(rows.get(), 0, 5)) {
      if (!PQgetisnull(rows.get(), 0, 24)) return failure(Error::UNAVAILABLE);
      return std::nullopt;
    }
    auto receipt = decode_receipt(rows, 5);
    if (!receipt) return failure(receipt.error());
    auto artifact_profile = cell(rows, 0, 21, 1);
    auto artifact_digest = cell(rows, 0, 22, 64);
    auto declarations_digest = cell(rows, 0, 23, 64);
    if (receipt->package_id != expected.package_id ||
        receipt->name != expected.name ||
        receipt->version != expected.version ||
        receipt->provenance != *provenance || artifact_profile != "1" ||
        artifact_digest != receipt->artifact_digest ||
        declarations_digest != receipt->registered_declarations_digest)
      return failure(Error::GENERATION_MISMATCH);
    auto reservation_id = cell(rows, 0, 24, 36);
    auto prefixes_raw = cell(rows, 0, 25, MAX_ARRAY_JSON);
    auto http_digest = cell(rows, 0, 26, 64);
    auto attached = cell(rows, 0, 27, 36);
    auto phase = cell(rows, 0, 28, 8);
    if (!prefixes_raw || reservation_id != expected.package_id ||
        attached != receipt->generation_receipt_id ||
        http_digest != receipt->http_declarations_digest || phase != "HELD")
      return failure(Error::UNAVAILABLE);
    auto prefixes = parse_array(*prefixes_raw);
    if (!prefixes || *prefixes != receipt->declaration.http_prefixes)
      return failure(Error::GENERATION_MISMATCH);
    return std::optional<Receipt>{std::move(*receipt)};
  };
  auto decoded = decode_snapshot();
  if (auto owned = idle_owned(); !owned) return failure(owned.error());
  return decoded;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::insert_or_match_receipt(const AuthorizedHttpSubmission& auth,
                                          const Receipt& proposed)
    -> Result<Receipt> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  if (!receipt_valid(proposed, true) || !matches_authorization(proposed, auth))
    return failure(Error::INVALID);
  auto package = check_authorization(*this, auth, proposed.package_id);
  if (!package) return failure(package.error());
  auto existing = read_receipt(proposed.package_id);
  if (!existing) return failure(existing.error());
  if (*existing) {
    auto matched = proposed;
    matched.generation_receipt_id = (**existing).generation_receipt_id;
    if (matched != **existing) return failure(Error::CONFLICT);
    return **existing;
  }
  if (package->state != "ACTIVATING")
    return failure(Error::GENERATION_MISMATCH);
  auto reservation = read_reservation(proposed.package_id);
  if (!reservation || !*reservation ||
      (**reservation).phase != ReservationPhase::HELD ||
      (**reservation).generation_receipt_id ||
      (**reservation).http_prefixes != proposed.declaration.http_prefixes ||
      (**reservation).http_declarations_digest !=
          proposed.http_declarations_digest)
    return failure(Error::GENERATION_MISMATCH);
  const generation::GenerationKey key{proposed.package_id, proposed.name,
                                      proposed.version, 1};
  auto artifact = generation::read_receipt(*operation, key);
  if (!artifact || !*artifact ||
      (**artifact).artifact_digest != proposed.artifact_digest ||
      (**artifact).declarations_digest !=
          proposed.registered_declarations_digest)
    return failure(Error::GENERATION_MISMATCH);
  auto inserted = query(
      connection(),
      "INSERT INTO "
      "plinth.package_http_claim_receipts(generation_receipt_id,package_id,"
      "capture_profile,name,version,"
      "provenance,schema,artifact_digest,registered_declarations_digest,http_"
      "declarations_digest,adapter_descriptor_digest,"
      "handler_mode,http_prefixes,unauthenticated_prefixes,adapter_binding_key,"
      "module_entry) VALUES("
      "COALESCE(NULLIF($1,'')::uuid,gen_random_uuid()),$2::uuid,1,$3,$4,$5,1,$"
      "6,$7,$8,$9,$10,"
      "ARRAY(SELECT jsonb_array_elements_text($11::jsonb)),ARRAY(SELECT "
      "jsonb_array_elements_text($12::jsonb)),$13,NULLIF($14,'')) "
      "RETURNING generation_receipt_id::text",
      {proposed.generation_receipt_id, proposed.package_id, proposed.name,
       proposed.version, proposed.provenance, proposed.artifact_digest,
       proposed.registered_declarations_digest,
       proposed.http_declarations_digest, proposed.adapter_descriptor_digest,
       std::string{mode_name(*proposed.declaration.handler_mode)},
       Json(proposed.declaration.http_prefixes).dump(),
       Json(proposed.declaration.unauthenticated_prefixes).dump(),
       proposed.adapter_binding_key, proposed.module_entry.value_or("")});
  if (!tuples(inserted, 1) || PQntuples(inserted.get()) != 1)
    return failure(Error::UNAVAILABLE);
  auto stored = read_receipt(proposed.package_id);
  if (!stored || !*stored) return failure(Error::UNAVAILABLE);
  return **stored;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::attach_receipt(const AuthorizedHttpSubmission& auth,
                                 std::string_view id,
                                 std::string_view receipt_id)
    -> Result<Reservation> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  if (!codec::valid_uuid(receipt_id)) return failure(Error::INVALID);
  auto package = check_authorization(*this, auth, id);
  if (!package) return failure(package.error());
  auto receipt = read_receipt(id);
  auto reservation = read_reservation(id);
  if (!receipt || !*receipt || !reservation || !*reservation ||
      !matches_authorization(**receipt, auth) ||
      (**receipt).generation_receipt_id != receipt_id ||
      (**reservation).phase != ReservationPhase::HELD ||
      (**reservation).http_prefixes != (**receipt).declaration.http_prefixes ||
      (**reservation).http_declarations_digest !=
          (**receipt).http_declarations_digest)
    return failure(Error::GENERATION_MISMATCH);
  if ((**reservation).generation_receipt_id) {
    if (*(**reservation).generation_receipt_id != receipt_id)
      return failure(Error::CONFLICT);
    return **reservation;
  }
  if (package->state != "ACTIVATING")
    return failure(Error::GENERATION_MISMATCH);
  auto updated = query(connection(),
                       "UPDATE plinth.package_http_claim_reservations SET "
                       "generation_receipt_id=$2::uuid "
                       "WHERE package_id=$1::uuid AND generation_receipt_id IS "
                       "NULL AND phase='HELD' RETURNING package_id::text",
                       {std::string{id}, std::string{receipt_id}});
  if (!tuples(updated, 1) || PQntuples(updated.get()) != 1)
    return failure(Error::CONFLICT);
  auto stored = read_reservation(id);
  if (!stored || !*stored) return failure(Error::UNAVAILABLE);
  return **stored;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::read_binding(std::string_view epoch) const
    -> Result<std::optional<AdapterBinding>> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  if (!codec::valid_uuid(epoch)) return failure(Error::INVALID);
  auto rows = query(connection(),
                    "SELECT "
                    "b.adapter_epoch::text,b.generation_receipt_id::text,b."
                    "owner_epoch::text,b.clock_domain_id::text,"
                    "b.adapter_descriptor_digest,(b.retired_at IS NOT "
                    "NULL)::text,r.package_id::text "
                    "FROM plinth.package_http_adapter_bindings b JOIN "
                    "plinth.package_http_claim_receipts r "
                    "ON r.generation_receipt_id=b.generation_receipt_id WHERE "
                    "b.adapter_epoch=$1::uuid",
                    {std::string{epoch}});
  if (!tuples(rows, 7)) return failure(Error::UNAVAILABLE);
  if (PQntuples(rows.get()) == 0) return std::nullopt;
  if (PQntuples(rows.get()) != 1) return failure(Error::INVALID);
  std::array<std::string, 7> f;
  constexpr std::array<std::size_t, 7> limits{36, 36, 36, 36, 64, 5, 36};
  for (int col = 0; col < 7; ++col) {
    auto value = cell(rows, 0, col, limits[static_cast<std::size_t>(col)]);
    if (!value) return failure(Error::INVALID);
    f[static_cast<std::size_t>(col)] = *value;
  }
  if (f[5] != "true" && f[5] != "false") return failure(Error::INVALID);
  AdapterBinding binding{f[0], f[1], f[2], f[3], f[4], f[5] == "true"};
  auto receipt = read_receipt(f[6]);
  if (!binding_valid(binding) || !receipt || !*receipt ||
      (**receipt).generation_receipt_id != binding.generation_receipt_id ||
      (**receipt).adapter_descriptor_digest !=
          binding.adapter_descriptor_digest)
    return failure(Error::GENERATION_MISMATCH);
  return std::optional<AdapterBinding>{std::move(binding)};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

auto Transaction::insert_or_match_binding(const AuthorizedHttpSubmission& auth,
                                          const AdapterBinding& proposed)
    -> Result<AdapterBinding> try {
  db::OperationScope scope{stop, deadline};
  if (auto current = assert_current(); !current)
    return failure(current.error());
  if (!binding_valid(proposed) || proposed.retired)
    return failure(Error::INVALID);
  auto owner =
      query(connection(),
            "SELECT package_id::text FROM plinth.package_http_claim_receipts "
            "WHERE generation_receipt_id=$1::uuid",
            {proposed.generation_receipt_id});
  if (!tuples(owner, 1) || PQntuples(owner.get()) != 1)
    return failure(Error::GENERATION_MISMATCH);
  auto id = cell(owner, 0, 0, 36);
  if (!id) return failure(Error::INVALID);
  auto package = check_authorization(*this, auth, *id);
  if (!package) return failure(package.error());
  auto receipt = read_receipt(*id);
  auto reservation = read_reservation(*id);
  if (!receipt || !*receipt || !reservation || !*reservation ||
      !matches_authorization(**receipt, auth) ||
      (**receipt).adapter_descriptor_digest !=
          proposed.adapter_descriptor_digest ||
      (**reservation).generation_receipt_id != proposed.generation_receipt_id ||
      (**reservation).phase != ReservationPhase::HELD)
    return failure(Error::GENERATION_MISMATCH);
  auto existing = read_binding(proposed.adapter_epoch);
  if (!existing) return failure(existing.error());
  if (*existing) {
    if (**existing != proposed) return failure(Error::CONFLICT);
    return **existing;
  }
  if (package->state != "ACTIVATING")
    return failure(Error::GENERATION_MISMATCH);
  auto inserted =
      query(connection(),
            "INSERT INTO "
            "plinth.package_http_adapter_bindings(adapter_epoch,generation_"
            "receipt_id,owner_epoch,clock_domain_id,adapter_descriptor_digest) "
            "VALUES($1::uuid,$2::uuid,$3::uuid,$4::uuid,$5) RETURNING "
            "adapter_epoch::text",
            {proposed.adapter_epoch, proposed.generation_receipt_id,
             proposed.owner_epoch, proposed.clock_domain_id,
             proposed.adapter_descriptor_digest});
  if (!tuples(inserted, 1) || PQntuples(inserted.get()) != 1)
    return failure(Error::UNAVAILABLE);
  return proposed;
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

namespace {
struct TableDefinition {
  std::string_view name;
  std::string_view columns;
  std::string_view actual_foreign;
  std::string_view temporary_foreign;
};
constexpr std::array<TableDefinition, 3> TABLES{
    {{"package_http_claim_receipts", R"sql(
generation_receipt_id UUID PRIMARY KEY,
package_id UUID NOT NULL UNIQUE,
capture_profile INTEGER NOT NULL CHECK(capture_profile=1),
name TEXT NOT NULL CHECK(name COLLATE "C" ~ '^[a-z][a-z0-9-]{1,63}$'),
version TEXT NOT NULL CHECK(octet_length(version) BETWEEN 1 AND 256 AND version COLLATE "C" ~ '^[0-9A-Za-z.+-]+$'),
provenance TEXT NOT NULL CHECK(provenance IN ('user','bundled')),
schema SMALLINT NOT NULL CHECK(schema=1),
artifact_digest TEXT NOT NULL CHECK(artifact_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
registered_declarations_digest TEXT NOT NULL CHECK(registered_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
http_declarations_digest TEXT NOT NULL CHECK(http_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
adapter_descriptor_digest TEXT NOT NULL CHECK(adapter_descriptor_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
handler_mode TEXT NOT NULL CHECK(handler_mode IN ('quickjs','sidecar','bundled_native')),
http_prefixes TEXT[] NOT NULL CHECK(array_ndims(http_prefixes)=1 AND array_lower(http_prefixes,1)=1 AND cardinality(http_prefixes) BETWEEN 1 AND 64 AND array_position(http_prefixes,NULL) IS NULL AND octet_length(array_to_string(http_prefixes,E'\n'))<=16447),
unauthenticated_prefixes TEXT[] NOT NULL CHECK((cardinality(unauthenticated_prefixes)=0 OR (array_ndims(unauthenticated_prefixes)=1 AND array_lower(unauthenticated_prefixes,1)=1)) AND cardinality(unauthenticated_prefixes) BETWEEN 0 AND 64 AND array_position(unauthenticated_prefixes,NULL) IS NULL AND octet_length(array_to_string(unauthenticated_prefixes,E'\n'))<=16447 AND unauthenticated_prefixes <@ http_prefixes),
adapter_binding_key TEXT NOT NULL CHECK(octet_length(adapter_binding_key) BETWEEN 1 AND 256),
module_entry TEXT CHECK((handler_mode='quickjs' AND module_entry IS NOT NULL AND module_entry='server/http.js') OR (handler_mode<>'quickjs' AND module_entry IS NULL)),
UNIQUE(package_id,generation_receipt_id)
)sql",
      ",FOREIGN KEY(package_id,capture_profile) REFERENCES "
      "plinth.package_artifact_receipts(package_id,profile) ON DELETE RESTRICT",
      ",FOREIGN KEY(package_id,capture_profile) REFERENCES "
      "pg_temp.http_claims_expected_artifacts(package_id,profile) ON DELETE "
      "RESTRICT"},
     {"package_http_claim_reservations", R"sql(
package_id UUID PRIMARY KEY,
http_prefixes TEXT[] NOT NULL CHECK(array_ndims(http_prefixes)=1 AND array_lower(http_prefixes,1)=1 AND cardinality(http_prefixes) BETWEEN 1 AND 64 AND array_position(http_prefixes,NULL) IS NULL AND octet_length(array_to_string(http_prefixes,E'\n'))<=16447),
http_declarations_digest TEXT NOT NULL CHECK(http_declarations_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
generation_receipt_id UUID,
phase TEXT NOT NULL CHECK(phase IN ('HELD','RETIRING'))
)sql",
      ",FOREIGN KEY(package_id) REFERENCES plinth.packages(id) ON DELETE "
      "RESTRICT,FOREIGN KEY(package_id,generation_receipt_id) REFERENCES "
      "plinth.package_http_claim_receipts(package_id,generation_receipt_id) ON "
      "DELETE RESTRICT",
      ",FOREIGN KEY(package_id) REFERENCES "
      "pg_temp.http_claims_expected_packages(id) ON DELETE RESTRICT,FOREIGN "
      "KEY(package_id,generation_receipt_id) REFERENCES "
      "pg_temp.http_claims_expected_package_http_claim_receipts(package_id,"
      "generation_receipt_id) ON DELETE RESTRICT"},
     {"package_http_adapter_bindings", R"sql(
adapter_epoch UUID PRIMARY KEY,
generation_receipt_id UUID NOT NULL,
owner_epoch UUID NOT NULL,
clock_domain_id UUID NOT NULL,
adapter_descriptor_digest TEXT NOT NULL CHECK(adapter_descriptor_digest COLLATE "C" ~ '^[0-9a-f]{64}$'),
retired_at TIMESTAMPTZ
)sql",
      ",FOREIGN KEY(generation_receipt_id) REFERENCES "
      "plinth.package_http_claim_receipts(generation_receipt_id) ON DELETE "
      "RESTRICT",
      ",FOREIGN KEY(generation_receipt_id) REFERENCES "
      "pg_temp.http_claims_expected_package_http_claim_receipts(generation_"
      "receipt_id) ON DELETE RESTRICT"}}};

auto table_definition(const TableDefinition& table, bool temporary)
    -> std::string {
  return (temporary ? "CREATE TEMP TABLE http_claims_expected_"
                    : "CREATE TABLE IF NOT EXISTS plinth.") +
         std::string{table.name} + "(" + std::string{table.columns} +
         std::string{temporary ? table.temporary_foreign
                               : table.actual_foreign} +
         (temporary ? ") ON COMMIT DROP;" : ");");
}
auto schema_matches(PGconn& connection, std::string_view name) -> bool {
  const std::string actual = "plinth." + std::string{name};
  const std::string expected =
      "pg_temp.http_claims_expected_" + std::string{name};
  auto ownership = query(&connection, R"sql(
SELECT EXISTS(SELECT 1 FROM pg_class actual JOIN pg_class packages ON packages.oid='plinth.packages'::regclass
WHERE actual.oid=$1::regclass AND actual.relkind='r' AND actual.relpersistence='p' AND actual.relowner=packages.relowner
AND NOT actual.relrowsecurity AND NOT actual.relforcerowsecurity AND NOT actual.relispartition
AND NOT EXISTS(SELECT 1 FROM pg_inherits WHERE inhrelid=actual.oid OR inhparent=actual.oid)
AND NOT EXISTS(SELECT 1 FROM pg_rewrite WHERE ev_class=actual.oid)
AND NOT EXISTS(SELECT 1 FROM aclexplode(COALESCE(actual.relacl,acldefault('r',actual.relowner))) acl WHERE acl.grantee<>actual.relowner)
AND NOT EXISTS(SELECT 1 FROM pg_trigger WHERE tgrelid=actual.oid AND NOT tgisinternal)
AND NOT EXISTS(SELECT 1 FROM pg_constraint c WHERE c.conrelid=actual.oid AND c.contype='f' AND
 ((SELECT count(*) FROM pg_trigger t WHERE t.tgconstraint=c.oid)<>4 OR EXISTS
  (SELECT 1 FROM pg_trigger t WHERE t.tgconstraint=c.oid AND (NOT t.tgisinternal OR t.tgenabled NOT IN ('O','A')))))
AND NOT EXISTS(SELECT 1 FROM pg_constraint c JOIN pg_index i ON i.indexrelid=c.conindid
 WHERE c.conrelid=actual.oid AND c.contype IN ('p','u') AND
 (NOT i.indisvalid OR NOT i.indisready OR NOT i.indisunique OR NOT i.indimmediate OR i.indpred IS NOT NULL OR i.indexprs IS NOT NULL)))::text
)sql",
                         {actual});
  if (!tuples(ownership, 1) || PQntuples(ownership.get()) != 1 ||
      cell(ownership, 0, 0, 5) != "true")
    return false;
  auto columns = query(&connection, R"sql(
WITH columns AS (SELECT a.attrelid,a.attnum,a.attname,format_type(a.atttypid,a.atttypmod) AS type,a.attcollation,
a.attnotnull,a.attgenerated,a.attidentity,a.attisdropped,pg_get_expr(d.adbin,d.adrelid) AS default_value,a.attacl
FROM pg_attribute a LEFT JOIN pg_attrdef d ON d.adrelid=a.attrelid AND d.adnum=a.attnum
WHERE a.attnum>0 AND a.attrelid IN ($1::regclass,$2::regclass)),
actual AS (SELECT attnum,attname,type,attcollation,attnotnull,attgenerated,attidentity,attisdropped,default_value,attacl FROM columns WHERE attrelid=$1::regclass),
expected AS (SELECT attnum,attname,type,attcollation,attnotnull,attgenerated,attidentity,attisdropped,default_value,attacl FROM columns WHERE attrelid=$2::regclass)
(SELECT * FROM actual EXCEPT SELECT * FROM expected) UNION ALL (SELECT * FROM expected EXCEPT SELECT * FROM actual)
)sql",
                       {actual, expected});
  if (!columns || PQresultStatus(columns.get()) != PGRES_TUPLES_OK ||
      PQntuples(columns.get()) != 0)
    return false;
  auto constraints = query(&connection, R"sql(
WITH definitions AS (SELECT conrelid,contype,convalidated,condeferrable,condeferred,
CASE WHEN contype='f' THEN jsonb_build_object('keys',conkey,'foreign_keys',confkey,'delete',confdeltype,'update',confupdtype,'match',confmatchtype,'relation',
 CASE confrelid WHEN 'pg_temp.http_claims_expected_packages'::regclass THEN 'plinth.packages'::regclass
 WHEN 'pg_temp.http_claims_expected_artifacts'::regclass THEN 'plinth.package_artifact_receipts'::regclass
 WHEN 'pg_temp.http_claims_expected_package_http_claim_receipts'::regclass THEN 'plinth.package_http_claim_receipts'::regclass ELSE confrelid END)::text
ELSE pg_get_constraintdef(oid,true) END AS definition FROM pg_constraint WHERE conrelid IN ($1::regclass,$2::regclass)),
actual AS (SELECT contype,convalidated,condeferrable,condeferred,definition FROM definitions WHERE conrelid=$1::regclass),
expected AS (SELECT contype,convalidated,condeferrable,condeferred,definition FROM definitions WHERE conrelid=$2::regclass)
(SELECT * FROM actual EXCEPT SELECT * FROM expected) UNION ALL (SELECT * FROM expected EXCEPT SELECT * FROM actual)
)sql",
                           {actual, expected});
  return constraints && PQresultStatus(constraints.get()) == PGRES_TUPLES_OK &&
         PQntuples(constraints.get()) == 0;
}
} // namespace

auto schema_sql() -> std::string {
  std::string result;
  for (const auto& table : TABLES)
    result += table_definition(table, false);
  return result;
}
auto ensure_schema(PGconn& connection) -> Result<void> try {
  if (PQtransactionStatus(&connection) != PQTRANS_INTRANS)
    return failure(Error::TRANSACTION_REQUIRED);
  if (!command(&connection, schema_sql()) ||
      !command(&connection,
               "CREATE TEMP TABLE http_claims_expected_packages(id UUID "
               "PRIMARY KEY) ON COMMIT DROP") ||
      !command(&connection,
               "CREATE TEMP TABLE http_claims_expected_artifacts(package_id "
               "UUID NOT NULL,profile INTEGER NOT NULL,PRIMARY "
               "KEY(package_id,profile)) ON COMMIT DROP"))
    return failure(Error::SCHEMA_MISMATCH);
  for (const auto& table : TABLES)
    if (!command(&connection, table_definition(table, true)))
      return failure(Error::SCHEMA_MISMATCH);
  for (const auto& table : TABLES)
    if (!schema_matches(connection, table.name))
      return failure(Error::SCHEMA_MISMATCH);
  if (!command(
          &connection,
          "DROP TABLE "
          "pg_temp.http_claims_expected_package_http_adapter_bindings,pg_temp."
          "http_claims_expected_package_http_claim_reservations,pg_temp.http_"
          "claims_expected_package_http_claim_receipts,pg_temp.http_claims_"
          "expected_artifacts,pg_temp.http_claims_expected_packages"))
    return failure(Error::SCHEMA_MISMATCH);
  return {};
} catch (...) {
  return failure(Error::UNAVAILABLE);
}

} // namespace plinth::packages::http_claims
