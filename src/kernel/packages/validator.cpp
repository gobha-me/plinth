#include "kernel/packages/validator.hpp"

#include "kernel/logging.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/config_manifest.hpp"
#include "kernel/packages/cross_file_validator.hpp"
#include "kernel/packages/detail/reporter.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/packages/manifest_error.hpp"
#include "kernel/packages/panels_manifest.hpp"
#include "kernel/security/unicode_scanner.hpp"

#include <json/value.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <list>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace plinth::packages {

auto ValidationReport::error_count() const noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(messages.begin(), messages.end(), [](const auto& m) {
        return m.severity == Severity::ERROR;
      }));
}

auto ValidationReport::warning_count() const noexcept -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(messages.begin(), messages.end(), [](const auto& m) {
        return m.severity == Severity::WARNING;
      }));
}

auto ValidationReport::disposition() const noexcept -> int {
  if (error_count() > 0) {
    return 1;
  }
  if (warning_count() > 0) {
    return 2;
  }
  return 0;
}

namespace {

constexpr std::array<std::string_view, 9> KNOWN_ROOT_FILES{
    "manifest.json", "capabilities.json", "rbac.json",
    "panels.json",   "config.json",       "README.md",
    "LICENSE",       "CHANGELOG.md",      ".plinthignore",
};

constexpr std::array<std::string_view, 3> LAYOUT_DIRS{
    "server",
    "client",
    "migrations",
};

constexpr std::array<std::string_view, 3> VCS_DIRS{
    ".git",
    ".hg",
    ".svn",
};

auto is_known_root_file(std::string_view name) -> bool {
  return std::ranges::any_of(KNOWN_ROOT_FILES,
                             [&](std::string_view f) { return f == name; });
}

auto is_layout_dir(std::string_view name) -> bool {
  return std::ranges::any_of(LAYOUT_DIRS,
                             [&](std::string_view d) { return d == name; });
}

auto is_vcs_dir(std::string_view name) -> bool {
  return std::ranges::any_of(VCS_DIRS,
                             [&](std::string_view d) { return d == name; });
}

auto read_file_bytes(const fs::path& p) -> std::optional<std::string> {
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

auto relative_string(const fs::path& root, const fs::path& p) -> std::string {
  std::error_code ec;
  auto rel = fs::relative(p, root, ec);
  if (ec) {
    return p.generic_string();
  }
  return rel.generic_string();
}

using plinth::packages::detail::Reporter;

auto run_input_preflight(const fs::path& root, Reporter& r) -> bool {
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    r.error("input", "path does not exist", root.generic_string());
    return false;
  }
  if (!fs::is_directory(root, ec)) {
    r.error("input", "path is not a directory", root.generic_string());
    return false;
  }
  // Readability smoke-check: try to open the directory for iteration.
  auto it = fs::directory_iterator(root, ec);
  if (ec) {
    r.error("input", "path is not readable (" + ec.message() + ")",
            root.generic_string());
    return false;
  }
  (void)it;
  return true;
}

// Rule R1 — required files present.
auto run_required_files(const fs::path& root, Reporter& r) -> void {
  std::error_code ec;
  if (!fs::exists(root / "manifest.json", ec)) {
    r.error("required-files",
            "missing manifest.json at " + root.generic_string(),
            "manifest.json",
            "create manifest.json with at minimum name, version, description, "
            "author, license, and entry_point fields");
  }
  if (!fs::exists(root / "capabilities.json", ec)) {
    r.error("required-files",
            "missing capabilities.json at " + root.generic_string(),
            "capabilities.json",
            "create capabilities.json with a provides[] entry for each "
            "exported capability");
  }
}

auto symlink_escapes_root(const fs::path& p, const fs::path& root_real)
    -> bool {
  std::error_code rec;
  auto target = fs::weakly_canonical(p, rec);
  if (rec) {
    return false;
  }
  auto target_str = target.generic_string();
  auto root_str = root_real.generic_string();
  if (!target_str.starts_with(root_str)) {
    return true;
  }
  return target_str.size() > root_str.size() &&
         target_str[root_str.size()] != '/';
}

auto handle_symlink(const fs::directory_entry& /*entry*/, const fs::path& p,
                    const std::string& rel, const fs::path& root_real,
                    Reporter& r) -> void {
  bool escapes = symlink_escapes_root(p, root_real);
  r.error("forbidden-paths",
          escapes ? "symlink escapes package root: " + rel
                  : "symlinks are forbidden: " + rel,
          rel, "remove the symlink and include the target content directly");
}

auto handle_directory(const fs::path& p, const fs::path& root,
                      const std::string& name, const std::string& rel,
                      Reporter& r) -> void {
  if (p == root) {
    return;
  }
  if (p.parent_path() == root && !is_layout_dir(name)) {
    r.error("forbidden-paths", "unexpected top-level directory: " + rel, rel,
            "move the content under server/, client/, or migrations/");
  }
}

auto handle_regular_file(const fs::path& p, const fs::path& root,
                         const std::string& name, const std::string& rel,
                         const ValidationConfig& cfg, Reporter& r,
                         std::size_t& files_scanned, std::size_t& total_bytes,
                         bool& size_exceeded) -> void {
  if (p.parent_path() == root && !is_known_root_file(name)) {
    r.error(
        "forbidden-paths", "unexpected top-level file: " + rel, rel,
        "move the file under server/, client/, or migrations/, or remove it");
  }
  std::error_code sec;
  auto sz = fs::file_size(p, sec);
  if (sec) {
    return;
  }
  total_bytes += static_cast<std::size_t>(sz);
  ++files_scanned;
  if (!size_exceeded && total_bytes > cfg.max_size_bytes) {
    r.error("size-limit",
            "package exceeds configured maximum size (" +
                std::to_string(cfg.max_size_bytes) + " bytes)",
            std::nullopt,
            "reduce package contents or pass --max-size <bytes> to override");
    size_exceeded = true;
  }
}

// R2 + R6 — walk the tree: detect symlinks, stray files, layout violations,
// and accumulate size. Short-circuits R6 the moment size is exceeded.
auto run_walk(const fs::path& root, const ValidationConfig& cfg, Reporter& r,
              std::size_t& files_scanned, std::size_t& total_bytes) -> void {
  std::error_code ec;
  auto root_real = fs::weakly_canonical(root, ec);
  if (ec) {
    r.error("input", "cannot canonicalize package root (" + ec.message() + ")",
            root.generic_string());
    return;
  }

  // Use recursive_directory_iterator; disable following symlinks; handle
  // skipping VCS dirs manually via disable_recursion_pending.
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    r.error("input", "cannot walk package tree (" + ec.message() + ")",
            root.generic_string());
    return;
  }
  fs::recursive_directory_iterator end;

