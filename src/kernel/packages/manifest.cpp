#include "kernel/packages/manifest.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::packages {

namespace {

constexpr std::size_t DESCRIPTION_MAX_LEN = 1024;
constexpr std::size_t AUTHOR_MAX_LEN = 256;
constexpr std::size_t NAME_MAX_LEN = 64;

constexpr std::array<std::string_view, 7> SPDX_WHITELIST{
    "MIT",          "Apache-2.0", "GPL-3.0",   "AGPL-3.0",
    "BSD-3-Clause", "ISC",        "Unlicense",
};

constexpr std::array<std::string_view, 10> KNOWN_TOP_LEVEL{
    "name",        "version",  "description", "author",    "license",
    "entry_point", "frontend", "runtime",     "shareable", "provider_extension",
};

auto is_known_top_level(std::string_view key) -> bool {
  return std::ranges::any_of(KNOWN_TOP_LEVEL,
                             [&](std::string_view k) { return k == key; });
}

auto is_name_char(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

auto is_name_valid(std::string_view s) -> bool {
  if (s.size() < 2 || s.size() > NAME_MAX_LEN) {
    return false;
  }
  if (s[0] < 'a' || s[0] > 'z') {
    return false;
  }
  return std::ranges::all_of(s.substr(1), is_name_char);
}

auto is_digit_char(char c) -> bool {
  return c >= '0' && c <= '9';
}

auto is_alnum_or_dash(char c) -> bool {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '-';
}

// Consume a SemVer numeric identifier: either "0" alone or a non-zero digit
// followed by more digits. Leading zeros on multi-digit numerics are rejected.
auto consume_numeric(std::string_view s, std::size_t& i) -> bool {
  auto start = i;
  while (i < s.size() && is_digit_char(s[i])) {
    ++i;
  }
  if (i == start) {
    return false;
  }
  // Reject leading-zero multi-digit numerics per SemVer.
  return i - start == 1 || s[start] != '0';
}

auto consume_major_minor_patch(std::string_view s, std::size_t& i) -> bool {
  if (!consume_numeric(s, i) || i >= s.size() || s[i] != '.') {
    return false;
  }
  ++i;
  if (!consume_numeric(s, i) || i >= s.size() || s[i] != '.') {
    return false;
  }
  ++i;
  return consume_numeric(s, i);
}

auto consume_prerelease_ident(std::string_view s, std::size_t& i) -> bool {
  auto start = i;
  bool all_digits = true;
  while (i < s.size() && s[i] != '.' && s[i] != '+') {
    if (!is_alnum_or_dash(s[i])) {
      return false;
    }
    if (!is_digit_char(s[i])) {
      all_digits = false;
    }
    ++i;
  }
  auto len = i - start;
  if (len == 0) {
    return false;
  }
  // Numeric identifiers in the prerelease block cannot have leading zeros.
  return !all_digits || len == 1 || s[start] != '0';
}

auto consume_prerelease(std::string_view s, std::size_t& i) -> bool {
  while (true) {
    if (!consume_prerelease_ident(s, i)) {
      return false;
    }
    if (i >= s.size() || s[i] != '.') {
      return true;
    }
    ++i;
  }
}

auto consume_build_ident(std::string_view s, std::size_t& i) -> bool {
  auto start = i;
  while (i < s.size() && s[i] != '.') {
    if (!is_alnum_or_dash(s[i])) {
      return false;
    }
    ++i;
  }
  return i != start;
}

auto consume_build(std::string_view s, std::size_t& i) -> bool {
  while (true) {
    if (!consume_build_ident(s, i)) {
      return false;
    }
    if (i >= s.size() || s[i] != '.') {
      return true;
    }
    ++i;
  }
}

auto is_semver_valid(std::string_view s) -> bool {
  std::size_t i = 0;
  if (!consume_major_minor_patch(s, i)) {
    return false;
  }
  if (i < s.size() && s[i] == '-') {
    ++i;
    if (!consume_prerelease(s, i)) {
      return false;
    }
  }
  if (i < s.size() && s[i] == '+') {
    ++i;
    if (!consume_build(s, i)) {
      return false;
    }
  }
  return i == s.size();
}

auto is_relative_path_valid(std::string_view s) -> bool {
  if (s.empty() || s[0] == '/') {
    return false;
  }
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      if (s.substr(start, i - start) == "..") {
        return false;
      }
      start = i + 1;
    }
  }
  return true;
}

