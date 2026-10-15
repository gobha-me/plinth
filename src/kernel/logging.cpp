#include "kernel/logging.hpp"
#include "kernel/audit/async_write_registry.hpp"
#include "kernel/audit/detail_encoding.hpp"
#include "kernel/db/connection_info.hpp"
#include "kernel/db/operations.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/config.hpp"

#include <array>
#include <chrono>
#include <drogon/drogon.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace plinth::log {

namespace {

// Admission precedes all Drogon access. Pure tests, pre-init callers and calls
// after shutdown must not materialize its framework singleton. Final database
// producers use this separate registry after root async admission has closed.
detail::AuditWriteRegistry g_audit_writes;
std::mutex g_init_mutex;
std::mutex g_metadata_mutex;
std::shared_ptr<spdlog::logger> g_audit_logger;

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

// Protected metadata is copied into each admitted operation.
std::string g_node_id;

struct AuditWrite {
  // Declared first, destroyed last: callback-held strings/logger must cease
  // being accessible before the registry may publish active == 0.
  std::shared_ptr<detail::AuditWriteRegistry::Lease> lease;
  std::shared_ptr<spdlog::logger> logger;
  std::string action;
  std::string detail;
  std::string detail_encoding;
  AuditCtx context;
  std::string node_id;
};

// Pinned SqlBinder::cancel prevents incomplete setup from dispatching through
// the binder destructor. The borrowed binder is synchronous caller-stack state.
class AuditBinderSetupGuard {
 public:
  explicit AuditBinderSetupGuard(drogon::orm::internal::SqlBinder& binder)
      : binder_(binder) {}
  AuditBinderSetupGuard(const AuditBinderSetupGuard&) = delete;
  auto operator=(const AuditBinderSetupGuard&)
      -> AuditBinderSetupGuard& = delete;
  AuditBinderSetupGuard(AuditBinderSetupGuard&&) = delete;
  auto operator=(AuditBinderSetupGuard&&) -> AuditBinderSetupGuard& = delete;
  ~AuditBinderSetupGuard() { binder_.cancel(); }

 private:
  drogon::orm::internal::SqlBinder& binder_;
};

auto report_audit_failure(const std::shared_ptr<AuditWrite>& operation,
                          const char* message) noexcept -> void {
  operation->lease->record_failure();
  try {
    operation->logger->error("audit insert failed for {}: {}",
                             operation->action, message);
  } catch (...) {
    // A logging failure cannot turn an already failed INSERT into green drain.
    operation->lease->record_failure();
  }
}

} // namespace

auto init(const Config& cfg) -> void {
  std::lock_guard init_lock(g_init_mutex);
  if (g_audit_writes.initialized()) {
    // Test-only readiness pauses may resume; a sealed production generation
    // cannot reopen. Never replace a pool underneath admitted async loggers.
    static_cast<void>(g_audit_writes.start());
    return;
  }
  if (g_audit_writes.closed()) {
    return;
  }
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

  {
    std::lock_guard metadata_lock(g_metadata_mutex);
    g_audit_logger = std::move(logger);
  }
  static_cast<void>(g_audit_writes.start());
}

auto set_node_id(std::string node_id) -> void {
  std::lock_guard lock(g_metadata_mutex);
  g_node_id = std::move(node_id);
}

auto is_audit_ready() noexcept -> bool {
  return g_audit_writes.accepting();
}

auto shutdown() noexcept -> void {
  std::lock_guard init_lock(g_init_mutex);
  g_audit_writes.close();
}

auto wait_for_audits(std::chrono::milliseconds timeout) -> bool {
  // If admission was open at entry, this is only a fixture barrier. A later
  // concurrent close must not allow us to drop metadata for a newly admitted
  // operation which was not covered by the zero-count observation.
  const bool sealed_at_entry = g_audit_writes.closed();
  if (!g_audit_writes.wait_for_settlement(timeout)) {
    return false;
  }
  if (sealed_at_entry) {
    std::lock_guard lock(g_metadata_mutex);
    g_audit_logger.reset();
  }
  return true;
}

#ifdef PLINTH_JS_TEST_SHIMS
auto test_reset_ready() noexcept -> void {
  g_audit_writes.pause();
}
#endif

