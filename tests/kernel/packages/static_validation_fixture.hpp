#pragma once

// Ordinary copies of existing package fixtures, shared by offline and PG
// lifecycle parity tests. All writes and archives stay in the owned temp tree.
#include "kernel/packages/validator.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <zip.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages::static_test {

inline auto fixture(std::string_view relative) -> std::filesystem::path {
  return std::filesystem::path{CMAKE_SOURCE_DIR} / "tests" / "fixtures" /
         relative;
}

class PackageCopy {
 public:
  explicit PackageCopy(
      std::string_view source = "install_lifecycle/valid-install") {
    static std::atomic<unsigned> next{0};
    base_ = std::filesystem::temp_directory_path() /
            ("plinth-static-validation-" + std::to_string(::getpid()) + "-" +
             std::to_string(next.fetch_add(1)));
    std::filesystem::create_directories(base_);
    try {
      std::filesystem::copy(fixture(source), root(),
                            std::filesystem::copy_options::recursive);
    } catch (...) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(base_, cleanup_error);
      throw;
    }
  }

  ~PackageCopy() {
    std::error_code ec;
    std::filesystem::remove_all(base_, ec);
  }
  PackageCopy(const PackageCopy&) = delete;
  auto operator=(const PackageCopy&) -> PackageCopy& = delete;
  PackageCopy(PackageCopy&&) = delete;
  auto operator=(PackageCopy&&) -> PackageCopy& = delete;

  [[nodiscard]] auto root() const -> std::filesystem::path {
    return base_ / "package";
  }

  auto write(std::string_view relative, std::string_view bytes) const -> void {
    const auto path = root() / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    REQUIRE(output.is_open());
    output << bytes;
    output.close();
    REQUIRE(output.good());
  }

  [[nodiscard]] auto json(std::string_view relative) const -> nlohmann::json {
    std::ifstream input{root() / relative};
    REQUIRE(input.is_open());
    return nlohmann::json::parse(input);
  }

  auto assertion(std::string_view call) const -> void {
    auto document = json("rbac.json");
    document["rules"][0]["test"] = {
        {"assert_allow", {{"call", call}, {"expect", "success"}}}};
    write("rbac.json", document.dump());
  }

  [[nodiscard]] auto tree_bytes() const -> std::size_t {
    std::size_t total = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root())) {
      if (entry.is_regular_file()) {
        total += static_cast<std::size_t>(entry.file_size());
      }
    }
    return total;
  }

  [[nodiscard]] auto zip() const -> std::vector<std::byte> {
    const auto archive = base_ / "package.zip";
    int error = 0;
    auto* raw = zip_open(archive.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    REQUIRE(raw != nullptr);
    std::unique_ptr<zip_t, decltype(&zip_discard)> output{raw, zip_discard};
    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root())) {
      if (entry.is_regular_file()) files.push_back(entry.path());
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
      auto* source = zip_source_file(output.get(), file.c_str(), 0, 0);
      REQUIRE(source != nullptr);
      const auto name = file.lexically_relative(root()).generic_string();
      const auto added =
          zip_file_add(output.get(), name.c_str(), source, ZIP_FL_ENC_UTF_8);
      if (added < 0) zip_source_free(source);
      REQUIRE(added >= 0);
      REQUIRE(zip_set_file_compression(output.get(),
                                       static_cast<zip_uint64_t>(added),
                                       ZIP_CM_DEFLATE, 6) == 0);
    }
    REQUIRE(zip_close(output.get()) == 0);
    static_cast<void>(output.release());
    std::ifstream input{archive, std::ios::binary};
    REQUIRE(input.is_open());
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::vector<std::byte> result(bytes.size());
    std::ranges::transform(bytes, result.begin(), [](char value) {
      return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return result;
  }

 private:
  std::filesystem::path base_;
};

inline auto finding_projection(const nlohmann::json& messages)
    -> nlohmann::json {
  auto result = nlohmann::json::array();
  for (const auto& message : messages) {
    result.push_back(
        {{"severity", message.at("severity")},
         {"rule", message.at("rule")},
         {"path", message.value("path", nlohmann::json(nullptr))}});
  }
  return result;
}

inline auto cli_report(const ValidationReport& report,
                       const std::filesystem::path& root) -> nlohmann::json {
  std::ostringstream output;
  render_json(report, root, output);
  return nlohmann::json::parse(output.str());
}

} // namespace plinth::packages::static_test