auto is_spdx_whitelisted(std::string_view s) -> bool {
  return std::ranges::any_of(SPDX_WHITELIST,
                             [&](std::string_view w) { return w == s; });
}

auto is_mount_valid(std::string_view s) -> std::pair<bool, std::string_view> {
  if (s.empty() || s[0] != '/') {
    return {false, "leading_slash"};
  }
  if (s == "/ext" || s.starts_with("/ext/")) {
    return {false, "reserved_ext_prefix"};
  }
  return {true, {}};
}

auto line_column_from_byte(std::string_view text, std::size_t byte_offset)
    -> std::pair<std::optional<std::size_t>, std::optional<std::size_t>> {
  if (byte_offset > text.size()) {
    return {std::nullopt, std::nullopt};
  }
  std::size_t line = 1;
  std::size_t column = 1;
  for (std::size_t i = 0; i < byte_offset; ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }
  return {line, column};
}

// ErrorSink threads a file-tagged push() through the per-field helpers.
// `out` is a pointer (not reference) to comply with
// cppcoreguidelines-avoid-const-or-ref-data-members; the sink is never
// null in practice — callers construct it with &result.messages.
struct ErrorSink {
  std::string file;
  std::vector<ManifestParseError>* out = nullptr;

  auto push(std::string rule, std::string message,
            std::optional<std::string> field_path = std::nullopt,
            std::optional<std::string> remediation = std::nullopt,
            Severity severity = Severity::ERROR) -> void {
    out->push_back(ManifestParseError{
        .file = file,
        .line = std::nullopt,
        .column = std::nullopt,
        .field_path = std::move(field_path),
        .rule = std::move(rule),
        .message = std::move(message),
        .remediation = std::move(remediation),
        .severity = severity,
    });
  }
};

auto parse_name(const nlohmann::json& j, ErrorSink& sink, PackageManifest& m)
    -> void {
  if (!j.contains("name")) {
    sink.push("manifest.name.missing", "required field 'name' is missing",
              "/name");
    return;
  }
  if (!j["name"].is_string()) {
    sink.push("manifest.name.invalid", "'name' must be a string", "/name");
    return;
  }
  m.name = j["name"].get<std::string>();
  if (!is_name_valid(m.name)) {
    sink.push("manifest.name.invalid",
              "'name' must match ^[a-z][a-z0-9-]{1,63}$", "/name",
              "use lowercase letters, digits, and dashes; must start with a "
              "letter; 2–64 chars");
  }
}

auto parse_version(const nlohmann::json& j, ErrorSink& sink, PackageManifest& m)
    -> void {
  if (!j.contains("version")) {
    sink.push("manifest.version.missing", "required field 'version' is missing",
              "/version");
    return;
  }
  if (!j["version"].is_string()) {
    sink.push("manifest.version.invalid_semver", "'version' must be a string",
              "/version");
    return;
  }
  m.version = j["version"].get<std::string>();
  if (!is_semver_valid(m.version)) {
    sink.push("manifest.version.invalid_semver",
              "'version' must be a SemVer 2.0.0 string", "/version",
              "use major.minor.patch (e.g. \"1.2.3\")");
  }
}

