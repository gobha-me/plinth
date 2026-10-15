#include "kernel/packages/panels.hpp"
#include "kernel/db/operations.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace plinth::packages {

namespace {
using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto parse_order(std::string_view value, bool allow_zero = false)
    -> std::optional<std::uint64_t> {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      (!allow_zero && parsed == 0)) {
    return std::nullopt;
  }
  return parsed;
}
} // namespace

auto panel_type_to_string(PanelType t) -> std::string_view {
  switch (t) {
    case PanelType::PRIMARY: return "primary";
    case PanelType::FLOAT: return "float";
    case PanelType::SETTINGS: return "settings";
    case PanelType::TRAY: return "tray";
  }
  return "primary";
}

auto slot_type_to_string(SlotType s) -> std::string_view {
  switch (s) {
    case SlotType::HOME: return "home";
  }
  return "home";
}

auto register_panel(PGconn& conn, const PanelRegistration& reg)
    -> std::expected<void, std::string> {
  if (reg.logical_install_order && !parse_order(*reg.logical_install_order)) {
    return std::unexpected("handler order must be canonical positive uint64");
  }
  std::string panel_type_s{panel_type_to_string(reg.panel_type)};
  std::string slot_type_s =
      reg.slot_type.has_value()
          ? std::string{slot_type_to_string(*reg.slot_type)}
          : std::string{};
  std::string declaration_s = reg.declaration.dump();
  const auto format_s = std::to_string(reg.validated_format_version);

  std::array<const char*, 7> values = {
      reg.package_id.c_str(),
      reg.panel_id.c_str(),
      panel_type_s.c_str(),
      reg.slot_type.has_value() ? slot_type_s.c_str() : nullptr,
      declaration_s.c_str(),
      format_s.c_str(),
      reg.logical_install_order ? reg.logical_install_order->c_str() : nullptr,
  };

  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      plinth::db::exec_params(
          &conn,
          "INSERT INTO plinth.panels "
          "(package_id, panel_id, panel_type, slot_type, declaration, "
          "validated_format_version, logical_install_order) "
          "VALUES ($1::uuid, $2, $3, $4, $5::jsonb, $6::smallint, $7::numeric)",
          7, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto finalize_panel_install_order(PGconn& conn, std::string_view package_id)
    -> std::expected<void, std::string> {
  if (PQtransactionStatus(&conn) != PQTRANS_INTRANS) {
    return std::unexpected("panel order requires an owned ACTIVE transaction");
  }
  const std::string id{package_id};
  std::array<const char*, 1> id_values{id.c_str()};
  PgResultPtr package(
      plinth::db::exec_params(
          &conn,
          "SELECT p.state, COALESCE(p.supersedes_id::text,''), p.name, "
          "old.name "
          "FROM plinth.packages p LEFT JOIN plinth.packages old "
          "ON old.id=p.supersedes_id WHERE p.id=$1::uuid FOR UPDATE OF p",
          1, nullptr, id_values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(package.get()) != PGRES_TUPLES_OK ||
      PQntuples(package.get()) != 1) {
    return std::unexpected("panel order package identity unavailable");
  }
  const std::string state{PQgetvalue(package.get(), 0, 0)};
  const std::string predecessor{PQgetvalue(package.get(), 0, 1)};
  std::array<const char*, 2> query_values{id.c_str(), predecessor.c_str()};
  PgResultPtr panels(
      plinth::db::exec_params(
          &conn,
          "SELECT pn.panel_id, pn.logical_install_order::text, "
          "old.logical_install_order::text, old.panel_id "
          "FROM plinth.panels pn LEFT JOIN plinth.panels old "
          "ON old.package_id=NULLIF($2,'')::uuid AND old.panel_id=pn.panel_id "
          "AND old.validated_format_version=2 AND old.panel_type='float' "
          "WHERE pn.package_id=$1::uuid AND pn.validated_format_version=2 "
          "AND pn.panel_type='float' ORDER BY pn.panel_id COLLATE \"C\" "
          "LIMIT 257 FOR UPDATE OF pn",
          2, nullptr, query_values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(panels.get()) != PGRES_TUPLES_OK ||
      PQntuples(panels.get()) > 256) {
    return std::unexpected("typed float order inventory unavailable");
  }
  if (PQntuples(panels.get()) == 0) {
    return {};
  }
  if (!predecessor.empty() &&
      (PQgetisnull(package.get(), 0, 3) != 0 ||
       std::string_view{PQgetvalue(package.get(), 0, 2)} !=
           PQgetvalue(package.get(), 0, 3))) {
    return std::unexpected("panel order predecessor identity mismatch");
  }
  struct Assignment {
    std::string panel_id;
    std::optional<std::uint64_t> rank;
  };
  std::vector<Assignment> assignments;
  std::uint64_t new_count = 0;
  std::uint64_t greatest_existing = 0;
  for (int row = 0; row < PQntuples(panels.get()); ++row) {
    if (PQgetisnull(panels.get(), row, 1) == 0) {
      auto rank = parse_order(PQgetvalue(panels.get(), row, 1));
      if (!rank) {
        return std::unexpected("invalid installed handler order");
      }
      greatest_existing = std::max(greatest_existing, *rank);
      continue;
    }
    if (state != "EXTRACTING" && state != "ACTIVATING") {
      return std::unexpected("installed typed float is missing its order");
    }
    std::optional<std::uint64_t> inherited;
    if (PQgetisnull(panels.get(), row, 3) == 0) {
      if (PQgetisnull(panels.get(), row, 2) != 0 ||
          !(inherited = parse_order(PQgetvalue(panels.get(), row, 2)))) {
        return std::unexpected("predecessor typed float is missing its order");
      }
      greatest_existing = std::max(greatest_existing, *inherited);
    } else {
      ++new_count;
    }
    assignments.push_back(
        {.panel_id = PQgetvalue(panels.get(), row, 0), .rank = inherited});
  }
  std::uint64_t last = 0;
  if (PQntuples(panels.get()) != 0) {
    // This singleton serializes otherwise unrelated installations. Bound
    // only its lock acquisition, retaining a shorter configured timeout and
    // the caller's existing cancellation/statement deadline unchanged.
    PgResultPtr timeout(
        plinth::db::exec(
            &conn, "SELECT setting, current_setting('lock_timeout') "
                   "FROM pg_catalog.pg_settings WHERE name='lock_timeout'"),
        PQclear);
    if (PQresultStatus(timeout.get()) != PGRES_TUPLES_OK ||
        PQntuples(timeout.get()) != 1 ||
        PQgetisnull(timeout.get(), 0, 0) != 0 ||
        PQgetisnull(timeout.get(), 0, 1) != 0) {
      return std::unexpected("handler order lock timeout unavailable");
    }
    const auto configured_ms =
        parse_order(PQgetvalue(timeout.get(), 0, 0), true);
    if (!configured_ms) {
      return std::unexpected("handler order lock timeout invalid");
    }
    const std::string previous_timeout{PQgetvalue(timeout.get(), 0, 1)};
    constexpr std::uint64_t maximum_wait_ms = 10000;
    const auto wait_ms = *configured_ms == 0
                             ? maximum_wait_ms
                             : std::min(*configured_ms, maximum_wait_ms);
    const auto bounded_timeout = std::to_string(wait_ms) + "ms";
    std::array<const char*, 1> timeout_values{bounded_timeout.c_str()};
    PgResultPtr bounded(
        plinth::db::exec_params(
            &conn, "SELECT pg_catalog.set_config('lock_timeout',$1,true)", 1,
            nullptr, timeout_values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(bounded.get()) != PGRES_TUPLES_OK ||
        PQntuples(bounded.get()) != 1) {
      return std::unexpected("handler order lock timeout setup failed");
    }
    PgResultPtr allocator(
        plinth::db::exec(&conn, "SELECT last_value::text FROM "
                                "plinth.handler_install_order_allocator "
                                "WHERE id=1 FOR UPDATE"),
        PQclear);
    if (PQresultStatus(allocator.get()) != PGRES_TUPLES_OK) {
      // The transaction owner rolls back any failed acquisition. Do not
      // retry or issue restoration SQL in an aborted/cancelled transaction.
      return std::unexpected("handler order allocator unavailable");
    }
    timeout_values[0] = previous_timeout.c_str();
    PgResultPtr restored(
        plinth::db::exec_params(
            &conn, "SELECT pg_catalog.set_config('lock_timeout',$1,true)", 1,
            nullptr, timeout_values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(restored.get()) != PGRES_TUPLES_OK ||
        PQntuples(restored.get()) != 1) {
      return std::unexpected("handler order lock timeout restoration failed");
    }
    // Restoration changes policy only; the allocator row remains locked
    // until the enclosing ACTIVE transaction commits or rolls back.
    if (PQntuples(allocator.get()) != 1) {
      return std::unexpected("handler order allocator unavailable");
    }
    auto parsed = parse_order(PQgetvalue(allocator.get(), 0, 0), true);
    if (!parsed || *parsed < greatest_existing ||
        new_count > std::numeric_limits<std::uint64_t>::max() - *parsed) {
      return std::unexpected("handler order allocator invalid or exhausted");
    }
    last = *parsed;
    if (new_count != 0) {
      const auto upper = std::to_string(last + new_count);
      std::array<const char*, 1> value{upper.c_str()};
      PgResultPtr updated(
          plinth::db::exec_params(
              &conn,
              "UPDATE plinth.handler_install_order_allocator SET "
              "last_value=$1::numeric WHERE id=1",
              1, nullptr, value.data(), nullptr, nullptr, 0),
          PQclear);
      if (PQresultStatus(updated.get()) != PGRES_COMMAND_OK ||
          std::string_view{PQcmdTuples(updated.get())} != "1") {
        return std::unexpected("handler order allocator update failed");
      }
    }
  }
  for (const auto& assignment : assignments) {
    const auto rank =
        std::to_string(assignment.rank ? *assignment.rank : ++last);
    std::array<const char*, 3> values{id.c_str(), assignment.panel_id.c_str(),
                                      rank.c_str()};
    PgResultPtr updated(
        plinth::db::exec_params(
            &conn,
            "UPDATE plinth.panels SET logical_install_order=$3::numeric "
            "WHERE package_id=$1::uuid AND panel_id=$2 "
            "AND logical_install_order IS NULL AND validated_format_version=2 "
            "AND panel_type='float'",
            3, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(updated.get()) != PGRES_COMMAND_OK ||
        std::string_view{PQcmdTuples(updated.get())} != "1") {
      return std::unexpected("handler order assignment failed");
    }
  }
  return {};
}

} // namespace plinth::packages
