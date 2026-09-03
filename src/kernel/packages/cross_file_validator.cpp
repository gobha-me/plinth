#include "kernel/packages/cross_file_validator.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/js/runtime_config.hpp"
#include "kernel/packages/detail/reporter.hpp"
#include "kernel/packages/reserved_names.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace plinth::packages {

namespace {

using detail::Reporter;

// Reserved HTTP mount prefixes (CF5). Source of truth:
// architecture/05-extensions.md §2. Order does not matter — the
// check is a linear scan, not a trie.
constexpr std::array<std::string_view, 6> RESERVED_MOUNTS{
    "/api", "/ext", "/s", "/healthz", "/metrics", "/ws",
};

auto normalize_mount(std::string_view s) -> std::string {
  if (s.empty()) {
    return {};
  }
  std::string out{s};
  // Strip trailing slash except the root "/" sentinel.
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

auto mount_collides(std::string_view mount) -> bool {
  if (mount.empty()) {
    return false;
  }
  auto norm = normalize_mount(mount);
  if (norm == "/") {
    return false; // root redirect is configurable; permitted here
  }
  for (auto reserved : RESERVED_MOUNTS) {
    if (norm == reserved) {
      return true;
    }
    std::string prefix{reserved};
    prefix.push_back('/');
    if (norm.starts_with(prefix)) {
      return true;
    }
  }
  return false;
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

// Match the validator.cpp helper so CF3 agrees with R2's symlink
// discipline. Duplicated here instead of promoted to the detail header
// because the CF3 path is the only cross-file caller and the check is
// three lines.
auto path_within_root(const fs::path& root_real, const fs::path& candidate)
    -> bool {
  std::error_code ec;
  auto resolved = fs::weakly_canonical(candidate, ec);
  if (ec) {
    return false;
  }
  auto rstr = resolved.generic_string();
  auto roots = root_real.generic_string();
  if (!rstr.starts_with(roots)) {
    return false;
  }
  return rstr.size() == roots.size() || rstr[roots.size()] == '/';
}

// ─── CF1 — rbac rule namespace matches a provided capability ─────────

auto cf1_rbac_orphan_namespace(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.rbac_raw || !pkg.capabilities) {
    return;
  }
  if (!pkg.rbac_raw->is_object() || !pkg.rbac_raw->contains("rules")) {
    return;
  }
  const auto& rules = (*pkg.rbac_raw)["rules"];
  if (!rules.is_array()) {
    r.error("rbac-json-shape", "'rules' must be an array", "rbac.json",
            std::nullopt, Phase::CROSS_FILE);
    return;
  }
  std::set<std::string> provided;
  for (const auto& pc : pkg.capabilities->provides) {
    provided.insert(pc.namespace_);
  }
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const auto& rule = rules[i];
    if (!rule.is_object() || !rule.contains("namespace") ||
        !rule["namespace"].is_string()) {
      r.error("rbac-json-shape",
              "rules[" + std::to_string(i) +
                  "] is missing a string 'namespace'",
              "rbac.json", std::nullopt, Phase::CROSS_FILE);
      continue;
    }
    auto ns = rule["namespace"].get<std::string>();
    if (is_reserved_kernel_namespace(ns) || provided.contains(ns)) {
      continue;
    }
    r.error("rbac-orphan-namespace",
            "rbac.json rules[" + std::to_string(i) + "] namespace '" + ns +
                "' does not match any capability namespace",
            "rbac.json",
            "rename the rule's namespace to match a provides[] entry in "
            "capabilities.json, or add the capability",
            Phase::CROSS_FILE);
  }
}

// ─── CF2 — rbac test-contract call is a real capability ─────────────

auto cf2_rbac_test_call(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.rbac_raw || !pkg.capabilities) {
    return;
  }
  if (!pkg.rbac_raw->is_object() || !pkg.rbac_raw->contains("rules")) {
    return;
  }
  const auto& rules = (*pkg.rbac_raw)["rules"];
  if (!rules.is_array()) {
    return; // CF1 emitted the shape error
  }
  for (std::size_t i = 0; i < rules.size(); ++i) {
    const auto& rule = rules[i];
    if (!rule.is_object() || !rule.contains("test")) {
      continue;
    }
    const auto& test = rule["test"];
    if (!test.is_object() || !test.contains("call") ||
        !test["call"].is_string()) {
      continue;
    }
    auto call = test["call"].get<std::string>();
    auto parsed = plinth::capabilities::parse_signature(call);
    if (std::holds_alternative<plinth::capabilities::CapabilityError>(parsed)) {
      r.error("rbac-test-call-parse",
              "rules[" + std::to_string(i) + "].test.call '" + call +
                  "' is not a valid capability signature",
              "rbac.json",
              "format: <namespace>:<version>:<function> (e.g. notes:1:create)",
              Phase::CROSS_FILE);
      continue;
    }
    const auto& sig = std::get<plinth::capabilities::ParsedSignature>(parsed);
    bool hit =
        std::ranges::any_of(pkg.capabilities->provides, [&](const auto& pc) {
          return pc.namespace_ == sig.namespace_ && pc.version == sig.version &&
                 pc.function == sig.function;
        });
    if (!hit) {
      r.error("rbac-test-call",
              "rules[" + std::to_string(i) + "].test.call '" + call +
                  "' does not match any provides[] entry",
              "rbac.json",
              "add the capability to capabilities.json or fix the "
              "signature to match an existing provides[] entry",
              Phase::CROSS_FILE);
    }
  }
}

