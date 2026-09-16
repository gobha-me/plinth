#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include "kernel/capabilities/drain.hpp"
#include "kernel/capabilities/registration.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/capabilities/validation.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/capabilities_manifest.hpp"
#include "kernel/packages/manifest.hpp"
#include "kernel/packages/migrations.hpp"
#include "kernel/packages/panels.hpp"
#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/packages/validator.hpp"
#include "kernel/rbac/ephemeral_user.hpp"
#include "kernel/rbac/rbac_manifest.hpp"
#include "kernel/rbac/rule_registrar.hpp"
#include "kernel/rbac/rule_validator.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/emit.hpp"

#include <json/value.h>
#include <libpq-fe.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <zip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace plinth::packages {

auto provenance_to_string(Provenance p) -> std::string_view {
  switch (p) {
    case Provenance::USER: return "user";
    case Provenance::BUNDLED: return "bundled";
  }
  return "user";
}

auto stage_to_string(InstallStage s) -> std::string_view {
  switch (s) {
    case InstallStage::UPLOADING: return "UPLOADING";
    case InstallStage::VALIDATING: return "VALIDATING";
    case InstallStage::MIGRATING: return "MIGRATING";
    case InstallStage::REGISTERING: return "REGISTERING";
    case InstallStage::EXTRACTING: return "EXTRACTING";
    case InstallStage::ACTIVATING: return "ACTIVATING";
    case InstallStage::ACTIVE: return "ACTIVE";
    case InstallStage::ACTIVE_FLAGGED: return "ACTIVE_FLAGGED";
    case InstallStage::DISABLED: return "DISABLED";
    case InstallStage::INSTALL_FAILED: return "INSTALL_FAILED";
    case InstallStage::UNINSTALLING: return "UNINSTALLING";
    case InstallStage::SUPERSEDED: return "SUPERSEDED";
  }
  return "UNKNOWN";
}

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

struct PgGuard {
  PGconn* conn = nullptr;
  explicit PgGuard(const Config::Database& db) {
    conn = plinth::db::connect(plinth::db::connection_info(db).c_str());
  }
  ~PgGuard() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  PgGuard(const PgGuard&) = delete;
  auto operator=(const PgGuard&) -> PgGuard& = delete;
  PgGuard(PgGuard&&) = delete;
  auto operator=(PgGuard&&) -> PgGuard& = delete;
  [[nodiscard]] auto ok() const -> bool {
    return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
  }
};

auto uuid_v4() -> std::string {
  // Random 128-bit ID formatted as UUID v4 string. Not cryptographically
  // rigorous — installs are admin-audited and uniqueness is the only
  // requirement.
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uint64_t a = rng();
  std::uint64_t b = rng();
  a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL; // version 4
  b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL; // RFC 4122 variant
  std::array<char, 37> buf{};
  // natural fit for UUID formatting; buf.data() is deliberate pointer access.
  std::snprintf(
      buf.data(), buf.size(), "%08x-%04x-%04x-%04x-%012llx",
      static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF),
      static_cast<unsigned>(a & 0xFFFF), static_cast<unsigned>(b >> 48),
      static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
  return std::string{buf.data()};
}

auto sha256_hex(std::string_view bytes) -> std::string {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  // requires unsigned char*; std::string_view's char backing is bit-compatible.
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
         digest.data());
  static constexpr std::array<char, 17> HEX_DIGITS{'0', '1', '2', '3', '4', '5',
                                                   '6', '7', '8', '9', 'a', 'b',
                                                   'c', 'd', 'e', 'f', '\0'};
  std::string out;
  out.reserve(SHA256_DIGEST_LENGTH * 2);
  for (unsigned char c : digest) {
    out.push_back(HEX_DIGITS.at((c >> 4U) & 0x0FU));
    out.push_back(HEX_DIGITS.at(c & 0x0FU));
  }
  return out;
}

auto pg_exec(PGconn* conn, const char* sql)
    -> std::expected<void, std::string> {
  PgResultPtr res(plinth::db::exec(conn, sql), PQclear);
  auto status = PQresultStatus(res.get());
  if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto emit_applications_changed(PGconn* conn)
    -> std::expected<void, std::string> {
  Json::Value envelope(Json::objectValue);
  envelope["layer"] = "system";
  envelope["channel"] = "plinth:system:applications.changed";
  envelope["payload"] = Json::Value(Json::objectValue);
  auto emitted = plinth::realtime::emit_notify(*conn, envelope);
  if (!emitted.has_value()) {
    return std::unexpected("application catalog invalidation enqueue failed");
  }
  return {};
}

auto has_primary_panels(PGconn* conn, std::string_view package_id)
    -> std::expected<bool, std::string> {
  std::string id{package_id};
  std::array<const char*, 1> values{id.c_str()};
  PgResultPtr result(
      plinth::db::exec_params(
          conn,
          "SELECT EXISTS(SELECT 1 FROM plinth.panels "
          "WHERE package_id = $1::uuid AND panel_type = 'primary')",
          1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK ||
      PQntuples(result.get()) != 1) {
    return std::unexpected(std::string{PQresultErrorMessage(result.get())});
  }
  return std::string_view{PQgetvalue(result.get(), 0, 0)} == "t";
}

auto registered_panel_assets_ready(PGconn* conn, std::string_view package_id,
                                   const fs::path& version_root)
    -> std::expected<bool, std::string> {
  std::string id{package_id};
  std::array<const char*, 1> values{id.c_str()};
  PgResultPtr result(
      plinth::db::exec_params(
          conn,
          "SELECT declaration->>'client_path' FROM plinth.panels "
          "WHERE package_id = $1::uuid AND panel_type = 'primary'",
          1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(result.get())});
  }
  if (PQntuples(result.get()) == 0) {
    return false;
  }

  const auto panels_root = version_root / "client" / "panels";
  std::error_code ec;
  const auto canonical_root = fs::canonical(panels_root, ec);
  if (ec || !fs::is_directory(canonical_root, ec) || ec) {
    return false;
  }
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    if (PQgetisnull(result.get(), row, 0) != 0) {
      return false;
    }
    const std::string path{PQgetvalue(result.get(), row, 0)};
    if (path.empty() || path.front() == '/' || path.back() == '/' ||
        path.find('\\') != std::string::npos ||
        path.find('\0') != std::string::npos) {
      return false;
    }
    fs::path relative{path};
    for (const auto& component : relative) {
      if (component.empty() || component == "." || component == "..") {
        return false;
      }
    }
    const auto candidate = fs::weakly_canonical(canonical_root / relative, ec);
    if (ec) {
      return false;
    }
    const auto [root_end, ignored] =
        std::ranges::mismatch(canonical_root, candidate);
    (void)ignored;
    if (root_end != canonical_root.end() ||
        !fs::is_regular_file(candidate, ec) || ec) {
      return false;
    }
  }
  return true;
}

struct CapabilityAuthorityRow {
  std::string namespace_;
  int version = 0;
  std::string function;
  std::string signature;
  std::string scope;
  std::string description;
  std::string rbac_rule;

  auto operator<=>(const CapabilityAuthorityRow&) const = default;
};

auto registered_capabilities_ready(PGconn* conn, std::string_view name,
                                   const fs::path& version_root)
    -> std::expected<bool, std::string> {
  const auto manifest_path = version_root / "capabilities.json";
  std::ifstream in(manifest_path);
  if (!in) {
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto parsed = CapabilityManifest::parse(buffer.str(), manifest_path.string());
  if (!parsed.value.has_value()) {
    return false;
  }

  std::vector<CapabilityAuthorityRow> declared;
  declared.reserve(parsed.value->provides.size());
  for (const auto& cap : parsed.value->provides) {
    capabilities::CapabilityRegistration registration{
        .namespace_ = cap.namespace_,
        .version = cap.version,
        .function = cap.function,
        .provider_type = "extension",
        .extension_name = std::string{name},
        .scope = cap.scope.empty() ? std::string{"instance"} : cap.scope,
        .description = cap.description,
        .rbac_rule = cap.rbac_rule.value_or(std::string{}),
    };
    declared.push_back({
        .namespace_ = cap.namespace_,
        .version = cap.version,
        .function = cap.function,
        .signature = capabilities::make_signature(registration),
        .scope = registration.scope,
        .description = cap.description,
        .rbac_rule = registration.rbac_rule,
    });
  }
  std::ranges::sort(declared);

  std::string name_s{name};
  std::array<const char*, 1> values{name_s.c_str()};
  PgResultPtr result(
      plinth::db::exec_params(
          conn,
          "SELECT namespace, version, function, signature, scope, description, "
          "       rbac_rule, enabled, provider_type "
          "FROM plinth.capabilities WHERE extension_name = $1 "
          "ORDER BY namespace, version, function, scope",
          1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(result.get())});
  }
  if (static_cast<std::size_t>(PQntuples(result.get())) != declared.size()) {
    return false;
  }
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    const auto& expected = declared[static_cast<std::size_t>(row)];
    if (std::string_view{PQgetvalue(result.get(), row, 0)} !=
            expected.namespace_ ||
        std::stoi(PQgetvalue(result.get(), row, 1)) != expected.version ||
        std::string_view{PQgetvalue(result.get(), row, 2)} !=
            expected.function ||
        std::string_view{PQgetvalue(result.get(), row, 3)} !=
            expected.signature ||
        std::string_view{PQgetvalue(result.get(), row, 4)} != expected.scope ||
        std::string_view{PQgetvalue(result.get(), row, 5)} !=
            expected.description ||
        std::string_view{PQgetvalue(result.get(), row, 6)} !=
            expected.rbac_rule ||
        std::string_view{PQgetvalue(result.get(), row, 7)} != "t" ||
        std::string_view{PQgetvalue(result.get(), row, 8)} != "extension") {
      return false;
    }
  }
  return true;
}

auto json_nlohmann_to_jsoncpp(const nlohmann::json& src) -> Json::Value {
  // Round-trip via string; shared detail-builder convenience.
  Json::Value out;
  Json::CharReaderBuilder b;
  std::string errs;
  std::string dumped = src.dump();
  std::istringstream in(dumped);
  Json::parseFromStream(b, in, &out, &errs);
  return out;
}

// ─── Advisory lock ───────────────────────────────────────────────────

auto advisory_lock_key(std::string_view name) -> std::string {
  // `hashtextextended('plinth.packages.' || name, 0)` on the PG side;
  // here we build the seed string and let PG compute the hash via the
  // SQL-side call — see `try_acquire_name_lock` below.
  return std::string{"plinth.packages."} + std::string{name};
}

auto try_acquire_name_lock(PGconn* conn, std::string_view name)
    -> std::expected<void, std::string> {
  std::string seed = advisory_lock_key(name);
  std::array<const char*, 1> values = {seed.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "SELECT pg_try_advisory_lock(hashtextextended($1, 0))", 1,
                      nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) == 0) {
    return std::unexpected(std::string{"advisory-lock probe returned no rows"});
  }
  const char* got = PQgetvalue(res.get(), 0, 0);
  if (got == nullptr || std::strcmp(got, "t") != 0) {
    return std::unexpected(std::string{"advisory-lock-held"});
  }
  return {};
}

auto release_name_lock(PGconn* conn, std::string_view name) -> void {
  std::string seed = advisory_lock_key(name);
  std::array<const char*, 1> values = {seed.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "SELECT pg_advisory_unlock(hashtextextended($1, 0))", 1,
                      nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  (void)res; // best-effort; backend cleanup covers us on connection drop
}

// Worker scheduling is a handoff to another PostgreSQL session. Unlike the
// best-effort unwind path, it must observe the server's unlock acknowledgement.
auto release_name_lock_checked(PGconn* conn, std::string_view name)
    -> std::expected<void, std::string> {
  const auto seed = advisory_lock_key(name);
  const char* value = seed.c_str();
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "SELECT pg_advisory_unlock(hashtextextended($1, 0))", 1,
                      nullptr, &value, nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) != 1 || PQnfields(res.get()) != 1 ||
      PQgetisnull(res.get(), 0, 0) != 0 ||
      std::strcmp(PQgetvalue(res.get(), 0, 0), "t") != 0) {
    return std::unexpected("package name lock release was not acknowledged");
  }
  return {};
}

auto rbac_handoff_failure_report(std::string_view error) -> nlohmann::json {
  return {{"kind", "rbac-test-lock-release-failed"},
          {"message",
           "Package is committed ACTIVE, but the name lock release "
           "was not acknowledged; RBAC testing was not scheduled. "
           "Inspect package state and explicitly rerun its RBAC test."},
          {"committed", true},
          {"state", "ACTIVE"},
          {"rbac_test_scheduled", false},
          {"detail", std::string{error}}};
}

// ─── Audit emission (terminal only) ──────────────────────────────────

auto emit_installed_audit(const InstallerContext& ctx, const PackageRecord& rec)
    -> void {
  Json::Value detail(Json::objectValue);
  detail["id"] = rec.id;
  detail["name"] = rec.name;
  detail["version"] = rec.version;
  detail["provenance"] = std::string{provenance_to_string(rec.provenance)};
  if (!ctx.caller_user_id.empty()) {
    detail["installed_by_user_id"] = ctx.caller_user_id;
  }
  plinth::log::audit_sync(ctx.db, "packages.installed", detail);
}

auto emit_install_failed_audit(const InstallerContext& ctx,
                               const InstallFailure& f, std::string_view name,
                               std::string_view version) -> void {
  Json::Value detail(Json::objectValue);
  if (!f.package_id.empty()) {
    detail["id"] = f.package_id;
  }
  if (!name.empty()) {
    detail["name"] = std::string{name};
  }
  if (!version.empty()) {
    detail["version"] = std::string{version};
  }
  detail["stage"] = std::string{stage_to_string(f.failed_at)};
  detail["kind"] = f.kind;
  detail["message"] = f.message;
  plinth::log::audit_sync(ctx.db, "packages.install_failed", detail);
}

// ─── Zip extraction with traversal + zip-bomb guard ──────────────────

auto entry_name_is_safe(std::string_view raw) -> bool {
  if (raw.empty() || raw.starts_with('/')) {
    return false;
  }
  std::string_view s{raw};
  while (!s.empty()) {
    auto slash = s.find('/');
    std::string_view component =
        (slash == std::string_view::npos) ? s : s.substr(0, slash);
    if (component == ".." || component == ".") {
      return false;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    s.remove_prefix(slash + 1);
  }
  return true;
}

// over libzip's per-entry validation steps; splitting into helpers would
// obscure the linear "scan, stat, size-check, name-check, traversal-check,
// bomb-cap" flow.
auto extract_zip_to(std::span<const std::byte> blob, const fs::path& dest_root,
                    std::size_t max_uncompressed_bytes)
    -> std::expected<void, InstallFailure> {
  zip_error_t zerr;
  zip_error_init(&zerr);
  zip_source_t* src =
      zip_source_buffer_create(blob.data(), blob.size(), /*freep=*/0, &zerr);
  if (src == nullptr) {
    zip_error_fini(&zerr);
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "not-a-zip",
        .message = "zip_source_buffer_create failed",
    });
  }
  zip_t* z = zip_open_from_source(src, ZIP_RDONLY | ZIP_CHECKCONS, &zerr);
  if (z == nullptr) {
    zip_source_free(src);
    zip_error_fini(&zerr);
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "not-a-zip",
        .message = "zip_open_from_source failed",
    });
  }
  zip_error_fini(&zerr);
  auto z_guard = std::unique_ptr<zip_t, decltype(&zip_close)>(z, zip_close);

  zip_int64_t entry_count = zip_get_num_entries(z, 0);
  zip_uint64_t total_uncompressed = 0;
  std::size_t bomb_cap = max_uncompressed_bytes * std::size_t{2};

  for (zip_int64_t i = 0; i < entry_count; ++i) {
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(z, i, 0, &st) != 0) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "not-a-zip",
          .message = "zip_stat_index failed",
      });
    }
    if ((st.valid & ZIP_STAT_SIZE) == 0 || st.size == ZIP_UINT64_MAX) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "not-a-zip",
          .message = "zip entry missing uncompressed size",
      });
    }
    if ((st.valid & ZIP_STAT_NAME) == 0 || st.name == nullptr) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "not-a-zip",
          .message = "zip entry missing name",
      });
    }
    if (!entry_name_is_safe(st.name)) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "path-traversal",
          .message = std::string{"entry escapes staging root: "} + st.name,
      });
    }
    total_uncompressed += st.size;
    if (total_uncompressed > bomb_cap) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "upload-too-large",
          .message = "uncompressed total exceeds 2× max_package_size",
      });
    }
  }

  // Second pass: extract.
  for (zip_int64_t i = 0; i < entry_count; ++i) {
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(z, i, 0, &st) != 0) {
      continue;
    }
    std::string_view entry_name{st.name};
    fs::path out_path = dest_root / entry_name;
    if (entry_name.ends_with('/')) {
      std::error_code ec;
      fs::create_directories(out_path, ec);
      if (ec) {
        return std::unexpected(InstallFailure{
            .failed_at = InstallStage::UPLOADING,
            .kind = "extraction-failed",
            .message = ec.message(),
        });
      }
      continue;
    }
    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    if (ec) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "extraction-failed",
          .message = ec.message(),
      });
    }
    zip_file_t* f = zip_fopen_index(z, i, 0);
    if (f == nullptr) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "not-a-zip",
          .message = std::string{"zip_fopen_index failed for "} + st.name,
      });
    }
    auto f_guard =
        std::unique_ptr<zip_file_t, decltype(&zip_fclose)>(f, zip_fclose);

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "extraction-failed",
          .message = std::string{"cannot open "} + out_path.string(),
      });
    }
    constexpr std::size_t BUF_SIZE = std::size_t{64} * 1024;
    std::array<char, BUF_SIZE> buffer{};
    zip_int64_t rd = 0;
    while ((rd = zip_fread(f, buffer.data(), buffer.size())) > 0) {
      out.write(buffer.data(), rd);
      if (!out) {
        return std::unexpected(InstallFailure{
            .failed_at = InstallStage::UPLOADING,
            .kind = "extraction-failed",
            .message = std::string{"write to "} + out_path.string() + " failed",
        });
      }
    }
    if (rd < 0) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "not-a-zip",
          .message = std::string{"zip_fread failed for "} + st.name,
      });
    }
  }
  return {};
}

// ─── Minimal manifest probe (name + version) ─────────────────────────

struct MinimalManifest {
  std::string name;
  std::string version;
  std::string entry_point;
  std::optional<std::string> frontend_mount;
  std::optional<std::string> frontend_entry; // ICD-0.6.1 §4.3
};

auto read_minimal_manifest(const fs::path& package_root, Provenance provenance)
    -> std::expected<MinimalManifest, InstallFailure> {
  fs::path mf = package_root / "manifest.json";
  std::ifstream in(mf);
  if (!in) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "bad-manifest-name",
        .message = "manifest.json missing",
    });
  }
  std::stringstream buf;
  buf << in.rdbuf();
  auto parse_result = PackageManifest::parse(buf.str(), mf.string(),
                                             provenance == Provenance::BUNDLED);
  if (!parse_result.value.has_value()) {
    // Surface the first error's rule string so callers (e.g. the bundled
    // first-boot pre-flight) can distinguish RESERVED_NAME from generic
    // parse errors via `kind`.
    std::string kind = "bad-manifest-name";
    std::string msg = "manifest.json failed to parse";
    for (const auto& m : parse_result.messages) {
      if (m.severity == Severity::ERROR) {
        kind = m.rule;
        msg = m.message;
        break;
      }
    }
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = std::move(kind),
        .message = std::move(msg),
    });
  }
  const PackageManifest& pm = *parse_result.value;
  if (pm.name.empty() || pm.version.empty() || pm.entry_point.empty()) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "bad-manifest-name",
        .message = "manifest.json missing required fields",
    });
  }
  MinimalManifest m{
      .name = pm.name,
      .version = pm.version,
      .entry_point = pm.entry_point,
  };
  if (pm.frontend.has_value()) {
    m.frontend_mount = pm.frontend->mount;
    m.frontend_entry = pm.frontend->entry;
  }
  return m;
}

// ─── UPLOADING helper: INSERT plinth.packages row ────────────────────

