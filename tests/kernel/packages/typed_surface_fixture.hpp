#pragma once

// Real-package fixture builder: retain the existing valid package's code,
// migrations, capabilities and RBAC; change only explicit panel metadata and
// package version. No install/validation/readiness behavior is simulated here.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kernel/capabilities/drain.hpp"

namespace plinth::typed_surface_test {

struct NotesFenceCleanup {
  ~NotesFenceCleanup() { plinth::capabilities::drain::end_drain("notes"); }
};

inline auto primary(std::string_view id = "editor") -> nlohmann::json {
  return {{"id", id},
          {"type", "primary"},
          {"client_path", "editor.js"},
          {"rbac_rule", "notes.read"}};
}

inline auto floating(std::string_view id = "preview") -> nlohmann::json {
  return {{"id", id},
          {"type", "float"},
          {"client_path", "editor.js"},
          {"rbac_rule", "notes.read"},
          {"capability", "notes:1:read"},
          {"content_types", {"text/plain"}},
          {"context_schema",
           {{"version", 1},
            {"fields", nlohmann::json::object()},
            {"persist_keys", nlohmann::json::array()}}}};
}

inline auto tray(std::string_view id = "status") -> nlohmann::json {
  return {{"id", id},
          {"type", "tray"},
          {"client_path", "editor.js"},
          {"rbac_rule", "notes.read"},
          {"tray_states", {{"idle", "bell"}}},
          {"tray_initial_state", "idle"},
          {"preferred_width", 320}};
}

inline auto panels(nlohmann::json entries) -> nlohmann::json {
  return {{"format_version", 2}, {"panels", std::move(entries)}};
}

inline auto package(const nlohmann::json& panel_manifest,
                    std::string_view version = "1.2.3")
    -> std::vector<std::byte> {
  std::ifstream input(std::filesystem::path{CMAKE_BINARY_DIR} / "fixtures" /
                          "valid-install.zip",
                      std::ios::binary);
  REQUIRE(input.is_open());
  const std::vector<char> original{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
  REQUIRE_FALSE(input.bad());
  REQUIRE_FALSE(original.empty());
  std::unique_ptr<zip_source_t, decltype(&zip_source_free)> source(
      zip_source_buffer_create(original.data(), original.size(), 0, nullptr),
      zip_source_free);
  REQUIRE(source);
  zip_source_keep(
      source.get()); // One reference for the archive, one for readback.
  std::unique_ptr<zip_t, decltype(&zip_discard)> archive(
      zip_open_from_source(source.get(), 0, nullptr), zip_discard);
  if (!archive) {
    zip_source_free(
        source.get()); // Open failure did not consume its reference.
    FAIL("could not open valid-install ZIP for fixture rewrite");
  }
  const auto panel_bytes = panel_manifest.dump();
  const auto manifest_bytes =
      nlohmann::json{{"name", "notes"},
                     {"version", version},
                     {"description", "Typed surface foundation fixture"},
                     {"author", "plinth-tests"},
                     {"license", "Apache-2.0"},
                     {"entry_point", "server/main.js"}}
          .dump();
  auto replace = [&](const char* name, const std::string& body) {
    std::unique_ptr<zip_source_t, decltype(&zip_source_free)> entry(
        zip_source_buffer(archive.get(), body.data(), body.size(), 0),
        zip_source_free);
    REQUIRE(entry);
    REQUIRE(zip_file_add(archive.get(), name, entry.get(), ZIP_FL_OVERWRITE) >=
            0);
    static_cast<void>(
        entry.release()); // zip_file_add owns the successful source.
  };
  replace("manifest.json", manifest_bytes);
  replace("panels.json", panel_bytes);
  REQUIRE(zip_close(archive.get()) == 0);
  static_cast<void>(archive.release());
  REQUIRE(zip_source_open(source.get()) == 0);
  struct SourceClose {
    zip_source_t& source;
    bool open = true;
    ~SourceClose() {
      if (open) {
        zip_source_close(&source);
      }
    }
  } close{*source};
  REQUIRE(zip_source_seek(source.get(), 0, SEEK_END) == 0);
  const auto size = zip_source_tell(source.get());
  REQUIRE(size > 0);
  REQUIRE(zip_source_seek(source.get(), 0, SEEK_SET) == 0);
  std::vector<std::byte> result(static_cast<std::size_t>(size));
  REQUIRE(zip_source_read(source.get(), result.data(), result.size()) == size);
  const auto close_result = zip_source_close(source.get());
  close.open = false;
  REQUIRE(close_result == 0);
  return result;
}

} // namespace plinth::typed_surface_test