  bool size_exceeded = false;

  for (; it != end; it.increment(ec)) {
    if (ec) {
      r.warn("filesystem-race",
             "filesystem race while scanning (" + ec.message() + ")");
      ec.clear();
      continue;
    }
    const auto& entry = *it;
    const auto& p = entry.path();
    auto rel = relative_string(root, p);
    auto name = p.filename().generic_string();

    // VCS directories: skip descent + do not count.
    if (entry.is_directory(ec) && is_vcs_dir(name) && p != root) {
      it.disable_recursion_pending();
      continue;
    }

    // R2: symlinks → error (no following either way).
    if (entry.is_symlink(ec)) {
      handle_symlink(entry, p, rel, root_real, r);
      if (entry.is_directory(ec)) {
        it.disable_recursion_pending();
      }
      continue;
    }

    if (entry.is_directory(ec)) {
      handle_directory(p, root, name, rel, r);
      continue;
    }

    if (!entry.is_regular_file(ec)) {
      continue;
    }

    handle_regular_file(p, root, name, rel, cfg, r, files_scanned, total_bytes,
                        size_exceeded);
  }
}

// R3 — JSON parse for known files. Uses 0.4.1 parsers for the two
// required files and 0.4.2 typed parsers for panels.json and
// config.json; rbac.json stays as raw JSON until 0.4.6. The resulting
// ParsedPackage is threaded through both the 0.4.0 R-rules and the
// 0.4.2 cross-file pass (parse-once discipline per ICD-0.4.2
// §Implementation Hints).