auto insert_packages_row(PGconn* conn, std::string_view id,
                         const MinimalManifest& m, Provenance provenance,
                         std::string_view manifest_checksum,
                         std::string_view manifest_raw,
                         const std::string& caller_user_id)
    -> std::expected<void, std::string> {
  std::string id_s{id};
  std::string name_s{m.name};
  std::string version_s{m.version};
  std::string provenance_s{provenance_to_string(provenance)};
  std::string manifest_raw_s{manifest_raw};
  std::string mount_s = m.frontend_mount.value_or(std::string{});
  std::string fentry_s = m.frontend_entry.value_or(std::string{});
  std::string entry_s = m.entry_point;
  std::string checksum_s{manifest_checksum};

  std::array<const char*, 10> values = {
      id_s.c_str(),
      name_s.c_str(),
      version_s.c_str(),
      provenance_s.c_str(),
      manifest_raw_s.c_str(),
      m.frontend_mount.has_value() ? mount_s.c_str() : nullptr,
      m.frontend_entry.has_value() ? fentry_s.c_str() : nullptr,
      entry_s.c_str(),
      checksum_s.c_str(),
      caller_user_id.c_str(),
  };
  PgResultPtr res(
      plinth::db::exec_params(
          conn,
          "INSERT INTO plinth.packages "
          "(id, name, version, state, provenance, manifest_json, "
          " frontend_mount, frontend_entry, entry_point, manifest_checksum, "
          " installed_by_user_id) "
          "VALUES ($1::uuid, $2, $3, 'UPLOADING', $4, $5::jsonb, "
          "        $6, $7, $8, $9, NULLIF($10, '')::uuid)",
          10, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (std::string_view{PQcmdTuples(res.get())} != "1") {
    return std::unexpected("package state update matched no row");
  }
  return {};
}

auto update_packages_state(PGconn* conn, std::string_view id,
                           std::string_view new_state)
    -> std::expected<void, std::string> {
  std::string id_s{id};
  std::string state_s{new_state};
  std::array<const char*, 2> values = {state_s.c_str(), id_s.c_str()};
  PgResultPtr res(
      plinth::db::exec_params(
          conn, "UPDATE plinth.packages SET state = $1 WHERE id = $2::uuid", 2,
          nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (std::string_view{PQcmdTuples(res.get())} != "1") {
    return std::unexpected("package report update matched no row");
  }
  return {};
}

auto update_packages_report(PGconn* conn, std::string_view id,
                            const nlohmann::json& report)
    -> std::expected<void, std::string> {
  std::string id_s{id};
  std::string report_s = report.dump();
  std::array<const char*, 2> values = {report_s.c_str(), id_s.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "UPDATE plinth.packages "
                      "SET last_install_report = $1::jsonb WHERE id = $2::uuid",
                      2, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return {};
}

auto name_already_installed(PGconn* conn, std::string_view name)
    -> std::expected<bool, std::string> {
  std::string name_s{name};
  std::array<const char*, 1> values = {name_s.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "SELECT 1 FROM plinth.packages "
                      "WHERE name = $1 "
                      "  AND state IN ('ACTIVE','ACTIVE_FLAGGED','DISABLED')",
                      1, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  return PQntuples(res.get()) > 0;
}

// ICD-0.4.5 §UPLOADING augmented collision check. 3-way disposition:
// {first_install, disabled_present, upgrade_candidate, version_not_newer}.
// first_install: no row; caller proceeds with the 0.4.4 path.
// disabled_present: a DISABLED row for this name exists; caller 409s
//                   with `disabled-version-present` per ICD-0.4.5.
// upgrade_candidate: ACTIVE/ACTIVE_FLAGGED row exists and the incoming
//                    version compares strictly greater per SemVer.
//                    Caller forwards to upgrade_package (B6+).
// version_not_newer: ACTIVE row exists but incoming version is not
//                    strictly greater. Caller 409s with
//                    `upgrade-version-not-newer`.
enum class UploadingDisposition : std::uint8_t {
  FIRST_INSTALL,
  DISABLED_PRESENT,
  UPGRADE_CANDIDATE,
  VERSION_NOT_NEWER,
};

struct CollisionInfo {
  UploadingDisposition disposition = UploadingDisposition::FIRST_INSTALL;
  std::string existing_id;      // populated for every non-first-install case
  std::string existing_version; // ditto
  std::string existing_state;   // ditto
};

auto classify_uploading_collision(PGconn* conn, std::string_view name,
                                  std::string_view incoming_version)
    -> std::expected<CollisionInfo, std::string> {
  std::string name_s{name};
  std::array<const char*, 1> values = {name_s.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      conn,
                      "SELECT id::text, version, state FROM plinth.packages "
                      "WHERE name = $1 "
                      "  AND state IN ('ACTIVE','ACTIVE_FLAGGED','DISABLED') "
                      "  AND uninstalling_at IS NULL "
                      "LIMIT 1",
                      1, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) == 0) {
    return CollisionInfo{};
  }

  CollisionInfo info;
  info.existing_id = PQgetvalue(res.get(), 0, 0);
  info.existing_version = PQgetvalue(res.get(), 0, 1);
  info.existing_state = PQgetvalue(res.get(), 0, 2);

  if (info.existing_state == "DISABLED") {
    info.disposition = UploadingDisposition::DISABLED_PRESENT;
    return info;
  }
  // ACTIVE or ACTIVE_FLAGGED → SemVer-compare for upgrade.
  int cmp = compare_semver(incoming_version, info.existing_version);
  info.disposition = (cmp > 0) ? UploadingDisposition::UPGRADE_CANDIDATE
                               : UploadingDisposition::VERSION_NOT_NEWER;
  return info;
}

// ─── Validation + migration + registration + extract + activate ──────

auto build_validation_report(const ValidationReport& vr) -> nlohmann::json {
  nlohmann::json out;
  out["files_scanned"] = vr.files_scanned;
  out["total_bytes"] = vr.total_bytes;
  out["disposition"] = vr.disposition();
  nlohmann::json msgs = nlohmann::json::array();
  for (const auto& m : vr.messages) {
    nlohmann::json item;
    item["severity"] = (m.severity == Severity::ERROR ? "error" : "warning");
    item["rule"] = m.rule;
    item["message"] = m.message;
    if (m.path.has_value()) {
      item["path"] = *m.path;
    }
    if (m.remediation.has_value()) {
      item["remediation"] = *m.remediation;
    }
    msgs.push_back(std::move(item));
  }
  out["messages"] = std::move(msgs);
  return out;
}

auto build_migration_report(const MigrationReport& mr) -> nlohmann::json {
  nlohmann::json out;
  out["applied"] = mr.applied;
  out["skipped"] = mr.skipped;
  nlohmann::json warnings = nlohmann::json::array();
  for (const auto& w : mr.warnings) {
    nlohmann::json item;
    item["kind"] = w.kind;
    item["detail"] = w.detail;
    warnings.push_back(std::move(item));
  }
  out["warnings"] = std::move(warnings);
  return out;
}

// 0.4.6: typed parse + rule validation against the sibling
// capabilities manifest. Missing rbac.json is silent (returns empty
// RbacManifest). Present-but-malformed JSON or any rule-validation ERROR
// propagates up, triggering the REGISTERING RollbackGuard.
auto parse_and_validate_rbac(PGconn* admin, const fs::path& package_root,
                             std::string_view package_name,
                             const CapabilityManifest& cm)
    -> std::expected<plinth::rbac::RbacManifest, std::string>;

// 0.4.6: REGISTERING stage's RBAC sub-step. Extracted from
// run_stage_registering so the caller's cognitive complexity stays
// below the tidy threshold (each inlined loop adds nesting weight).
auto register_extension_rbac_rules(PGconn* admin, const fs::path& package_root,
                                   std::string_view name,
                                   const CapabilityManifest& cm)
    -> std::expected<void, std::string> {
  auto rm = parse_and_validate_rbac(admin, package_root, name, cm);
  if (!rm.has_value()) {
    return std::unexpected(rm.error());
  }
  for (const auto& r : rm->rules) {
    auto u = rbac::upsert_extension_rule(
        *admin, r.rule, r.namespace_, r.description, std::string{name}, r.test);
    if (!u.has_value()) {
      return std::unexpected(std::string{"rbac rule insert failed: "} + r.rule);
    }
  }
  // ICD-0.6.1 §7.2 — apply `default_grants[]` after rules are upserted.
  // Idempotent (ON CONFLICT DO NOTHING) — re-installs and upgrades
  // re-apply without duplicating group_rules rows. Unknown group or
  // rule names log + skip (cross-rule ref already validated by parser
  // against this manifest's `rules[]`; group existence is checked here
  // because it's PG state, not manifest state).
  for (const auto& g : rm->default_grants) {
    std::array<const char*, 2> values = {g.group.c_str(), g.rule.c_str()};
    PgResultPtr res(plinth::db::exec_params(
                        admin,
                        "INSERT INTO plinth.group_rules (group_id, rule_id) "
                        "SELECT gp.id, rl.id "
                        "FROM plinth.groups gp, plinth.rbac_rules rl "
                        "WHERE gp.name = $1 AND rl.rule = $2 "
                        "ON CONFLICT (group_id, rule_id) DO NOTHING",
                        2, nullptr, values.data(), nullptr, nullptr, 0),
                    PQclear);
    if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
      spdlog::warn("rbac default_grant '{}' → '{}' INSERT failed: {} "
                   "(continuing — install does not abort on grant gap)",
                   g.group, g.rule, PQresultErrorMessage(res.get()));
    }
  }
  return {};
}

auto parse_and_validate_rbac(PGconn* admin, const fs::path& package_root,
                             std::string_view package_name,
                             const CapabilityManifest& cm)
    -> std::expected<plinth::rbac::RbacManifest, std::string> {
  fs::path p = package_root / "rbac.json";
  if (!fs::exists(p)) {
    return plinth::rbac::RbacManifest{};
  }
  std::ifstream in(p);
  if (!in) {
    return std::unexpected(std::string{"rbac.json read error"});
  }
  std::stringstream buf;
  buf << in.rdbuf();
  auto pr = plinth::rbac::parse_rbac_manifest(buf.str(), "rbac.json");
  for (const auto& m : pr.messages) {
    if (m.severity == Severity::ERROR) {
      return std::unexpected(std::string{"rbac.json parse: "} + m.rule + " — " +
                             m.message);
    }
  }
  if (!pr.value.has_value()) {
    return std::unexpected(std::string{"rbac.json parse failed"});
  }
  auto findings =
      plinth::rbac::validate_rules(*pr.value, cm, package_name, *admin);
  for (const auto& f : findings) {
    if (f.severity == Severity::ERROR) {
      return std::unexpected(std::string{"rbac rule validation: "} + f.rule +
                             " — " + f.message);
    }
  }
  return std::move(*pr.value);
}

// Three-way RBAC reconciliation for the upgrade REGISTERING branch.
// Compares V1_RULES (already in plinth.rbac_rules for this extension)
// against V2_RULES (from the new package's rbac.json). INSERTs new,
// UPDATEs existing in place (preserving id + plinth.group_rules
// grants), orphans missing. Returns the three name lists for the
// UpgradeReport surface. Caller owns BEGIN/COMMIT.
auto reconcile_rbac_on_upgrade(
    PGconn* admin, std::string_view extension_name,
    const std::vector<plinth::rbac::RbacRule>& v2_rules)
    -> std::expected<RbacReconciliation, std::string> {
  RbacReconciliation out;

  // Load V1 rule names for this extension.
  std::string name_s{extension_name};
  std::array<const char*, 1> name_v = {name_s.c_str()};
  PgResultPtr v1_res(
      plinth::db::exec_params(
          admin, "SELECT rule FROM plinth.rbac_rules WHERE extension_name = $1",
          1, nullptr, name_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(v1_res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(v1_res.get())});
  }
  std::vector<std::string> v1_rules;
  v1_rules.reserve(static_cast<std::size_t>(PQntuples(v1_res.get())));
  for (int i = 0; i < PQntuples(v1_res.get()); ++i) {
    v1_rules.emplace_back(PQgetvalue(v1_res.get(), i, 0));
  }
  auto v1_has = [&](std::string_view name) {
    return std::ranges::find(v1_rules, name) != v1_rules.end();
  };
  auto v2_has = [&](std::string_view name) {
    return std::ranges::any_of(v2_rules,
                               [&](const auto& r) { return r.rule == name; });
  };

  // V2 rules: INSERT new OR UPDATE in-place (upsert_extension_rule's
  // 0.4.4 CHANGELOG deviation also clears orphaned_at on success).
  for (const auto& r : v2_rules) {
    auto u =
        rbac::upsert_extension_rule(*admin, r.rule, r.namespace_, r.description,
                                    std::string{extension_name}, r.test);
    if (!u.has_value()) {
      return std::unexpected(std::string{"rbac upsert failed: "} + r.rule);
    }
    if (v1_has(r.rule)) {
      out.updated.push_back(r.rule);
    } else {
      out.added.push_back(r.rule);
    }
  }

  // V1-only rules: orphan them.
  std::vector<std::string> to_orphan;
  for (const auto& rule : v1_rules) {
    if (!v2_has(rule)) {
      to_orphan.push_back(rule);
    }
  }
  if (!to_orphan.empty()) {
    // mark_extension_rules_orphaned touches every not-yet-orphaned
    // row for the extension; we need selective orphaning so the
    // v2-present rules keep their orphaned_at NULL via upsert
    // above. Per-rule UPDATE keeps scope tight.
    for (const auto& rule : to_orphan) {
      std::string rule_s{rule};
      std::array<const char*, 2> uv = {name_s.c_str(), rule_s.c_str()};
      PgResultPtr u(plinth::db::exec_params(
                        admin,
                        "UPDATE plinth.rbac_rules SET orphaned_at = NOW() "
                        "WHERE extension_name = $1 AND rule = $2 "
                        "  AND orphaned_at IS NULL",
                        2, nullptr, uv.data(), nullptr, nullptr, 0),
                    PQclear);
      if (PQresultStatus(u.get()) != PGRES_COMMAND_OK) {
        return std::unexpected(std::string{PQresultErrorMessage(u.get())});
      }
      out.orphaned.push_back(rule);
    }
  }

  return out;
}

// Upgrade variant of run_stage_registering. Same shape as the first-
// install version but: (a) performs RBAC reconciliation (new/updated/
// orphaned), (b) DELETEs old plinth.capabilities rows for the extension
// before INSERT so the UNIQUE(namespace, version, function, scope)
// constraint stays satisfied — the NOTIFY buffering makes the full
// DELETE+INSERT visible atomically to registry cache listeners on
// COMMIT, (c) inserts new panel rows linked to the NEW package_id
// (old rows are deleted at T4 during the atomic swap). Caller
// constructs the transaction; this helper runs entirely inside it.
auto run_stage_registering_upgrade(PGconn* admin, std::string_view new_id,
                                   std::string_view name,
                                   const fs::path& package_root,
                                   const CapabilityManifest& cm,
                                   const std::optional<PanelsManifest>& panels)
    -> std::expected<RbacReconciliation, std::string> {
  auto upd1 = update_packages_state(admin, new_id, "REGISTERING");
  if (!upd1.has_value()) {
    return std::unexpected(upd1.error());
  }

  auto rbac_manifest = parse_and_validate_rbac(admin, package_root, name, cm);
  if (!rbac_manifest.has_value()) {
    return std::unexpected(rbac_manifest.error());
  }
  auto rbac_report =
      reconcile_rbac_on_upgrade(admin, name, rbac_manifest->rules);
  if (!rbac_report.has_value()) {
    return std::unexpected(rbac_report.error());
  }

  // DELETE old capability rows for the extension before INSERT of new
  // set — NOTIFY events buffer in the tx and fire atomically on COMMIT,
  // so registry cache listeners see a coherent delete+register pair.
  std::string ext_s{name};
  std::array<const char*, 1> name_v = {ext_s.c_str()};
  PgResultPtr del_caps(
      plinth::db::exec_params(
          admin, "DELETE FROM plinth.capabilities WHERE extension_name = $1", 1,
          nullptr, name_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(del_caps.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(del_caps.get())});
  }

  for (const auto& cap : cm.provides) {
    capabilities::CapabilityRegistration reg{
        .namespace_ = cap.namespace_,
        .version = cap.version,
        .function = cap.function,
        .provider_type = "extension",
        .extension_name = std::string{name},
        .scope = (cap.scope.empty() ? std::string{"instance"} : cap.scope),
        .description = cap.description,
        .rbac_rule = cap.rbac_rule.value_or(std::string{}),
    };
    auto r = capabilities::register_capability_tx(*admin, reg);
    if (!r.has_value()) {
      return std::unexpected(
          std::string{"capability registration failed: "} + cap.function +
          " (error=" + std::to_string(static_cast<int>(r.error())) + ")");
    }
  }

  if (panels.has_value()) {
    for (const auto& p : panels->panels) {
      PanelRegistration preg{
          .package_id = std::string{new_id},
          .panel_id = p.id,
          .panel_type = PanelType::PRIMARY,
          .slot_type = std::nullopt,
          .declaration = p.declaration(),
      };
      auto rp = register_panel(*admin, preg);
      if (!rp.has_value()) {
        return std::unexpected(std::string{"panel insert failed: "} + p.id);
      }
    }
  }

  auto upd2 = update_packages_state(admin, new_id, "EXTRACTING");
  if (!upd2.has_value()) {
    return std::unexpected(upd2.error());
  }

  return *rbac_report;
}

auto run_stage_registering(PGconn* admin, std::string_view id,
                           std::string_view name, const fs::path& package_root,
                           const CapabilityManifest& cm,
                           const std::optional<PanelsManifest>& panels)
    -> std::expected<void, std::string> {
  auto begin = pg_exec(admin, "BEGIN");
  if (!begin.has_value()) {
    return std::unexpected(begin.error());
  }

  bool committed = false;
  auto rollback = [admin, &committed]() {
    if (!committed) {
      if (auto r = pg_exec(admin, "ROLLBACK"); !r) {
        spdlog::warn("install: ROLLBACK failed: {}", r.error());
      }
    }
  };
  struct RollbackGuard {
    std::function<void()> f;
    explicit RollbackGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~RollbackGuard() {
      if (f) {
        f();
      }
    }
    RollbackGuard(const RollbackGuard&) = delete;
    auto operator=(const RollbackGuard&) -> RollbackGuard& = delete;
    RollbackGuard(RollbackGuard&&) = delete;
    auto operator=(RollbackGuard&&) -> RollbackGuard& = delete;
  } rg{rollback};

  auto upd1 = update_packages_state(admin, id, "REGISTERING");
  if (!upd1.has_value()) {
    return std::unexpected(upd1.error());
  }

  // 0.4.6: typed rbac.json parser + rule validation (A.1..A.5)
  // + upsert loop. Fires before register_capability_tx so any
  // capability-level rbac_rule override satisfies the
  // rbac_rule_exists precondition. Rule-validation ERRORs propagate
  // through the RollbackGuard.
  auto rbac_ok = register_extension_rbac_rules(admin, package_root, name, cm);
  if (!rbac_ok.has_value()) {
    return std::unexpected(rbac_ok.error());
  }

  for (const auto& cap : cm.provides) {
    capabilities::CapabilityRegistration reg{
        .namespace_ = cap.namespace_,
        .version = cap.version,
        .function = cap.function,
        .provider_type = "extension",
        .extension_name = std::string{name},
        .scope = (cap.scope.empty() ? std::string{"instance"} : cap.scope),
        .description = cap.description,
        .rbac_rule = cap.rbac_rule.value_or(std::string{}),
    };
    auto r = capabilities::register_capability_tx(*admin, reg);
    if (!r.has_value()) {
      return std::unexpected(
          std::string{"capability registration failed: "} + cap.function +
          " (error=" + std::to_string(static_cast<int>(r.error())) + ")");
    }
  }

  if (panels.has_value()) {
    for (const auto& p : panels->panels) {
      PanelRegistration preg{
          .package_id = std::string{id},
          .panel_id = p.id,
          .panel_type = PanelType::PRIMARY, // 0.4.4 default; future
                                            // panels_manifest carries
                                            // a typed field.
          .slot_type = std::nullopt,
          .declaration = p.declaration(),
      };
      auto rp = register_panel(*admin, preg);
      if (!rp.has_value()) {
        return std::unexpected(std::string{"panel insert failed: "} + p.id);
      }
    }
  }

  auto upd2 = update_packages_state(admin, id, "EXTRACTING");
  if (!upd2.has_value()) {
    return std::unexpected(upd2.error());
  }

  auto commit = pg_exec(admin, "COMMIT");
  if (!commit.has_value()) {
    return std::unexpected(commit.error());
  }
  committed = true;
  return {};
}

auto run_stage_extracting(const fs::path& staging_tree,
                          const fs::path& dest_root)
    -> std::expected<void, InstallFailure> {
  std::error_code ec;
  fs::create_directories(dest_root.parent_path(), ec);
  if (ec) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::EXTRACTING,
        .kind = "extraction-failed",
        .message = ec.message(),
    });
  }
  fs::copy(staging_tree, dest_root,
           fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
  if (ec) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::EXTRACTING,
        .kind = "extraction-failed",
        .message = ec.message(),
    });
  }
  // `active` symlink next to the version dir.
  fs::path active = dest_root.parent_path() / "active";
  fs::remove(active, ec);
  fs::create_symlink(dest_root.filename(), active, ec);
  if (ec) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::EXTRACTING,
        .kind = "extraction-failed",
        .message = std::string{"symlink active failed: "} + ec.message(),
    });
  }
  return {};
}

