#include "kernel/shell/firstboot.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include "kernel/config.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/manifest.hpp"

#include <json/value.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <ios>
#include <memory>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zip.h>

namespace plinth::shell {

namespace {

namespace fs = std::filesystem;

// Audit categories — single-shot, no dedup; ICD-0.6.1 §10.1.
constexpr std::string_view AUDIT_STARTED =
    "shell.firstboot.bundled_install_started";
constexpr std::string_view AUDIT_COMPLETED =
    "shell.firstboot.bundled_install_completed";
constexpr std::string_view AUDIT_FAILED =
    "shell.firstboot.bundled_install_failed";

auto build_actor() -> Json::Value {
  Json::Value actor(Json::objectValue);
  actor["kind"] = "system";
  actor["id"] = "kernel-firstboot";
  return actor;
}

auto emit_failed(const Config::Database& db, std::string_view failure_kind,
                 std::string_view message, std::string_view failed_stage = {})
    -> void {
  Json::Value detail(Json::objectValue);
  detail["actor"] = build_actor();
  detail["failure_kind"] = std::string{failure_kind};
  detail["message"] = std::string{message};
  if (!failed_stage.empty()) {
    detail["failed_stage"] = std::string{failed_stage};
  }
  plinth::log::audit_sync(db, AUDIT_FAILED, detail);
}

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

struct PgResult {
  PGresult* res = nullptr;
  explicit PgResult(PGresult* r) : res(r) {}
  ~PgResult() {
    if (res != nullptr) {
      PQclear(res);
    }
  }
  PgResult(const PgResult&) = delete;
  auto operator=(const PgResult&) -> PgResult& = delete;
  PgResult(PgResult&&) = delete;
  auto operator=(PgResult&&) -> PgResult& = delete;
};

enum class DetectOutcome : std::uint8_t {
  NONE,
  EXACTLY_ONE,
  TOO_MANY,
};

// SELECT for the singleton-invariant detection. Also surfaces an
// existing user-uploaded `name='shell'` row separately so the
// pre-flight can return SCHEMA_RESERVED before attempting an install
// that would race the unique-name index.
struct DetectResult {
  DetectOutcome outcome = DetectOutcome::NONE;
  bool user_shell_present = false;
  std::string id;
  std::string name;
  std::string version;
};

auto detect_active_bundled_frontend(PGconn* conn)
    -> std::expected<DetectResult, std::string> {
  PgResult res{plinth::db::exec(
      conn, "SELECT name, provenance, state, id::text, version "
            "FROM plinth.packages "
            "WHERE name = 'shell' "
            "   OR (provenance = 'bundled' "
            "       AND frontend_mount IS NOT NULL "
            "       AND state IN ('ACTIVE','ACTIVE_FLAGGED'))")};

  if (PQresultStatus(res.res) != PGRES_TUPLES_OK) {
    return std::unexpected(std::string{PQresultErrorMessage(res.res)});
  }
  DetectResult d{};
  int active_bundled = 0;
  int n = PQntuples(res.res);
  for (int i = 0; i < n; ++i) {
    std::string name{PQgetvalue(res.res, i, 0)};
    std::string prov{PQgetvalue(res.res, i, 1)};
    std::string state{PQgetvalue(res.res, i, 2)};
    bool is_active = (state == "ACTIVE" || state == "ACTIVE_FLAGGED");
    if (prov == "bundled" && is_active) {
      ++active_bundled;
      d.id = PQgetvalue(res.res, i, 3);
      d.name = name;
      d.version = PQgetvalue(res.res, i, 4);
    }
    if (name == "shell" && prov == "user") {
      d.user_shell_present = true;
    }
  }
  if (active_bundled == 0) {
    d.outcome = DetectOutcome::NONE;
  } else if (active_bundled == 1) {
    d.outcome = DetectOutcome::EXACTLY_ONE;
  } else {
    d.outcome = DetectOutcome::TOO_MANY;
  }
  return d;
}

auto read_bundle_bytes(const fs::path& path, std::size_t max_bytes)
    -> std::expected<std::vector<std::byte>, std::string> {
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    return std::unexpected("bundle file is not readable at " + path.string());
  }
  struct FileOwner {
    int fd;
    ~FileOwner() { ::close(fd); }
  } owner{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      static_cast<std::uintmax_t>(info.st_size) > max_bytes) {
    return std::unexpected(
        "bundle must be a nonempty regular file within the package size limit");
  }
  std::vector<std::byte> buf(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < buf.size()) {
    auto count = ::read(fd, buf.data() + offset, buf.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return std::unexpected("bundle read failed or changed during inspection");
    }
    offset += static_cast<std::size_t>(count);
  }
  std::byte extra{};
  if (::read(fd, &extra, 1) != 0) {
    return std::unexpected("bundle changed during inspection");
  }
  return buf;
}

struct BundleSnapshot {
  std::vector<std::byte> bytes;
  std::string version;
};

auto inspect_bundle(const fs::path& path, std::size_t max_bytes)
    -> std::expected<BundleSnapshot, std::string> {
  auto bytes = read_bundle_bytes(path, max_bytes);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  zip_error_t error;
  zip_error_init(&error);
  auto* source =
      zip_source_buffer_create(bytes->data(), bytes->size(), 0, &error);
  if (source == nullptr) {
    zip_error_fini(&error);
    return std::unexpected("cannot inspect bundled ZIP");
  }
  auto* raw = zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error);
  zip_error_fini(&error);
  if (raw == nullptr) {
    zip_source_free(source);
    return std::unexpected("invalid bundled ZIP");
  }
  std::unique_ptr<zip_t, decltype(&zip_close)> archive{raw, zip_close};
  const auto entries = zip_get_num_entries(raw, 0);
  if (entries < 1 || entries > 4096) {
    return std::unexpected("bundled ZIP exceeds the entry limit");
  }
  std::optional<zip_uint64_t> manifest_index;
  zip_uint64_t manifest_size = 0;
  for (zip_int64_t i = 0; i < entries; ++i) {
    zip_stat_t info;
    zip_stat_init(&info);
    if (zip_stat_index(raw, static_cast<zip_uint64_t>(i), 0, &info) != 0 ||
        info.name == nullptr) {
      return std::unexpected("invalid bundled ZIP entry");
    }
    if (std::string_view{info.name} == "manifest.json") {
      if (manifest_index || (info.valid & ZIP_STAT_SIZE) == 0 ||
          info.size == 0 || info.size > zip_uint64_t{256} * 1024) {
        return std::unexpected(
            "bundled ZIP requires one bounded root manifest");
      }
      manifest_index = static_cast<zip_uint64_t>(i);
      manifest_size = info.size;
    }
  }
  if (!manifest_index) {
    return std::unexpected("bundled ZIP has no root manifest.json");
  }
  std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file{
      zip_fopen_index(raw, *manifest_index, 0), zip_fclose};
  if (!file) {
    return std::unexpected("bundled manifest cannot be opened");
  }
  std::string text(static_cast<std::size_t>(manifest_size), '\0');
  if (zip_fread(file.get(), text.data(), text.size()) !=
      static_cast<zip_int64_t>(text.size())) {
    return std::unexpected("bundled manifest cannot be read");
  }
  const auto parsed =
      packages::PackageManifest::parse(text, "shell.zip/manifest.json", true);
  if (!parsed.value || parsed.value->name != "shell" ||
      !parsed.value->frontend) {
    return std::unexpected(
        "bundle must contain a valid shell frontend manifest");
  }
  return BundleSnapshot{.bytes = std::move(*bytes),
                        .version = parsed.value->version};
}

} // namespace