auto forward_manifest_messages(const std::vector<ManifestParseError>& messages,
                               const fs::path& root, const fs::path& fpath,
                               Reporter& r) -> void {
  auto base = relative_string(root, fpath);
  for (const auto& m : messages) {
    r.out->push_back(ValidationMessage{
        .severity = m.severity,
        .phase = Phase::STRUCTURE,
        .rule = m.rule,
        .path = m.field_path.has_value()
                    ? std::optional<std::string>(base + m.field_path.value())
                    : std::optional<std::string>(base),
        .message = m.message,
        .remediation = m.remediation,
    });
  }
}

auto parse_required_manifest(const fs::path& root, bool is_bundled,
                             ParsedPackage& parsed, Reporter& r) -> void {
  auto mpath = root / "manifest.json";
  if (!fs::exists(mpath)) {
    return;
  }
  auto bytes = read_file_bytes(mpath);
  if (!bytes) {
    r.error("json-structure", "unable to read manifest.json",
            relative_string(root, mpath));
    return;
  }
  auto res = PackageManifest::parse(*bytes, "manifest.json", is_bundled);
  forward_manifest_messages(res.messages, root, mpath, r);
  if (res.value) {
    parsed.manifest = std::move(*res.value);
  }
}

auto parse_required_capabilities(const fs::path& root, ParsedPackage& parsed,
                                 Reporter& r) -> void {
  auto cpath = root / "capabilities.json";
  if (!fs::exists(cpath)) {
    return;
  }
  auto bytes = read_file_bytes(cpath);
  if (!bytes) {
    r.error("json-structure", "unable to read capabilities.json",
            relative_string(root, cpath));
    return;
  }
  auto res = CapabilityManifest::parse(*bytes, "capabilities.json");
  forward_manifest_messages(res.messages, root, cpath, r);
  if (res.value) {
    parsed.capabilities = std::move(*res.value);
  }
}

auto parse_panels_typed(const fs::path& root, const fs::path& op,
                        ParsedPackage& parsed, Reporter& r) -> void {
  auto bytes = read_file_bytes(op);
  if (!bytes) {
    r.error("json-structure", "unable to read panels.json",
            relative_string(root, op));
    return;
  }
  auto res = PanelsManifest::parse(*bytes, "panels.json");
  forward_manifest_messages(res.messages, root, op, r);
  if (res.value) {
    parsed.panels = std::move(*res.value);
  }
}

auto parse_config_typed(const fs::path& root, const fs::path& op,
                        ParsedPackage& parsed, Reporter& r) -> void {
  auto bytes = read_file_bytes(op);
  if (!bytes) {
    r.error("json-structure", "unable to read config.json",
            relative_string(root, op));
    return;
  }
  auto res = ConfigManifest::parse(*bytes, "config.json");
  forward_manifest_messages(res.messages, root, op, r);
  if (res.value) {
    parsed.config = std::move(*res.value);
  }
}

auto parse_rbac_raw(const fs::path& root, const fs::path& op,
                    ParsedPackage& parsed, Reporter& r) -> void {
  auto bytes = read_file_bytes(op);
  if (!bytes) {
    r.error("json-structure", "unable to read rbac.json",
            relative_string(root, op));
    return;
  }
  try {
    parsed.rbac_raw = nlohmann::json::parse(*bytes);
  } catch (const nlohmann::json::parse_error& e) {
    r.error("json-structure",
            std::string{"rbac.json does not parse: "} + e.what(),
            relative_string(root, op),
            "ensure the file is syntactically valid JSON");
  }
}

auto parse_optional_files(const fs::path& root, ParsedPackage& parsed,
                          Reporter& r) -> void {
  auto rbac_path = root / "rbac.json";
  if (fs::exists(rbac_path)) {
    parse_rbac_raw(root, rbac_path, parsed, r);
  }
  auto panels_path = root / "panels.json";
  if (fs::exists(panels_path)) {
    parse_panels_typed(root, panels_path, parsed, r);
  }
  auto config_path = root / "config.json";
  if (fs::exists(config_path)) {
    parse_config_typed(root, config_path, parsed, r);
  }
}