// ─── CF3 — panel client_path resolves under client/{panels,components}/ ──

auto cf3_panel_client_files(const ParsedPackage& pkg,
                            const fs::path& package_root, Reporter& r) -> void {
  if (!pkg.panels) {
    return;
  }
  std::error_code ec;
  auto root_real = fs::weakly_canonical(package_root, ec);
  if (ec) {
    return;
  }
  for (std::size_t i = 0; i < pkg.panels->panels.size(); ++i) {
    const auto& pe = pkg.panels->panels[i];
    auto under_panels = package_root / "client" / "panels" / pe.client_path;
    auto under_components =
        package_root / "client" / "components" / pe.client_path;
    bool ok_panels = fs::is_regular_file(under_panels, ec) &&
                     path_within_root(root_real, under_panels);
    bool ok_components = fs::is_regular_file(under_components, ec) &&
                         path_within_root(root_real, under_components);
    if (ok_panels || ok_components) {
      continue;
    }
    r.error("panel-missing-client-file",
            "panels[" + std::to_string(i) + "].client_path '" + pe.client_path +
                "' resolves to neither client/panels/" + pe.client_path +
                " nor client/components/" + pe.client_path,
            "panels.json",
            "create the missing file under client/panels/ or "
            "client/components/",
            Phase::CROSS_FILE);
  }
}

// ─── CF4 — handler file exists for every provided capability ─────────

auto cf4_handler_files(const ParsedPackage& pkg, const fs::path& package_root,
                       Reporter& r) -> void {
  if (!pkg.capabilities) {
    return;
  }
  std::error_code ec;
  for (const auto& pc : pkg.capabilities->provides) {
    if (pc.function.empty()) {
      continue;
    }
    auto handler = package_root / "server" / "handlers" / (pc.function + ".js");
    if (fs::is_regular_file(handler, ec)) {
      continue;
    }
    r.error("cf4-handler-missing",
            "capabilities.json declares " + pc.namespace_ + ":" +
                std::to_string(pc.version) + ":" + pc.function +
                " but server/handlers/" + pc.function + ".js does not exist",
            "server/handlers/" + pc.function + ".js",
            "create the handler file, or remove the provides[] entry",
            Phase::CROSS_FILE);
  }
}

// ─── CF5 — frontend.mount not on reserved prefix ─────────────────────

auto cf5_frontend_mount_reserved(const ParsedPackage& pkg, Reporter& r)
    -> void {
  if (!pkg.manifest || !pkg.manifest->frontend) {
    return;
  }
  const auto& m = pkg.manifest->frontend->mount;
  if (!mount_collides(m)) {
    return;
  }
  r.error("frontend-mount-reserved",
          "manifest.frontend.mount '" + m +
              "' overlaps a kernel-reserved prefix",
          "manifest.json",
          "choose a mount that does not start with /api, /ext, /s, "
          "/healthz, /metrics, or /ws",
          Phase::CROSS_FILE);
}

// ─── CF6 — name not reserved ─────────────────────────────────────────

auto cf6_name_reserved(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.manifest) {
    return;
  }
  if (!is_reserved_kernel_namespace(pkg.manifest->name)) {
    return;
  }
  r.error("name-reserved",
          "manifest.name '" + pkg.manifest->name +
              "' is a kernel-reserved identifier",
          "manifest.json",
          "pick a different package name (kernel/plinth/system are reserved)",
          Phase::CROSS_FILE);
}