// ─── 0.4.5 transition helpers ────────────────────────────────────────

// Snapshot of a plinth.packages row sufficient to drive
// disable/enable/uninstall without repeated round-trips.
struct LoadedPackage {
  std::string id;
  std::string name;
  std::string version;
  std::string state; // raw PG string
  std::string provenance;
  std::string manifest_checksum;
  std::string entry_point;
  bool application_ready{false};
  std::optional<std::string> frontend_mount;
  std::optional<std::string> frontend_entry; // ICD-0.6.1 §4.3
};

auto load_package_row(PGconn* conn, std::string_view package_id)
    -> std::expected<LoadedPackage, std::string> {
  std::string id_s{package_id};
  std::array<const char*, 1> values = {id_s.c_str()};
  PgResultPtr res(
      plinth::db::exec_params(
          conn,
          "SELECT name, version, state, manifest_checksum, frontend_mount, "
          "       frontend_entry, provenance, application_ready, entry_point "
          "FROM plinth.packages WHERE id = $1::uuid",
          1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) == 0) {
    return std::unexpected(std::string{"not-found"});
  }
  LoadedPackage lp;
  lp.id = std::string{package_id};
  lp.name = PQgetvalue(res.get(), 0, 0);
  lp.version = PQgetvalue(res.get(), 0, 1);
  lp.state = PQgetvalue(res.get(), 0, 2);
  lp.provenance = PQgetvalue(res.get(), 0, 6);
  lp.application_ready = std::string_view{PQgetvalue(res.get(), 0, 7)} == "t";
  lp.entry_point = PQgetvalue(res.get(), 0, 8);
  lp.manifest_checksum = PQgetvalue(res.get(), 0, 3);
  if (PQgetisnull(res.get(), 0, 4) == 0) {
    lp.frontend_mount = std::string{PQgetvalue(res.get(), 0, 4)};
  }
  if (PQgetisnull(res.get(), 0, 5) == 0) {
    lp.frontend_entry = std::string{PQgetvalue(res.get(), 0, 5)};
  }
  return lp;
}

auto transition_failure(TransitionKind kind, std::string_view package_id,
                        std::string_view code, std::string_view message)
    -> TransitionFailure {
  nlohmann::json report;
  report["kind"] = std::string{code};
  report["message"] = std::string{message};
  return TransitionFailure{
      .kind = kind,
      .package_id = std::string{package_id},
      .message = std::string{message},
      .report = std::move(report),
  };
}

auto emit_transition_audit(const InstallerContext& ctx, std::string_view action,
                           const LoadedPackage& lp,
                           std::string_view actor_field) -> void {
  Json::Value detail(Json::objectValue);
  detail["id"] = lp.id;
  detail["name"] = lp.name;
  detail["version"] = lp.version;
  if (!ctx.caller_user_id.empty()) {
    detail[std::string{actor_field}] = ctx.caller_user_id;
  }
  plinth::log::audit_sync(ctx.db, std::string{action}, detail);
}

// Iterate plinth.capabilities rows for the extension and call
// unregister_capability once per (namespace, version, function). The
// SELECT + DELETE sequence is inside the caller's transaction; NOTIFY
// payloads are buffered until COMMIT per PG semantics.
auto unregister_all_capabilities_for(PGconn* conn,
                                     std::string_view extension_name)
    -> std::expected<void, std::string> {
  std::string ext_s{extension_name};
  std::array<const char*, 1> values = {ext_s.c_str()};
  PgResultPtr res(
      plinth::db::exec_params(conn,
                              "SELECT DISTINCT namespace, version, function "
                              "FROM plinth.capabilities "
                              "WHERE extension_name = $1",
                              1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  int n = PQntuples(res.get());
  for (int i = 0; i < n; ++i) {
    std::string ns{PQgetvalue(res.get(), i, 0)};
    int ver = std::stoi(std::string{PQgetvalue(res.get(), i, 1)});
    std::string fn{PQgetvalue(res.get(), i, 2)};
    auto u = capabilities::unregister_capability(ns, ver, fn, *conn);
    if (!u.has_value()) {
      std::ostringstream msg;
      msg << "unregister_capability " << ns << ':' << ver << ':' << fn
          << " failed: " << u.error();
      return std::unexpected(msg.str());
    }
  }
  return {};
}

// In-memory rollback for asset_server changes: if disable/uninstall's
// PG COMMIT fails after `unregister_routes` has fired, restore the map
// entry so the serving state matches the (still-)ACTIVE row.
auto restore_routes_from_loaded(const InstallerContext& ctx,
                                const LoadedPackage& lp) -> void {
  fs::path client_root =
      ctx.data_dir / "extensions" / lp.name / lp.version / "client";
  asset_server::register_routes(lp.name, lp.version, client_root,
                                lp.manifest_checksum);
}

// Idempotent uninstall cleanup (ICD §UNINSTALLING steps 4-8). Called
// both from `uninstall_package` after Tx A and from
// `reconcile_in_flight_installs` when an UNINSTALLING row is observed
// at kernel boot. Every sub-step tolerates partial completion from a
// prior crash — DELETE WHERE against missing rows is a no-op, DROP
// SCHEMA IF EXISTS handles missing schemas, fs::remove_all on a
// missing tree returns cleanly.
// over five independent resource classes (routes, rbac+panels+caps tx, schema,
// fs, row) with idempotency inline; splitting would scatter the "one failure is
// fatal" error flow.
auto run_uninstall_cleanup(PGconn* admin, const LoadedPackage& lp,
                           const InstallerContext& ctx)
    -> std::expected<void, TransitionFailure> {
  // Step 4: in-memory route + capability unregister. Safe if prior
  // crash already cleared them.
  asset_server::unregister_routes(lp.name, lp.version);

  // Step 5: rules + group_rules + panels + capabilities in one tx.
  // Order matters: group_rules must go before rbac_rules (FK).
  auto begin = pg_exec(admin, "BEGIN");
  if (!begin.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL, lp.id,
                                              "db-error", begin.error()));
  }
  bool b_committed = false;
  auto rollback = [&]() {
    if (!b_committed) {
      if (auto r = pg_exec(admin, "ROLLBACK"); !r) {
        spdlog::warn("uninstall Tx B: ROLLBACK failed: {}", r.error());
      }
    }
  };
  struct RollbackGuard {
    std::function<void()> f;
    explicit RollbackGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~RollbackGuard() {
      if (f) {
        f();
      }
    }
    RollbackGuard(const RollbackGuard&) = delete;
    auto operator=(const RollbackGuard&) -> RollbackGuard& = delete;
    RollbackGuard(RollbackGuard&&) = delete;
    auto operator=(RollbackGuard&&) -> RollbackGuard& = delete;
  } rg{rollback};

  std::string name_s{lp.name};
  std::string id_s{lp.id};
  std::array<const char*, 1> name_v = {name_s.c_str()};
  std::array<const char*, 1> id_v = {id_s.c_str()};

  // 5a: strip group grants referencing this extension's rules.
  PgResultPtr del_gr(
      plinth::db::exec_params(
          admin,
          "DELETE FROM plinth.group_rules WHERE rule_id IN ("
          "  SELECT id FROM plinth.rbac_rules WHERE extension_name = $1)",
          1, nullptr, name_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(del_gr.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(
        transition_failure(TransitionKind::UNINSTALL, lp.id, "db-error",
                           PQresultErrorMessage(del_gr.get())));
  }
  // 5b: rbac_rules.
  auto del_rules = rbac::delete_extension_rules(lp.name, *admin);
  if (!del_rules.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL, lp.id,
                                              "db-error", del_rules.error()));
  }
  // 5c: panels.
  PgResultPtr del_panels(
      plinth::db::exec_params(
          admin, "DELETE FROM plinth.panels WHERE package_id = $1::uuid", 1,
          nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(del_panels.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(
        transition_failure(TransitionKind::UNINSTALL, lp.id, "db-error",
                           PQresultErrorMessage(del_panels.get())));
  }
  // 5d: capabilities (plinth.capabilities row deletion with NOTIFY
  // per scope). unregister_all_capabilities_for does DELETE+NOTIFY
  // via unregister_capability; it's inside the current tx.
  if (auto u = unregister_all_capabilities_for(admin, lp.name);
      !u.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL, lp.id,
                                              "db-error", u.error()));
  }
  auto commit_b = pg_exec(admin, "COMMIT");
  if (!commit_b.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL, lp.id,
                                              "db-error", commit_b.error()));
  }
  b_committed = true;

  // ICD-0.5.0.3 §Lifecycle — drop the pool now that capability rows
  // are gone. Idempotent; a DISABLED extension will have an already-
  // absent pool and this is a no-op.
  plinth::extensions::destroy_pool(lp.name);

  // Step 6: drop extension schema (irreversible, outside any tx).
  auto drop = drop_schema_and_migrations(lp.name, *admin);
  if (!drop.has_value()) {
    return std::unexpected(
        transition_failure(TransitionKind::UNINSTALL, lp.id, "db-error",
                           std::string{"drop_schema_and_migrations failed: "} +
                               drop.error().message));
  }

  // Step 7: filesystem cleanup. Residuals are warnings, not failures
  // — a broken fs state is recoverable by reinstall (the row is
  // deleted in step 8 next).
  fs::path ext_dir = ctx.data_dir / "extensions" / lp.name;
  std::error_code ec;
  fs::remove_all(ext_dir, ec);
  if (ec) {
    spdlog::warn("uninstall({}): fs::remove_all {} failed: {}", lp.name,
                 ext_dir.string(), ec.message());
  }

  // Step 8: delete the packages row (last; FK cascade unnecessary
  // after explicit 5c).
  PgResultPtr del_row(
      plinth::db::exec_params(admin,
                              "DELETE FROM plinth.packages WHERE id = $1::uuid",
                              1, nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(del_row.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(
        transition_failure(TransitionKind::UNINSTALL, lp.id, "db-error",
                           PQresultErrorMessage(del_row.get())));
  }
  return {};
}

} // namespace

// ─── Public entry ────────────────────────────────────────────────────

// state-machine driver over the
// UPLOADING→VALIDATING→MIGRATING→REGISTERING→EXTRACTING→ACTIVATING→ACTIVE
// sequence; splitting into per-stage helpers would multiply signatures to
// thread rollback + audit state, harming readability.
auto install_package(std::span<const std::byte> zip_blob, Provenance provenance,
                     const InstallerContext& ctx, bool dry_run,
                     nlohmann::json* dry_run_report)
    -> std::expected<PackageRecord, InstallFailure> {
  // ── Pre-UPLOADING: size cap ──────────────────────────────────────
  if (zip_blob.size() > ctx.max_package_size_bytes) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "upload-too-large",
        .message = "zip exceeds max_package_size_bytes",
    });
  }
  // Magic-number probe: PK\x03\x04
  if (zip_blob.size() < 4 || zip_blob[0] != std::byte{'P'} ||
      zip_blob[1] != std::byte{'K'} || zip_blob[2] != std::byte{0x03} ||
      zip_blob[3] != std::byte{0x04}) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "not-a-zip",
        .message = "magic number mismatch",
    });
  }

  // ── Staging dir + extract ────────────────────────────────────────
  std::string install_uuid = uuid_v4();
  fs::path staging = ctx.staging_dir / (std::string{"package-"} + install_uuid);
  std::error_code ec;
  fs::create_directories(staging, ec);
  if (ec) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "extraction-failed",
        .message = ec.message(),
    });
  }
  auto cleanup_staging = [&]() {
    std::error_code e;
    fs::remove_all(staging, e);
  };
  struct StagingGuard {
    std::function<void()> f;
    explicit StagingGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~StagingGuard() {
      if (f) {
        f();
      }
    }
    StagingGuard(const StagingGuard&) = delete;
    auto operator=(const StagingGuard&) -> StagingGuard& = delete;
    StagingGuard(StagingGuard&&) = delete;
    auto operator=(StagingGuard&&) -> StagingGuard& = delete;
  } sg{cleanup_staging};

  auto ex = extract_zip_to(zip_blob, staging, ctx.max_package_size_bytes);
  if (!ex.has_value()) {
    return std::unexpected(ex.error());
  }

  // ── Minimal manifest parse ───────────────────────────────────────
  auto mmr = read_minimal_manifest(staging, provenance);
  if (!mmr.has_value()) {
    return std::unexpected(mmr.error());
  }
  MinimalManifest minimal = *mmr;

  // ── Admin PG connection ──────────────────────────────────────────
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "db-error",
        .message = PQerrorMessage(pg.conn),
    });
  }

  // Advisory lock — automatic release on connection drop.
  auto lock = try_acquire_name_lock(pg.conn, minimal.name);
  if (!lock.has_value()) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "advisory-lock-held",
        .message = lock.error(),
    });
  }
  auto release_lock = [&]() { release_name_lock(pg.conn, minimal.name); };
  struct LockGuard {
    std::function<void()> f;
    explicit LockGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~LockGuard() {
      if (f) {
        f();
      }
    }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  } lg{release_lock};

  // ICD-0.4.5 §UPLOADING — 3-way disposition on same-name uploads.
  auto coll =
      classify_uploading_collision(pg.conn, minimal.name, minimal.version);
  if (!coll.has_value()) {
    return std::unexpected(InstallFailure{
        .failed_at = InstallStage::UPLOADING,
        .kind = "db-error",
        .message = coll.error(),
    });
  }
  switch (coll->disposition) {
    case UploadingDisposition::FIRST_INSTALL:
      break; // proceed with the 0.4.4 first-install path
    case UploadingDisposition::DISABLED_PRESENT:
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "disabled-version-present",
          .message = std::string{minimal.name} + " is disabled at version " +
                     coll->existing_version +
                     "; re-enable or uninstall before upgrading",
      });
    case UploadingDisposition::VERSION_NOT_NEWER:
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "upgrade-version-not-newer",
          .message = "incoming version " + minimal.version +
                     " is not strictly newer than installed " +
                     coll->existing_version,
      });
    case UploadingDisposition::UPGRADE_CANDIDATE: {
      // Release install_package's advisory lock explicitly before
      // dispatching — upgrade_package uses its own PG connection
      // (different session) and would fail the non-blocking
      // try_acquire if we still held it here. Brief release/
      // reacquire race is acceptable (advisory locks are
      // cooperative); another session snagging the lock in between
      // causes upgrade_package to return 409 `in-flight-operation`,
      // a sane user-visible outcome.
      release_lock();
      lg.f = {}; // prevent LockGuard double-release on scope exit
      auto up = upgrade_package(zip_blob, coll->existing_id, ctx, provenance);
      if (!up.has_value()) {
        const auto& tf = up.error();
        return std::unexpected(InstallFailure{
            .failed_at = InstallStage::UPLOADING,
            .package_id = tf.package_id,
            .kind = tf.report.contains("kind")
                        ? tf.report["kind"].get<std::string>()
                        : std::string{"upgrade-failed"},
            .message = tf.message,
            .report = tf.report,
        });
      }
      // Translate UpgradeReport → PackageRecord via the new_record
      // field. Callers (HTTP POST handler) surface the upgrade block
      // separately; the install_package return contract is the new
      // record only.
      return up->new_record;
    }
  }

  // The collision path above delegates upgrades to their own long-lived
  // fence. First installs fence only after that decision so an outer guard can
  // never reopen a deliberately preserved failed-upgrade fence.
  auto install_drain = capabilities::drain::begin_drain(minimal.name);
  (void)install_drain;
  struct InstallFenceGuard {
    std::string name;
    bool release_on_destroy{true};
    ~InstallFenceGuard() {
      if (release_on_destroy) {
        capabilities::drain::end_drain(name);
      } else {
        capabilities::drain::block_application(name);
      }
    }
    auto preserve() -> void { release_on_destroy = false; }
  } install_fence{minimal.name};

  // ── INSERT plinth.packages row ───────────────────────────────────
  fs::path mf_path = staging / "manifest.json";
  std::ifstream mf_in(mf_path);
  std::stringstream mf_buf;
  mf_buf << mf_in.rdbuf();
  std::string manifest_raw = mf_buf.str();
  std::string manifest_checksum = sha256_hex(manifest_raw);

  if (!dry_run) {
    auto ins = insert_packages_row(pg.conn, install_uuid, minimal, provenance,
                                   manifest_checksum, manifest_raw,
                                   ctx.caller_user_id);
    if (!ins.has_value()) {
      return std::unexpected(InstallFailure{
          .failed_at = InstallStage::UPLOADING,
          .kind = "db-error",
          .message = ins.error(),
      });
    }
  }

  // Helper to set a state transition best-effort; failure is logged but
  // non-fatal (the caller is already returning/failing). Dry-run skips
  // the UPDATE entirely — there is no row.
  auto set_state = [&](std::string_view s) {
    if (dry_run) {
      return;
    }
    if (auto r = update_packages_state(pg.conn, install_uuid, s); !r) {
      spdlog::warn("install: update state -> {} failed: {}", s, r.error());
    }
  };

  // Helper to mark the row failed + emit audit. Dry-run skips all side
  // effects — no row to update, and audit is reserved for terminal state
  // changes (a dry-run is not a state change). Caller (HTTP handler)
  // surfaces the failure body from the InstallFailure struct.
  auto fail_at = [&](InstallFailure f) -> std::unexpected<InstallFailure> {
    if (dry_run) {
      return std::unexpected(std::move(f));
    }
    f.package_id = install_uuid;
    set_state("INSTALL_FAILED");
    if (!f.report.is_null()) {
      static_cast<void>(
          update_packages_report(pg.conn, install_uuid, f.report));
    }
    emit_install_failed_audit(ctx, f, minimal.name, minimal.version);
    return std::unexpected(std::move(f));
  };

  // Track first-install schema cleanup across stages.
  auto drop_schema_if_first_install = [&]() {
    auto d = drop_schema_and_migrations(minimal.name, *pg.conn);
    if (!d.has_value()) {
      spdlog::warn("install: drop_schema_and_migrations({}) failed: {}",
                   minimal.name, d.error().message);
    }
  };

  // ── VALIDATING ───────────────────────────────────────────────────
  set_state("VALIDATING");
  ValidationConfig vcfg{};
  vcfg.cross_file = true;
  vcfg.against_running_kernel = false; // RT1 handled inline above;
                                       // RT2 handled by PG UNIQUE index
                                       // at REGISTERING; RT3 enforced
                                       // structurally via manifest parse.
  vcfg.is_bundled = (provenance == Provenance::BUNDLED);
  auto vr = validate(staging, vcfg);
  nlohmann::json vreport = build_validation_report(vr);
  if (!dry_run) {
    static_cast<void>(update_packages_report(pg.conn, install_uuid, vreport));
  }
  if (vr.disposition() == 1) {
    return fail_at(InstallFailure{
        .failed_at = InstallStage::VALIDATING,
        .kind = "validation-errors",
        .message = "validation failed",
        .report = vreport,
    });
  }

  // ── Dry-run early return — ICD-0.4.4 line 173 (I.19) ──────────────
  // UPLOADING + VALIDATING completed cleanly. Do not proceed to
  // MIGRATING; do not persist any row; do not emit audits. Return a
  // synthesised PackageRecord with state=VALIDATING and id="" so the
  // handler can build the 200 dry-run body; pass back the validation
  // report through the out-param.
  if (dry_run) {
    if (dry_run_report != nullptr) {
      *dry_run_report = vreport;
    }
    auto manifest_parsed = nlohmann::json::parse(manifest_raw, nullptr, false);
    if (manifest_parsed.is_discarded()) {
      manifest_parsed = nlohmann::json::object();
    }
    return PackageRecord{
        .id = "",
        .name = minimal.name,
        .version = minimal.version,
        .state = InstallStage::VALIDATING,
        .provenance = provenance,
        .frontend_mount = minimal.frontend_mount,
        .frontend_entry = minimal.frontend_entry,
        .manifest_json = std::move(manifest_parsed),
        .installed_at = std::chrono::system_clock::now(),
    };
  }

  // ── MIGRATING ────────────────────────────────────────────────────
  set_state("MIGRATING");
  auto mig = run_migrations(minimal.name, staging, *pg.conn);
  if (!mig.has_value()) {
    drop_schema_if_first_install();
    const auto& mig_err = mig.error();
    nlohmann::json mreport;
    mreport["kind"] = "migration-failed";
    mreport["message"] = mig_err.message;
    if (mig_err.migration_file.has_value()) {
      mreport["migration_file"] = *mig_err.migration_file;
    }
    if (mig_err.pg_sqlstate.has_value()) {
      mreport["pg_sqlstate"] = *mig_err.pg_sqlstate;
    }
    return fail_at(InstallFailure{
        .failed_at = InstallStage::MIGRATING,
        .kind = "migration-failed",
        .message = mig_err.message,
        .report = mreport,
    });
  }
  nlohmann::json mig_report = build_migration_report(*mig);

  // ── Full-manifest parse for REGISTERING payload ──────────────────
  std::ifstream cm_in(staging / "capabilities.json");
  std::stringstream cm_buf;
  cm_buf << cm_in.rdbuf();
  auto cm_parse = CapabilityManifest::parse(
      cm_buf.str(), (staging / "capabilities.json").string());
  if (!cm_parse.value.has_value()) {
    drop_schema_if_first_install();
    return fail_at(InstallFailure{
        .failed_at = InstallStage::REGISTERING,
        .kind = "registration-failed",
        .message = "capabilities.json failed to re-parse at REGISTERING",
    });
  }

  std::optional<PanelsManifest> panels_opt;
  fs::path panels_path = staging / "panels.json";
  if (fs::exists(panels_path)) {
    std::ifstream pn_in(panels_path);
    std::stringstream pn_buf;
    pn_buf << pn_in.rdbuf();
    auto pn_parse = PanelsManifest::parse(pn_buf.str(), panels_path.string());
    if (pn_parse.value.has_value()) {
      panels_opt = *pn_parse.value;
    }
  }

  // ── REGISTERING ──────────────────────────────────────────────────
  auto reg = run_stage_registering(pg.conn, install_uuid, minimal.name, staging,
                                   *cm_parse.value, panels_opt);
  if (!reg.has_value()) {
    drop_schema_if_first_install();
    return fail_at(InstallFailure{
        .failed_at = InstallStage::REGISTERING,
        .kind = "registration-failed",
        .message = reg.error(),
    });
  }

  // ── EXTRACTING ───────────────────────────────────────────────────
  fs::path dest_root =
      ctx.data_dir / "extensions" / minimal.name / minimal.version;
  auto ext = run_stage_extracting(staging, dest_root);
  if (!ext.has_value()) {
    std::error_code e;
    fs::remove_all(dest_root, e);
    fs::remove(dest_root.parent_path() / "active", e);
    drop_schema_if_first_install();
    return fail_at(std::move(ext.error()));
  }

  // ── ACTIVATING ───────────────────────────────────────────────────
  set_state("ACTIVATING");
  fs::path client_root = dest_root / "client";
  asset_server::register_routes(minimal.name, minimal.version, client_root,
                                manifest_checksum);
  const bool server_package =
      fs::is_regular_file(dest_root / minimal.entry_point);
  const bool pool_created = plinth::extensions::create_pool(minimal.name);
  if (!server_package || !pool_created) {
    asset_server::unregister_routes(minimal.name, minimal.version);
    return fail_at(InstallFailure{
        .failed_at = InstallStage::ACTIVATING,
        .kind = "activation-failed",
        .message = !server_package ? "declared entry point is unavailable"
                                   : "extension runtime failed to start",
    });
  }
  auto cache_reload = plinth::capabilities::reload_tier2_cache(ctx.db);
  if (!cache_reload.has_value()) {
    // Registration is durable but the resolver is still serving its prior
    // snapshot. Do not expose the package through either capability ingress
    // or launcher discovery until an explicit retry/reconciliation proves the
    // authoritative snapshot is admitted.
    install_fence.preserve();
    asset_server::unregister_routes(minimal.name, minimal.version);
    if (pool_created) {
      plinth::extensions::destroy_pool(minimal.name);
    }
    return fail_at(InstallFailure{
        .failed_at = InstallStage::ACTIVATING,
        .kind = "activation-failed",
        .message =
            "capability cache refresh failed; application remains unready",
    });
  }
  const bool application_ready =
      panels_opt.has_value() && !panels_opt->panels.empty();
  auto activation_begin = pg_exec(pg.conn, "BEGIN");
  if (!activation_begin.has_value()) {
    asset_server::unregister_routes(minimal.name, minimal.version);
    return fail_at(InstallFailure{
        .failed_at = InstallStage::ACTIVATING,
        .kind = "activation-failed",
        .message = activation_begin.error(),
    });
  }
  std::string activation_error;
  std::array<const char*, 2> activation_values{
      install_uuid.c_str(), application_ready ? "true" : "false"};
  PgResultPtr activated(
      plinth::db::exec_params(
          pg.conn,
          "UPDATE plinth.packages SET state = 'ACTIVE', "
          "application_ready = $2::boolean WHERE id = $1::uuid",
          2, nullptr, activation_values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(activated.get()) != PGRES_COMMAND_OK ||
      std::string_view{PQcmdTuples(activated.get())} != "1") {
    activation_error = "application activation update failed";
  } else if (application_ready) {
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      activation_error = notified.error();
    }
  }
  bool activation_commit_uncertain = false;
  if (activation_error.empty()) {
    auto commit_result = pg_exec(pg.conn, "COMMIT");
    if (!commit_result.has_value()) {
      activation_error = commit_result.error();
      activation_commit_uncertain = true;
    }
  }
  if (!activation_error.empty()) {
    if (activation_commit_uncertain) {
      // A lost COMMIT acknowledgement may still mean ACTIVE+ready is durable.
      // Retain the verified route/runtime but keep both capability ingress and
      // discovery blocked until restart reconciliation establishes the exact
      // durable outcome.
      install_fence.preserve();
    } else {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      asset_server::unregister_routes(minimal.name, minimal.version);
      if (pool_created) {
        plinth::extensions::destroy_pool(minimal.name);
      }
    }
    return fail_at(InstallFailure{
        .failed_at = InstallStage::ACTIVATING,
        .kind = "activation-failed",
        .message = activation_error,
    });
  }

  // Build success record.
  PackageRecord rec{
      .id = install_uuid,
      .name = minimal.name,
      .version = minimal.version,
      .state = InstallStage::ACTIVE,
      .provenance = provenance,
      .frontend_mount = minimal.frontend_mount,
      .frontend_entry = minimal.frontend_entry,
      .manifest_json = nlohmann::json::parse(manifest_raw, nullptr, false),
      .installed_at = std::chrono::system_clock::now(),
  };
  if (rec.manifest_json.is_discarded()) {
    rec.manifest_json = nlohmann::json::object();
  }

  (void)mig_report; // reserved for future last_install_report augmentation
  emit_installed_audit(ctx, rec);
  spdlog::info("package activation committed: {} {}; releasing name lock",
               rec.name, rec.version);
  if (auto released = release_name_lock_checked(pg.conn, rec.name); !released) {
    auto report = rbac_handoff_failure_report(released.error());
    // ACTIVE and its runtime/routes are already committed. Do not use fail_at,
    // which would mislabel this installed package as INSTALL_FAILED.
    return std::unexpected(
        InstallFailure{.failed_at = InstallStage::ACTIVE,
                       .package_id = rec.id,
                       .kind = report["kind"].get<std::string>(),
                       .message = report["message"].get<std::string>(),
                       .report = std::move(report)});
  }
  lg.f = {};
  if (ctx.schedule_rbac_tests) {
    rbac_test::schedule_rbac_test(rec.id, ctx, "install");
  }
  spdlog::info("install complete: {} {} ({})", rec.name, rec.version,
               provenance_to_string(provenance));
  return rec;
}