auto run_json_parse(const fs::path& root, bool is_bundled, Reporter& r)
    -> ParsedPackage {
  ParsedPackage parsed;
  parse_required_manifest(root, is_bundled, parsed, r);
  parse_required_capabilities(root, parsed, r);
  parse_optional_files(root, parsed, r);
  return parsed;
}

// R4 — every capability has a handler file under server/handlers/.
// Suppressed when cross-file is on (the CF4 error at
// cross_file_validator.cpp supersedes — ICD-0.4.2 §Open Questions #4).
auto run_handler_files(const fs::path& root, const ParsedPackage& parsed,
                       const ValidationConfig& cfg, Reporter& r) -> void {
  if (!parsed.capabilities || cfg.cross_file) {
    return;
  }
  for (const auto& pc : parsed.capabilities->provides) {
    if (pc.function.empty()) {
      continue;
    }
    auto handler = root / "server" / "handlers" / (pc.function + ".js");
    if (!fs::exists(handler)) {
      r.warn("handler-missing",
             "capabilities.json declares " + pc.namespace_ + ":" +
                 std::to_string(pc.version) + ":" + pc.function +
                 " but server/handlers/" + pc.function + ".js not found",
             "server/handlers/" + pc.function + ".js",
             "create server/handlers/" + pc.function +
                 ".js, or remove the capability from capabilities.json");
    }
  }
}

// ─── ICD-0.4.1 Layer 1 — Unicode-smuggle scan pass ─────────────────────
//
// Explicit allowlist (ICD §Files Scanned). Implementation must NOT
// derive the scan list by negating a binary-asset list — inversion is
// fragile. Three sub-allowlists stitched together: root-JSON files,
// directory-prefix + extension whitelist, and the manifest entry_point.

constexpr std::array<std::string_view, 5> SCAN_ROOT_FILES{
    "manifest.json", "capabilities.json", "rbac.json",
    "panels.json",   "config.json",
};

constexpr std::array<std::string_view, 1> SCAN_HANDLERS_EXTS{".js"};

constexpr std::array<std::string_view, 3> SCAN_CLIENT_EXTS{
    ".js",
    ".css",
    ".html",
};

auto extension_in(const fs::path& p, std::span<const std::string_view> exts)
    -> bool {
  auto ext = p.extension().generic_string();
  return std::ranges::any_of(exts,
                             [&](std::string_view e) { return e == ext; });
}

auto path_under(const fs::path& root, std::string_view rel) -> fs::path {
  return root / fs::path{rel};
}

// ICD-0.4.1 §Audit / Rate-limit. 1 Hz token bucket per
// (layer, source_path). File-scoped because Layer 1 only runs from the
// `validate` CLI (single-threaded) — no mutex is needed. A future
// kernel installer calling validate() from a worker thread would need
// to add one (cross-reference Layer 2's mutex-guarded LRU in
// eval_guard.cpp). The audit writer itself is gated on g_audit_ready,
// so this LRU's effects are observable only when run inside a kernel
// process that has called plinth::log::init() (CLI never does).
constexpr std::size_t L1_RATE_LIMIT_CAP = 64;

struct L1RateEntry {
  std::chrono::steady_clock::time_point last_emit;
  std::size_t suppressed_since_last = 0;
};

struct L1RateLimiter {
  std::list<std::string> order;
  std::unordered_map<std::string, L1RateEntry> entries;
  std::unordered_map<std::string, std::list<std::string>::iterator> positions;

  auto touch(const std::string& key) -> L1RateEntry& {
    if (auto it = positions.find(key); it != positions.end()) {
      order.splice(order.end(), order, it->second);
      return entries[key];
    }
    if (entries.size() >= L1_RATE_LIMIT_CAP) {
      const auto& victim = order.front();
      entries.erase(victim);
      positions.erase(victim);
      order.pop_front();
    }
    order.push_back(key);
    positions[key] = std::prev(order.end());
    return entries[key];
  }
};

// process-wide audit-emit rate limit. Layer 1 runs only from the
// single-threaded `validate` CLI today, so no mutex; future kernel installer
// (0.4.4) calling validate() from a worker thread will need to add one
// (cross-reference Layer 2's mutex-guarded LRU in eval_guard.cpp).
L1RateLimiter g_l1_rate;