// ─── CF7 — capability namespace matches package name ────────────────

auto cf7_namespace_matches_name(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.manifest || !pkg.capabilities) {
    return;
  }
  // provider_extension gate reserved but unconditional in 0.4.2 per
  // ICD-0.4.2 §Open Questions #2.
  (void)pkg.manifest->provider_extension;
  for (std::size_t i = 0; i < pkg.capabilities->provides.size(); ++i) {
    const auto& pc = pkg.capabilities->provides[i];
    if (pc.namespace_ == pkg.manifest->name) {
      continue;
    }
    r.error("capability-namespace-mismatch",
            "provides[" + std::to_string(i) + "].namespace '" + pc.namespace_ +
                "' does not equal manifest.name '" + pkg.manifest->name + "'",
            "capabilities.json",
            "set the provides[] namespace to match the package name "
            "(provider_extension opt-in is not yet available)",
            Phase::CROSS_FILE);
  }
}

// ─── CFW1 — every provided capability has an RBAC rule ──────────────

auto cfw1_capability_without_rule(const ParsedPackage& pkg, Reporter& r)
    -> void {
  if (!pkg.capabilities || !pkg.rbac_raw) {
    // If rbac.json is absent AND there are provides, every capability
    // is without a rule. Emit one warning per provides[] entry.
    if (pkg.capabilities && !pkg.rbac_raw) {
      for (std::size_t i = 0; i < pkg.capabilities->provides.size(); ++i) {
        const auto& pc = pkg.capabilities->provides[i];
        r.warn("capability-without-rule",
               "provides[" + std::to_string(i) + "] '" + pc.namespace_ + ":" +
                   std::to_string(pc.version) + ":" + pc.function +
                   "' has no RBAC rule (no rbac.json)",
               "capabilities.json",
               "add rbac.json with a rule whose namespace matches "
               "this capability",
               Phase::CROSS_FILE);
      }
    }
    return;
  }
  if (!pkg.rbac_raw->is_object() || !pkg.rbac_raw->contains("rules")) {
    return;
  }
  const auto& rules = (*pkg.rbac_raw)["rules"];
  if (!rules.is_array()) {
    return;
  }
  std::set<std::string> ruled;
  for (const auto& rule : rules) {
    if (!rule.is_object() || !rule.contains("namespace") ||
        !rule["namespace"].is_string()) {
      continue;
    }
    ruled.insert(rule["namespace"].get<std::string>());
  }
  for (std::size_t i = 0; i < pkg.capabilities->provides.size(); ++i) {
    const auto& pc = pkg.capabilities->provides[i];
    if (ruled.contains(pc.namespace_)) {
      continue;
    }
    r.warn("capability-without-rule",
           "provides[" + std::to_string(i) + "] '" + pc.namespace_ + ":" +
               std::to_string(pc.version) + ":" + pc.function +
               "' has no matching rbac.json rule",
           "capabilities.json",
           "add a rule to rbac.json whose namespace matches '" + pc.namespace_ +
               "'",
           Phase::CROSS_FILE);
  }
}

// ─── CFW2 — shareable[] non-empty warning ───────────────────────────

auto cfw2_shareable_non_empty(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.manifest || pkg.manifest->shareable.empty()) {
    return;
  }
  r.warn("shareable-non-empty",
         "manifest.shareable is reserved for the deferred share primitive "
         "(ROADMAP 0.11); entries will be ignored at install time",
         "manifest.json",
         "leave shareable as [] until the share primitive ships",
         Phase::CROSS_FILE);
}

// ─── CFW3 — entry_point imports from client/ ─────────────────────────

auto cfw3_entry_imports_client(const ParsedPackage& pkg,
                               const fs::path& package_root, Reporter& r)
    -> void {
  if (!pkg.manifest || pkg.manifest->entry_point.empty()) {
    return;
  }
  auto ep = package_root / pkg.manifest->entry_point;
  std::error_code ec;
  if (!fs::is_regular_file(ep, ec)) {
    return;
  }
  auto bytes = read_file_bytes(ep);
  if (!bytes) {
    return;
  }
  // Bare-string regex. False positives on non-import literals are
  // acceptable at warning severity per ICD-0.4.2 §Open Questions #5.
  static const std::regex IMPORT_RE{
      R"(import\s+[^;]*?from\s*['"](?:\.\.?/)+client/)"};
  static const std::regex REQUIRE_RE{
      R"(require\s*\(\s*['"](?:\.\.?/)+client/)"};
  if (std::regex_search(*bytes, IMPORT_RE) ||
      std::regex_search(*bytes, REQUIRE_RE)) {
    r.warn("entry-imports-client",
           "entry_point '" + pkg.manifest->entry_point +
               "' imports from a client/ path (sign of a mis-structured "
               "package)",
           pkg.manifest->entry_point,
           "keep server and client code separate; re-export shared "
           "constants instead of importing the client bundle",
           Phase::CROSS_FILE);
  }
}

