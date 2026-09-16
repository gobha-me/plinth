#include "kernel/browser_origin.hpp"

#include <arpa/inet.h>

#include <array>
#include <regex>
#include <string>

namespace plinth {

auto valid_browser_origin(std::string_view origin) -> bool {
  if (origin.empty()) {
    return true;
  }
  // Browser Origin serialization is lowercase and has no credentials, path,
  // query, fragment or trailing slash. IPv6 authorities require brackets.
  static const std::regex ORIGIN(
      R"(^(https?)://(\[[0-9a-f:.]+\]|[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)*)(?::([1-9][0-9]{0,4}))?$)");
  std::smatch match;
  const std::string value{origin};
  if (!std::regex_match(value, match, ORIGIN)) {
    return false;
  }
  const auto host = match[2].str();
  if (host.front() == '[') {
    in6_addr address{};
    const auto ip = host.substr(1, host.size() - 2);
    if (inet_pton(AF_INET6, ip.c_str(), &address) != 1) {
      return false;
    }
    if (IN6_IS_ADDR_V4MAPPED(&address) ||
        (IN6_IS_ADDR_V4COMPAT(&address) && !IN6_IS_ADDR_UNSPECIFIED(&address) &&
         !IN6_IS_ADDR_LOOPBACK(&address))) {
      return false;
    }
    std::array<char, INET6_ADDRSTRLEN> canonical{};
    if (inet_ntop(AF_INET6, &address, canonical.data(), canonical.size()) ==
            nullptr ||
        host != "[" + std::string{canonical.data()} + "]") {
      return false;
    }
  } else {
    in_addr address{};
    if (inet_pton(AF_INET, host.c_str(), &address) == 1) {
      std::array<char, INET_ADDRSTRLEN> canonical{};
      if (inet_ntop(AF_INET, &address, canonical.data(), canonical.size()) ==
              nullptr ||
          host != canonical.data()) {
        return false;
      }
    } else if (host.find_first_not_of("0123456789.") == std::string::npos) {
      return false;
    }
  }
  if (!match[3].matched) {
    return true;
  }
  const auto port = std::stoi(match[3].str());
  return port <= 65535 && !((match[1].str() == "http" && port == 80) ||
                            (match[1].str() == "https" && port == 443));
}

auto browser_origin_matches(const drogon::HttpRequestPtr& request,
                            std::string_view configured_origin) -> bool {
  const auto& host = request->getHeader("host");
  const auto& origin = request->getHeader("origin");
  if (host.empty() || origin.empty()) {
    return false;
  }

  const std::string expected =
      configured_origin.empty()
          ? std::string{request->isOnSecureConnection() ? "https://"
                                                        : "http://"} +
                host
          : std::string{configured_origin};
  const auto separator = expected.find("://");
  return separator != std::string::npos &&
         expected.substr(separator + 3) == host && origin == expected;
}

auto browser_shaped_request(const drogon::HttpRequestPtr& request) -> bool {
  return !request->getHeader("cookie").empty() ||
         !request->getCookie("plinth_session").empty() ||
         !request->getCookie("plinth_csrf").empty() ||
         !request->getHeader("sec-fetch-site").empty() ||
         !request->getHeader("sec-fetch-mode").empty() ||
         !request->getHeader("sec-fetch-dest").empty();
}

} // namespace plinth
