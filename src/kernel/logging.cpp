#include "kernel/logging.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/config.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <drogon/drogon.h>
#include <libpq-fe.h>
#include <memory>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sstream>

namespace plinth::log {

namespace {

// Set by plinth::log::init(); cleared by plinth::log::shutdown();
// read by audit() to decide whether the Drogon event loop + DbClient
// are available. Rationale: merely touching drogon::app() — even to
// read getDbClient() — materializes the HttpAppFramework singleton in
// a half-initialized state when no main has set it up, and its global
// destructors then SIGSEGV at process exit. Unit tests (Catch2) that
// never call log::init() leave the flag false and audit() bails out
// without reaching into any Drogon state.
//
// Two lifecycle transitions, both release-ordered:
//   false → true  : init() once the async sink + Drogon DbClient are wired
//   true  → false : shutdown() called by the process coordinator before
//                   drogon::app().quit(), so in-flight audit
//                   callers no-op instead of racing a torn-down
//                   DbClient weak_ptr during the loop drain.
// The false→true→false cycle mirrors the 0.3.3.1 g_shutdown_pending
// pattern from src/kernel/ws/connection_registry.cpp and closes the
// audit sub-path of the bad_weak_ptr teardown race.
//
// This matches the original 0.1.7 contract ("audit is fire-and-forget,
// safe from any thread") — "safe" now includes "safe before init and
// after shutdown".
// process-wide readiness flag; std::atomic so visibility is well-defined under
// the async_logger worker thread.
std::atomic<bool> g_audit_ready{false};

// Rotating file sink: 10 MB per file, 5 files retained. Matches
// DESIGN-logging-subsystem.md §Configuration & Sinks defaults.
constexpr std::size_t LOG_FILE_MAX_BYTES = 10ULL * 1024 * 1024;
constexpr std::size_t LOG_FILE_MAX_FILES = 5;
constexpr int LOG_FLUSH_SECONDS = 1;

// Async thread pool sizing per DESIGN-logging-subsystem.md §Configuration:
// queue holds up to 8192 messages, single worker thread, block on overflow
// (backpressure preferred over data loss in early versions).
constexpr std::size_t LOG_ASYNC_QUEUE_SIZE = 8192;
constexpr std::size_t LOG_ASYNC_THREADS = 1;

// Module-level node_id. Set once at startup; read on every audit insert.
// is process-wide and set before any audit call
std::string g_node_id;

auto build_conninfo(const Config::Database& db_cfg) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db_cfg.host << " port=" << db_cfg.port
     << " dbname=" << db_cfg.database << " user=" << db_cfg.user
     << " password=" << db_cfg.password;
  return ss.str();
}

} // namespace

auto init(const Config& cfg) -> void {
  spdlog::init_thread_pool(LOG_ASYNC_QUEUE_SIZE, LOG_ASYNC_THREADS);

  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      "logs/plinth.log", LOG_FILE_MAX_BYTES, LOG_FILE_MAX_FILES);
  auto logger = std::make_shared<spdlog::async_logger>(
      "plinth", spdlog::sinks_init_list{console_sink, file_sink},
      spdlog::thread_pool(), spdlog::async_overflow_policy::block);
  spdlog::set_default_logger(logger);
  spdlog::set_level(cfg.dev_mode ? spdlog::level::debug : spdlog::level::info);
  spdlog::flush_every(std::chrono::seconds(LOG_FLUSH_SECONDS));

  // Main has initialized spdlog. The Drogon app is created + has a
  // DbClient attached before app().run() by the same main. From this
  // point on audit() may safely reach into drogon::app().
  g_audit_ready.store(true, std::memory_order_release);
}

auto set_node_id(std::string node_id) -> void {
  g_node_id = std::move(node_id);
}

auto is_audit_ready() noexcept -> bool {
  return g_audit_ready.load(std::memory_order_acquire);
}

auto shutdown() noexcept -> void {
  g_audit_ready.store(false, std::memory_order_release);
}

#ifdef PLINTH_JS_TEST_SHIMS
auto test_reset_ready() noexcept -> void {
  g_audit_ready.store(false, std::memory_order_release);
}
#endif

auto audit(std::string_view action, const Json::Value& detail,
           const AuditCtx& ctx) -> void {
  auto detail_str = Json::FastWriter{}.write(detail);
  auto action_str = std::string{action};

  // Only production — where main has run plinth::log::init and
  // attached a DbClient to drogon::app() — is allowed to touch
  // Drogon state from here. See g_audit_ready block comment.
  if (!g_audit_ready.load(std::memory_order_acquire)) {
    spdlog::warn("audit skipped (not initialized) for {}: {}", action_str,
                 detail_str);
    return;
  }
  auto db = drogon::app().getDbClient();
  if (db == nullptr) {
    spdlog::warn("audit skipped (no DbClient) for {}: {}", action_str,
                 detail_str);
    return;
  }
  db->execSqlAsync(
      "INSERT INTO plinth.audit_log "
      "(action, user_id, session_id, detail, ip_address, node_id) "
      "VALUES ($1, NULLIF($2, '')::uuid, NULLIF($3, '')::uuid, "
      "$4::jsonb, NULLIF($5, '')::inet, $6)",
      [](const drogon::orm::Result&) {},
      [action_str](const drogon::orm::DrogonDbException& e) {
        spdlog::error("audit insert failed for {}: {}", action_str,
                      e.base().what());
      },
      action_str, ctx.user_id, ctx.session_id, detail_str, ctx.ip_address,
      g_node_id);
}

auto audit(std::string_view action, const Json::Value& detail,
           const drogon::HttpRequestPtr& req) -> void {
  AuditCtx ctx;
  auto attrs = req->attributes();
  ctx.user_id = attrs->get<std::string>(plinth::auth::ATTR_USER_ID);
  ctx.session_id = attrs->get<std::string>(plinth::auth::ATTR_SESSION_ID);
  ctx.ip_address = req->peerAddr().toIp();
  audit(action, detail, ctx);
}

auto audit_sync(const Config::Database& db, std::string_view action,
                const Json::Value& detail) -> void {
  auto conninfo = build_conninfo(db);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::error("audit_sync: PG connect failed: {}", PQerrorMessage(conn));
    PQfinish(conn);
    return;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  auto detail_str = Json::FastWriter{}.write(detail);
  auto action_str = std::string{action};
  std::array<const char*, 3> params = {
      action_str.c_str(),
      detail_str.c_str(),
      g_node_id.c_str(),
  };
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      PQexecParams(conn,
                   "INSERT INTO plinth.audit_log (action, detail, node_id) "
                   "VALUES ($1, $2::jsonb, $3)",
                   3, nullptr, params.data(), nullptr, nullptr, 0),
      PQclear);

  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("audit_sync insert failed for {}: {}", action_str,
                  PQresultErrorMessage(res.get()));
  }
}

} // namespace plinth::log
