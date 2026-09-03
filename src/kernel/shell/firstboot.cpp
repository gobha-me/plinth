#include "kernel/shell/firstboot.hpp"

#include "kernel/config.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/install_lifecycle.hpp"

#include <json/value.h>
#include <libpq-fe.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <span>
#include <sstream>
#include <vector>

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
    std::ostringstream ss;
    ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
       << " user=" << db.user << " password=" << db.password;
    conn = PQconnectdb(ss.str().c_str());
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
};

auto detect_active_bundled_frontend(PGconn* conn)
    -> std::expected<DetectResult, std::string> {
  PgResult res{PQexec(conn,
                      "SELECT name, provenance, state "
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

auto read_bundle_bytes(const fs::path& path)
    -> std::expected<std::vector<std::byte>, std::string> {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    return std::unexpected("bundle file not found at " + path.string());
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected("bundle file at " + path.string() +
                           " is not readable");
  }
  auto size = fs::file_size(path, ec);
  if (ec) {
    return std::unexpected("bundle file_size failed: " + ec.message());
  }
  std::vector<std::byte> buf(size);
  if (size > 0) {
    in.read(reinterpret_cast<char*>(buf.data()),
            static_cast<std::streamsize>(size));
    if (in.gcount() != static_cast<std::streamsize>(size)) {
      return std::unexpected("bundle short read at " + path.string());
    }
  }
  return buf;
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

auto ensure_bundled_shell_installed(
    const Config& cfg, const packages::InstallerContext& bootstrap_ctx)
    -> std::expected<void, FirstBootFailure> {
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

  auto bytes = read_bundle_bytes(zip_path);
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