auto build_l1_audit_detail(std::string_view path,
                           const plinth::security::UnicodeScanResult& r,
                           std::size_t threshold) -> Json::Value {
  Json::Value d{Json::objectValue};
  d["layer"] = "install";
  d["source_path"] = std::string{path};
  d["total_count"] = static_cast<Json::UInt64>(r.total_count);
  d["threshold"] = static_cast<Json::UInt64>(threshold);
  Json::Value findings{Json::arrayValue};
  for (const auto& f : r.first_findings) {
    Json::Value entry{Json::objectValue};
    entry["byte_offset"] = static_cast<Json::UInt64>(f.byte_offset);
    entry["codepoint"] = static_cast<Json::UInt64>(f.codepoint);
    entry["range_name"] = std::string{f.range_name};
    findings.append(entry);
  }
  d["first_findings"] = findings;
  d["decode_error"] = r.decode_error.has_value() ? Json::Value{*r.decode_error}
                                                 : Json::Value{Json::nullValue};
  return d;
}

auto emit_l1_audit(std::string_view path,
                   const plinth::security::UnicodeScanResult& r,
                   std::size_t threshold, bool log_findings) -> void {
  spdlog::warn(
      "unicode-smuggle detected: layer=install path={} count={} threshold={}",
      std::string{path}, r.total_count, threshold);
  if (!log_findings) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  std::string key{"install|"};
  key += path;
  auto& entry = g_l1_rate.touch(key);
  bool window_open = entry.last_emit.time_since_epoch().count() == 0 ||
                     (now - entry.last_emit) >= std::chrono::seconds{1};
  if (!window_open) {
    ++entry.suppressed_since_last;
    return;
  }
  if (entry.suppressed_since_last > 0) {
    Json::Value rl{Json::objectValue};
    rl["layer"] = "install";
    rl["source_path"] = std::string{path};
    rl["suppressed_count"] =
        static_cast<Json::UInt64>(entry.suppressed_since_last);
    plinth::log::audit("security.unicode_smuggle_rate_limited", rl,
                       plinth::log::AuditCtx{});
    entry.suppressed_since_last = 0;
  }
  plinth::log::audit("security.unicode_smuggle_detected",
                     build_l1_audit_detail(path, r, threshold),
                     plinth::log::AuditCtx{});
  entry.last_emit = now;
}

auto scan_one_file(const fs::path& file, std::string_view rel,
                   std::size_t threshold, bool log_findings, Reporter& r)
    -> void {
  auto bytes = read_file_bytes(file);
  if (!bytes) {
    return; // already-missing files are R1's concern, not ours
  }
  plinth::security::UnicodeScanConfig sc{.threshold = threshold};
  auto result = plinth::security::scan_for_invisible_unicode(*bytes, sc);
  if (!result.exceeds_threshold) {
    return;
  }
  emit_l1_audit(rel, result, threshold, log_findings);
  if (result.decode_error.has_value()) {
    r.error("unicode-smuggle",
            std::string{rel} + " failed UTF-8 decode (" + *result.decode_error +
                ")",
            std::string{rel},
            "the package source is not valid UTF-8. Re-export from your editor "
            "with UTF-8 encoding, or inspect the file at the cited offset");
    return;
  }
  auto first_offset = result.first_findings.empty()
                          ? 0UL
                          : result.first_findings.front().byte_offset;
  auto first_range = result.first_findings.empty()
                         ? std::string_view{"unknown"}
                         : result.first_findings.front().range_name;
  r.error("unicode-smuggle",
          std::string{rel} + " contains " + std::to_string(result.total_count) +
              " invisible Unicode characters (threshold " +
              std::to_string(threshold) + "); first finding at offset " +
              std::to_string(first_offset) + " — range '" +
              std::string{first_range} + "'",
          std::string{rel},
          "inspect with `iconv -f utf-8 -t ascii//TRANSLIT` or a Unicode-aware "
          "hex dump; legitimate emoji rarely contribute more than a handful of "
          "findings");
}