// ─── CFW4 — runtime.memory_limit_mb > kernel max ────────────────────

auto cfw4_memory_over_max(const ParsedPackage& pkg, Reporter& r) -> void {
  if (!pkg.manifest || !pkg.manifest->runtime ||
      !pkg.manifest->runtime->memory_limit_mb) {
    return;
  }
  auto requested = *pkg.manifest->runtime->memory_limit_mb;
  auto kernel_max = plinth::js::runtime_config::get_max_memory_mb();
  if (requested <= kernel_max) {
    return;
  }
  r.warn("memory-over-max",
         "manifest.runtime.memory_limit_mb = " + std::to_string(requested) +
             " exceeds kernel maximum (" + std::to_string(kernel_max) +
             " MB); value will be clamped at install time",
         "manifest.json",
         "reduce memory_limit_mb to at most " + std::to_string(kernel_max) +
             " MB",
         Phase::CROSS_FILE);
}

// ─── Runtime-state: RT1, RT2, RT3 ────────────────────────────────────
//
// Inline per ICD Open Question #4. 0.4.2 does not yet have a live
// HTTP path to the kernel's registry (0.4.4 scope); when
// --against-running-kernel is requested, emit the documented
// "runtime-validate-unimplemented" marker so CI can grep for the stub
// exit without hanging on a connection attempt.

auto rt_stubbed(const ParsedPackage& pkg, const ValidationConfig& cfg,
                Reporter& r) -> void {
  (void)pkg;
  std::string url = cfg.kernel_url.value_or("<unset>");
  r.error("runtime-validate-unimplemented",
          "--against-running-kernel requires POST /api/packages/validate "
          "which is not wired until 0.4.4 (kernel_url=" +
              url + ")",
          std::nullopt,
          "run without --against-running-kernel until the 0.4.4 install "
          "lifecycle lands, or target a kernel built from a 0.4.4+ branch",
          Phase::RUNTIME_STATE);
}

} // namespace

auto run_cross_file_validation(const ParsedPackage& pkg,
                               const fs::path& package_root,
                               const ValidationConfig& cfg, Reporter& r)
    -> void {
  (void)cfg;
  bool have_manifest = pkg.manifest.has_value();
  bool have_caps = pkg.capabilities.has_value();
  if (!have_manifest || !have_caps) {
    r.warn("cross-file-skipped",
           std::string{"cross-file validation skipped: "} +
               (!have_manifest ? "manifest.json did not parse; " : "") +
               (!have_caps ? "capabilities.json did not parse" : ""),
           std::nullopt, "fix the parse errors emitted in the structure phase",
           Phase::CROSS_FILE);
    return;
  }
  cf1_rbac_orphan_namespace(pkg, r);
  cf2_rbac_test_call(pkg, r);
  cf3_panel_client_files(pkg, package_root, r);
  cf4_handler_files(pkg, package_root, r);
  cf5_frontend_mount_reserved(pkg, r);
  cf6_name_reserved(pkg, r);
  cf7_namespace_matches_name(pkg, r);
  cfw1_capability_without_rule(pkg, r);
  cfw2_shareable_non_empty(pkg, r);
  cfw3_entry_imports_client(pkg, package_root, r);
  cfw4_memory_over_max(pkg, r);
}

auto run_runtime_state_validation(const ParsedPackage& pkg,
                                  const ValidationConfig& cfg, Reporter& r)
    -> void {
  // ICD-0.4.5 §VALIDATING: upgrade_from_id whitelists the predecessor row
  // from RT1 (name collision). Since RT1/RT2/RT3 bodies are stubbed until
  // a future milestone wires them, the whitelist presently manifests as
  // "skip the stub emission entirely when upgrade context is declared".
  if (cfg.upgrade_from_id.has_value()) {
    return;
  }
  rt_stubbed(pkg, cfg, r);
}

} // namespace plinth::packages
