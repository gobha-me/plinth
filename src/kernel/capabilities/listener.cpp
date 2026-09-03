#include "kernel/capabilities/listener.hpp"
#include "kernel/capabilities/resolution.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <json/reader.h>
#include <json/value.h>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

namespace plinth::capabilities {

namespace {

constexpr const char* CHANGE_CHANNEL = "plinth_capability_changed";
constexpr int POLL_TIMEOUT_MS = 1000; // shutdown ceiling
constexpr auto RECONNECT_BACKOFF = std::chrono::seconds{1};

// ── Module-local state ───────────────────────────────────────────────
//
// The listener thread is owned by an optional<jthread>. start/stop are
// serialized by a plain mutex so that accidental double-start or
// start-during-stop is safe. The eventfd is created by start_notify_
// listener and closed by the thread on exit — the start/stop code only
// reads it (for the wake write).

// lifecycle mutex; cannot be const (lock() mutates).
std::mutex lifecycle_mutex;
// thread handle; optional engaged only while the thread is running.
std::optional<std::jthread> listener_thread;
// jointly by start (creates), stop (wakes), thread (closes on exit).
int wakeup_fd = -1;
std::mutex listener_exit_mutex;
std::condition_variable listener_exit_cv;
bool listener_exited = true;

// ── Helpers ──────────────────────────────────────────────────────────

auto build_conninfo(const Config::Database& db) -> std::string {
  // Duplicated in registration.cpp and resolution.cpp — not lifted
  // into a shared header because the producer/consumer paths are
  // intentionally independent (see listener.hpp threading note).
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

struct ParsedNotification {
  std::string action;    // "register" | "deregister" | "disable" | "enable"
  std::string signature; // may be empty for bulk disable/enable
  std::string scope;     // may be empty for bulk
  std::optional<std::string> extension_name; // null payload → nullopt
};

auto parse_payload(std::string_view json) -> std::optional<ParsedNotification> {
  Json::CharReaderBuilder reader_b;
  std::unique_ptr<Json::CharReader> reader(reader_b.newCharReader());
  Json::Value root;
  std::string err;
  // Json::CharReader::parse takes a (begin, end) pointer pair
  if (!reader->parse(json.data(), json.data() + json.size(), &root, &err)) {
    spdlog::warn("listener: payload parse failed: {} ({})", err, json);
    return std::nullopt;
  }
  if (!root.isObject()) {
    spdlog::warn("listener: payload is not an object: {}", json);
    return std::nullopt;
  }
  if (!root["action"].isString() || !root["signature"].isString() ||
      !root["scope"].isString()) {
    spdlog::warn("listener: payload missing required string fields: {}", json);
    return std::nullopt;
  }

  ParsedNotification out;
  out.action = root["action"].asString();
  out.signature = root["signature"].asString();
  out.scope = root["scope"].asString();

  const auto& ext = root["extension_name"];
  if (ext.isNull()) {
    out.extension_name = std::nullopt;
  } else if (ext.isString()) {
    out.extension_name = ext.asString();
  } else {
    spdlog::warn("listener: extension_name is neither null nor string: {}",
                 json);
    return std::nullopt;
  }

  if (out.action != "register" && out.action != "deregister" &&
      out.action != "disable" && out.action != "enable") {
    spdlog::warn("listener: unknown action '{}' in payload: {}", out.action,
                 json);
    return std::nullopt;
  }
  return out;
}

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

// Short-lived sync fetch for the `register` action. Returns nullopt if
// the row no longer exists (e.g. a deregister raced ahead). Pulls the
// columns the Tier 2 cache needs, including `enabled` so that a
// register→disable race lands on the correct state.
auto fetch_row(const Config::Database& db_cfg, std::string_view signature,
               std::string_view scope) -> std::optional<CachedCapability> {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::error("listener: fetch_row connect failed: {}",
                  PQerrorMessage(conn));
    PQfinish(conn);
    return std::nullopt;
  }
  std::unique_ptr<PGconn, decltype(&PQfinish)> guard(conn, PQfinish);

  std::string sig_str{signature};
  std::string scope_str{scope};
  std::array<const char*, 2> values = {sig_str.c_str(), scope_str.c_str()};
  PgResultPtr res{PQexecParams(conn,
                               "SELECT signature, provider_type, "
                               "       COALESCE(extension_name, ''), scope, "
                               "       rbac_rule, enabled "
                               "FROM plinth.capabilities "
                               "WHERE signature = $1 AND scope = $2",
                               2, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear};
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    spdlog::error("listener: fetch_row SELECT failed: {}",
                  PQresultErrorMessage(res.get()));
    return std::nullopt;
  }
  if (PQntuples(res.get()) == 0) {
    return std::nullopt;
  }
  CachedCapability entry{
      .signature = PQgetvalue(res.get(), 0, 0),
      .provider_type = PQgetvalue(res.get(), 0, 1),
      .extension_name = PQgetvalue(res.get(), 0, 2),
      .scope = PQgetvalue(res.get(), 0, 3),
      .user_id = {},
      .rbac_rule = PQgetvalue(res.get(), 0, 4),
      .enabled = std::string_view{PQgetvalue(res.get(), 0, 5)} == "t",
  };
  return entry;
}

// Apply a parsed notification to the Tier 2 cache. Returns true if the
// action was recognized and dispatched (even if the effect was a no-op,
// e.g. deregister on a missing signature — semantically the cache is
// now in the intended state).
auto apply(const Config::Database& db_cfg, const ParsedNotification& n)
    -> bool {
  if (n.action == "register") {
    if (n.signature.empty()) {
      spdlog::warn("listener: register payload missing signature");
      return false;
    }
    auto row = fetch_row(db_cfg, n.signature, n.scope);
    if (!row.has_value()) {
      spdlog::warn("listener: register fetch miss for {} ({})", n.signature,
                   n.scope);
      return true; // nothing to insert, but payload was valid
    }
    upsert_tier2_entry(*row);
    spdlog::debug("listener: register applied for {}", n.signature);
    return true;
  }
  if (n.action == "deregister") {
    if (n.signature.empty()) {
      spdlog::warn("listener: deregister payload missing signature");
      return false;
    }
    erase_tier2_entry(n.signature, n.scope);
    spdlog::debug("listener: deregister applied for {}", n.signature);
    return true;
  }
  // disable / enable: bulk form (extension_name set, signature empty)
  // is the only one registration.cpp emits today. A future per-entry
  // form would set signature and leave extension_name unset — support
  // is wired so the listener remains forward-compatible.
  bool target_enabled = (n.action == "enable");
  if (n.extension_name.has_value() && n.signature.empty()) {
    auto count =
        set_enabled_by_extension_in_cache(*n.extension_name, target_enabled);
    spdlog::debug("listener: {} applied for extension {} (count={})", n.action,
                  *n.extension_name, count);
    return true;
  }
  if (!n.signature.empty()) {
    // Per-entry flip: fetch and upsert so the cached row reflects
    // the new enabled value. (No per-entry NOTIFY is emitted in
    // 0.2.x; this branch is forward-compat.)
    auto row = fetch_row(db_cfg, n.signature, n.scope);
    if (row.has_value()) {
      row->enabled = target_enabled;
      upsert_tier2_entry(*row);
    }
    spdlog::debug("listener: {} applied for {}", n.action, n.signature);
    return true;
  }
  spdlog::warn("listener: {} payload lacks signature and extension_name",
               n.action);
  return false;
}

// ── Listener thread body ─────────────────────────────────────────────

auto open_listen_conn(const Config::Database& db_cfg) -> PGconn* {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::error("listener: connect failed: {}", PQerrorMessage(conn));
    PQfinish(conn);
    return nullptr;
  }
  PgResultPtr res{PQexec(conn, "LISTEN plinth_capability_changed"), PQclear};
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("listener: LISTEN failed: {}",
                  PQresultErrorMessage(res.get()));
    PQfinish(conn);
    return nullptr;
  }
  spdlog::info("listener: subscribed to channel {}", CHANGE_CHANNEL);
  return conn;
}