auto FirstBootFailure::exit_code() const noexcept -> int {
  // ICD-0.6.1 §3.5: SCHEMA_RESERVED + MULTIPLE_ACTIVE_FRONTENDS share
  // exit code 3 ("database-state-fixable: admin intervention").
  switch (kind) {
    case FirstBootError::BUNDLE_MISSING: return 1;
    case FirstBootError::BUNDLE_INSTALL_FAILED: return 2;
    case FirstBootError::MULTIPLE_ACTIVE_FRONTENDS:
    case FirstBootError::SCHEMA_RESERVED: return 3;
    case FirstBootError::DETECTION_FAILED: return 4;
  }
  return 99;
}

auto FirstBootFailure::kind_string() const noexcept -> std::string_view {
  switch (kind) {
    case FirstBootError::BUNDLE_MISSING: return "bundle-missing";
    case FirstBootError::BUNDLE_INSTALL_FAILED:
      return "install-lifecycle-failed";
    case FirstBootError::MULTIPLE_ACTIVE_FRONTENDS:
      return "singleton-violation";
    case FirstBootError::SCHEMA_RESERVED: return "schema-name-conflict";
    case FirstBootError::DETECTION_FAILED: return "detection-failed";
  }
  return "unknown";
}

auto resolve_bundle_path(const std::string& configured) -> fs::path {
  if (!configured.empty()) {
    fs::path p{configured};
    return p.is_absolute() ? p : fs::current_path() / p;
  }
  std::error_code ec;
  auto exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    // /proc/self/exe unavailable (non-Linux, chroot, …); CWD-relative.
    return fs::current_path() / "share" / "plinth" / "bundled";
  }
  auto bin_dir = exe.parent_path();
  auto dev = bin_dir / "share" / "plinth" / "bundled";
  std::error_code probe_ec;
  if (fs::is_directory(dev, probe_ec)) {
    return dev;
  }
  return bin_dir.parent_path() / "share" / "plinth" / "bundled";
}