auto audit(std::string_view action, const Json::Value& detail,
           const AuditCtx& ctx) -> void {
  auto lease = g_audit_writes.try_acquire();
  if (lease == nullptr) {
    // Rejected callers have no lifecycle lease. They must not access even the
    // global logger, whose sinks may already be closing after a clean drain.
    return;
  }
  std::shared_ptr<AuditWrite> operation;
  try {
    operation = std::make_shared<AuditWrite>();
    operation->lease = lease;
    operation->action = std::string{action};
    auto stored_detail = plinth::audit::encode_detail(detail);
    operation->detail = std::move(stored_detail.json);
    // SqlBinder sends integral values in binary using their C++ width.
    // Bind text for the explicit SQL smallint cast, as the sync writer does.
    operation->detail_encoding = std::to_string(stored_detail.encoding);
    operation->context = ctx;
    {
      std::lock_guard lock(g_metadata_mutex);
      operation->logger = g_audit_logger;
      operation->node_id = g_node_id;
    }
    // This client reference is caller-local, never captured by its callbacks.
    // It is released before the caller's lease; the coordinator retains the
    // framework's client until callback owners also release their leases.
    auto db = drogon::app().getDbClient();
    if (db == nullptr) {
      static_cast<void>(lease->skip());
      operation->logger->warn("audit skipped (no DbClient) for {}: {}",
                              operation->action, operation->detail);
      return;
    }
    auto binder =
        *db << "INSERT INTO plinth.audit_log "
               "(action, user_id, session_id, detail, ip_address, "
               "node_id, detail_encoding) "
               "VALUES ($1, NULLIF($2, '')::uuid, NULLIF($3, '')::uuid, "
               "$4::jsonb, NULLIF($5, '')::inet, $6, $7::smallint)";
    AuditBinderSetupGuard setup_guard(binder);
    binder << operation->action << operation->context.user_id
           << operation->context.session_id << operation->detail
           << operation->context.ip_address << operation->node_id
           << operation->detail_encoding;
    binder >> [operation](const drogon::orm::Result&) {
      // Explicit owning copy survives destruction of the driver-held closure
      // during callback execution; a reference would not retain its state.
      const auto callback_owner = std::shared_ptr<AuditWrite>{operation};
      static_cast<void>(callback_owner->lease->succeed());
    };
    binder >> [operation](const drogon::orm::DrogonDbException& error) {
      const auto callback_owner = std::shared_ptr<AuditWrite>{operation};
      if (callback_owner->lease->fail()) {
        report_audit_failure(callback_owner, error.base().what());
      }
    };
    // Do not use execSqlAsync: that facade is noexcept and dispatches through
    // destruction even when parameter/callback construction is incomplete.
    binder.exec();
  } catch (const std::exception& error) {
    static_cast<void>(lease->fail());
    if (operation != nullptr && operation->logger != nullptr) {
      report_audit_failure(operation, error.what());
    }
  } catch (...) {
    static_cast<void>(lease->fail());
    if (operation != nullptr && operation->logger != nullptr) {
      report_audit_failure(operation, "unknown setup failure");
    }
  }
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
  auto conninfo = plinth::db::connection_info(db);
  PGconn* conn = plinth::db::connect(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::error("audit_sync: PG connect failed: {}", PQerrorMessage(conn));
    PQfinish(conn);
    return;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  const auto stored_detail = plinth::audit::encode_detail(detail);
  const auto encoding_str = std::to_string(stored_detail.encoding);
  auto action_str = std::string{action};
  std::string node_id;
  {
    std::lock_guard lock(g_metadata_mutex);
    node_id = g_node_id;
  }
  std::array<const char*, 4> params = {
      action_str.c_str(),
      stored_detail.json.c_str(),
      node_id.c_str(),
      encoding_str.c_str(),
  };
  std::unique_ptr<PGresult, decltype(&PQclear)> res(
      plinth::db::exec_params(conn,
                              "INSERT INTO plinth.audit_log "
                              "(action, detail, node_id, detail_encoding) "
                              "VALUES ($1, $2::jsonb, $3, $4::smallint)",
                              4, nullptr, params.data(), nullptr, nullptr, 0),
      PQclear);

  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("audit_sync insert failed for {}: {}", action_str,
                  PQresultErrorMessage(res.get()));
  }
}

} // namespace plinth::log