// transaction over the eight §DISABLED steps; splitting into helpers would
// multiply signatures to thread the RollbackGuard + asset-server rollback
// capture.
auto disable_package(std::string_view package_id, const InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, "db-error",
                                              PQerrorMessage(pg.conn)));
  }

  auto lp_r = load_package_row(pg.conn, package_id);
  if (!lp_r.has_value()) {
    const auto* code = (lp_r.error() == "not-found") ? "not-found" : "db-error";
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, code, lp_r.error()));
  }
  const LoadedPackage& lp = *lp_r;

  // State guard ahead of any mutation — friendlier error shape than
  // discovering mid-transaction. Reconfirmed FOR UPDATE below in tx.
  if (lp.state == "DISABLED") {
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, "already-disabled",
                                              "package is already disabled"));
  }
  if (lp.state != "ACTIVE" && lp.state != "ACTIVE_FLAGGED") {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "invalid-state-transition",
        "cannot disable from state " + lp.state));
  }

  // Advisory lock on name. Released explicitly below; connection drop
  // also releases per PG's session-advisory-lock semantics.
  auto lock = try_acquire_name_lock(pg.conn, lp.name);
  if (!lock.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, "in-flight-operation",
                                              lock.error()));
  }
  auto release_lock = [&]() { release_name_lock(pg.conn, lp.name); };
  struct LockGuard {
    std::function<void()> f;
    explicit LockGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~LockGuard() {
      if (f) {
        f();
      }
    }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  } lg{release_lock};

  auto disable_drain = capabilities::drain::begin_drain(lp.name);
  struct DisableFenceGuard {
    std::string name;
    bool release_on_destroy{true};
    bool finished{false};
    ~DisableFenceGuard() {
      if (finished) {
        return;
      }
      if (release_on_destroy) {
        capabilities::drain::end_drain(name);
      } else {
        capabilities::drain::block_application(name);
      }
    }
    auto preserve() -> void { release_on_destroy = false; }
    auto finish() -> void {
      capabilities::drain::end_drain(name);
      finished = true;
    }
  } disable_fence{lp.name};
  auto [disable_drained, disable_outstanding] =
      capabilities::drain::wait_for_zero(disable_drain,
                                         ctx.upgrade_drain_timeout_ms);
  if (!disable_drained) {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "drain-timeout",
        "capability ingress fence timed out with " +
            std::to_string(disable_outstanding) + " in-flight calls"));
  }

  // ICD-0.5.1 §Extension-lifecycle integration — synchronously
  // flush every open coalescer window owned by this extension before
  // the state row update + capability deletion. Runs outside the Tx
  // so the flush's emit_notify_async round-trips on a separate
  // connection and cannot block the Tx lock hold.
  plinth::realtime::CoalescerRegistry::instance().drain_extension(lp.name);
  // ICD-0.5.3 §`db.batch()` §Drain hook — discard any in-flight
  // batch scopes owned by this extension so a pending
  // flush_batch_scope can't fire a stale envelope after the
  // extension's DB state has been rewound.
  plinth::js::discard_batches_for_extension(lp.name);
  // ICD-0.5.2 §Extension Lifecycle Integration — evict WS + JS
  // subscriptions on this extension's channels. Paired with the
  // coalescer drain above; the two touch disjoint state so ordering
  // between them doesn't matter (both must run before the state row
  // update, which is the DB truth point for the transition).
  plinth::realtime::broker::drain_extension(lp.name, "disabled");

  auto begin = pg_exec(pg.conn, "BEGIN");
  if (!begin.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "db-error", begin.error()));
  }
  bool committed = false;
  bool routes_unregistered = false;
  auto rollback = [&]() {
    if (!committed) {
      if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
        spdlog::warn("disable: ROLLBACK failed: {}", r.error());
      }
      if (routes_unregistered) {
        restore_routes_from_loaded(ctx, lp);
      }
    }
  };
  struct RollbackGuard {
    std::function<void()> f;
    explicit RollbackGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~RollbackGuard() {
      if (f) {
        f();
      }
    }
    RollbackGuard(const RollbackGuard&) = delete;
    auto operator=(const RollbackGuard&) -> RollbackGuard& = delete;
    RollbackGuard(RollbackGuard&&) = delete;
    auto operator=(RollbackGuard&&) -> RollbackGuard& = delete;
  } rg{rollback};

  // Recheck state FOR UPDATE — another session may have raced us to
  // UNINSTALLING / DISABLED between our initial read and BEGIN.
  std::string id_s{package_id};
  std::array<const char*, 1> id_v = {id_s.c_str()};
  PgResultPtr recheck(
      plinth::db::exec_params(
          pg.conn,
          "SELECT state, application_ready FROM plinth.packages "
          "WHERE id = $1::uuid FOR UPDATE",
          1, nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(recheck.get()) != PGRES_TUPLES_OK ||
      PQntuples(recheck.get()) == 0) {
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, "db-error",
                                              "recheck SELECT failed"));
  }
  std::string current_state = PQgetvalue(recheck.get(), 0, 0);
  const bool was_application_ready =
      std::string_view{PQgetvalue(recheck.get(), 0, 1)} == "t";
  if (current_state != "ACTIVE" && current_state != "ACTIVE_FLAGGED") {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id,
        current_state == "DISABLED" ? "already-disabled"
                                    : "invalid-state-transition",
        "state raced to " + current_state + " before disable tx opened"));
  }

  // §DISABLED step 3: orphan every rbac rule for the extension.
  auto marked = rbac::mark_extension_rules_orphaned(lp.name, *pg.conn);
  if (!marked.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "db-error", marked.error()));
  }

  // §DISABLED step 4: unregister every capability row for the extension.
  if (auto u = unregister_all_capabilities_for(pg.conn, lp.name);
      !u.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "db-error", u.error()));
  }

  // §DISABLED step 2 (state + disabled_at).
  PgResultPtr upd(
      plinth::db::exec_params(pg.conn,
                              "UPDATE plinth.packages "
                              "SET state = 'DISABLED', disabled_at = NOW(), "
                              "application_ready = FALSE "
                              "WHERE id = $1::uuid",
                              1, nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(upd.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(transition_failure(TransitionKind::DISABLE,
                                              package_id, "db-error",
                                              PQresultErrorMessage(upd.get())));
  }
  if (was_application_ready) {
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      return std::unexpected(transition_failure(
          TransitionKind::DISABLE, package_id, "db-error", notified.error()));
    }
  }

  // §DISABLED step 7 (COMMIT).
  // Preserve the fence before COMMIT: a failed acknowledgement cannot tell us
  // whether PostgreSQL made the withdrawal durable. The guard only turns this
  // into a process-lifetime application block if we leave without reaching
  // the confirmed-success teardown below.
  disable_fence.preserve();
  auto commit = pg_exec(pg.conn, "COMMIT");
  if (!commit.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::DISABLE, package_id, "db-error", commit.error()));
  }
  committed = true;

  // Visibility is now durably withdrawn; only now remove module ingress.
  asset_server::unregister_routes(lp.name, lp.version);
  routes_unregistered = true;

  // ICD-0.5.0.3 §Lifecycle — tear down the extension's pool now that
  // the DISABLED state has committed. Idempotent; safe under racing
  // dispatches (the shared_lock in `extensions::dispatch` waits for
  // in-flight callers before destroy_pool's exclusive lock proceeds).
  plinth::extensions::destroy_pool(lp.name);
  disable_fence.finish();

  // §DISABLED step 8 (audit, post-commit, best-effort).
  emit_transition_audit(ctx, "packages.disabled", lp, "disabled_by_user_id");

  PackageRecord rec{
      .id = lp.id,
      .name = lp.name,
      .version = lp.version,
      .state = InstallStage::DISABLED,
      .provenance = Provenance::USER,
      .frontend_mount = lp.frontend_mount,
      .frontend_entry = lp.frontend_entry,
      .manifest_json = nlohmann::json::object(),
      .installed_at = std::chrono::system_clock::now(),
  };
  spdlog::info("package disabled: {} {}", rec.name, rec.version);
  return rec;
}