auto bundled_shell_status(const Config& cfg)
    -> std::expected<BundledShellStatus, std::string> {
  auto bundle = inspect_bundle(
      resolve_bundle_path(cfg.shell.bundle_path) / "shell.zip",
      cfg.packages_max_package_size_mb * std::size_t{1024} * 1024);
  if (!bundle) {
    return std::unexpected(bundle.error());
  }
  PgGuard pg(cfg.db);
  if (!pg.ok()) {
    return std::unexpected("cannot connect to inspect installed shell");
  }
  BundledShellStatus status{.available_version = bundle->version};
  PgResult exists{plinth::db::exec(
      pg.conn, "SELECT to_regclass('plinth.packages') IS NOT NULL")};
  if (PQresultStatus(exists.res) != PGRES_TUPLES_OK ||
      PQntuples(exists.res) != 1) {
    return std::unexpected("cannot inspect package schema");
  }
  if (std::string_view{PQgetvalue(exists.res, 0, 0)} == "f") {
    return status;
  }
  auto detected = detect_active_bundled_frontend(pg.conn);
  if (!detected) {
    return std::unexpected(detected.error());
  }
  if (detected->outcome == DetectOutcome::TOO_MANY ||
      detected->user_shell_present) {
    return std::unexpected(
        "installed frontend state requires operator reconciliation");
  }
  if (detected->outcome == DetectOutcome::EXACTLY_ONE) {
    if (detected->name != "shell") {
      return std::unexpected(
          "active bundled frontend is not the reserved shell");
    }
    status.installed_version = detected->version;
    status.upgrade_available =
        packages::compare_semver(bundle->version, detected->version) > 0;
  }
  return status;
}