auto parse_bounded_string(const nlohmann::json& j, ErrorSink& sink,
                          std::string_view key, std::size_t max_len,
                          std::string_view rule_base, std::string& out)
    -> void {
  std::string key_str{key};
  std::string field_path = "/";
  field_path.append(key);
  std::string missing_rule = std::string{rule_base} + ".missing";
  std::string too_long_rule = std::string{rule_base} + ".too_long";
  if (!j.contains(key_str)) {
    sink.push(missing_rule, "required field '" + key_str + "' is missing",
              field_path);
    return;
  }
  if (!j[key_str].is_string()) {
    sink.push(missing_rule, "'" + key_str + "' must be a string", field_path);
    return;
  }
  out = j[key_str].get<std::string>();
  if (out.empty()) {
    sink.push(missing_rule, "'" + key_str + "' must be non-empty", field_path);
  } else if (out.size() > max_len) {
    sink.push(too_long_rule,
              "'" + key_str + "' exceeds " + std::to_string(max_len) +
                  " characters",
              field_path);
  }
}

auto parse_license(const nlohmann::json& j, ErrorSink& sink, PackageManifest& m)
    -> void {
  if (!j.contains("license")) {
    sink.push("manifest.license.missing", "required field 'license' is missing",
              "/license");
    return;
  }
  if (!j["license"].is_string()) {
    sink.push("manifest.license.missing", "'license' must be a string",
              "/license");
    return;
  }
  m.license = j["license"].get<std::string>();
  if (m.license.empty()) {
    sink.push("manifest.license.missing", "'license' must be non-empty",
              "/license");
    return;
  }
  if (!is_spdx_whitelisted(m.license)) {
    sink.push("manifest.license.unknown_spdx",
              "'license' is not on the known SPDX whitelist", "/license",
              "one of: MIT, Apache-2.0, GPL-3.0, AGPL-3.0, BSD-3-Clause, ISC, "
              "Unlicense",
              Severity::WARNING);
  }
}

auto parse_entry_point(const nlohmann::json& j, ErrorSink& sink,
                       PackageManifest& m) -> void {
  if (!j.contains("entry_point")) {
    sink.push("manifest.entry_point.missing",
              "required field 'entry_point' is missing", "/entry_point");
    return;
  }
  if (!j["entry_point"].is_string()) {
    sink.push("manifest.entry_point.invalid_path",
              "'entry_point' must be a string", "/entry_point");
    return;
  }
  m.entry_point = j["entry_point"].get<std::string>();
  if (m.entry_point.empty()) {
    sink.push("manifest.entry_point.missing", "'entry_point' must be non-empty",
              "/entry_point");
  } else if (!is_relative_path_valid(m.entry_point)) {
    sink.push("manifest.entry_point.invalid_path",
              "'entry_point' must be a relative path (no leading '/', no '..' "
              "components)",
              "/entry_point");
  }
}

auto parse_frontend(const nlohmann::json& j, ErrorSink& sink,
                    PackageManifest& m) -> void {
  if (!j.contains("frontend")) {
    return;
  }
  const auto& f = j["frontend"];
  if (!f.is_object()) {
    sink.push("manifest.frontend.invalid", "'frontend' must be an object",
              "/frontend");
    return;
  }
  FrontendMount fm;
  if (!f.contains("mount")) {
    sink.push("manifest.frontend.mount.missing",
              "'frontend.mount' is required when 'frontend' is present",
              "/frontend/mount");
  } else if (!f["mount"].is_string()) {
    sink.push("manifest.frontend.mount.leading_slash",
              "'frontend.mount' must be a string", "/frontend/mount");
  } else {
    fm.mount = f["mount"].get<std::string>();
    auto [ok, subkey] = is_mount_valid(fm.mount);
    if (!ok) {
      std::string rule = "manifest.frontend.mount.";
      rule.append(subkey);
      sink.push(std::move(rule),
                "'frontend.mount' must start with '/' and not overlap the "
                "reserved '/ext' prefix",
                "/frontend/mount");
    }
  }
  if (!f.contains("entry")) {
    sink.push("manifest.frontend.entry.missing",
              "'frontend.entry' is required when 'frontend' is present",
              "/frontend/entry");
  } else if (!f["entry"].is_string()) {
    sink.push("manifest.frontend.entry.invalid_path",
              "'frontend.entry' must be a string", "/frontend/entry");
  } else {
    fm.entry = f["entry"].get<std::string>();
    if (fm.entry.empty() || !is_relative_path_valid(fm.entry)) {
      sink.push("manifest.frontend.entry.invalid_path",
                "'frontend.entry' must be a non-empty relative path",
                "/frontend/entry");
    }
  }
  m.frontend = std::move(fm);
}

