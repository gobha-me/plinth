// Unit-level tests for 0.4.4 pure helpers. Broader coverage (fixture
// packages, HTTP round-trips, crash recovery) lands alongside the PG-
// gated suite in a follow-up commit.

#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/panels.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace plinth::packages;

TEST_CASE("provenance_to_string covers both values", "[install_lifecycle]") {
  REQUIRE(provenance_to_string(Provenance::USER) == "user");
  REQUIRE(provenance_to_string(Provenance::BUNDLED) == "bundled");
}

TEST_CASE("stage_to_string round-trips every state", "[install_lifecycle]") {
  // Every value the schema CHECK permits must have a printable form so
  // audit events + `last_install_report` emit something useful. 0.4.5
  // adds DISABLED / UNINSTALLING / SUPERSEDED as writers; ACTIVE_FLAGGED
  // is carried for decode (0.4.7 RBAC test writes).
  REQUIRE(stage_to_string(InstallStage::UPLOADING) == "UPLOADING");
  REQUIRE(stage_to_string(InstallStage::VALIDATING) == "VALIDATING");
  REQUIRE(stage_to_string(InstallStage::MIGRATING) == "MIGRATING");
  REQUIRE(stage_to_string(InstallStage::REGISTERING) == "REGISTERING");
  REQUIRE(stage_to_string(InstallStage::EXTRACTING) == "EXTRACTING");
  REQUIRE(stage_to_string(InstallStage::ACTIVATING) == "ACTIVATING");
  REQUIRE(stage_to_string(InstallStage::ACTIVE) == "ACTIVE");
  REQUIRE(stage_to_string(InstallStage::ACTIVE_FLAGGED) == "ACTIVE_FLAGGED");
  REQUIRE(stage_to_string(InstallStage::DISABLED) == "DISABLED");
  REQUIRE(stage_to_string(InstallStage::INSTALL_FAILED) == "INSTALL_FAILED");
  REQUIRE(stage_to_string(InstallStage::UNINSTALLING) == "UNINSTALLING");
  REQUIRE(stage_to_string(InstallStage::SUPERSEDED) == "SUPERSEDED");
}

TEST_CASE("panel_type_to_string covers every panel kind",
          "[install_lifecycle][panels]") {
  REQUIRE(panel_type_to_string(PanelType::PRIMARY) == "primary");
  REQUIRE(panel_type_to_string(PanelType::FLOAT) == "float");
  REQUIRE(panel_type_to_string(PanelType::SETTINGS) == "settings");
  REQUIRE(panel_type_to_string(PanelType::TRAY) == "tray");
}

TEST_CASE("slot_type_to_string covers home", "[install_lifecycle][panels]") {
  REQUIRE(slot_type_to_string(SlotType::HOME) == "home");
}