// transaction over the nine §ACTIVE-from-DISABLED steps (checksum verify,
// capability rematerialise, rbac clear, state flip); splitting duplicates the
// RollbackGuard + asset-server-restore plumbing.
auto enable_package(std::string_view package_id, const InstallerContext& ctx)
    -> std::expected<PackageRecord, TransitionFailure> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "db-error",
                                              PQerrorMessage(pg.conn)));
  }

  auto lp_r = load_package_row(pg.conn, package_id);
  if (!lp_r.has_value()) {
    const auto* code = (lp_r.error() == "not-found") ? "not-found" : "db-error";
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, code, lp_r.error()));
  }
  const LoadedPackage& lp = *lp_r;

  if (lp.state == "ACTIVE" || lp.state == "ACTIVE_FLAGGED") {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "already-enabled",
                                              "package is already enabled"));
  }
  if (lp.state != "DISABLED") {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "invalid-state-transition",
        "cannot enable from state " + lp.state));
  }

  // §ACTIVE-from-DISABLED step 2 (integrity check). Reread on-disk
  // manifest.json and rehash — mismatch means the admin edited files
  // out of band; 0.4.5 does NOT auto-heal per ICD.
  fs::path mf_path =
      ctx.data_dir / "extensions" / lp.name / lp.version / "manifest.json";
  std::ifstream mf_in(mf_path);
  if (!mf_in) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "checksum-mismatch-on-enable",
        "manifest.json missing at " + mf_path.string()));
  }
  std::stringstream mf_buf;
  mf_buf << mf_in.rdbuf();
  auto recomputed = sha256_hex(mf_buf.str());
  if (recomputed != lp.manifest_checksum) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "checksum-mismatch-on-enable",
        "on-disk manifest.json diverges from "
        "plinth.packages.manifest_checksum"));
  }

  auto lock = try_acquire_name_lock(pg.conn, lp.name);
  if (!lock.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "in-flight-operation",
                                              lock.error()));
  }
  auto release_lock = [&]() { release_name_lock(pg.conn, lp.name); };
  struct LockGuard {
    std::function<void()> f;
    explicit LockGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~LockGuard() {
      if (f) {
        f();
      }
    }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  } lg{release_lock};

  auto enable_drain = capabilities::drain::begin_drain(lp.name);
  struct EnableFenceGuard {
    std::string name;
    bool release_on_destroy{true};
    bool finished{false};
    ~EnableFenceGuard() {
      if (finished) {
        return;
      }
      if (release_on_destroy) {
        capabilities::drain::end_drain(name);
      } else {
        capabilities::drain::block_application(name);
      }
    }
    auto preserve() -> void { release_on_destroy = false; }
    auto finish() -> void {
      capabilities::drain::end_drain(name);
      finished = true;
    }
  } enable_fence{lp.name};
  auto [enable_drained, enable_outstanding] =
      capabilities::drain::wait_for_zero(enable_drain,
                                         ctx.upgrade_drain_timeout_ms);
  if (!enable_drained) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "drain-timeout",
        "capability ingress fence timed out with " +
            std::to_string(enable_outstanding) + " in-flight calls"));
  }

  auto begin = pg_exec(pg.conn, "BEGIN");
  if (!begin.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "db-error", begin.error()));
  }
  bool committed = false;
  bool routes_registered = false;
  auto rollback = [&]() {
    if (!committed) {
      if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
        spdlog::warn("enable: ROLLBACK failed: {}", r.error());
      }
      if (routes_registered) {
        // Roll back the in-memory route registration so the
        // unchanged DB state (DISABLED) matches serving.
        asset_server::unregister_routes(lp.name, lp.version);
      }
    }
  };
  struct RollbackGuard {
    std::function<void()> f;
    explicit RollbackGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~RollbackGuard() {
      if (f) {
        f();
      }
    }
    RollbackGuard(const RollbackGuard&) = delete;
    auto operator=(const RollbackGuard&) -> RollbackGuard& = delete;
    RollbackGuard(RollbackGuard&&) = delete;
    auto operator=(RollbackGuard&&) -> RollbackGuard& = delete;
  } rg{rollback};

  std::string id_s{package_id};
  std::array<const char*, 1> id_v = {id_s.c_str()};
  PgResultPtr recheck(
      plinth::db::exec_params(
          pg.conn,
          "SELECT state FROM plinth.packages WHERE id = $1::uuid FOR UPDATE", 1,
          nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(recheck.get()) != PGRES_TUPLES_OK ||
      PQntuples(recheck.get()) == 0) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "db-error",
                                              "recheck SELECT failed"));
  }
  std::string current_state = PQgetvalue(recheck.get(), 0, 0);
  if (current_state != "DISABLED") {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "invalid-state-transition",
        "state raced to " + current_state + " before enable tx opened"));
  }

  // §ACTIVE-from-DISABLED step 3 (asset routes). In-memory; rollback
  // guard undoes on COMMIT failure.
  fs::path package_root = ctx.data_dir / "extensions" / lp.name / lp.version;
  fs::path client_root = package_root / "client";
  asset_server::register_routes(lp.name, lp.version, client_root,
                                lp.manifest_checksum);
  routes_registered = true;

  // §ACTIVE-from-DISABLED step 5 (capabilities). Re-parse on-disk
  // capabilities.json and INSERT within the tx. RBAC rule rows are
  // still present (orphaned only), so register_capability_tx's
  // rbac_rule_exists precondition passes.
  std::ifstream cm_in(package_root / "capabilities.json");
  if (cm_in) {
    std::stringstream cm_buf;
    cm_buf << cm_in.rdbuf();
    auto cm_parse = CapabilityManifest::parse(
        cm_buf.str(), (package_root / "capabilities.json").string());
    if (!cm_parse.value.has_value()) {
      return std::unexpected(
          transition_failure(TransitionKind::ENABLE, package_id, "db-error",
                             "capabilities.json failed to parse at enable"));
    }
    for (const auto& cap : cm_parse.value->provides) {
      capabilities::CapabilityRegistration reg{
          .namespace_ = cap.namespace_,
          .version = cap.version,
          .function = cap.function,
          .provider_type = "extension",
          .extension_name = std::string{lp.name},
          .scope = (cap.scope.empty() ? std::string{"instance"} : cap.scope),
          .description = cap.description,
          .rbac_rule = cap.rbac_rule.value_or(std::string{}),
      };
      auto r = capabilities::register_capability_tx(*pg.conn, reg);
      if (!r.has_value()) {
        return std::unexpected(transition_failure(
            TransitionKind::ENABLE, package_id, "db-error",
            std::string{"capability re-register failed: "} + cap.function +
                " (error=" + std::to_string(static_cast<int>(r.error())) +
                ")"));
      }
    }
  }

  // §ACTIVE-from-DISABLED step 6 (clear orphaned rbac). Must run
  // after any potential capability INSERT so the rule-existence probe
  // doesn't interact with an in-flight UPDATE.
  auto cleared = rbac::clear_extension_rules_orphaned(lp.name, *pg.conn);
  if (!cleared.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "db-error", cleared.error()));
  }

  auto panel_presence = has_primary_panels(pg.conn, package_id);
  if (!panel_presence.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "db-error",
                                              panel_presence.error()));
  }

  // §ACTIVE-from-DISABLED step 7 (state + disabled_at NULL).
  PgResultPtr upd(
      plinth::db::exec_params(pg.conn,
                              "UPDATE plinth.packages SET state = 'ACTIVE', "
                              "disabled_at = NULL, application_ready = FALSE "
                              "WHERE id = $1::uuid",
                              1, nullptr, id_v.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(upd.get()) != PGRES_COMMAND_OK) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "db-error",
                                              PQresultErrorMessage(upd.get())));
  }
  // §ACTIVE-from-DISABLED step 8 (COMMIT). Fence discovery before the
  // commit attempt so a lost acknowledgement cannot expose an uncertain row.
  enable_fence.preserve();
  auto commit = pg_exec(pg.conn, "COMMIT");
  if (!commit.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "db-error", commit.error()));
  }
  committed = true;

  // The package is now ACTIVE with rematerialized authority but remains
  // deliberately unready. Any later runtime/cache/admission failure must keep
  // capability ingress fenced until an explicit retry repairs the generation.
  const bool server_package =
      fs::is_regular_file(package_root / lp.entry_point);
  const bool pool_created = plinth::extensions::create_pool(lp.name);
  if (!server_package || !pool_created) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "runtime-start-failed",
        !server_package
            ? "declared entry point is unavailable; application remains unready"
            : "extension runtime failed to start; application remains "
              "unready"));
  }
  auto cache_reload = plinth::capabilities::reload_tier2_cache(ctx.db);
  if (!cache_reload.has_value()) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "capability-refresh-failed",
        "capability cache refresh failed; application remains unready"));
  }

  bool panel_assets_ready = true;
  if (*panel_presence) {
    auto assets_ready =
        registered_panel_assets_ready(pg.conn, package_id, package_root);
    if (!assets_ready.has_value()) {
      return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                                package_id, "db-error",
                                                assets_ready.error()));
    }
    panel_assets_ready = *assets_ready;
  }
  auto capability_authority =
      registered_capabilities_ready(pg.conn, lp.name, package_root);
  if (!capability_authority.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                              package_id, "db-error",
                                              capability_authority.error()));
  }
  if (!panel_assets_ready || !*capability_authority) {
    return std::unexpected(transition_failure(
        TransitionKind::ENABLE, package_id, "readiness-verification-failed",
        !panel_assets_ready
            ? "registered panel assets are unavailable; application remains "
              "unready"
            : "registered capability authority does not match the package "
              "manifest; application remains unready"));
  }

  if (*panel_presence) {
    auto admit_begin = pg_exec(pg.conn, "BEGIN");
    if (!admit_begin.has_value()) {
      return std::unexpected(transition_failure(
          TransitionKind::ENABLE, package_id, "db-error", admit_begin.error()));
    }
    PgResultPtr admit(
        plinth::db::exec_params(
            pg.conn,
            "UPDATE plinth.packages SET application_ready = TRUE "
            "WHERE id = $1::uuid AND state IN ('ACTIVE','ACTIVE_FLAGGED')",
            1, nullptr, id_v.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(admit.get()) != PGRES_COMMAND_OK ||
        std::string_view{PQcmdTuples(admit.get())} != "1") {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(
          transition_failure(TransitionKind::ENABLE, package_id, "db-error",
                             "application readiness admission failed"));
    }
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(transition_failure(
          TransitionKind::ENABLE, package_id, "db-error", notified.error()));
    }
    auto admit_commit = pg_exec(pg.conn, "COMMIT");
    if (!admit_commit.has_value()) {
      return std::unexpected(transition_failure(TransitionKind::ENABLE,
                                                package_id, "db-error",
                                                admit_commit.error()));
    }
  }

  emit_transition_audit(ctx, "packages.enabled", lp, "enabled_by_user_id");
  enable_fence.finish();
  if (auto released = release_name_lock_checked(pg.conn, lp.name); !released) {
    auto report = rbac_handoff_failure_report(released.error());
    return std::unexpected(
        TransitionFailure{.kind = TransitionKind::ENABLE,
                          .package_id = lp.id,
                          .message = report["message"].get<std::string>(),
                          .report = std::move(report)});
  }
  lg.f = {};
  if (ctx.schedule_rbac_tests) {
    rbac_test::schedule_rbac_test(lp.id, ctx, "enable");
  }

  PackageRecord rec{
      .id = lp.id,
      .name = lp.name,
      .version = lp.version,
      .state = InstallStage::ACTIVE,
      .provenance = Provenance::USER,
      .frontend_mount = lp.frontend_mount,
      .frontend_entry = lp.frontend_entry,
      .manifest_json = nlohmann::json::object(),
      .installed_at = std::chrono::system_clock::now(),
  };
  spdlog::info("package enabled: {} {}", rec.name, rec.version);
  return rec;
}

auto uninstall_package(std::string_view package_id, bool confirmed,
                       const InstallerContext& ctx)
    -> std::expected<void, TransitionFailure> {
  if (!confirmed) {
    return std::unexpected(transition_failure(
        TransitionKind::UNINSTALL, package_id, "confirmation-required",
        "uninstall requires ?confirm=true"));
  }

  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL,
                                              package_id, "db-error",
                                              PQerrorMessage(pg.conn)));
  }

  auto lp_r = load_package_row(pg.conn, package_id);
  if (!lp_r.has_value()) {
    const auto* code = (lp_r.error() == "not-found") ? "not-found" : "db-error";
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL,
                                              package_id, code, lp_r.error()));
  }
  const LoadedPackage& lp = *lp_r;

  if (lp.state == "UNINSTALLING") {
    return std::unexpected(transition_failure(
        TransitionKind::UNINSTALL, package_id, "already-uninstalling",
        "uninstall already in progress"));
  }

  auto lock = try_acquire_name_lock(pg.conn, lp.name);
  if (!lock.has_value()) {
    return std::unexpected(transition_failure(TransitionKind::UNINSTALL,
                                              package_id, "in-flight-operation",
                                              lock.error()));
  }
  auto release_lock = [&]() { release_name_lock(pg.conn, lp.name); };
  struct LockGuard {
    std::function<void()> f;
    explicit LockGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~LockGuard() {
      if (f) {
        f();
      }
    }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  } lg{release_lock};

  auto uninstall_drain = capabilities::drain::begin_drain(lp.name);
  struct UninstallFenceGuard {
    std::string name;
    bool release_on_destroy{true};
    bool finished{false};
    ~UninstallFenceGuard() {
      if (finished) {
        return;
      }
      if (release_on_destroy) {
        capabilities::drain::end_drain(name);
      } else {
        capabilities::drain::block_application(name);
      }
    }
    auto preserve() -> void { release_on_destroy = false; }
    auto finish() -> void {
      capabilities::drain::end_drain(name);
      finished = true;
    }
  } uninstall_fence{lp.name};
  auto [uninstall_drained, uninstall_outstanding] =
      capabilities::drain::wait_for_zero(uninstall_drain,
                                         ctx.upgrade_drain_timeout_ms);
  if (!uninstall_drained) {
    return std::unexpected(transition_failure(
        TransitionKind::UNINSTALL, package_id, "drain-timeout",
        "capability ingress fence timed out with " +
            std::to_string(uninstall_outstanding) + " in-flight calls"));
  }

  // ICD-0.5.1 §Extension-lifecycle integration — flush coalescer
  // windows owned by this extension before the UNINSTALLING marker.
  // After Tx A commits, downstream uninstall_cleanup will delete
  // capability rows; the drain here guarantees any pending Layer-1
  // envelopes land while the extension's NOTIFY consumer chain is
  // still intact.
  plinth::realtime::CoalescerRegistry::instance().drain_extension(lp.name);
  // ICD-0.5.3 §`db.batch()` §Drain hook — UNINSTALL counterpart to
  // the DISABLED call above.
  plinth::js::discard_batches_for_extension(lp.name);
  // ICD-0.5.2 §Extension Lifecycle Integration — evict WS + JS
  // subscriptions on this extension's channels before the schema
  // drop in uninstall_cleanup. Mirrors the DISABLED call site.
  plinth::realtime::broker::drain_extension(lp.name, "uninstall");

  // Tx A: mark UNINSTALLING. This is the destructive-action marker —
  // past this point, uninstall drives to completion (or a recoverable
  // mid-state cleaned up by `reconcile_in_flight_installs`).
  {
    auto begin = pg_exec(pg.conn, "BEGIN");
    if (!begin.has_value()) {
      return std::unexpected(transition_failure(
          TransitionKind::UNINSTALL, package_id, "db-error", begin.error()));
    }
    bool committed_a = false;
    auto rb = [&]() {
      if (!committed_a) {
        if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
          spdlog::warn("uninstall Tx A: ROLLBACK failed: {}", r.error());
        }
      }
    };
    struct RollbackGuardA {
      std::function<void()> f;
      explicit RollbackGuardA(std::function<void()> fn) : f(std::move(fn)) {}
      ~RollbackGuardA() {
        if (f) {
          f();
        }
      }
      RollbackGuardA(const RollbackGuardA&) = delete;
      auto operator=(const RollbackGuardA&) -> RollbackGuardA& = delete;
      RollbackGuardA(RollbackGuardA&&) = delete;
      auto operator=(RollbackGuardA&&) -> RollbackGuardA& = delete;
    } rga{rb};

    std::string id_s{package_id};
    std::array<const char*, 1> id_v = {id_s.c_str()};
    PgResultPtr recheck(
        plinth::db::exec_params(
            pg.conn,
            "SELECT state, application_ready FROM plinth.packages "
            "WHERE id = $1::uuid FOR UPDATE",
            1, nullptr, id_v.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(recheck.get()) != PGRES_TUPLES_OK ||
        PQntuples(recheck.get()) == 0) {
      return std::unexpected(transition_failure(TransitionKind::UNINSTALL,
                                                package_id, "db-error",
                                                "recheck SELECT failed"));
    }
    std::string current_state = PQgetvalue(recheck.get(), 0, 0);
    const bool was_application_ready =
        std::string_view{PQgetvalue(recheck.get(), 0, 1)} == "t";
    if (current_state == "UNINSTALLING") {
      return std::unexpected(transition_failure(
          TransitionKind::UNINSTALL, package_id, "already-uninstalling",
          "state raced to UNINSTALLING before uninstall tx opened"));
    }

    PgResultPtr upd(plinth::db::exec_params(
                        pg.conn,
                        "UPDATE plinth.packages "
                        "SET state = 'UNINSTALLING', uninstalling_at = NOW(), "
                        "application_ready = FALSE "
                        "WHERE id = $1::uuid",
                        1, nullptr, id_v.data(), nullptr, nullptr, 0),
                    PQclear);
    if (PQresultStatus(upd.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(
          transition_failure(TransitionKind::UNINSTALL, package_id, "db-error",
                             PQresultErrorMessage(upd.get())));
    }
    if (was_application_ready) {
      auto notified = emit_applications_changed(pg.conn);
      if (!notified.has_value()) {
        return std::unexpected(transition_failure(TransitionKind::UNINSTALL,
                                                  package_id, "db-error",
                                                  notified.error()));
      }
    }
    // A failed COMMIT acknowledgement may still mean the UNINSTALLING marker
    // became durable. Keep ingress fenced unless the entire idempotent cleanup
    // reaches its terminal success path.
    uninstall_fence.preserve();
    auto commit_a = pg_exec(pg.conn, "COMMIT");
    if (!commit_a.has_value()) {
      return std::unexpected(transition_failure(
          TransitionKind::UNINSTALL, package_id, "db-error", commit_a.error()));
    }
    committed_a = true;

    // Destructive-action marker: audit emits once Tx A has committed
    // (the record of intent).
    Json::Value detail(Json::objectValue);
    detail["id"] = lp.id;
    detail["name"] = lp.name;
    detail["version"] = lp.version;
    detail["prior_state"] = current_state;
    if (!ctx.caller_user_id.empty()) {
      detail["uninstalled_by_user_id"] = ctx.caller_user_id;
    }
    plinth::log::audit_sync(ctx.db, "packages.uninstall_confirmed", detail);
  }

  // Steps 4-8: idempotent cleanup (may be re-run by the reconciler
  // if the kernel crashes between any pair of steps).
  auto cleanup = run_uninstall_cleanup(pg.conn, lp, ctx);
  if (!cleanup.has_value()) {
    // Terminal failure audit; the row stays in UNINSTALLING for
    // the next reconciler pass to retry.
    Json::Value detail(Json::objectValue);
    detail["id"] = lp.id;
    detail["name"] = lp.name;
    detail["version"] = lp.version;
    detail["message"] = cleanup.error().message;
    plinth::log::audit_sync(ctx.db, "packages.uninstall_failed", detail);
    return std::unexpected(cleanup.error());
  }

  uninstall_fence.finish();

  emit_transition_audit(ctx, "packages.uninstalled", lp,
                        "uninstalled_by_user_id");
  spdlog::info("package uninstalled: {} {}", lp.name, lp.version);
  return {};
}