auto parse_runtime_u64(const nlohmann::json& runtime, ErrorSink& sink,
                       std::string_view field,
                       std::optional<std::uint64_t>& dst) -> void {
  std::string key{field};
  if (!runtime.contains(key)) {
    return;
  }
  const auto& v = runtime[key];
  if (!v.is_number_unsigned() || v.get<std::uint64_t>() == 0) {
    std::string rule = "manifest.runtime.";
    rule.append(field);
    rule.append(".zero");
    std::string field_path = "/runtime/";
    field_path.append(field);
    sink.push(std::move(rule),
              std::string{field} + " must be a positive integer",
              std::move(field_path));
    return;
  }
  dst = v.get<std::uint64_t>();
}

auto parse_runtime(const nlohmann::json& j, ErrorSink& sink, PackageManifest& m)
    -> void {
  if (!j.contains("runtime")) {
    return;
  }
  const auto& r = j["runtime"];
  if (!r.is_object()) {
    sink.push("manifest.runtime.invalid", "'runtime' must be an object",
              "/runtime");
    return;
  }
  RuntimeOverrides ro;
  parse_runtime_u64(r, sink, "memory_limit_mb", ro.memory_limit_mb);
  parse_runtime_u64(r, sink, "cpu_time_limit_ms", ro.cpu_time_limit_ms);
  parse_runtime_u64(r, sink, "max_stack_depth", ro.max_stack_depth);
  m.runtime = ro;
}

auto parse_provider_extension(const nlohmann::json& j, ErrorSink& sink,
                              PackageManifest& m) -> void {
  if (!j.contains("provider_extension")) {
    return;
  }
  const auto& v = j["provider_extension"];
  if (!v.is_boolean()) {
    sink.push("manifest.provider_extension.invalid",
              "'provider_extension' must be a boolean", "/provider_extension");
    return;
  }
  m.provider_extension = v.get<bool>();
}

auto parse_shareable(const nlohmann::json& j, ErrorSink& sink,
                     PackageManifest& m) -> void {
  if (!j.contains("shareable")) {
    return;
  }
  const auto& s = j["shareable"];
  if (!s.is_array()) {
    sink.push("manifest.shareable.not_array", "'shareable' must be an array",
              "/shareable");
    return;
  }
  if (!s.empty()) {
    sink.push("manifest.shareable.non_empty_reserved",
              "'shareable' is reserved and must be empty in 0.4.x (entries "
              "will be ignored)",
              "/shareable", "remove entries until the share primitive ships",
              Severity::WARNING);
  }
  for (const auto& elem : s) {
    m.shareable.push_back(elem);
  }
}

auto collect_unknown_fields(const nlohmann::json& j, PackageManifest& m)
    -> void {
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!is_known_top_level(it.key())) {
      m.unknown_fields[it.key()] = it.value();
    }
  }
}

auto has_error(const std::vector<ManifestParseError>& msgs) -> bool {
  return std::ranges::any_of(
      msgs, [](const auto& e) { return e.severity == Severity::ERROR; });
}

auto serialize_runtime(const RuntimeOverrides& r) -> nlohmann::json {
  auto obj = nlohmann::json::object();
  if (r.memory_limit_mb) {
    obj["memory_limit_mb"] = *r.memory_limit_mb;
  }
  if (r.cpu_time_limit_ms) {
    obj["cpu_time_limit_ms"] = *r.cpu_time_limit_ms;
  }
  if (r.max_stack_depth) {
    obj["max_stack_depth"] = *r.max_stack_depth;
  }
  return obj;
}

} // namespace

