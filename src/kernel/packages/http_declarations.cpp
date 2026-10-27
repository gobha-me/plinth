#include "kernel/packages/http_declarations.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace plinth::packages {
namespace {

using Json = nlohmann::json;
constexpr std::size_t MAX_CLAIMS = 64;
constexpr std::size_t MAX_CLAIM_BYTES = 256;
constexpr std::array<std::string_view, 6> KERNEL_CLAIMS{
    "/api/*", "/ext/*", "/s/*", "/healthz/*", "/metrics/*", "/ws/*"};

struct DuplicateKey {};

// The callback sees decoded keys before the parser can overwrite an existing
// DOM member. Each object has its own key set; arrays introduce no key scope.
auto parse_unique_object(std::string_view bytes) -> Json {
  std::vector<std::set<std::string>> objects;
  return Json::parse(bytes, [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) {
      objects.emplace_back();
    } else if (event == Json::parse_event_t::key) {
      if (!objects.back().insert(value.get_ref<const std::string&>()).second) {
        throw DuplicateKey{};
      }
    } else if (event == Json::parse_event_t::object_end) {
      objects.pop_back();
    }
    return true;
  });
}

auto segment_character(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '~' || c == '-';
}

auto claim_base(std::string_view claim) -> std::string_view {
  return claim.ends_with("/*") ? claim.substr(0, claim.size() - 2) : claim;
}

auto subtree_contains(std::string_view base, std::string_view path) -> bool {
  return path == base || (path.size() > base.size() && path.starts_with(base) &&
                          path[base.size()] == '/');
}

struct Finding {
  std::string claim;
  ManifestParseError error;
};

class DeclarationParser {
 public:
  explicit DeclarationParser(std::string_view source) : source(source) {}

  auto parse(const Json& document) -> HttpDeclarationParseResult {
    HttpDeclaration declaration;
    declaration.http_prefixes = read_claims(document, "http_prefixes");
    declaration.unauthenticated_prefixes =
        read_claims(document, "unauthenticated_prefixes");
    check_relations(declaration.http_prefixes, "http_prefixes");
    check_relations(declaration.unauthenticated_prefixes,
                    "unauthenticated_prefixes");
    for (const auto& claim : declaration.http_prefixes) {
      if (std::ranges::any_of(KERNEL_CLAIMS, [&](std::string_view reserved) {
            return http_claims_intersect(claim, reserved);
          })) {
        add(claim, "/http_prefixes", "http-prefix-reserved",
            "HTTP claim intersects a fixed kernel mount");
      }
    }
    for (const auto& claim : declaration.unauthenticated_prefixes) {
      if (!std::ranges::binary_search(declaration.http_prefixes, claim)) {
        add(claim, "/unauthenticated_prefixes",
            "http-public-prefix-not-declared",
            "public HTTP claim must exactly equal a declared HTTP claim");
      }
    }
    const auto raw_claims = document.find("http_prefixes");
    const bool has_claims = raw_claims != document.end() &&
                            raw_claims->is_array() && !raw_claims->empty();
    const auto mode = document.find("handler_mode");
    if (mode != document.end()) {
      declaration.handler_mode = read_mode(*mode);
      if (!has_claims) {
        add({}, "/handler_mode", "http-mode-without-claims",
            "handler_mode requires nonempty valid HTTP claims");
      }
    } else if (has_claims) {
      add({}, "/handler_mode", "http-handler-mode-unavailable",
          "HTTP claims require an explicit handler_mode");
    }
    std::ranges::sort(findings, [](const Finding& left, const Finding& right) {
      return std::tie(left.claim, left.error.rule, left.error.field_path) <
             std::tie(right.claim, right.error.rule, right.error.field_path);
    });
    HttpDeclarationParseResult result;
    for (auto& finding : findings) {
      result.messages.push_back(std::move(finding.error));
    }
    if (result.messages.empty()) result.value = std::move(declaration);
    return result;
  }

 private:
  auto add(std::string_view claim, std::string field, std::string rule,
           std::string message) -> void {
    findings.push_back(Finding{
        .claim = std::string{claim},
        .error = ManifestParseError{.file = source,
                                    .line = std::nullopt,
                                    .column = std::nullopt,
                                    .field_path = std::move(field),
                                    .rule = std::move(rule),
                                    .message = std::move(message),
                                    .remediation = std::nullopt,
                                    .severity = Severity::ERROR},
    });
  }

