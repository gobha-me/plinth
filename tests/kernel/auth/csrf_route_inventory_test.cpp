#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Route = std::pair<std::string, std::string>;

auto read_source(const std::filesystem::path& path) -> std::string {
  std::ifstream input{path};
  REQUIRE(input.is_open());
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

auto method_in(std::string_view block) -> std::string {
  for (const auto method : {"Post", "Put", "Patch", "Delete"}) {
    if (block.contains(std::string{"drogon::"} + method)) {
      return method;
    }
  }
  return {};
}

auto require_filter_order(std::string_view block, std::string_view path)
    -> void {
  INFO(path);
  if (path == "/api/auth/login" || path == "/api/auth/register") {
    REQUIRE(block.contains("PublicOriginFilter"));
    return;
  }

  auto session = block.find("SessionFilter");
  auto csrf = block.find("CsrfFilter");
  if (session == std::string_view::npos) {
    session = block.find(", SF");
  }
  if (csrf == std::string_view::npos) {
    csrf = block.find(", CF");
  }
  REQUIRE(session != std::string_view::npos);
  REQUIRE(csrf != std::string_view::npos);
  REQUIRE(session < csrf);

  const auto rbac = block.find("RbacFilter");
  const auto rbac_alias = block.find(", RF");
  if (rbac != std::string_view::npos) {
    REQUIRE(csrf < rbac);
  }
  if (rbac_alias != std::string_view::npos) {
    REQUIRE(csrf < rbac_alias);
  }
}

} // namespace

TEST_CASE("every mutating API route declares the CSRF contract",
          "[auth][csrf][route-inventory]") {
  const auto source_root =
      std::filesystem::path{CMAKE_SOURCE_DIR} / "src" / "kernel";
  std::set<Route> actual;

  for (const auto& entry :
       std::filesystem::recursive_directory_iterator{source_root}) {
    if (!entry.is_regular_file() || entry.path().extension() != ".cpp") {
      continue;
    }
    const auto source = read_source(entry.path());
    auto begin = source.find("registerHandler(");
    while (begin != std::string::npos) {
      const auto next = source.find("registerHandler(", begin + 1);
      const auto block = std::string_view{source}.substr(
          begin,
          next == std::string::npos ? source.size() - begin : next - begin);
      const auto path_begin = block.find("\"/api/");
      const auto method = method_in(block);
      if (path_begin != std::string_view::npos && !method.empty()) {
        const auto value_begin = path_begin + 1;
        const auto path_end = block.find('"', value_begin);
        REQUIRE(path_end != std::string_view::npos);
        const auto path =
            std::string{block.substr(value_begin, path_end - value_begin)};
        require_filter_order(block, path);
        REQUIRE(actual.emplace(method, path).second);
      }
      begin = next;
    }
  }

  const std::set<Route> expected{
      {"Post", "/api/auth/register"},
      {"Post", "/api/auth/login"},
      {"Post", "/api/auth/logout"},
      {"Delete", "/api/auth/session/{id}"},
      {"Post", "/api/auth/pats"},
      {"Delete", "/api/auth/pats/{id}"},
      {"Post", "/api/groups"},
      {"Put", "/api/groups/{id}"},
      {"Delete", "/api/groups/{id}"},
      {"Post", "/api/groups/{id}/members"},
      {"Delete", "/api/groups/{id}/members/{user_id}"},
      {"Post", "/api/groups/{id}/rules"},
      {"Delete", "/api/groups/{id}/rules/{rule}"},
      {"Post", "/api/packages"},
      {"Patch", "/api/packages/{id}"},
      {"Delete", "/api/packages/{id}"},
      {"Post", "/api/cap/{capability}"},
  };
  REQUIRE(actual == expected);
}
