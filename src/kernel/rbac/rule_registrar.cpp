#include "kernel/rbac/rule_registrar.hpp"

#include <libpq-fe.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::rbac {

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

// Helper: run a single UPDATE/DELETE against rbac_rules filtered by
// extension_name. Returns rows affected on success.
auto exec_rule_mutation(PGconn& conn, const char* sql,
                        std::string_view extension_name)
    -> std::expected<std::size_t, std::string> {
  std::string ext_s{extension_name};
  std::array<const char*, 1> values = {ext_s.c_str()};
  PgResultPtr res(
      PQexecParams(&conn, sql, 1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  const char* tuples = PQcmdTuples(res.get());
  return tuples != nullptr ? std::stoul(tuples) : 0UL;
}

} // namespace

auto upsert_extension_rule(PGconn& conn, std::string_view rule,
                           std::string_view namespace_,
                           std::string_view description,
                           std::string_view extension_name,
                           std::optional<nlohmann::json> test_contract)
    -> std::expected<void, std::string> {
  // `std::string` copies ensure null-terminated C strings for libpq
  // (string_view is not guaranteed null-terminated).
  std::string rule_s{rule};
  std::string ns_s{namespace_};
  std::string desc_s{description};
  std::string ext_s{extension_name};
  std::string tc_s;
  const char* tc_ptr = nullptr;
  if (test_contract.has_value()) {
    tc_s = test_contract->dump();
    tc_ptr = tc_s.c_str();
  }
  std::array<const char*, 5> values = {
      rule_s.c_str(), ns_s.c_str(), desc_s.c_str(), ext_s.c_str(), tc_ptr,
  };
  PgResultPtr res(
      PQexecParams(
          &conn,
          "INSERT INTO plinth.rbac_rules "
          "(rule, namespace, description, extension_name, test_contract) "
          "VALUES ($1, $2, $3, $4, $5::jsonb) "
          "ON CONFLICT (rule) DO UPDATE SET "
          "  namespace      = EXCLUDED.namespace, "
          "  description    = EXCLUDED.description, "
          "  extension_name = EXCLUDED.extension_name, "
          "  test_contract  = EXCLUDED.test_contract, "
          "  orphaned_at    = NULL",
          5, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto mark_extension_rules_orphaned(std::string_view extension_name,
                                   PGconn& conn)
    -> std::expected<std::size_t, std::string> {
  return exec_rule_mutation(conn,
                            "UPDATE plinth.rbac_rules SET orphaned_at = NOW() "
                            "WHERE extension_name = $1 AND orphaned_at IS NULL",
                            extension_name);
}

auto clear_extension_rules_orphaned(std::string_view extension_name,
                                    PGconn& conn)
    -> std::expected<std::size_t, std::string> {
  return exec_rule_mutation(conn,
                            "UPDATE plinth.rbac_rules SET orphaned_at = NULL "
                            "WHERE extension_name = $1",
                            extension_name);
}

auto delete_extension_rules(std::string_view extension_name, PGconn& conn)
    -> std::expected<std::size_t, std::string> {
  return exec_rule_mutation(
      conn, "DELETE FROM plinth.rbac_rules WHERE extension_name = $1",
      extension_name);
}

auto fetch_extension_rules_for_rbac_test(std::string_view extension_name,
                                         PGconn& conn)
    -> std::expected<std::vector<RbacTestRule>, std::string> {
  std::string ext_s{extension_name};
  std::array<const char*, 1> values = {ext_s.c_str()};
  PgResultPtr res(
      PQexecParams(&conn,
                   "SELECT rule, namespace, extension_name, test_contract "
                   "FROM plinth.rbac_rules "
                   "WHERE extension_name = $1 "
                   "ORDER BY id ASC",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  std::vector<RbacTestRule> out;
  int n = PQntuples(res.get());
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    RbacTestRule r;
    r.rule = PQgetvalue(res.get(), i, 0);
    r.namespace_ = PQgetvalue(res.get(), i, 1);
    r.extension_name = PQgetvalue(res.get(), i, 2);
    if (PQgetisnull(res.get(), i, 3) == 0) {
      try {
        r.test_contract = nlohmann::json::parse(PQgetvalue(res.get(), i, 3));
      } catch (const nlohmann::json::parse_error& e) {
        return std::unexpected(
            std::string{"test_contract JSON parse for rule "} + r.rule + ": " +
            e.what());
      }
    }
    out.push_back(std::move(r));
  }
  return out;
}

} // namespace plinth::rbac
