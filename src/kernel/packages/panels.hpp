#pragma once

// plinth::packages::panels — plinth.panels CRUD (ICD-0.4.4 §REGISTERING).
//
// One row per panel declared by an installed package. Rows CASCADE-
// delete when the owning plinth.packages row is removed (0.4.5's
// uninstall path). 0.4.4 only inserts on install; the shell (0.6.x)
// will query this table via a kernel capability.
//
// Synchronous libpq — install lifecycle owns the connection and wraps
// the register call in its REGISTERING transaction.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

struct pg_conn;
using PGconn = pg_conn;

namespace plinth::packages {

enum class PanelType : std::uint8_t { PRIMARY, FLOAT, SETTINGS, TRAY };
enum class SlotType : std::uint8_t { HOME };

struct PanelRegistration {
  std::string package_id; // UUID as string
  std::string panel_id;   // unique within the package
  PanelType panel_type;
  std::optional<SlotType> slot_type;
  nlohmann::json declaration; // arbitrary per-panel JSON from panels.json
};

// Map enum → DB-CHECK string token.
[[nodiscard]] auto panel_type_to_string(PanelType t) -> std::string_view;
[[nodiscard]] auto slot_type_to_string(SlotType s) -> std::string_view;

// INSERT one (package_id, panel_id, ...) row. Returns an error string
// on PG failure (including PRIMARY KEY violation).
auto register_panel(PGconn& conn, const PanelRegistration& reg)
    -> std::expected<void, std::string>;

} // namespace plinth::packages
