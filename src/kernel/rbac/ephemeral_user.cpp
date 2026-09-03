#include "kernel/rbac/ephemeral_user.hpp"

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::rbac {

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

// Syntactically-valid argon2id token that verifies against no password.
// Ephemeral users synthesise their `UserContext` inside the kernel and
// never traverse the HTTP login path; the placeholder exists only to
// satisfy the `password_hash TEXT NOT NULL` schema constraint.
constexpr auto PLACEHOLDER_HASH =
    "$argon2id$v=19$m=65536,t=3,p=4$placeholder$placeholder";

auto format_utc_iso(std::chrono::system_clock::time_point tp) -> std::string {
  auto t = std::chrono::system_clock::to_time_t(tp);
  std::tm gm{};
  (void)::gmtime_r(&t, &gm);
  std::array<char, 32> buf{};
  (void)std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S+00", &gm);
  return std::string{buf.data()};
}

} // namespace

auto create_run_users(std::string_view run_id, PGconn& conn)
    -> std::expected<RunUserPair, std::string> {
  std::string run_s{run_id};
  std::string denied_user = "__test_denied_" + run_s;
  std::string allowed_user = "__test_allowed_" + run_s;
  std::string group_name = "__rbac_test_" + run_s;

  // denied user
  {
    std::array<const char*, 2> values = {denied_user.c_str(), PLACEHOLDER_HASH};
    PgResultPtr res(PQexecParams(&conn,
                                 "INSERT INTO plinth.users "
                                 "(username, password_hash, is_test_user) "
                                 "VALUES ($1, $2, true) RETURNING id",
                                 2, nullptr, values.data(), nullptr, nullptr,
                                 0),
                    PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // allowed user
  {
    std::array<const char*, 2> values = {allowed_user.c_str(),
                                         PLACEHOLDER_HASH};
    PgResultPtr res(PQexecParams(&conn,
                                 "INSERT INTO plinth.users "
                                 "(username, password_hash, is_test_user) "
                                 "VALUES ($1, $2, true) RETURNING id",
                                 2, nullptr, values.data(), nullptr, nullptr,
                                 0),
                    PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // synthetic group
  {
    std::array<const char*, 1> values = {group_name.c_str()};
    PgResultPtr res(
        PQexecParams(&conn,
                     "INSERT INTO plinth.groups "
                     "(name, description, built_in) "
                     "VALUES ($1, 'RBAC test group; auto-cleaned', false) "
                     "RETURNING id",
                     1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // everyone membership for both users
  {
    std::array<const char*, 2> values = {denied_user.c_str(),
                                         allowed_user.c_str()};
    PgResultPtr res(
        PQexecParams(&conn,
                     "INSERT INTO plinth.group_members (group_id, user_id) "
                     "SELECT g.id, u.id "
                     "FROM plinth.groups g, plinth.users u "
                     "WHERE g.name = 'everyone' "
                     "  AND u.username IN ($1, $2)",
                     2, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // allowed user → synthetic group membership
  {
    std::array<const char*, 2> values = {group_name.c_str(),
                                         allowed_user.c_str()};
    PgResultPtr res(
        PQexecParams(&conn,
                     "INSERT INTO plinth.group_members (group_id, user_id) "
                     "SELECT g.id, u.id "
                     "FROM plinth.groups g, plinth.users u "
                     "WHERE g.name     = $1 "
                     "  AND u.username = $2",
                     2, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // Re-read the generated IDs. Doing this once at the end (rather
  // than threading RETURNING through each INSERT) keeps the happy
  // path simple; an extra SELECT costs ~1ms on a local PG.
  RunUserPair pair;
  pair.run_id = run_s;
  pair.denied_username = denied_user;
  pair.allowed_username = allowed_user;
  pair.allowed_group_name = group_name;
  std::array<const char*, 3> values = {
      denied_user.c_str(), allowed_user.c_str(), group_name.c_str()};
  PgResultPtr res(
      PQexecParams(
          &conn,
          "SELECT "
          "  (SELECT id::text FROM plinth.users  WHERE username = $1) AS du, "
          "  (SELECT id::text FROM plinth.users  WHERE username = $2) AS au, "
          "  (SELECT id::text FROM plinth.groups WHERE name     = $3) AS g",
          3, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) != 1) {
    return std::unexpected(std::string{"post-create ID readback failed"});
  }
  pair.denied_user_id = PQgetvalue(res.get(), 0, 0);
  pair.allowed_user_id = PQgetvalue(res.get(), 0, 1);
  pair.allowed_group_id = PQgetvalue(res.get(), 0, 2);
  return pair;
}

auto grant_rule_to_run_group(std::string_view run_id, std::string_view rule,
                             PGconn& conn) -> std::expected<void, std::string> {
  std::string group_name = "__rbac_test_" + std::string{run_id};
  std::string rule_s{rule};
  std::array<const char*, 2> values = {group_name.c_str(), rule_s.c_str()};
  PgResultPtr res(
      PQexecParams(&conn,
                   "INSERT INTO plinth.group_rules (group_id, rule_id) "
                   "SELECT g.id, r.id "
                   "FROM plinth.groups g, plinth.rbac_rules r "
                   "WHERE g.name = $1 AND r.rule = $2 "
                   "ON CONFLICT (group_id, rule_id) DO NOTHING",
                   2, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto revoke_rule_from_run_group(std::string_view run_id, std::string_view rule,
                                PGconn& conn)
    -> std::expected<void, std::string> {
  std::string group_name = "__rbac_test_" + std::string{run_id};
  std::string rule_s{rule};
  std::array<const char*, 2> values = {group_name.c_str(), rule_s.c_str()};
  PgResultPtr res(
      PQexecParams(
          &conn,
          "DELETE FROM plinth.group_rules "
          "WHERE group_id = (SELECT id FROM plinth.groups WHERE name = $1) "
          "  AND rule_id  = (SELECT id FROM plinth.rbac_rules WHERE rule = $2)",
          2, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto destroy_run_users(std::string_view run_id, PGconn& conn)
    -> std::expected<void, std::string> {
  std::string run_s{run_id};
  std::string denied_user = "__test_denied_" + run_s;
  std::string allowed_user = "__test_allowed_" + run_s;
  std::string group_name = "__rbac_test_" + run_s;

  // Dependency order: group_rules → group_members → users → groups.
  // Every step is idempotent; missing rows are no-ops.
  {
    std::array<const char*, 1> values = {group_name.c_str()};
    PgResultPtr res(
        PQexecParams(
            &conn,
            "DELETE FROM plinth.group_rules "
            "WHERE group_id = (SELECT id FROM plinth.groups WHERE name = $1)",
            1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  {
    std::array<const char*, 3> values = {
        denied_user.c_str(), allowed_user.c_str(), group_name.c_str()};
    PgResultPtr res(
        PQexecParams(
            &conn,
            "DELETE FROM plinth.group_members "
            "WHERE user_id IN (SELECT id FROM plinth.users "
            "                  WHERE username IN ($1, $2)) "
            "   OR group_id = (SELECT id FROM plinth.groups WHERE name = $3)",
            3, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  {
    std::array<const char*, 2> values = {denied_user.c_str(),
                                         allowed_user.c_str()};
    PgResultPtr res(
        PQexecParams(&conn,
                     "DELETE FROM plinth.users WHERE username IN ($1, $2)", 2,
                     nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  {
    std::array<const char*, 1> values = {group_name.c_str()};
    PgResultPtr res(PQexecParams(&conn,
                                 "DELETE FROM plinth.groups WHERE name = $1", 1,
                                 nullptr, values.data(), nullptr, nullptr, 0),
                    PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  return {};
}

auto cleanup_orphaned_test_users(
    std::chrono::system_clock::time_point older_than, PGconn& conn)
    -> std::expected<std::size_t, std::string> {
  std::string cutoff = format_utc_iso(older_than);

  // Bulk cleanup: one statement per FK layer. Counts rows removed
  // from `plinth.groups` (one per RBAC test run) as the "runs cleaned".
  // group_rules are removed by cascade-of-intent (we delete rules
  // whose group matches); group_members are removed by the user OR
  // group predicate; users are filtered by is_test_user+cutoff; test
  // groups are filtered by name-prefix + created_at.

  std::array<const char*, 1> values = {cutoff.c_str()};

  // 1) group_rules for test groups older than cutoff
  {
    PgResultPtr res(PQexecParams(&conn,
                                 "DELETE FROM plinth.group_rules "
                                 "WHERE group_id IN ( "
                                 "  SELECT id FROM plinth.groups "
                                 "  WHERE starts_with(name, '__rbac_test_') "
                                 "    AND created_at < $1::timestamptz)",
                                 1, nullptr, values.data(), nullptr, nullptr,
                                 0),
                    PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // 2) group_members for test users OR test groups older than cutoff
  {
    PgResultPtr res(
        PQexecParams(
            &conn,
            "DELETE FROM plinth.group_members "
            "WHERE user_id IN ( "
            "    SELECT id FROM plinth.users "
            "    WHERE is_test_user = true AND created_at < $1::timestamptz) "
            "   OR group_id IN ( "
            "    SELECT id FROM plinth.groups "
            "    WHERE starts_with(name, '__rbac_test_') "
            "      AND created_at < $1::timestamptz)",
            1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // 3) test users older than cutoff
  {
    PgResultPtr res(
        PQexecParams(
            &conn,
            "DELETE FROM plinth.users "
            "WHERE is_test_user = true AND created_at < $1::timestamptz",
            1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(res.get())});
    }
  }
  // 4) test groups older than cutoff — count these for the return.
  PgResultPtr res(PQexecParams(&conn,
                               "DELETE FROM plinth.groups "
                               "WHERE starts_with(name, '__rbac_test_') "
                               "  AND created_at < $1::timestamptz",
                               1, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  const char* tuples = PQcmdTuples(res.get());
  return tuples != nullptr ? std::stoul(tuples) : 0UL;
}

auto build_test_user_context(std::string_view username,
                             std::string_view user_id,
                             std::vector<std::string> effective_rules)
    -> plinth::capabilities::UserContext {
  return plinth::capabilities::UserContext{
      .user_id = std::string{user_id},
      .username = std::string{username},
      .auth_type = "test",
      .effective_rules = std::move(effective_rules),
      .session_id = "",
      .ip_address = "",
  };
}

} // namespace plinth::rbac