auto drain_notifications(PGconn* conn, const Config::Database& db_cfg) -> void {
  if (PQconsumeInput(conn) == 0) {
    spdlog::warn("listener: PQconsumeInput failed: {}", PQerrorMessage(conn));
    return;
  }
  while (auto* n = PQnotifies(conn)) {
    std::string payload = n->extra == nullptr ? "" : n->extra;
    PQfreemem(n);
    if (auto parsed = parse_payload(payload)) {
      static_cast<void>(apply(db_cfg, *parsed));
    }
  }
}

auto run_listener(const std::stop_token& tok, const Config::Database& db_cfg,
                  int wake_fd) -> void {
  PGconn* conn = nullptr;
  while (!tok.stop_requested()) {
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
      if (conn != nullptr) {
        PQfinish(conn);
        conn = nullptr;
      }
      conn = open_listen_conn(db_cfg);
      if (conn == nullptr) {
        std::this_thread::sleep_for(RECONNECT_BACKOFF);
        continue;
      }
      // Full resync after every successful LISTEN open (initial
      // and every reconnect) — closes the window during which a
      // NOTIFY delivered while CONNECTION_BAD was in effect would
      // otherwise be lost. Bounded cost: one SELECT against
      // plinth.capabilities per reconnect. See listener.hpp
      // threading note and ICD-0.2.4 amendment.
      static_cast<void>(reload_tier2_cache(db_cfg));
    }

    std::array<pollfd, 2> fds{};
    fds[0].fd = PQsocket(conn);
    fds[0].events = POLLIN;
    fds[1].fd = wake_fd;
    fds[1].events = POLLIN;

    int rc = ::poll(fds.data(), fds.size(), POLL_TIMEOUT_MS);
    if (rc < 0) {
      // Interrupted — fall through to re-check the stop token.
      continue;
    }
    if ((fds[1].revents & POLLIN) != 0) {
      std::uint64_t discard = 0;
      static_cast<void>(::read(wake_fd, &discard, sizeof(discard)));
      continue;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      drain_notifications(conn, db_cfg);
    }
  }
  if (conn != nullptr) {
    PQfinish(conn);
  }
  // The thread owns the wake fd from this point onward; close it so
  // the descriptor is reclaimed immediately on exit.
  ::close(wake_fd);
  spdlog::info("listener: stopped");
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto start_notify_listener(const Config::Database& db_cfg) -> void {
  std::lock_guard lock(lifecycle_mutex);
  if (listener_thread.has_value()) {
    return; // already running
  }
  int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    spdlog::error("listener: eventfd() failed; listener will not start");
    return;
  }
  wakeup_fd = fd;
  {
    std::lock_guard exit_lock(listener_exit_mutex);
    listener_exited = false;
  }
  listener_thread.emplace([db_cfg, fd](const std::stop_token& tok) {
    run_listener(tok, db_cfg, fd);
    {
      std::lock_guard exit_lock(listener_exit_mutex);
      listener_exited = true;
    }
    listener_exit_cv.notify_all();
  });
}

auto stop_notify_listener(std::chrono::milliseconds timeout) -> bool {
  std::lock_guard lock(lifecycle_mutex);
  if (!listener_thread.has_value()) {
    return true;
  }
  listener_thread->request_stop();
  if (wakeup_fd >= 0) {
    std::uint64_t one = 1;
    static_cast<void>(::write(wakeup_fd, &one, sizeof(one)));
  }
  {
    std::unique_lock exit_lock(listener_exit_mutex);
    if (!listener_exit_cv.wait_for(exit_lock, timeout,
                                   [] { return listener_exited; })) {
      return false;
    }
  }
  // The completion barrier above makes the jthread reset an immediate join.
  listener_thread.reset();
  wakeup_fd = -1; // ownership handed to the thread, which has closed it
  return true;
}

auto apply_notification_for_test(const Config::Database& db_cfg,
                                 std::string_view payload_json) -> bool {
  auto parsed = parse_payload(payload_json);
  if (!parsed.has_value()) {
    return false;
  }
  return apply(db_cfg, *parsed);
}

} // namespace plinth::capabilities