namespace {

// Strip build metadata (`+...`) and split into `mmp` / `prerelease` halves.
// Returns {mmp, prerelease, ok}. `ok` is false on a missing MMP segment.
auto split_semver(std::string_view s)
    -> std::tuple<std::string_view, std::string_view, bool> {
  auto plus = s.find('+');
  auto core = (plus == std::string_view::npos) ? s : s.substr(0, plus);
  auto dash = core.find('-');
  if (dash == std::string_view::npos) {
    return {core, {}, !core.empty()};
  }
  return {core.substr(0, dash), core.substr(dash + 1), dash != 0};
}

auto parse_mmp(std::string_view mmp)
    -> std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, bool> {
  auto d1 = mmp.find('.');
  if (d1 == std::string_view::npos) {
    return {0, 0, 0, false};
  }
  auto d2 = mmp.find('.', d1 + 1);
  if (d2 == std::string_view::npos) {
    return {0, 0, 0, false};
  }
  std::uint64_t major_v = 0;
  std::uint64_t minor_v = 0;
  std::uint64_t patch_v = 0;
  auto scan = [](std::string_view part, std::uint64_t& out) -> bool {
    if (part.empty()) {
      return false;
    }
    out = 0;
    for (char c : part) {
      if (c < '0' || c > '9') {
        return false;
      }
      out = (out * 10) + static_cast<std::uint64_t>(c - '0');
    }
    return true;
  };
  if (!scan(mmp.substr(0, d1), major_v)) {
    return {0, 0, 0, false};
  }
  if (!scan(mmp.substr(d1 + 1, d2 - d1 - 1), minor_v)) {
    return {0, 0, 0, false};
  }
  if (!scan(mmp.substr(d2 + 1), patch_v)) {
    return {0, 0, 0, false};
  }
  return {major_v, minor_v, patch_v, true};
}

auto is_all_digits(std::string_view s) -> bool {
  if (s.empty()) {
    return false;
  }
  return std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

auto compare_prerelease_ident(std::string_view a, std::string_view b) -> int {
  bool ad = is_all_digits(a);
  bool bd = is_all_digits(b);
  if (ad && bd) {
    std::uint64_t an = 0;
    std::uint64_t bn = 0;
    for (char c : a) {
      an = (an * 10) + static_cast<std::uint64_t>(c - '0');
    }
    for (char c : b) {
      bn = (bn * 10) + static_cast<std::uint64_t>(c - '0');
    }
    if (an < bn) {
      return -1;
    }
    if (an > bn) {
      return 1;
    }
    return 0;
  }
  if (ad && !bd) {
    return -1;
  } // numeric < alnum per SemVer
  if (!ad && bd) {
    return 1;
  }
  if (a < b) {
    return -1;
  }
  if (a > b) {
    return 1;
  }
  return 0;
}

auto compare_prerelease(std::string_view a, std::string_view b) -> int {
  std::size_t ai = 0;
  std::size_t bi = 0;
  while (ai < a.size() && bi < b.size()) {
    auto ae = a.find('.', ai);
    auto be = b.find('.', bi);
    auto asub =
        a.substr(ai, ae == std::string_view::npos ? a.size() - ai : ae - ai);
    auto bsub =
        b.substr(bi, be == std::string_view::npos ? b.size() - bi : be - bi);
    int c = compare_prerelease_ident(asub, bsub);
    if (c != 0) {
      return c;
    }
    ai = (ae == std::string_view::npos) ? a.size() : ae + 1;
    bi = (be == std::string_view::npos) ? b.size() : be + 1;
  }
  // All shared identifiers equal; longer prerelease is higher.
  if (ai < a.size()) {
    return 1;
  }
  if (bi < b.size()) {
    return -1;
  }
  return 0;
}

} // namespace

auto compare_semver(std::string_view a, std::string_view b) -> int {
  auto [a_mmp, a_pre, a_ok] = split_semver(a);
  auto [b_mmp, b_pre, b_ok] = split_semver(b);
  if (!a_ok || !b_ok) {
    return 0;
  }
  auto [a_major, a_minor, a_patch, a_mmp_ok] = parse_mmp(a_mmp);
  auto [b_major, b_minor, b_patch, b_mmp_ok] = parse_mmp(b_mmp);
  if (!a_mmp_ok || !b_mmp_ok) {
    return 0;
  }
  if (a_major != b_major) {
    return (a_major < b_major) ? -1 : 1;
  }
  if (a_minor != b_minor) {
    return (a_minor < b_minor) ? -1 : 1;
  }
  if (a_patch != b_patch) {
    return (a_patch < b_patch) ? -1 : 1;
  }
  // MMP equal. Pre-release tagged is lower than untagged.
  if (a_pre.empty() && b_pre.empty()) {
    return 0;
  }
  if (a_pre.empty() && !b_pre.empty()) {
    return 1;
  }
  if (!a_pre.empty() && b_pre.empty()) {
    return -1;
  }
  return compare_prerelease(a_pre, b_pre);
}

auto PackageManifest::parse(std::string_view json_text,
                            std::string_view source_path, bool is_bundled)
    -> PackageManifestParseResult {
  PackageManifestParseResult result;
  ErrorSink sink{.file = std::string{source_path}, .out = &result.messages};

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(json_text);
  } catch (const nlohmann::json::parse_error& e) {
    auto [ln, col] = line_column_from_byte(json_text, e.byte);
    result.messages.push_back(ManifestParseError{
        .file = std::string{source_path},
        .line = ln,
        .column = col,
        .field_path = std::nullopt,
        .rule = "manifest.json.parse_error",
        .message = e.what(),
        .remediation = "ensure the file is syntactically valid JSON",
        .severity = Severity::ERROR,
    });
    return result;
  }