auto scan_directory_pattern(const fs::path& root, std::string_view subdir,
                            std::span<const std::string_view> exts,
                            std::size_t threshold, bool log_findings,
                            Reporter& r, bool recursive) -> void {
  auto dir = path_under(root, subdir);
  std::error_code ec;
  if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
    return;
  }
  auto walk = [&](auto&& iter_factory) {
    for (auto it = iter_factory(); it != decltype(it){}; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      const auto& entry = *it;
      if (entry.is_symlink(ec) || !entry.is_regular_file(ec)) {
        continue;
      }
      if (!extension_in(entry.path(), exts)) {
        continue;
      }
      auto rel = relative_string(root, entry.path());
      scan_one_file(entry.path(), rel, threshold, log_findings, r);
    }
  };
  if (recursive) {
    walk([&] {
      return fs::recursive_directory_iterator(
          dir, fs::directory_options::skip_permission_denied, ec);
    });
  } else {
    walk([&] {
      return fs::directory_iterator(
          dir, fs::directory_options::skip_permission_denied, ec);
    });
  }
}

auto scan_entry_point(const fs::path& root, const ParsedPackage& parsed,
                      std::size_t threshold, bool log_findings, Reporter& r)
    -> void {
  if (!parsed.manifest || parsed.manifest->entry_point.empty()) {
    return;
  }
  const auto& ep = parsed.manifest->entry_point;
  auto file = path_under(root, ep);
  std::error_code ec;
  if (!fs::exists(file, ec) || !fs::is_regular_file(file, ec)) {
    return;
  }
  if (!extension_in(file, SCAN_HANDLERS_EXTS)) {
    return;
  }
  scan_one_file(file, ep, threshold, log_findings, r);
}

auto run_unicode_scan_pass(const fs::path& root, const ValidationConfig& cfg,
                           const ParsedPackage& parsed, Reporter& r) -> void {
  if (!cfg.unicode_scanner_enabled) {
    return;
  }
  auto threshold = cfg.unicode_scanner_threshold;
  auto log_findings = cfg.unicode_scanner_log_findings;
  // Root JSON files (any of the five may be absent).
  for (auto name : SCAN_ROOT_FILES) {
    auto p = root / fs::path{name};
    std::error_code ec;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      scan_one_file(p, name, threshold, log_findings, r);
    }
  }
  // server/handlers/*.js (recursive within handlers/).
  scan_directory_pattern(root, "server/handlers", SCAN_HANDLERS_EXTS, threshold,
                         log_findings, r, /*recursive=*/true);
  // server/main.js (entry_point — derived from manifest, single file).
  scan_entry_point(root, parsed, threshold, log_findings, r);
  // client/panels/*.{js,css,html} and client/components/*.{js,css,html}.
  scan_directory_pattern(root, "client/panels", SCAN_CLIENT_EXTS, threshold,
                         log_findings, r, /*recursive=*/true);
  scan_directory_pattern(root, "client/components", SCAN_CLIENT_EXTS, threshold,
                         log_findings, r, /*recursive=*/true);
}

// R5 — panels.json entries with client_path reference an existing file.
// Suppressed when cross-file is on (CF3 covers the same check).
auto run_panel_files(const fs::path& root, const ParsedPackage& parsed,
                     const ValidationConfig& cfg, Reporter& r) -> void {
  if (!parsed.panels || cfg.cross_file) {
    return;
  }
  for (const auto& pe : parsed.panels->panels) {
    auto under_panels = root / "client" / "panels" / pe.client_path;
    auto under_components = root / "client" / "components" / pe.client_path;
    if (fs::exists(under_panels) || fs::exists(under_components)) {
      continue;
    }
    r.error(
        "panel-missing",
        "panels.json references '" + pe.client_path +
            "' but neither client/panels/" + pe.client_path +
            " nor client/components/" + pe.client_path + " exists",
        "client/panels/" + pe.client_path,
        "create the missing file under client/panels/ or client/components/");
  }
}

} // namespace