  auto read_claims(const Json& document, std::string_view field)
      -> std::vector<std::string> {
    const auto member = document.find(field);
    if (member == document.end()) return {};
    const auto path = "/" + std::string{field};
    if (!member->is_array() || member->size() > MAX_CLAIMS) {
      add({}, path, "http-prefix-invalid",
          "HTTP claims must be an array with at most 64 entries");
      return {};
    }
    std::vector<std::string> claims;
    for (std::size_t index = 0; index < member->size(); ++index) {
      const auto& value = (*member)[index];
      if (!value.is_string() ||
          !is_valid_http_claim(value.get_ref<const std::string&>())) {
        add({}, path + "/" + std::to_string(index), "http-prefix-invalid",
            "HTTP claim must follow the exact ASCII literal/subtree grammar "
            "and fit 256 bytes");
        continue;
      }
      claims.push_back(value.get<std::string>());
    }
    std::ranges::sort(claims);
    return claims;
  }

  auto check_relations(const std::vector<std::string>& claims,
                       std::string_view field) -> void {
    const auto path = "/" + std::string{field};
    for (std::size_t index = 0; index < claims.size(); ++index) {
      if (index != 0 && claims[index] == claims[index - 1]) {
        add(claims[index], path, "http-prefix-duplicate",
            "HTTP claim is declared more than once in this array");
        continue;
      }
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (previous != 0 && claims[previous] == claims[previous - 1]) continue;
        if (http_claims_intersect(claims[previous], claims[index])) {
          add(claims[previous], path, "http-prefix-overlap",
              "HTTP claims in the same array have intersecting ownership");
        }
      }
    }
  }

  auto read_mode(const Json& mode) -> std::optional<HttpHandlerMode> {
    if (mode == "quickjs") return HttpHandlerMode::QUICKJS;
    if (mode == "sidecar") return HttpHandlerMode::SIDECAR;
    if (mode == "bundled_native") return HttpHandlerMode::BUNDLED_NATIVE;
    add({}, "/handler_mode", "http-handler-mode-unavailable",
        "handler_mode must be quickjs, sidecar, or bundled_native");
    return std::nullopt;
  }

  std::string source;
  std::vector<Finding> findings;
};

auto invalid_document(std::string_view source, std::string message)
    -> HttpDeclarationParseResult {
  return {.value = std::nullopt,
          .messages = {ManifestParseError{.file = std::string{source},
                                          .line = std::nullopt,
                                          .column = std::nullopt,
                                          .field_path = std::nullopt,
                                          .rule = "http-prefix-invalid",
                                          .message = std::move(message),
                                          .remediation = std::nullopt,
                                          .severity = Severity::ERROR}}};
}

} // namespace

auto is_valid_http_claim(std::string_view claim) -> bool {
  if (claim.size() < 2 || claim.size() > MAX_CLAIM_BYTES ||
      claim.front() != '/') {
    return false;
  }
  auto literal = claim_base(claim);
  if (literal.size() < 2) return false;
  literal.remove_prefix(1);
  while (!literal.empty()) {
    const auto separator = literal.find('/');
    const auto segment = literal.substr(0, separator);
    if (segment.empty() || segment == "." || segment == ".." ||
        !std::ranges::all_of(segment, segment_character)) {
      return false;
    }
    if (separator == std::string_view::npos) return true;
    literal.remove_prefix(separator + 1);
  }
  return false;
}

auto http_claim_matches(std::string_view claim, std::string_view path) -> bool {
  if (!is_valid_http_claim(claim)) return false;
  return claim.ends_with("/*") ? subtree_contains(claim_base(claim), path)
                               : path == claim;
}

auto http_claims_intersect(std::string_view left, std::string_view right)
    -> bool {
  if (!is_valid_http_claim(left) || !is_valid_http_claim(right)) return false;
  if (left.ends_with("/*")) {
    if (subtree_contains(claim_base(left), claim_base(right))) return true;
  } else if (left == right) {
    return true;
  }
  return right.ends_with("/*") &&
         subtree_contains(claim_base(right), claim_base(left));
}

auto HttpDeclaration::parse(std::string_view manifest_bytes,
                            std::string_view source_path)
    -> HttpDeclarationParseResult {
  try {
    const auto document = parse_unique_object(manifest_bytes);
    if (!document.is_object()) {
      return invalid_document(source_path, "manifest must be a JSON object");
    }
    return DeclarationParser{source_path}.parse(document);
  } catch (const DuplicateKey&) {
    return invalid_document(source_path,
                            "manifest contains a duplicate decoded JSON key");
  } catch (const Json::exception&) {
    return invalid_document(source_path, "manifest is not valid JSON");
  }
}

} // namespace plinth::packages