  if (!j.is_object()) {
    sink.push("manifest.root.not_object",
              "manifest.json top-level value must be an object");
    return result;
  }

  PackageManifest m;
  parse_name(j, sink, m);
  if (!is_bundled && m.name == "shell") {
    sink.push(
        "manifest.name.reserved",
        "'name' value 'shell' is reserved for the bundled shell extension",
        "/name",
        "rename the package; 'shell' is the canonical name for the "
        "kernel-bundled frontend (ICD-0.6.1 §5.5)");
  }
  parse_version(j, sink, m);
  parse_bounded_string(j, sink, "description", DESCRIPTION_MAX_LEN,
                       "manifest.description", m.description);
  parse_bounded_string(j, sink, "author", AUTHOR_MAX_LEN, "manifest.author",
                       m.author);
  parse_license(j, sink, m);
  parse_entry_point(j, sink, m);
  parse_frontend(j, sink, m);
  parse_runtime(j, sink, m);
  parse_shareable(j, sink, m);
  parse_provider_extension(j, sink, m);
  collect_unknown_fields(j, m);

  if (!has_error(result.messages)) {
    result.value = std::move(m);
  }
  return result;
}

auto PackageManifest::serialize() const -> std::string {
  auto out = nlohmann::json::object();
  for (auto it = unknown_fields.begin(); it != unknown_fields.end(); ++it) {
    out[it.key()] = it.value();
  }
  out["name"] = name;
  out["version"] = version;
  out["description"] = description;
  out["author"] = author;
  out["license"] = license;
  out["entry_point"] = entry_point;
  if (frontend) {
    out["frontend"] = nlohmann::json::object({
        {"mount", frontend->mount},
        {"entry", frontend->entry},
    });
  }
  if (runtime) {
    out["runtime"] = serialize_runtime(*runtime);
  }
  auto arr = nlohmann::json::array();
  for (const auto& v : shareable) {
    arr.push_back(v);
  }
  out["shareable"] = arr;
  if (provider_extension) {
    out["provider_extension"] = true;
  }
  return out.dump(2);
}

} // namespace plinth::packages
