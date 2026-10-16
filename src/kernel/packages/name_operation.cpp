#include "kernel/packages/name_operation.hpp"

#include "kernel/db/operations.hpp"

#include <array>
#include <cstring>
#include <new>
#include <stdexcept>
#include <utility>

namespace plinth::packages::name_operation {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto valid_name(std::string_view name) -> bool {
  return !name.empty() && name.size() <= 64 &&
         name.find('\0') == std::string_view::npos;
}

auto assert_seed_owned(PGconn* connection, const std::string& seed)
    -> std::expected<void, NameError> {
  if (connection == nullptr || PQstatus(connection) != CONNECTION_OK)
    return std::unexpected(NameError::UNAVAILABLE);
  const auto state = PQtransactionStatus(connection);
  if (state != PQTRANS_IDLE && state != PQTRANS_INTRANS)
    return std::unexpected(NameError::UNAVAILABLE);
  const char* value = seed.c_str();
  // Same bigint key decomposition used by the lifecycle RBAC handoff tests.
  Result result(
      plinth::db::exec_params(
          connection,
          "WITH target AS (SELECT hashtextextended($1,0) AS key) "
          "SELECT EXISTS(SELECT 1 FROM pg_locks locks CROSS JOIN target "
          "WHERE locks.locktype='advisory' AND locks.granted "
          "AND locks.mode='ExclusiveLock' AND locks.objsubid=1 "
          "AND locks.database=(SELECT oid FROM pg_database "
          "WHERE datname=current_database()) "
          "AND locks.classid=((target.key >> 32) & 4294967295)::oid "
          "AND locks.objid=(target.key & 4294967295)::oid "
          "AND locks.pid=pg_backend_pid())",
          1, nullptr, &value, nullptr, nullptr, 0),
      PQclear);
  if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1 ||
      PQgetisnull(result.get(), 0, 0) != 0)
    return std::unexpected(NameError::UNAVAILABLE);
  if (std::strcmp(PQgetvalue(result.get(), 0, 0), "t") != 0)
    return std::unexpected(NameError::NOT_OWNED);
  return {};
}
} // namespace

auto lock_key(std::string_view name) -> std::string {
  return std::string{"plinth.packages."} + std::string{name};
}
auto try_acquire_seed(PGconn* connection, const std::string& seed)
    -> std::expected<void, std::string> {
  std::array<const char*, 1> values = {seed.c_str()};
  Result result(plinth::db::exec_params(
                    connection,
                    "SELECT pg_try_advisory_lock(hashtextextended($1, 0))", 1,
                    nullptr, values.data(), nullptr, nullptr, 0),
                PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK)
    return std::unexpected(std::string{PQresultErrorMessage(result.get())});
  if (PQntuples(result.get()) == 0)
    return std::unexpected(std::string{"advisory-lock probe returned no rows"});
  const char* got = PQgetvalue(result.get(), 0, 0);
  if (got == nullptr || std::strcmp(got, "t") != 0)
    return std::unexpected(std::string{"advisory-lock-held"});
  return {};
}
auto release_seed(PGconn* connection, const std::string& seed) -> void {
  std::array<const char*, 1> values = {seed.c_str()};
  Result result(plinth::db::exec_params(
                    connection,
                    "SELECT pg_advisory_unlock(hashtextextended($1, 0))", 1,
                    nullptr, values.data(), nullptr, nullptr, 0),
                PQclear);
  (void)result;
}
auto release_seed_checked(PGconn* connection, const std::string& seed)
    -> std::expected<void, std::string> {
  const char* value = seed.c_str();
  Result result(plinth::db::exec_params(
                    connection,
                    "SELECT pg_advisory_unlock(hashtextextended($1, 0))", 1,
                    nullptr, &value, nullptr, nullptr, 0),
                PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK)
    return std::unexpected(std::string{PQresultErrorMessage(result.get())});
  if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1 ||
      PQgetisnull(result.get(), 0, 0) != 0 ||
      std::strcmp(PQgetvalue(result.get(), 0, 0), "t") != 0)
    return std::unexpected("package name lock release was not acknowledged");
  return {};
}

NameOperation::NameOperation(PGconn* connection, std::shared_ptr<PGconn> owner,
                             std::string name, std::string lock_seed,
                             bool release)
    : retained(std::move(owner)), session(connection),
      package_name(std::move(name)), seed(std::move(lock_seed)),
      releases(release) {
}
NameOperation::~NameOperation() {
  release_on_destroy();
}
NameOperation::NameOperation(NameOperation&& other) noexcept
    : retained(std::move(other.retained)),
      session(std::exchange(other.session, nullptr)),
      package_name(std::move(other.package_name)), seed(std::move(other.seed)),
      releases(std::exchange(other.releases, false)) {
}
auto NameOperation::operator=(NameOperation&& other) noexcept
    -> NameOperation& {
  if (this != &other) {
    release_on_destroy();
    retained = std::move(other.retained);
    session = std::exchange(other.session, nullptr);
    package_name = std::move(other.package_name);
    seed = std::move(other.seed);
    releases = std::exchange(other.releases, false);
  }
  return *this;
}
auto NameOperation::release_on_destroy() noexcept -> void {
  if (releases && session != nullptr) release_seed(session, seed);
  releases = false;
}
auto NameOperation::acquire(std::shared_ptr<PGconn> connection,
                            std::string name)
    -> std::expected<NameOperation, NameError> {
  if (!valid_name(name)) return std::unexpected(NameError::INVALID);
  if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
    return std::unexpected(NameError::UNAVAILABLE);
  try {
    auto seed = lock_key(name);
    auto existing = assert_seed_owned(connection.get(), seed);
    if (existing) return std::unexpected(NameError::INVALID);
    if (existing.error() != NameError::NOT_OWNED)
      return std::unexpected(existing.error());
    auto acquired = try_acquire_seed(connection.get(), seed);
    if (!acquired)
      return std::unexpected(acquired.error() == "advisory-lock-held"
                                 ? NameError::BUSY
                                 : NameError::UNAVAILABLE);
    auto* session = connection.get();
    return NameOperation(session, std::move(connection), std::move(name),
                         std::move(seed), true);
  } catch (const std::bad_alloc&) {
    return std::unexpected(NameError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(NameError::LIMIT_EXCEEDED);
  }
}
auto NameOperation::borrow_existing(PGconn& connection, std::string name)
    -> std::expected<NameOperation, NameError> {
  if (!valid_name(name)) return std::unexpected(NameError::INVALID);
  try {
    auto seed = lock_key(name);
    if (auto owned = assert_seed_owned(&connection, seed); !owned)
      return std::unexpected(owned.error());
    return NameOperation(&connection, {}, std::move(name), std::move(seed),
                         false);
  } catch (const std::bad_alloc&) {
    return std::unexpected(NameError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(NameError::LIMIT_EXCEEDED);
  }
}
auto NameOperation::assert_owned() const -> std::expected<void, NameError> {
  return assert_seed_owned(session, seed);
}
auto NameOperation::release_checked() -> std::expected<void, NameError> {
  if (!releases || session == nullptr)
    return std::unexpected(NameError::NOT_OWNED);
  if (auto released = release_seed_checked(session, seed); !released)
    return std::unexpected(NameError::UNAVAILABLE);
  releases = false;
  session = nullptr;
  retained.reset();
  return {};
}

} // namespace plinth::packages::name_operation