// machine over 6 InstallStage values; splitting would hide the "one row, one
// disposition" structure.
auto reconcile_in_flight_installs(const InstallerContext& ctx)
    -> std::expected<void, std::string> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(pg.conn != nullptr
                               ? std::string{PQerrorMessage(pg.conn)}
                               : std::string{"PQconnectdb returned null"});
  }

  PgResultPtr res(plinth::db::exec(
                      pg.conn,
                      "SELECT id::text, name, version, state, "
                      "       supersedes_id::text "
                      "FROM plinth.packages "
                      "WHERE state IN ('UPLOADING','VALIDATING','MIGRATING',"
                      "                'REGISTERING','EXTRACTING','ACTIVATING',"
                      "                'UNINSTALLING','SUPERSEDED') "
                      "   OR (state IN ('ACTIVE','ACTIVE_FLAGGED') "
                      "       AND supersedes_id IS NOT NULL) "
                      "ORDER BY installed_at ASC"),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }

  std::vector<std::string> names;
  std::vector<std::string> candidate_ids;
  names.reserve(static_cast<std::size_t>(PQntuples(res.get())));
  candidate_ids.reserve(static_cast<std::size_t>(PQntuples(res.get())));
  for (int row = 0; row < PQntuples(res.get()); ++row) {
    candidate_ids.emplace_back(PQgetvalue(res.get(), row, 0));
    names.emplace_back(PQgetvalue(res.get(), row, 1));
  }
  std::ranges::sort(names);
  names.erase(std::unique(names.begin(), names.end()), names.end());
  struct ReconciliationLocks {
    PGconn* conn;
    std::vector<std::string> held;
    ~ReconciliationLocks() {
      for (auto it = held.rbegin(); it != held.rend(); ++it) {
        release_name_lock(conn, *it);
      }
    }
  } locks{pg.conn, {}};
  locks.held.reserve(names.size());
  for (const auto& name : names) {
    auto acquired = try_acquire_name_lock(pg.conn, name);
    if (!acquired.has_value()) {
      return std::unexpected("in-flight reconciliation lock for '" + name +
                             "' failed: " + acquired.error());
    }
    locks.held.push_back(name);
  }

  // Re-read every candidate under the package-name locks. The first query is
  // identity discovery only; lifecycle state from that snapshot is never
  // used for recovery decisions.
  std::string id_list;
  for (const auto& id : candidate_ids) {
    if (!id_list.empty()) {
      id_list.push_back('\n');
    }
    id_list.append(id);
  }
  std::array<const char*, 1> id_values{id_list.c_str()};
  PgResultPtr locked_res(
      plinth::db::exec_params(
          pg.conn,
          "SELECT id::text, name, version, state, "
          "       supersedes_id::text "
          "FROM plinth.packages "
          "WHERE id::text = ANY(string_to_array($1, E'\\n')) "
          "  AND (state IN ('UPLOADING','VALIDATING','MIGRATING',"
          "                'REGISTERING','EXTRACTING','ACTIVATING',"
          "                'UNINSTALLING','SUPERSEDED') "
          "   OR (state IN ('ACTIVE','ACTIVE_FLAGGED') "
          "       AND supersedes_id IS NOT NULL)) "
          "ORDER BY installed_at ASC",
          1, nullptr, id_values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(locked_res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(locked_res.get())});
  }
  res = std::move(locked_res);
  int row_count = PQntuples(res.get());
  if (row_count == 0) {
    spdlog::info("reconcile_in_flight_installs: no in-flight rows");
  } else {
    spdlog::info("reconcile_in_flight_installs: reconciling {} row(s)",
                 row_count);
  }

  auto write_report = [&](std::string_view id, const nlohmann::json& report) {
    return update_packages_report(pg.conn, id, report);
  };
  auto write_recovery_record =
      [&](std::string_view id, std::string_view state,
          const nlohmann::json& report) -> std::expected<void, std::string> {
    auto begin = pg_exec(pg.conn, "BEGIN");
    if (!begin.has_value()) {
      return std::unexpected(begin.error());
    }
    auto rollback = [&]() { static_cast<void>(pg_exec(pg.conn, "ROLLBACK")); };
    if (auto updated = update_packages_state(pg.conn, id, state);
        !updated.has_value()) {
      rollback();
      return updated;
    }
    if (auto reported = write_report(id, report); !reported.has_value()) {
      rollback();
      return reported;
    }
    auto committed = pg_exec(pg.conn, "COMMIT");
    if (!committed.has_value()) {
      return std::unexpected(committed.error());
    }
    return {};
  };
  auto mark_failed = [&](std::string_view id, std::string_view name,
                         std::string_view from_state, std::string_view reason,
                         bool drop_schema) -> std::expected<void, std::string> {
    nlohmann::json report;
    report["recovered_from_state"] = std::string{from_state};
    report["reconciled_at_bootstrap"] = true;
    report["disposition"] = "INSTALL_FAILED";
    report["reason"] = std::string{reason};
    if (drop_schema) {
      auto drop = drop_schema_and_migrations(name, *pg.conn);
      if (!drop.has_value()) {
        return std::unexpected("reconcile schema cleanup failed for " +
                               std::string{name} + ": " + drop.error().message);
      }
    }
    if (auto r = write_recovery_record(id, "INSTALL_FAILED", report); !r) {
      return std::unexpected("reconcile terminal record failed for " +
                             std::string{id} + ": " + r.error());
    }
    spdlog::info("reconcile: id={} name={} -> INSTALL_FAILED ({})", id, name,
                 reason);
    return {};
  };
  auto mark_active =
      [&](std::string_view id, std::string_view name,
          std::string_view from_state,
          std::string_view note) -> std::expected<void, std::string> {
    nlohmann::json report;
    report["recovered_from_state"] = std::string{from_state};
    report["reconciled_at_bootstrap"] = true;
    report["disposition"] = "ACTIVE";
    report["note"] = std::string{note};
    // Bootstrap currently initializes the runtime registry before replaying
    // package recovery. Ensure a row promoted here becomes executable during
    // this same boot rather than waiting for a second restart.
    if (!plinth::extensions::create_pool(name)) {
      return std::unexpected("reconcile runtime pool creation failed for " +
                             std::string{name});
    }
    if (auto r = write_recovery_record(id, "ACTIVE", report); !r) {
      plinth::extensions::destroy_pool(name);
      return std::unexpected("reconcile active record failed for " +
                             std::string{id} + ": " + r.error());
    }
    spdlog::info("reconcile: id={} name={} -> ACTIVE ({})", id, name, note);
    return {};
  };

  // ICD-0.4.5 §Crash Recovery mid-swap helper. The non-bundled T3 database
  // transaction can durably mark the new row ACTIVE before the active symlink
  // rename. Scan both ACTIVATING and successor ACTIVE rows so that shape is
  // forward-completed before HTTP ingress, then recreate the runtime pool.
  auto resolve_upgrade_mid_swap =
      [&](std::string_view new_id, std::string_view new_name,
          std::string_view new_version, std::string_view new_state,
          std::string_view supersedes_id) -> std::expected<void, std::string> {
    std::string sup_s{supersedes_id};
    std::array<const char*, 1> sup_v = {sup_s.c_str()};
    PgResultPtr row(
        plinth::db::exec_params(pg.conn,
                                "SELECT state, version FROM plinth.packages "
                                "WHERE id = $1::uuid",
                                1, nullptr, sup_v.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(row.get()) != PGRES_TUPLES_OK) {
      return std::unexpected("reconcile upgrade predecessor lookup failed: " +
                             std::string{PQresultErrorMessage(row.get())});
    }
    if (PQntuples(row.get()) == 0) {
      // Predecessor row gone (deleted, perhaps by a separate
      // uninstall path); back out the orphan ACTIVATING.
      return mark_failed(new_id, new_name, "ACTIVATING",
                         "upgrade predecessor missing; backed out",
                         /*drop_schema=*/false);
    }
    std::string old_state = PQgetvalue(row.get(), 0, 0);
    std::string old_version = PQgetvalue(row.get(), 0, 1);

    if (old_state == "ACTIVE" || old_state == "ACTIVE_FLAGGED") {
      // T3 PG tx never committed. Back out: new → INSTALL_FAILED,
      // old stays ACTIVE.
      if (new_state == "ACTIVATING") {
        return mark_failed(new_id, new_name, new_state,
                           "upgrade swap tx did not commit; backed out",
                           /*drop_schema=*/false);
      } else {
        return std::unexpected(
            "reconcile upgrade found active successor and predecessor for " +
            std::string{new_name});
      }
      return {};
    }
    if (old_state != "SUPERSEDED") {
      return std::unexpected("reconcile upgrade predecessor " + sup_s +
                             " has unexpected state '" + old_state + "'");
    }

    // old=SUPERSEDED + new=ACTIVATING → T3 PG tx committed, symlink
    // rename may not have. Compare symlink target against versions
    // to decide forward-complete vs. (rarely) back-out.
    fs::path active_path = ctx.data_dir / "extensions" / new_name / "active";
    std::error_code ec;
    fs::path link_target = fs::read_symlink(active_path, ec);
    std::string link_str = link_target.filename().string();

    if (link_str == new_version) {
      // Symlink already points at new; just advance state.
      if (new_state == "ACTIVATING") {
        return mark_active(new_id, new_name, new_state,
                           "mid-swap forward-completed "
                           "(symlink already points at new version)");
      }
      if (!plinth::extensions::create_pool(new_name)) {
        return std::unexpected("reconcile runtime pool recreation failed for " +
                               std::string{new_name});
      }
      return {};
    }
    if (link_str == old_version || ec) {
      // Symlink still at old (or unreadable). Complete the rename
      // to match the committed PG state.
      fs::path tmp = ctx.data_dir / "extensions" / new_name / "active.tmp";
      std::error_code ec2;
      fs::remove(tmp, ec2);
      fs::create_symlink(new_version, tmp, ec2);
      if (ec2) {
        spdlog::warn("reconcile mid-swap: create_symlink tmp for {} failed: "
                     "{}",
                     new_name, ec2.message());
        return std::unexpected("reconcile mid-swap symlink creation failed "
                               "for " +
                               std::string{new_name} + ": " + ec2.message());
      }
      if (::rename(tmp.c_str(), active_path.c_str()) != 0) {
        spdlog::warn("reconcile mid-swap: rename(active.tmp→active) for {} "
                     "failed: {}",
                     new_name, std::strerror(errno));
        return std::unexpected("reconcile mid-swap rename failed for " +
                               std::string{new_name} + ": " +
                               std::strerror(errno));
      }
      if (new_state == "ACTIVATING") {
        auto active = mark_active(new_id, new_name, new_state,
                                  "mid-swap forward-completed "
                                  "(symlink replayed new version)");
        if (!active.has_value()) {
          return active;
        }
      } else {
        spdlog::info("reconcile: active successor {} pointer replayed to {}",
                     new_id, new_version);
      }
      if (new_state != "ACTIVATING") {
        if (!plinth::extensions::create_pool(new_name)) {
          return std::unexpected(
              "reconcile runtime pool recreation failed for " +
              std::string{new_name});
        }
      }
      return {};
    }

    // Symlink points at some third version — unexpected. Leave for
    // human inspection rather than guess.
    return std::unexpected("reconcile active symlink for " +
                           std::string{new_name} + " points at '" + link_str +
                           "' (expected '" + old_version + "' or '" +
                           std::string{new_version} + "')");
  };

  for (int i = 0; i < row_count; ++i) {
    plinth::db::OperationScope::checkpoint_current();
    std::string id = PQgetvalue(res.get(), i, 0);
    std::string name = PQgetvalue(res.get(), i, 1);
    std::string version = PQgetvalue(res.get(), i, 2);
    std::string state = PQgetvalue(res.get(), i, 3);
    std::string supersedes_id = (PQgetisnull(res.get(), i, 4) != 0)
                                    ? std::string{}
                                    : std::string{PQgetvalue(res.get(), i, 4)};

    if (state == "UPLOADING" || state == "VALIDATING") {
      // Staging zip + validator scratch live outside plinth.packages;
      // no persistent schema side effects to clean.
      if (auto recovered = mark_failed(
              id, name, state, "crashed before any persistent side effects",
              /*drop_schema=*/false);
          !recovered.has_value()) {
        return recovered;
      }
      continue;
    }
    if (state == "MIGRATING" || state == "REGISTERING") {
      // ICD OQ #6: mid-MIGRATING is always INSTALL_FAILED because
      // the kernel cannot cheaply determine which migration was
      // in flight or whether an explicit COMMIT escaped the per-
      // migration transaction. REGISTERING rolls back cleanly on
      // crash (capability / panel / rbac inserts are TX-scoped
      // in Slice A), but the schema created in MIGRATING still
      // needs to go — drop unconditionally.
      if (auto recovered = mark_failed(
              id, name, state, "crashed during " + state + "; schema dropped",
              /*drop_schema=*/true);
          !recovered.has_value()) {
        return recovered;
      }
      continue;
    }
    if ((state == "ACTIVATING" || state == "ACTIVE" ||
         state == "ACTIVE_FLAGGED") &&
        !supersedes_id.empty()) {
      if (auto recovered =
              resolve_upgrade_mid_swap(id, name, version, state, supersedes_id);
          !recovered.has_value()) {
        return recovered;
      }
      continue;
    }
    if (state == "EXTRACTING" || state == "ACTIVATING") {
      // ICD-0.4.5 §Crash Recovery: ACTIVATING rows with a
      // supersedes_id are upgrade mid-swap and need the symlink-
      // aware resolution path. First-install ACTIVATING rows
      // fall through to the 0.4.4 logic below.
      fs::path tree = ctx.data_dir / "extensions" / name / version;
      fs::path manifest = tree / "manifest.json";
      std::error_code ec;
      bool tree_present = fs::exists(tree, ec) && !ec;
      bool manifest_present = fs::exists(manifest, ec) && !ec;
      if (tree_present && manifest_present) {
        // Tree fully extracted before the crash; REGISTERING's
        // transaction already committed (otherwise state would be
        // MIGRATING, not EXTRACTING/ACTIVATING). Advancing to ACTIVE
        // is safe — the bootstrap restore_routes() call rebuilds
        // the in-memory asset route map from every ACTIVE row.
        if (auto recovered = mark_active(
                id, name, state, "on-disk tree and manifest present; advanced");
            !recovered.has_value()) {
          return recovered;
        }
      } else {
        if (auto recovered = mark_failed(
                id, name, state, "on-disk tree incomplete after " + state,
                /*drop_schema=*/true);
            !recovered.has_value()) {
          return recovered;
        }
      }
      continue;
    }
    if (state == "SUPERSEDED") {
      // ICD-0.4.5 §Crash Recovery: a SUPERSEDED row with no
      // matching ACTIVE for the same name is either (a) an
      // uninstalled-child orphan (scenario U.05) — no action,
      // GC collects on its schedule — or (b) swap T4 was
      // partial; the partner ACTIVATING row from the same swap
      // is handled by resolve_upgrade_mid_swap earlier in this
      // loop. We just log and move on.
      std::string name_s = name;
      std::array<const char*, 1> name_v = {name_s.c_str()};
      PgResultPtr active_row(
          plinth::db::exec_params(
              pg.conn,
              "SELECT 1 FROM plinth.packages "
              "WHERE name = $1 AND state IN ('ACTIVE','ACTIVE_FLAGGED')",
              1, nullptr, name_v.data(), nullptr, nullptr, 0),
          PQclear);
      if (PQresultStatus(active_row.get()) != PGRES_TUPLES_OK) {
        return std::unexpected(
            std::string{PQresultErrorMessage(active_row.get())});
      }
      bool has_active = (PQresultStatus(active_row.get()) == PGRES_TUPLES_OK) &&
                        (PQntuples(active_row.get()) > 0);
      if (!has_active) {
        spdlog::info("reconcile: SUPERSEDED orphan id={} name={} version={}: "
                     "no matching ACTIVE for same name; leaving for GC",
                     id, name, version);
      }
      continue;
    }
    if (state == "UNINSTALLING") {
      // §Crash Recovery: the row entered UNINSTALLING (Tx A
      // committed + audit) but either Tx B / DROP SCHEMA / fs
      // cleanup / row-DELETE did not finish. Every step in
      // run_uninstall_cleanup is idempotent, so replay forward.
      auto lp_r = load_package_row(pg.conn, id);
      if (!lp_r.has_value()) {
        return std::unexpected("reconcile UNINSTALLING load failed for " + id +
                               ": " + lp_r.error());
      }
      auto cleanup = run_uninstall_cleanup(pg.conn, *lp_r, ctx);
      if (!cleanup.has_value()) {
        std::string message{"reconcile UNINSTALLING cleanup failed for "};
        message.append(name).append(" ").append(version).append(": ").append(
            cleanup.error().message);
        return std::unexpected(std::move(message));
      }
      Json::Value detail(Json::objectValue);
      detail["id"] = lp_r->id;
      detail["name"] = lp_r->name;
      detail["version"] = lp_r->version;
      detail["source"] = "reconciler";
      plinth::log::audit_sync(ctx.db, "packages.uninstalled", detail);
      spdlog::info("reconcile: id={} name={} -> uninstalled "
                   "(recovered from UNINSTALLING)",
                   id, name);
      continue;
    }
    std::string message{"reconcile encountered unexpected state '"};
    message.append(state)
        .append("' for id=")
        .append(id)
        .append(" name=")
        .append(name);
    return std::unexpected(std::move(message));
  }

  // Recovery mutations are complete. Release every name lock before launching
  // RBAC workers because each worker independently acquires the same lock.
  while (!locks.held.empty()) {
    const auto name = locks.held.back();
    if (auto released = release_name_lock_checked(pg.conn, name);
        !released.has_value()) {
      return std::unexpected("in-flight reconciliation lock release for '" +
                             name + "' failed: " + released.error());
    }
    locks.held.pop_back();
  }

  // ─── RBAC test sweeps (ICD-0.4.7) ─────────────────────────────
  // (a) Reap orphaned ephemeral users from a prior RBAC test run
  // that crashed before its scope guard fired. Threshold is 1 h —
  // the per-rule wall-clock cap is 5 s so anything older is by
  // definition stranded.
  auto cleaned = plinth::rbac::cleanup_orphaned_test_users(
      std::chrono::system_clock::now() - std::chrono::hours(1), *pg.conn);
  if (cleaned.has_value() && *cleaned > 0) {
    spdlog::info("reconcile: cleaned {} orphaned RBAC test run(s)", *cleaned);
  } else if (!cleaned.has_value()) {
    return std::unexpected("reconcile RBAC user cleanup failed: " +
                           cleaned.error());
  }

  if (ctx.schedule_rbac_tests) {
    auto scheduled = schedule_pending_rbac_tests(ctx);
    if (!scheduled.has_value()) {
      return std::unexpected(scheduled.error());
    }
  }

  spdlog::info("reconcile_in_flight_installs: done");
  return {};
}

auto schedule_pending_rbac_tests(const InstallerContext& ctx)
    -> std::expected<std::size_t, std::string> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(pg.conn != nullptr
                               ? std::string{PQerrorMessage(pg.conn)}
                               : std::string{"PQconnectdb returned null"});
  }

  // Schedule the RBAC test for fresh ACTIVE/ACTIVE_FLAGGED rows where
  // last_rbac_test_run_at is NULL. This covers the crash-during-post-install
  // handoff window. The one-hour ceiling bounds replays; older rows are
  // treated as administratively complete.
  PgResultPtr rbac_test_rows(
      plinth::db::exec(pg.conn,
                       "SELECT id::text FROM plinth.packages "
                       "WHERE state IN ('ACTIVE','ACTIVE_FLAGGED') "
                       "  AND last_rbac_test_run_at IS NULL "
                       "  AND installed_at > NOW() - interval '1 hour'"),
      PQclear);
  if (PQresultStatus(rbac_test_rows.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(
        "reconcile RBAC test schedule SELECT failed: " +
        std::string{PQresultErrorMessage(rbac_test_rows.get())});
  }
  const int rbac_test_count = PQntuples(rbac_test_rows.get());
  for (int i = 0; i < rbac_test_count; ++i) {
    plinth::db::OperationScope::checkpoint_current();
    std::string rbac_test_id = PQgetvalue(rbac_test_rows.get(), i, 0);
    rbac_test::schedule_rbac_test(rbac_test_id, ctx, "reconcile");
  }
  if (rbac_test_count > 0) {
    spdlog::info("reconcile: scheduled {} RBAC test run(s)", rbac_test_count);
  }
  return static_cast<std::size_t>(rbac_test_count);
}

// ─── 0.4.5 garbage collection ─────────────────────────────────────────

auto is_gc_eligible(std::chrono::system_clock::time_point retired_at,
                    std::chrono::system_clock::time_point now,
                    std::chrono::hours retention) -> bool {
  return retired_at + retention <= now;
}

namespace {

// PG timestamp "YYYY-MM-DD HH:MM:SS.ffffff+00" → system_clock::time_point.
// Returns nullopt on parse failure (the row is then skipped as unsafe).
auto parse_pg_timestamp(std::string_view s)
    -> std::optional<std::chrono::system_clock::time_point> {
  std::tm tm{};
  int micros = 0;
  // Minimal parser: strptime handles the date/time; we recover micros
  // from the tail. Timezone offset is assumed UTC (TIMESTAMPTZ always
  // stored normalized).
  std::string tmp{s};
  char* end = ::strptime(tmp.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
  if (end == nullptr) {
    return std::nullopt;
  }
  if (*end == '.') {
    std::string_view tail{tmp};
    std::size_t idx = static_cast<std::size_t>(end - tmp.c_str()) + 1;
    while (idx < tail.size() && tail[idx] >= '0' && tail[idx] <= '9') {
      micros = (micros * 10) + (tail[idx] - '0');
      ++idx;
    }
  }
  std::time_t tt = ::timegm(&tm);
  if (tt == -1) {
    return std::nullopt;
  }
  return std::chrono::system_clock::from_time_t(tt) +
         std::chrono::microseconds{micros};
}

// Best-effort recursive size. Used for GcReport.bytes_freed reporting;
// failure to stat is not fatal (warning recorded by caller).
auto tree_size_bytes(const fs::path& root) -> std::uintmax_t {
  std::error_code ec;
  std::uintmax_t total = 0;
  for (const auto& entry : fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file(ec) && !ec) {
      auto sz = entry.file_size(ec);
      if (!ec) {
        total += sz;
      }
    }
  }
  return total;
}

} // namespace

// scan touches PG + fs + advisory locks; splitting it into helpers obscures the
// ordering between tx, fs::remove_all, and lock release.
auto garbage_collect_superseded_versions(std::chrono::hours retention,
                                         const InstallerContext& ctx)
    -> std::expected<GcReport, GcFailure> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(GcFailure{
        .message = std::string{"db-connect: "} + PQerrorMessage(pg.conn),
        .partial_collected_ids = {},
    });
  }

  PgResultPtr rows(
      plinth::db::exec(
          pg.conn, "SELECT id, name, version, retired_at FROM plinth.packages "
                   "WHERE state = 'SUPERSEDED' AND retired_at IS NOT NULL "
                   "AND application_ready = FALSE "
                   "ORDER BY retired_at ASC"),
      PQclear);
  if (PQresultStatus(rows.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(GcFailure{
        .message =
            std::string{"scan-superseded: "} + PQresultErrorMessage(rows.get()),
        .partial_collected_ids = {},
    });
  }

  GcReport report;
  auto now = std::chrono::system_clock::now();
  int ntuples = PQntuples(rows.get());
  for (int i = 0; i < ntuples; ++i) {
    plinth::db::OperationScope::checkpoint_current();
    std::string id = PQgetvalue(rows.get(), i, 0);
    std::string name = PQgetvalue(rows.get(), i, 1);
    std::string version = PQgetvalue(rows.get(), i, 2);
    std::string retired = PQgetvalue(rows.get(), i, 3);

    auto retired_tp = parse_pg_timestamp(retired);
    if (!retired_tp.has_value()) {
      std::string msg = "gc: parse retired_at failed for id=" + id;
      msg += " retired=";
      msg += retired;
      report.warnings.push_back(std::move(msg));
      continue;
    }
    if (!is_gc_eligible(*retired_tp, now, retention)) {
      continue; // still within retention window
    }

    auto lock = try_acquire_name_lock(pg.conn, name);
    if (!lock.has_value()) {
      report.skipped_ids.push_back(id);
      continue;
    }

    fs::path version_dir = ctx.data_dir / "extensions" / name / version;
    std::uintmax_t bytes = 0;
    std::error_code ec;
    if (fs::exists(version_dir, ec)) {
      bytes = tree_size_bytes(version_dir);
    }

    std::array<const char*, 1> idv = {id.c_str()};
    PgResultPtr del(plinth::db::exec_params(
                        pg.conn,
                        "DELETE FROM plinth.packages WHERE id = $1::uuid "
                        "AND state = 'SUPERSEDED' "
                        "AND application_ready = FALSE",
                        1, nullptr, idv.data(), nullptr, nullptr, 0),
                    PQclear);
    if (PQresultStatus(del.get()) != PGRES_COMMAND_OK) {
      report.warnings.push_back("gc: DELETE failed for id=" + id + ": " +
                                PQresultErrorMessage(del.get()));
      release_name_lock(pg.conn, name);
      continue;
    }
    if (std::string_view{PQcmdTuples(del.get())} != "1") {
      report.skipped_ids.push_back(id);
      release_name_lock(pg.conn, name);
      continue;
    }

    std::error_code rm_ec;
    fs::remove_all(version_dir, rm_ec);
    if (rm_ec) {
      report.warnings.push_back("gc: remove_all(" + version_dir.string() +
                                ") failed: " + rm_ec.message());
    }

    report.collected_ids.push_back(id);
    report.bytes_freed += static_cast<std::size_t>(bytes);
    release_name_lock(pg.conn, name);
  }

  return report;
}