auto validate(const fs::path& package_root, const ValidationConfig& cfg)
    -> ValidationReport {
  ValidationReport report;
  Reporter r{.out = &report.messages};

  if (!run_input_preflight(package_root, r)) {
    return report;
  }

  run_required_files(package_root, r);
  run_walk(package_root, cfg, r, report.files_scanned, report.total_bytes);
  auto parsed = run_json_parse(package_root, cfg.is_bundled, r);
  run_unicode_scan_pass(package_root, cfg, parsed, r);
  run_handler_files(package_root, parsed, cfg, r);
  run_panel_files(package_root, parsed, cfg, r);

  if (cfg.cross_file) {
    run_cross_file_validation(parsed, package_root, cfg, r);
  }
  if (cfg.against_running_kernel) {
    auto cross_file_errors = static_cast<std::size_t>(std::count_if(
        report.messages.begin(), report.messages.end(), [](const auto& m) {
          return m.phase == Phase::CROSS_FILE && m.severity == Severity::ERROR;
        }));
    if (cross_file_errors > 0) {
      r.warn("runtime-state-skipped",
             "runtime-state validation skipped: cross-file phase "
             "reported " +
                 std::to_string(cross_file_errors) + " errors",
             std::nullopt, "fix the cross-file errors above and re-run",
             Phase::RUNTIME_STATE);
    } else {
      run_runtime_state_validation(parsed, cfg, r);
    }
  }

  return report;
}

namespace {

constexpr std::string_view ANSI_RED = "\x1b[31m";
constexpr std::string_view ANSI_YELLOW = "\x1b[33m";
constexpr std::string_view ANSI_RESET = "\x1b[0m";

auto colour_for(Severity s) -> std::string_view {
  return s == Severity::ERROR ? ANSI_RED : ANSI_YELLOW;
}

auto label_for(Severity s) -> std::string_view {
  return s == Severity::ERROR ? "error" : "warning";
}

} // namespace

auto render_text(const ValidationReport& report, const fs::path& package_root,
                 std::ostream& out, const RenderOptions& opts) -> void {
  if (opts.quiet) {
    return;
  }
  for (const auto& m : report.messages) {
    if (opts.colour) {
      out << colour_for(m.severity) << label_for(m.severity) << ANSI_RESET;
    } else {
      out << label_for(m.severity);
    }
    out << ": " << m.rule << ": " << m.message;
    if (m.path) {
      out << " (" << *m.path << ")";
    }
    out << '\n';
    if (m.remediation) {
      out << "  hint: " << *m.remediation << '\n';
    }
  }
  (void)package_root;
  out << "validated " << report.files_scanned << " files, "
      << report.error_count() << " errors, " << report.warning_count()
      << " warnings\n";
}

namespace {

auto phase_str(Phase p) -> std::string_view {
  switch (p) {
    case Phase::STRUCTURE: return "structure";
    case Phase::CROSS_FILE: return "cross_file";
    case Phase::RUNTIME_STATE: return "runtime_state";
  }
  return "structure";
}

} // namespace

auto render_json(const ValidationReport& report, const fs::path& package_root,
                 std::ostream& out) -> void {
  std::error_code ec;
  auto abs_path = fs::absolute(package_root, ec);
  auto path_str =
      ec ? package_root.generic_string() : abs_path.generic_string();
  auto j = nlohmann::json::object();
  j["path"] = path_str;
  j["exit_code"] = report.disposition();
  j["files_scanned"] = report.files_scanned;
  j["total_bytes"] = report.total_bytes;
  auto arr = nlohmann::json::array();
  for (const auto& m : report.messages) {
    auto obj = nlohmann::json::object();
    obj["severity"] = m.severity == Severity::ERROR ? "error" : "warning";
    obj["phase"] = std::string{phase_str(m.phase)};
    obj["rule"] = m.rule;
    obj["path"] =
        m.path.has_value() ? nlohmann::json(*m.path) : nlohmann::json(nullptr);
    obj["message"] = m.message;
    obj["remediation"] = m.remediation.has_value()
                             ? nlohmann::json(*m.remediation)
                             : nlohmann::json(nullptr);
    arr.push_back(std::move(obj));
  }
  j["messages"] = std::move(arr);
  out << j.dump() << '\n';
}

} // namespace plinth::packages
