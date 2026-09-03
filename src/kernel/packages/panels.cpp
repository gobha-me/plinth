#include "kernel/packages/panels.hpp"

#include <libpq-fe.h>

#include <array>
#include <memory>
#include <string>

namespace plinth::packages {

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
  std::string panel_type_s{panel_type_to_string(reg.panel_type)};
  std::string slot_type_s =
      reg.slot_type.has_value()
          ? std::string{slot_type_to_string(*reg.slot_type)}
          : std::string{};
  std::string declaration_s = reg.declaration.dump();

  std::array<const char*, 5> values = {
      reg.package_id.c_str(),
      reg.panel_id.c_str(),
      panel_type_s.c_str(),
      reg.slot_type.has_value() ? slot_type_s.c_str() : nullptr,
      declaration_s.c_str(),
  };

  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(&conn,
                   "INSERT INTO plinth.panels "
                   "(package_id, panel_id, panel_type, slot_type, declaration) "
                   "VALUES ($1::uuid, $2, $3, $4, $5::jsonb)",
                   5, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

} // namespace plinth::packages