auto reconcile_application_readiness(const InstallerContext& ctx)
    -> std::expected<std::size_t, std::string> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(std::string{PQerrorMessage(pg.conn)});
  }
  // Snapshot only identities first, then take every package-name advisory lock
  // in deterministic order. Each row is re-read under those locks below, so a
  // concurrent lifecycle transition cannot be overwritten from a stale
  // startup snapshot. Packages created under a brand-new name after this
  // identity scan are owned by their lifecycle operation and are not touched.
  PgResultPtr candidates(
      plinth::db::exec(pg.conn, "SELECT id::text, name FROM plinth.packages "
                                "ORDER BY name, installed_at, id"),
      PQclear);
  if (PQresultStatus(candidates.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(candidates.get())});
  }

  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(PQntuples(candidates.get())));
  for (int row = 0; row < PQntuples(candidates.get()); ++row) {
    names.emplace_back(PQgetvalue(candidates.get(), row, 1));
  }
  names.erase(std::unique(names.begin(), names.end()), names.end());
  struct ReadinessLocks {
    PGconn* conn;
    std::vector<std::string> held;
    ~ReadinessLocks() {
      for (auto it = held.rbegin(); it != held.rend(); ++it) {
        release_name_lock(conn, *it);
      }
    }
  } locks{pg.conn, {}};
  locks.held.reserve(names.size());
  for (const auto& name : names) {
    auto acquired = try_acquire_name_lock(pg.conn, name);
    if (!acquired.has_value()) {
      return std::unexpected("application readiness lock for '" + name +
                             "' failed: " + acquired.error());
    }
    locks.held.push_back(name);
  }

  // Make the in-memory resolver snapshot authoritative before any durable
  // application is admitted. A successful atomic reload plus the exact
  // manifest-to-database comparison below proves the three views agree.
  auto cache_reload = plinth::capabilities::reload_tier2_cache(ctx.db);
  if (!cache_reload.has_value()) {
    return std::unexpected(
        "application readiness capability cache reload failed");
  }

  struct ReadinessChange {
    std::string id;
    bool ready;
  };
  std::vector<ReadinessChange> changes;
  for (int row = 0; row < PQntuples(candidates.get()); ++row) {
    const std::string id{PQgetvalue(candidates.get(), row, 0)};
    const std::string name{PQgetvalue(candidates.get(), row, 1)};
    std::array<const char*, 1> values{id.c_str()};
    PgResultPtr current(
        plinth::db::exec_params(
            pg.conn,
            "SELECT p.version, p.state, p.application_ready, EXISTS ("
            " SELECT 1 FROM plinth.panels pn "
            " JOIN plinth.rbac_rules r "
            "   ON r.rule = pn.declaration->>'rbac_rule' "
            "  AND r.extension_name = p.name AND r.orphaned_at IS NULL "
            " WHERE pn.package_id = p.id AND pn.panel_type = 'primary' "
            "   AND jsonb_typeof(pn.declaration->'client_path') = 'string' "
            "   AND pn.declaration->>'client_path' <> ''"
            "), NOT EXISTS ("
            " SELECT 1 FROM plinth.packages successor "
            " WHERE successor.supersedes_id = p.id"
            "), p.entry_point FROM plinth.packages p WHERE p.id = $1::uuid",
            1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(current.get()) != PGRES_TUPLES_OK) {
      return std::unexpected(std::string{PQresultErrorMessage(current.get())});
    }
    if (PQntuples(current.get()) == 0) {
      continue;
    }
    const std::string version{PQgetvalue(current.get(), 0, 0)};
    const std::string_view state{PQgetvalue(current.get(), 0, 1)};
    const bool was_ready =
        std::string_view{PQgetvalue(current.get(), 0, 2)} == "t";
    const bool metadata_ready =
        std::string_view{PQgetvalue(current.get(), 0, 3)} == "t";
    const bool has_no_successor =
        std::string_view{PQgetvalue(current.get(), 0, 4)} == "t";
    const std::string entry_point{PQgetvalue(current.get(), 0, 5)};
    const auto version_root = ctx.data_dir / "extensions" / name / version;
    std::error_code ec;
    const bool active_pointer_matches =
        fs::equivalent(version_root.parent_path() / "active", version_root, ec);
    bool prerequisites_ready =
        !ec && active_pointer_matches &&
        (state == "ACTIVE" || state == "ACTIVE_FLAGGED") && metadata_ready &&
        fs::is_regular_file(version_root / entry_point, ec) && !ec &&
        asset_server::has_registered_route(name, version) &&
        !capabilities::drain::is_fenced(name);
    if (prerequisites_ready) {
      prerequisites_ready = extensions::has_pool(name);
    }
    if (prerequisites_ready) {
      auto assets_ready =
          registered_panel_assets_ready(pg.conn, id, version_root);
      if (!assets_ready.has_value()) {
        return std::unexpected(assets_ready.error());
      }
      prerequisites_ready = *assets_ready;
    }
    if (prerequisites_ready) {
      auto authority_ready =
          registered_capabilities_ready(pg.conn, name, version_root);
      if (!authority_ready.has_value()) {
        return std::unexpected(authority_ready.error());
      }
      prerequisites_ready = *authority_ready;
    }
    const bool should_be_ready =
        prerequisites_ready && (was_ready || has_no_successor);
    if (should_be_ready != was_ready) {
      changes.push_back({.id = id, .ready = should_be_ready});
    }
  }

  if (changes.empty()) {
    return 0;
  }
  auto begin = pg_exec(pg.conn, "BEGIN");
  if (!begin.has_value()) {
    return std::unexpected(begin.error());
  }
  auto rollback = [&]() { static_cast<void>(pg_exec(pg.conn, "ROLLBACK")); };
  std::size_t changed = 0;
  for (const auto& change : changes) {
    std::array<const char*, 2> values{change.id.c_str(),
                                      change.ready ? "true" : "false"};
    PgResultPtr updated(
        plinth::db::exec_params(
            pg.conn,
            "UPDATE plinth.packages SET application_ready = $2::boolean "
            "WHERE id = $1::uuid AND application_ready <> $2::boolean",
            2, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(updated.get()) != PGRES_COMMAND_OK) {
      auto error = std::string{PQresultErrorMessage(updated.get())};
      rollback();
      return std::unexpected(std::move(error));
    }
    changed +=
        static_cast<std::size_t>(std::stoull(PQcmdTuples(updated.get())));
  }
  if (changed > 0) {
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      rollback();
      return std::unexpected(notified.error());
    }
  }
  auto committed = pg_exec(pg.conn, "COMMIT");
  if (!committed.has_value()) {
    return std::unexpected(committed.error());
  }
  return changed;
}

// ─── 0.4.5 upgrade_package ───────────────────────────────────────────

namespace {

auto upgrade_failure(std::string_view package_id, std::string_view code,
                     std::string_view message) -> TransitionFailure {
  return transition_failure(TransitionKind::UPGRADE, package_id, code, message);
}

auto emit_upgrade_audit(const InstallerContext& ctx, std::string_view action,
                        const LoadedPackage& existing, std::string_view new_id,
                        std::string_view new_version) -> void {
  Json::Value detail(Json::objectValue);
  detail["name"] = existing.name;
  detail["old_id"] = existing.id;
  detail["old_version"] = existing.version;
  detail["new_id"] = std::string{new_id};
  detail["new_version"] = std::string{new_version};
  if (!ctx.caller_user_id.empty()) {
    detail["upgraded_by_user_id"] = ctx.caller_user_id;
  }
  plinth::log::audit_sync(ctx.db, std::string{action}, detail);
}

auto insert_upgrade_row(PGconn* admin, std::string& new_id,
                        const MinimalManifest& minimal,
                        std::string_view supersedes_id,
                        std::string_view manifest_checksum,
                        std::string_view manifest_raw,
                        std::string_view caller_user_id, Provenance provenance)
    -> std::expected<void, std::string> {
  std::string id_s{new_id};
  std::string name_s{minimal.name};
  std::string ver_s{minimal.version};
  std::string sup_s{supersedes_id};
  std::string ck_s{manifest_checksum};
  std::string mf_s{manifest_raw};
  std::string caller_s{caller_user_id};
  std::string provenance_s{provenance_to_string(provenance)};
  std::string mount_s = minimal.frontend_mount.value_or(std::string{});
  std::string fentry_s = minimal.frontend_entry.value_or(std::string{});
  std::string entry_s = minimal.entry_point;
  const char* caller_ptr = caller_s.empty() ? nullptr : caller_s.c_str();
  const char* mount_ptr =
      minimal.frontend_mount.has_value() ? mount_s.c_str() : nullptr;
  const char* fentry_ptr =
      minimal.frontend_entry.has_value() ? fentry_s.c_str() : nullptr;
  std::array<const char*, 11> values = {
      id_s.c_str(),         // $1 id
      name_s.c_str(),       // $2 name
      ver_s.c_str(),        // $3 version
      sup_s.c_str(),        // $4 supersedes_id
      mf_s.c_str(),         // $5 manifest_json
      mount_ptr,            // $6 frontend_mount (nullable)
      fentry_ptr,           // $7 frontend_entry (nullable; ICD-0.6.1 §4.3)
      entry_s.c_str(),      // $8 entry_point
      ck_s.c_str(),         // $9 manifest_checksum
      caller_ptr,           // $10 installed_by_user_id (nullable)
      provenance_s.c_str(), // $11 trusted caller provenance
  };
  std::string sql =
      "INSERT INTO plinth.packages "
      "(id, name, version, state, provenance, supersedes_id, "
      " manifest_json, frontend_mount, frontend_entry, entry_point, "
      " manifest_checksum, installed_by_user_id) "
      "VALUES ($1::uuid, $2, $3, 'UPLOADING', $11, $4::uuid, "
      " $5::jsonb, $6, $7, $8, $9, NULLIF($10, '')::uuid)";
  if (provenance == Provenance::BUNDLED) {
    sql += " ON CONFLICT (name, version) DO UPDATE SET state='UPLOADING', "
           "manifest_json=EXCLUDED.manifest_json, "
           "frontend_mount=EXCLUDED.frontend_mount, "
           "frontend_entry=EXCLUDED.frontend_entry, "
           "entry_point=EXCLUDED.entry_point, "
           "manifest_checksum=EXCLUDED.manifest_checksum "
           "WHERE plinth.packages.state='INSTALL_FAILED' "
           "AND plinth.packages.provenance='bundled' "
           "AND plinth.packages.supersedes_id=EXCLUDED.supersedes_id";
  }
  sql += " RETURNING id::text";
  PgResultPtr res(plinth::db::exec_params(admin, sql.c_str(), 11, nullptr,
                                          values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {

    return std::unexpected(std::string{PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) != 1) {
    return std::unexpected(
        "existing candidate is not an eligible bundled retry");
  }
  new_id = PQgetvalue(res.get(), 0, 0);
  return {};
}

auto mark_upgrade_failed(PGconn* admin, std::string_view new_id,
                         const nlohmann::json& report) -> void {
  if (auto u = update_packages_state(admin, new_id, "INSTALL_FAILED");
      !u.has_value()) {
    spdlog::warn("upgrade: mark INSTALL_FAILED failed: {}", u.error());
  }
  static_cast<void>(update_packages_report(admin, new_id, report));
}

// List (namespace, version, function) tuples currently registered in
// plinth.capabilities for this extension that are NOT in the new
// manifest's `provides` set. Caller unregisters each at T4.
auto compute_v1_only_capabilities(PGconn* admin, std::string_view name,
                                  const CapabilityManifest& v2)
    -> std::vector<std::tuple<std::string, int, std::string>> {
  std::vector<std::tuple<std::string, int, std::string>> out;
  std::string name_s{name};
  std::array<const char*, 1> name_v = {name_s.c_str()};
  PgResultPtr res(plinth::db::exec_params(
                      admin,
                      "SELECT DISTINCT namespace, version, function "
                      "FROM plinth.capabilities WHERE extension_name = $1",
                      1, nullptr, name_v.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return out;
  }
  int n = PQntuples(res.get());
  for (int i = 0; i < n; ++i) {
    std::string ns{PQgetvalue(res.get(), i, 0)};
    int ver = std::stoi(std::string{PQgetvalue(res.get(), i, 1)});
    std::string fn{PQgetvalue(res.get(), i, 2)};
    bool in_v2 = std::ranges::any_of(v2.provides, [&](const auto& cap) {
      return cap.namespace_ == ns && cap.version == ver && cap.function == fn;
    });
    if (!in_v2) {
      out.emplace_back(std::move(ns), ver, std::move(fn));
    }
  }
  return out;
}

} // namespace

// pipeline (UPLOADING → ACTIVATING → atomic swap T0-T5) is one ordered
// choreography; splitting into helpers scatters the rollback flow.
auto upgrade_package(std::span<const std::byte> zip_blob,
                     std::string_view existing_package_id,
                     const InstallerContext& ctx, Provenance provenance)
    -> std::expected<UpgradeReport, TransitionFailure> {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(upgrade_failure(existing_package_id, "db-error",
                                           PQerrorMessage(pg.conn)));
  }

  // 1. Load existing row, validate state.
  auto lp_r = load_package_row(pg.conn, existing_package_id);
  if (!lp_r.has_value()) {
    const auto* code = (lp_r.error() == "not-found") ? "not-found" : "db-error";
    return std::unexpected(
        upgrade_failure(existing_package_id, code, lp_r.error()));
  }
  LoadedPackage existing = *lp_r;
  if (existing.provenance != provenance_to_string(provenance)) {
    return std::unexpected(
        upgrade_failure(existing_package_id, "provenance-mismatch",
                        "upgrade provenance must match the installed package"));
  }
  if (existing.state != "ACTIVE" && existing.state != "ACTIVE_FLAGGED") {
    return std::unexpected(
        upgrade_failure(existing_package_id, "invalid-state-transition",
                        "cannot upgrade from state " + existing.state));
  }

  // 2. Advisory lock on name (released by LockGuard).
  auto lock = try_acquire_name_lock(pg.conn, existing.name);
  if (!lock.has_value()) {
    return std::unexpected(upgrade_failure(
        existing_package_id, "in-flight-operation", lock.error()));
  }
  auto release_lock = [&]() { release_name_lock(pg.conn, existing.name); };
  struct LockGuard {
    std::function<void()> f;
    explicit LockGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~LockGuard() {
      if (f) {
        f();
      }
    }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  } lg{release_lock};

  struct UpgradeDrainGuard {
    std::string name;
    bool active{false};
    bool release_on_destroy{true};
    explicit UpgradeDrainGuard(std::string n) : name(std::move(n)) {}
    ~UpgradeDrainGuard() {
      if (release_on_destroy) {
        finish();
      } else if (active) {
        capabilities::drain::block_application(name);
      }
    }
    auto start() -> std::shared_ptr<capabilities::drain::DrainState> {
      active = true;
      return capabilities::drain::begin_drain(name);
    }
    auto finish() -> void {
      if (active) {
        capabilities::drain::end_drain(name);
        active = false;
      }
    }
    auto preserve_fence() -> void { release_on_destroy = false; }
    UpgradeDrainGuard(const UpgradeDrainGuard&) = delete;
    auto operator=(const UpgradeDrainGuard&) -> UpgradeDrainGuard& = delete;
  } upgrade_drain{existing.name};

  // 3. Staging extract + minimal manifest parse.
  std::string new_id = uuid_v4();
  fs::path staging = ctx.staging_dir / new_id;
  std::error_code ec;
  fs::create_directories(staging, ec);
  if (ec) {
    return std::unexpected(
        upgrade_failure(existing_package_id, "staging-mkdir", ec.message()));
  }
  struct StagingGuard {
    fs::path p;
    explicit StagingGuard(fs::path path) : p(std::move(path)) {}
    ~StagingGuard() {
      std::error_code ec2;
      fs::remove_all(p, ec2);
    }
    StagingGuard(const StagingGuard&) = delete;
    auto operator=(const StagingGuard&) -> StagingGuard& = delete;
    StagingGuard(StagingGuard&&) = delete;
    auto operator=(StagingGuard&&) -> StagingGuard& = delete;
  };
  StagingGuard sg{staging};

  if (auto ex = extract_zip_to(zip_blob, staging, ctx.max_package_size_bytes);
      !ex.has_value()) {
    return std::unexpected(upgrade_failure(existing_package_id, ex.error().kind,
                                           ex.error().message));
  }
  // Only the kernel's explicit bundled-shell operation supplies BUNDLED.
  // HTTP callers retain USER and cannot upgrade a reserved bundled package.
  auto minimal_r = read_minimal_manifest(staging, provenance);
  if (!minimal_r.has_value()) {
    return std::unexpected(upgrade_failure(
        existing_package_id, "manifest-parse", minimal_r.error().message));
  }
  const MinimalManifest& minimal = *minimal_r;

  // 4. Sanity: incoming name matches existing; version strictly greater.
  if (minimal.name != existing.name) {
    return std::unexpected(upgrade_failure(
        existing_package_id, "name-mismatch",
        "upgrade package name '" + minimal.name +
            "' does not match existing '" + existing.name + "'"));
  }
  if (compare_semver(minimal.version, existing.version) <= 0) {
    return std::unexpected(upgrade_failure(
        existing_package_id, "upgrade-version-not-newer",
        "incoming " + minimal.version + " not > existing " + existing.version));
  }

  // 5. INSERT new row with state=UPLOADING, supersedes_id=existing.
  fs::path mf_path = staging / "manifest.json";
  std::ifstream mf_in(mf_path);
  std::stringstream mf_buf;
  mf_buf << mf_in.rdbuf();
  std::string manifest_raw = mf_buf.str();
  std::string manifest_checksum = sha256_hex(manifest_raw);
  if (auto ins = insert_upgrade_row(pg.conn, new_id, minimal, existing.id,
                                    manifest_checksum, manifest_raw,
                                    ctx.caller_user_id, provenance);
      !ins.has_value()) {
    return std::unexpected(
        upgrade_failure(existing_package_id, "db-error", ins.error()));
  }

  const bool bundled = provenance == Provenance::BUNDLED;
  bool bundle_transaction_open = false;
  bool bundle_committed = false;
  std::optional<fs::path> owned_version_dir;
  auto rollback_bundle = [&]() {
    if (bundle_transaction_open) {
      if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
        spdlog::error("bundled upgrade ROLLBACK failed: {}", r.error());
      }
      bundle_transaction_open = false;
    }
  };
  LockGuard bundle_guard{[&]() {
    rollback_bundle();
    if (owned_version_dir && !bundle_committed) {
      std::error_code cleanup_error;
      fs::remove_all(*owned_version_dir, cleanup_error);
      if (cleanup_error) {
        spdlog::error("bundled upgrade file cleanup failed: {}",
                      cleanup_error.message());
      }
    }
  }};

  auto fail_and_mark =
      [&](std::string_view code, std::string_view msg,
          nlohmann::json extra_report = {}) -> TransitionFailure {
    nlohmann::json r;
    r["kind"] = std::string{code};
    r["message"] = std::string{msg};
    if (!extra_report.is_null() && extra_report.is_object()) {
      for (auto it = extra_report.begin(); it != extra_report.end(); ++it) {
        r[it.key()] = it.value();
      }
    }
    rollback_bundle();
    mark_upgrade_failed(pg.conn, new_id, r);
    if (bundled) {
      Json::Value detail(Json::objectValue);
      detail["id"] = new_id;
      detail["name"] = minimal.name;
      detail["version"] = minimal.version;
      detail["kind"] = std::string{code};
      detail["message"] = std::string{msg};
      plinth::log::audit_sync(ctx.db, "packages.bundled_upgrade_failed",
                              detail);
    }
    return TransitionFailure{
        .kind = TransitionKind::UPGRADE,
        .package_id = new_id,
        .message = std::string{msg},
        .report = std::move(r),
    };
  };

  // 6. VALIDATING.
  if (auto u = update_packages_state(pg.conn, new_id, "VALIDATING");
      !u.has_value()) {
    return std::unexpected(fail_and_mark("db-error", u.error()));
  }
  ValidationConfig vcfg{};
  vcfg.cross_file = true;
  vcfg.against_running_kernel = false; // RT1 whitelisted via in-process path
  vcfg.upgrade_from_id = existing.id;
  vcfg.is_bundled = (provenance == Provenance::BUNDLED);
  auto vr = validate(staging, vcfg);
  if (vr.disposition() == 1) {
    return std::unexpected(fail_and_mark("validation-errors",
                                         "upgrade validation failed",
                                         build_validation_report(vr)));
  }

  // Fence capability ingress and drain before migration or registration can
  // mutate shared package authority. Once quiesced, atomically withdraw the
  // old launcher generation and publish the durable refresh hint.
  auto drain_state = upgrade_drain.start();
  auto drain_start = std::chrono::steady_clock::now();
  auto [drained, outstanding] = capabilities::drain::wait_for_zero(
      drain_state, ctx.upgrade_drain_timeout_ms);
  auto drain_waited = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - drain_start);
  if (!drained) {
    nlohmann::json report;
    report["kind"] = "upgrade-drain-timeout";
    report["outstanding"] = outstanding;
    return std::unexpected(
        fail_and_mark("upgrade-drain-timeout", "drain window expired", report));
  }
  if (existing.application_ready) {
    if (auto begin = pg_exec(pg.conn, "BEGIN"); !begin.has_value()) {
      return std::unexpected(fail_and_mark("db-error", begin.error()));
    }
    std::string old_id{existing.id};
    std::array<const char*, 1> old_values{old_id.c_str()};
    PgResultPtr hidden(
        plinth::db::exec_params(
            pg.conn,
            "UPDATE plinth.packages SET application_ready = FALSE "
            "WHERE id = $1::uuid",
            1, nullptr, old_values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(hidden.get()) != PGRES_COMMAND_OK) {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(
          fail_and_mark("db-error", PQresultErrorMessage(hidden.get())));
    }
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(fail_and_mark("db-error", notified.error()));
    }
    // A failed acknowledgement can still mean the readiness withdrawal and
    // invalidation committed remotely. Preserve ingress before COMMIT and only
    // release it from the verified terminal success path.
    upgrade_drain.preserve_fence();
    auto committed = pg_exec(pg.conn, "COMMIT");
    if (!committed.has_value()) {
      return std::unexpected(fail_and_mark("db-error", committed.error()));
    }
  }
  // From this point onward migration/registration may have changed shared
  // authority. Any failure remains fail-closed and leaves ingress fenced for
  // an explicit retry/reconciliation instead of exposing a mixed generation.
  upgrade_drain.preserve_fence();

  // Trusted bundled upgrades retain one transaction through migration,
  // registration, extraction and activation. A later failure restores the
  // installed shell's schema, data and identities, including migration rows.
  if (bundled) {
    if (auto begin_bundle = pg_exec(pg.conn, "BEGIN"); !begin_bundle) {
      return std::unexpected(fail_and_mark("db-error", begin_bundle.error()));
    }
    bundle_transaction_open = true;
  }

  // 7. MIGRATING.
  if (auto u = update_packages_state(pg.conn, new_id, "MIGRATING");
      !u.has_value()) {
    return std::unexpected(fail_and_mark("db-error", u.error()));
  }
  auto mig = run_migrations(minimal.name, staging, *pg.conn,
                            bundled ? MigrationTransaction::CALLER_OWNED
                                    : MigrationTransaction::PER_FILE);
  if (!mig.has_value()) {
    const auto& me = mig.error();
    nlohmann::json r;
    r["kind"] = "upgrade-migration-failed";
    r["message"] = me.message;
    if (me.migration_file.has_value()) {
      r["migration_file"] = *me.migration_file;
    }
    return std::unexpected(
        fail_and_mark("upgrade-migration-failed", me.message, r));
  }

  // Full-manifest parse for REGISTERING.
  std::ifstream cm_in(staging / "capabilities.json");
  std::stringstream cm_buf;
  cm_buf << cm_in.rdbuf();
  auto cm_parse = CapabilityManifest::parse(
      cm_buf.str(), (staging / "capabilities.json").string());
  if (!cm_parse.value.has_value()) {
    return std::unexpected(
        fail_and_mark("capabilities-parse", "capabilities.json parse failed"));
  }
  CapabilityManifest cm = *cm_parse.value;

  std::optional<PanelsManifest> panels;
  if (fs::exists(staging / "panels.json")) {
    std::ifstream pm_in(staging / "panels.json");
    std::stringstream pm_buf;
    pm_buf << pm_in.rdbuf();
    auto pm_parse =
        PanelsManifest::parse(pm_buf.str(), (staging / "panels.json").string());
    if (pm_parse.value.has_value()) {
      panels = *pm_parse.value;
    }
  }

  // 8. REGISTERING — upgrade variant, single tx.
  if (!bundled) {
    auto begin = pg_exec(pg.conn, "BEGIN");
    if (!begin.has_value()) {
      return std::unexpected(fail_and_mark("db-error", begin.error()));
    }
  }
  bool reg_committed = false;
  auto reg_rollback = [&]() {
    if (!bundled && !reg_committed) {
      if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
        spdlog::warn("upgrade: REGISTERING ROLLBACK failed: {}", r.error());
      }
    }
  };
  struct RollbackGuard {
    std::function<void()> f;
    explicit RollbackGuard(std::function<void()> fn) : f(std::move(fn)) {}
    ~RollbackGuard() {
      if (f) {
        f();
      }
    }
    RollbackGuard(const RollbackGuard&) = delete;
    auto operator=(const RollbackGuard&) -> RollbackGuard& = delete;
    RollbackGuard(RollbackGuard&&) = delete;
    auto operator=(RollbackGuard&&) -> RollbackGuard& = delete;
  };
  RbacReconciliation rbac_report;
  std::vector<std::tuple<std::string, int, std::string>> v1_only_caps;
  {
    RollbackGuard rg{reg_rollback};
    auto rr = run_stage_registering_upgrade(pg.conn, new_id, minimal.name,
                                            staging, cm, panels);
    if (!rr.has_value()) {
      return std::unexpected(fail_and_mark("db-error", rr.error()));
    }
    rbac_report = *rr;

    // Snapshot v1-only caps under the same tx — SELECT sees the
    // pre-DELETE state rolled back above only if we ROLLBACK;
    // since we COMMIT, v1-only is derived from plinth.capabilities
    // BEFORE the tx's DELETE. Run it before COMMIT by querying
    // against the existing state view? No — DELETE already ran in
    // run_stage_registering_upgrade, so a later SELECT sees empty.
    // Instead: snapshot BEFORE REGISTERING — do this below.
    (void)v1_only_caps; // populated below from a pre-tx snapshot

    if (!bundled) {
      auto commit = pg_exec(pg.conn, "COMMIT");
      if (!commit.has_value()) {
        return std::unexpected(fail_and_mark("db-error", commit.error()));
      }
    }
    reg_committed = true;
  }