auto ensure_bundled_shell_installed(
    const Config& cfg, const packages::InstallerContext& bootstrap_ctx,
    bool upgrade_requested) -> std::expected<void, FirstBootFailure> {
  if (upgrade_requested && cfg.dev_mode) {
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::BUNDLE_INSTALL_FAILED,
        .message = "bundled-shell upgrades require dev_mode=false"});
  }
  PgGuard pg(cfg.db);
  if (!pg.ok()) {
    std::string msg = pg.conn != nullptr
                          ? std::string{PQerrorMessage(pg.conn)}
                          : std::string{"PQconnectdb returned null"};
    emit_failed(cfg.db, "detection-failed",
                "PG connect failed at firstboot: " + msg);
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::DETECTION_FAILED,
        .message = "PG connect failed: " + msg,
    });
  }

  auto detect = detect_active_bundled_frontend(pg.conn);
  if (!detect.has_value()) {
    emit_failed(cfg.db, "detection-failed", detect.error());
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::DETECTION_FAILED,
        .message = detect.error(),
    });
  }
  if (detect->outcome == DetectOutcome::TOO_MANY) {
    std::string msg = "two or more ACTIVE bundled frontends in "
                      "plinth.packages — admin must reconcile";
    emit_failed(cfg.db, "singleton-violation", msg);
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::MULTIPLE_ACTIVE_FRONTENDS,
        .message = std::move(msg),
    });
  }
  if (detect->user_shell_present && detect->outcome == DetectOutcome::NONE) {
    // A pre-existing user package squatting on `name='shell'` would
    // collide with the bundled-shell install at the unique-name
    // index. ICD-0.6.1 §3.5 ERR_BUNDLE_SCHEMA_RESERVED. The
    // post-0.6.1 parse-time guard prevents new such uploads; this
    // catches the pre-existing 0.4.x edge case.
    std::string msg = "user-uploaded package with name='shell' "
                      "blocks bundled-shell install — rename or "
                      "uninstall it before next boot";
    emit_failed(cfg.db, "schema-name-conflict", msg);
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::SCHEMA_RESERVED,
        .message = std::move(msg),
    });
  }
  if (detect->outcome == DetectOutcome::EXACTLY_ONE) {
    // PG and the active symlink cannot commit atomically across a process
    // crash. Refuse ingress when they disagree; an operator can restore the
    // pointer to the database's committed version before restarting.
    std::error_code link_error;
    auto target = fs::read_symlink(bootstrap_ctx.data_dir / "extensions" /
                                       detect->name / "active",
                                   link_error);
    if (link_error || target != fs::path{detect->version}) {
      return std::unexpected(FirstBootFailure{
          .kind = FirstBootError::BUNDLE_INSTALL_FAILED,
          .message = "bundled frontend active symlink disagrees with installed "
                     "version; operator recovery required",
          .recovery_required = true});
    }
    if (upgrade_requested) {
      if (detect->name != "shell") {
        return std::unexpected(FirstBootFailure{
            .kind = FirstBootError::SCHEMA_RESERVED,
            .message = "active bundled frontend is not the reserved shell"});
      }
      auto bundle = inspect_bundle(resolve_bundle_path(cfg.shell.bundle_path) /
                                       "shell.zip",
                                   bootstrap_ctx.max_package_size_bytes);
      if (!bundle) {
        return std::unexpected(
            FirstBootFailure{.kind = FirstBootError::BUNDLE_INSTALL_FAILED,
                             .message = bundle.error()});
      }
      const int order =
          packages::compare_semver(bundle->version, detect->version);
      if (order == 0) {
        spdlog::info("shell: installed bundle version {} is already current",
                     detect->version);
        return {};
      }
      if (order < 0) {
        return std::unexpected(
            FirstBootFailure{.kind = FirstBootError::BUNDLE_INSTALL_FAILED,
                             .message = "available shell is older than "
                                        "installed shell; downgrade refused"});
      }
      auto upgraded =
          packages::upgrade_package(bundle->bytes, detect->id, bootstrap_ctx,
                                    packages::Provenance::BUNDLED);
      if (!upgraded) {
        return std::unexpected(FirstBootFailure{
            .kind = FirstBootError::BUNDLE_INSTALL_FAILED,
            .message =
                "bundled-shell upgrade failed: " + upgraded.error().message,
            .recovery_required = upgraded.error().report.value("kind", "") ==
                                 "upgrade-recovery-required"});
      }
      spdlog::info("shell: explicitly upgraded bundled shell {} to {}",
                   detect->version, upgraded->new_record.version);
      return {};
    }
    spdlog::info("shell::firstboot: bundled frontend already ACTIVE — skipping "
                 "first-boot install");
    return {};
  }

  // Zero rows: install the bundled shell from disk.
  auto bundle_dir = resolve_bundle_path(cfg.shell.bundle_path);
  auto zip_path = bundle_dir / "shell.zip";
  spdlog::info(
      "shell::firstboot: no ACTIVE bundled frontend; installing from {}",
      zip_path.string());

  Json::Value started_detail(Json::objectValue);
  started_detail["actor"] = build_actor();
  started_detail["bundle_path"] = zip_path.string();
  {
    std::error_code ec;
    auto sz = std::filesystem::file_size(zip_path, ec);
    started_detail["bundle_size_bytes"] =
        ec ? Json::Value{Json::nullValue}
           : Json::Value{static_cast<Json::UInt64>(sz)};
  }
  plinth::log::audit_sync(cfg.db, AUDIT_STARTED, started_detail);

  auto bytes =
      read_bundle_bytes(zip_path, bootstrap_ctx.max_package_size_bytes);
  if (!bytes.has_value()) {
    emit_failed(cfg.db, "bundle-missing", bytes.error());
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::BUNDLE_MISSING,
        .message = bytes.error(),
    });
  }

  auto t0 = std::chrono::steady_clock::now();
  auto result = packages::install_package(
      std::span<const std::byte>{bytes->data(), bytes->size()},
      packages::Provenance::BUNDLED, bootstrap_ctx);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();

  if (!result.has_value()) {
    const auto& f = result.error();
    std::string msg = "install_lifecycle failed at " +
                      std::string{packages::stage_to_string(f.failed_at)} +
                      ": " + f.message;
    spdlog::error("shell::firstboot: install_package returned Err — "
                  "stage={} kind={} message={}",
                  packages::stage_to_string(f.failed_at), f.kind, f.message);
    Json::Value detail(Json::objectValue);
    detail["actor"] = build_actor();
    detail["failure_kind"] = "install-lifecycle-failed";
    detail["failed_stage"] =
        std::string{packages::stage_to_string(f.failed_at)};
    detail["message"] = msg;
    plinth::log::audit_sync(cfg.db, AUDIT_FAILED, detail);
    return std::unexpected(FirstBootFailure{
        .kind = FirstBootError::BUNDLE_INSTALL_FAILED,
        .message = std::move(msg),
    });
  }

  Json::Value done_detail(Json::objectValue);
  done_detail["actor"] = build_actor();
  done_detail["package_id"] = result->id;
  done_detail["name"] = result->name;
  done_detail["version"] = result->version;
  done_detail["elapsed_ms"] = static_cast<Json::Int64>(elapsed);
  plinth::log::audit_sync(cfg.db, AUDIT_COMPLETED, done_detail);

  spdlog::info(
      "shell::firstboot: bundled shell ACTIVE id={} name={} v={} ({} ms)",
      result->id, result->name, result->version, elapsed);
  return {};
}

} // namespace plinth::shell