  // 9. EXTRACTING — copy to new version dir, do not touch active symlink.
  if (auto u = update_packages_state(pg.conn, new_id, "EXTRACTING");
      !u.has_value()) {
    return std::unexpected(fail_and_mark("db-error", u.error()));
  }
  fs::path new_version_dir =
      ctx.data_dir / "extensions" / minimal.name / minimal.version;
  fs::create_directories(new_version_dir.parent_path(), ec);
  if (ec) {
    return std::unexpected(fail_and_mark("extraction-failed", ec.message()));
  }
  if (bundled) {
    // Never overwrite or clean up a directory owned by another attempt.
    if (!fs::create_directory(new_version_dir, ec)) {
      return std::unexpected(fail_and_mark(
          "extraction-failed",
          ec ? ec.message() : "incoming version directory already exists"));
    }
    owned_version_dir = new_version_dir;
  }
  fs::copy(staging, new_version_dir,
           fs::copy_options::recursive | fs::copy_options::skip_symlinks, ec);
  if (ec) {
    return std::unexpected(fail_and_mark("extraction-failed", ec.message()));
  }

  // ── Atomic swap T0-T5 ──
  emit_upgrade_audit(ctx, "packages.upgrade_started", existing, new_id,
                     minimal.version);

  // T0 — declare intent.
  if (auto u = update_packages_state(pg.conn, new_id, "ACTIVATING");
      !u.has_value()) {
    return std::unexpected(fail_and_mark("db-error", u.error()));
  }

  // T1/T2 were completed before migration and registration above.

  // T3 — swap tx: old → SUPERSEDED+retired_at, new → ACTIVE.
  if (!bundled) {
    auto swap_begin = pg_exec(pg.conn, "BEGIN");
    if (!swap_begin.has_value()) {
      return std::unexpected(
          fail_and_mark("upgrade-swap-failed", swap_begin.error()));
    }
  }
  bool swap_committed = false;
  auto swap_rollback = [&]() {
    if (!bundled && !swap_committed) {
      if (auto r = pg_exec(pg.conn, "ROLLBACK"); !r) {
        spdlog::warn("upgrade: T3 ROLLBACK failed: {}", r.error());
      }
    }
  };
  {
    RollbackGuard rg{swap_rollback};
    std::string old_id_s = existing.id;
    std::array<const char*, 1> old_v = {old_id_s.c_str()};
    PgResultPtr r1(plinth::db::exec_params(
                       pg.conn,
                       "UPDATE plinth.packages SET state='SUPERSEDED', "
                       "retired_at=NOW() WHERE id=$1::uuid",
                       1, nullptr, old_v.data(), nullptr, nullptr, 0),
                   PQclear);
    if (PQresultStatus(r1.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(
          fail_and_mark("upgrade-swap-failed", PQresultErrorMessage(r1.get())));
    }
    std::array<const char*, 1> new_v = {new_id.c_str()};
    PgResultPtr r2(
        plinth::db::exec_params(pg.conn,
                                "UPDATE plinth.packages SET state='ACTIVE' "
                                "WHERE id=$1::uuid",
                                1, nullptr, new_v.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(r2.get()) != PGRES_COMMAND_OK) {
      return std::unexpected(
          fail_and_mark("upgrade-swap-failed", PQresultErrorMessage(r2.get())));
    }
    // Read retired_at back for the report.
    PgResultPtr rt(
        plinth::db::exec_params(
            pg.conn, "SELECT retired_at FROM plinth.packages WHERE id=$1::uuid",
            1, nullptr, old_v.data(), nullptr, nullptr, 0),
        PQclear);
    std::string retired_at_s;
    if (PQresultStatus(rt.get()) == PGRES_TUPLES_OK &&
        PQntuples(rt.get()) == 1) {
      retired_at_s = PQgetvalue(rt.get(), 0, 0);
    }
    if (!bundled) {
      auto commit = pg_exec(pg.conn, "COMMIT");
      if (!commit.has_value()) {
        return std::unexpected(
            fail_and_mark("upgrade-swap-failed", commit.error()));
      }
    }
    swap_committed = true;
    (void)retired_at_s; // structured report sent at T4
  }

  // T3 — symlink flip, synchronized via advisory lock (held).
  fs::path active_path = ctx.data_dir / "extensions" / minimal.name / "active";
  fs::path tmp_path = ctx.data_dir / "extensions" / minimal.name / "active.tmp";
  fs::remove(tmp_path, ec); // clear any leftover tmp
  fs::create_symlink(minimal.version, tmp_path, ec);
  if (ec) {
    return std::unexpected(
        fail_and_mark("upgrade-swap-failed", "symlink tmp: " + ec.message()));
  }
  // POSIX rename(2) is atomic when source and dest are on the same
  // filesystem (B9 bootstrap check enforces).
  if (::rename(tmp_path.c_str(), active_path.c_str()) != 0) {
    return std::unexpected(fail_and_mark(
        "upgrade-swap-failed", "rename(active.tmp → active) failed: " +
                                   std::string{std::strerror(errno)}));
  }

  if (bundled) {
    auto commit = pg_exec(pg.conn, "COMMIT");
    if (!commit) {
      // A lost COMMIT acknowledgement has an uncertain database outcome.
      // Retain both versions even if restoring the old pointer succeeds.
      owned_version_dir.reset();
      const bool outcome_unknown =
          PQstatus(pg.conn) != CONNECTION_OK ||
          PQtransactionStatus(pg.conn) == PQTRANS_UNKNOWN;
      // The process is still before HTTP ingress. Restore the old pointer
      // before reporting failure and rolling the database transaction back.
      fs::create_symlink(existing.version, tmp_path, ec);
      if (ec || ::rename(tmp_path.c_str(), active_path.c_str()) != 0) {
        // Retain the new files if the pointer cannot be restored.
        owned_version_dir.reset();
        if (outcome_unknown) {
          bundle_transaction_open = false;
          return std::unexpected(upgrade_failure(
              new_id, "upgrade-recovery-required",
              "database commit outcome is unknown and the active pointer could "
              "not be restored; inspect retained versions"));
        }
        return std::unexpected(
            fail_and_mark("upgrade-recovery-required",
                          "database commit failed and the active symlink could "
                          "not be restored"));
      }
      if (outcome_unknown) {
        bundle_transaction_open = false;
        return std::unexpected(upgrade_failure(
            new_id, "upgrade-recovery-required",
            "database commit outcome is unknown; inspect installed version and "
            "retained files before restart"));
      }
      return std::unexpected(
          fail_and_mark("upgrade-swap-failed", commit.error()));
    }
    bundle_transaction_open = false;
    bundle_committed = true;
  }

  emit_upgrade_audit(ctx, "packages.upgrade_swapped", existing, new_id,
                     minimal.version);

  // T4 — route + capability + pool cutover. The pool swap brackets
  // the route registration so in-flight callers on the old pool race-
  // lose to `cap.extension_not_loaded` after their bc is released
  // (the shared_lock in `extensions::dispatch` waits only for the
  // active in-flight dispatch, not subsequent ones). ICD-0.5.0.3
  // §Hot reload H.02.
  asset_server::unregister_routes(existing.name, existing.version);
  // ICD-0.5.1 §Extension-lifecycle integration — drain coalescer
  // windows owned by this extension before the pool cutover so the
  // v1 pool's last Layer-1 envelopes land under the old version's
  // identity.
  plinth::realtime::CoalescerRegistry::instance().drain_extension(minimal.name);
  // ICD-0.5.3 §`db.batch()` §Drain hook — UPGRADING counterpart.
  plinth::js::discard_batches_for_extension(minimal.name);
  // ICD-0.5.2 §Extension Lifecycle Integration — evict WS + JS
  // subscriptions tied to the v1 extension identity before the pool
  // cutover. Post-upgrade the client must re-subscribe (per ICD
  // §Reconnect posture — drained subs are not restored).
  plinth::realtime::broker::drain_extension(minimal.name, "upgrading");
  plinth::extensions::destroy_pool(minimal.name);
  fs::path new_client_root = new_version_dir / "client";
  asset_server::register_routes(minimal.name, minimal.version, new_client_root,
                                manifest_checksum);
  const bool server_package =
      fs::is_regular_file(new_version_dir / minimal.entry_point);
  const bool pool_created = plinth::extensions::create_pool(minimal.name);
  if (!server_package || !pool_created) {
    return std::unexpected(upgrade_failure(
        new_id, "runtime-start-failed",
        !server_package
            ? "declared entry point is unavailable; application remains unready"
            : "new extension runtime failed to start; application remains "
              "unready"));
  }
  // Unregister v1-only capabilities. (v1∩v2 rows were already
  // DELETEd + re-INSERTed during REGISTERING-upgrade, so this only
  // catches capabilities present in old but removed in new — which
  // our DELETE in REGISTERING-upgrade already handled. Re-run the
  // snapshot to be defensive: any leftover row from a concurrent
  // register that we missed.)
  v1_only_caps = compute_v1_only_capabilities(pg.conn, minimal.name, cm);
  for (const auto& [ns, ver, fn] : v1_only_caps) {
    if (auto u = capabilities::unregister_capability(ns, ver, fn, *pg.conn);
        !u.has_value()) {
      spdlog::warn("upgrade T4: unregister_capability({}:{}:{}) "
                   "failed: {}",
                   ns, ver, fn, u.error());
    }
  }

  // Refresh after the defensive unregister pass so releasing the lifecycle
  // fence can never expose a v1-only entry retained by the old snapshot.
  auto cache_reload = plinth::capabilities::reload_tier2_cache(ctx.db);
  if (!cache_reload.has_value()) {
    return std::unexpected(upgrade_failure(
        new_id, "capability-refresh-failed",
        "capability cache refresh failed; application remains unready"));
  }

  const bool new_application_ready =
      panels.has_value() && !panels->panels.empty();
  if (new_application_ready) {
    if (auto begin = pg_exec(pg.conn, "BEGIN"); !begin.has_value()) {
      return std::unexpected(
          upgrade_failure(new_id, "db-error", begin.error()));
    }
    std::array<const char*, 1> new_values{new_id.c_str()};
    PgResultPtr admitted(
        plinth::db::exec_params(
            pg.conn,
            "UPDATE plinth.packages SET application_ready = TRUE "
            "WHERE id = $1::uuid AND state IN ('ACTIVE','ACTIVE_FLAGGED')",
            1, nullptr, new_values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(admitted.get()) != PGRES_COMMAND_OK ||
        std::string_view{PQcmdTuples(admitted.get())} != "1") {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(upgrade_failure(
          new_id, "db-error", PQresultErrorMessage(admitted.get())));
    }
    auto notified = emit_applications_changed(pg.conn);
    if (!notified.has_value()) {
      static_cast<void>(pg_exec(pg.conn, "ROLLBACK"));
      return std::unexpected(
          upgrade_failure(new_id, "db-error", notified.error()));
    }
    auto committed = pg_exec(pg.conn, "COMMIT");
    if (!committed.has_value()) {
      return std::unexpected(
          upgrade_failure(new_id, "db-error", committed.error()));
    }
  }

  emit_upgrade_audit(ctx, "packages.upgrade_completed", existing, new_id,
                     minimal.version);
  upgrade_drain.finish();
  if (auto released = release_name_lock_checked(pg.conn, minimal.name);
      !released) {
    auto report = rbac_handoff_failure_report(released.error());
    // The swap is committed. Preserve both version rows and installed files;
    // fail_and_mark is only for errors before this completed cutover.
    return std::unexpected(
        TransitionFailure{.kind = TransitionKind::UPGRADE,
                          .package_id = new_id,
                          .message = report["message"].get<std::string>(),
                          .report = std::move(report)});
  }
  lg.f = {};
  if (ctx.schedule_rbac_tests) {
    rbac_test::schedule_rbac_test(new_id, ctx, "upgrade");
  }

  // T5 — retention starts with retired_at on the old row. 0.7.x
  // scheduler reads that column on a cadence; 0.4.5 ships only the
  // library entry garbage_collect_superseded_versions.
  UpgradeReport report;
  report.new_record.id = new_id;
  report.new_record.name = minimal.name;
  report.new_record.version = minimal.version;
  report.new_record.state = InstallStage::ACTIVE;
  report.new_record.provenance = provenance;
  report.new_record.manifest_json =
      nlohmann::json::parse(manifest_raw, nullptr, false);
  report.new_record.installed_at = std::chrono::system_clock::now();
  report.superseded_id = existing.id;
  report.retired_at = std::chrono::system_clock::now();
  report.rbac = std::move(rbac_report);
  report.drain_waited = drain_waited;
  return report;
}

auto check_single_mountpoint(const fs::path& data_dir,
                             const fs::path& staging_dir)
    -> std::expected<void, std::string> {
  // stat(2) each path that exists; missing paths are tolerated (the
  // installer creates them on first use). st_dev mismatches between
  // any two existing paths abort bootstrap — the atomic-swap
  // rename(2) would silently fall back to copy+unlink, losing the
  // atomicity guarantee readers rely on.
  std::array<fs::path, 3> paths = {
      data_dir,
      data_dir / "extensions",
      staging_dir,
  };
  std::optional<dev_t> anchor_dev;
  std::string anchor_path;
  for (const auto& p : paths) {
    struct ::stat st{};
    if (::stat(p.c_str(), &st) != 0) {
      continue; // path may not exist yet — tolerated
    }
    if (!anchor_dev.has_value()) {
      anchor_dev = st.st_dev;
      anchor_path = p.string();
      continue;
    }
    if (st.st_dev != *anchor_dev) {
      return std::unexpected(
          std::string{"atomic swap requires a single mountpoint: "} +
          anchor_path + " and " + p.string() +
          " are on different filesystems (st_dev " +
          std::to_string(*anchor_dev) + " vs " + std::to_string(st.st_dev) +
          ")");
    }
  }
  return {};
}

} // namespace plinth::packages
