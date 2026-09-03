#include "kernel/realtime/listener.hpp"

#include "kernel/realtime/channel.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <json/reader.h>
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
#include <utility>
#include <vector>

namespace plinth::realtime {

namespace {

// ICD §Channel Subscription — single-channel fan-in. The PG wire
// channel is always this literal; logical Layer 1/2/3 channels are
// discriminated by the envelope's `channel` + `layer` fields.
constexpr const char* WIRE_CHANNEL = "plinth:realtime";
constexpr int POLL_TIMEOUT_MS = 1000;

// ── Module-local state ───────────────────────────────────────────────
//
// The listener thread is owned by an optional<jthread>. start/stop are
// serialized by a plain mutex so that accidental double-start or
// start-during-stop is safe. The eventfd is created by start_listener
// and closed by the thread on exit — start/stop only write to it (stop
// writes a wake byte, start reads the fd only to pass to the thread).

// lifecycle mutex; cannot be const.
std::mutex lifecycle_mutex;
// thread; optional engaged while running.
std::optional<std::jthread> listener_thread;
// co-owned by start (creates) + thread (closes on exit).
int wakeup_fd = -1;
std::mutex listener_exit_mutex;
std::condition_variable listener_exit_cv;
bool listener_exited = true;

// Handler registry. Separate mutex so handler registration never
// blocks on lifecycle transitions (and vice versa). Handlers run on
// the listener thread under handlers_mutex to keep the list stable
// across dispatch.
// mutex
std::mutex handlers_mutex;
// list
std::vector<EventHandler> handlers;

// ── Helpers ──────────────────────────────────────────────────────────

auto build_conninfo(const Config::Database& db) -> std::string {
  std::ostringstream ss;
  ss << "host=" << db.host << " port=" << db.port << " dbname=" << db.database
     << " user=" << db.user << " password=" << db.password;
  return ss.str();
}

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

// Parse a raw NOTIFY payload into a DispatchedEvent. Returns nullopt
// on JSON parse failure, missing required fields, or invalid channel.
// Logs at warn on every rejection path (R.08/R.09 expect the log).
auto parse_envelope(std::string_view channel, std::string_view payload_json)
    -> std::optional<DispatchedEvent> {
  Json::CharReaderBuilder reader_b;
  std::unique_ptr<Json::CharReader> reader(reader_b.newCharReader());
  Json::Value root;
  std::string err;
  // Json::CharReader::parse takes a (begin, end) pointer pair
  if (!reader->parse(payload_json.data(),
                     payload_json.data() + payload_json.size(), &root, &err)) {
    spdlog::warn("realtime listener: payload parse failed: {} ({})", err,
                 payload_json);
    return std::nullopt;
  }
  if (!root.isObject()) {
    spdlog::warn("realtime listener: payload is not an object: {}",
                 payload_json);
    return std::nullopt;
  }
  if (!root.isMember("layer") || !root["layer"].isString()) {
    spdlog::warn("realtime listener: payload missing 'layer': {}",
                 payload_json);
    return std::nullopt;
  }
  if (!root.isMember("channel") || !root["channel"].isString()) {
    spdlog::warn("realtime listener: payload missing 'channel': {}",
                 payload_json);
    return std::nullopt;
  }
  auto layer = root["layer"].asString();
  auto env_ch = root["channel"].asString();
  if (layer != "data" && layer != "system" && layer != "extension") {
    spdlog::warn("realtime listener: unknown layer '{}'", layer);
    return std::nullopt;
  }
  if (!validate_channel(env_ch)) {
    spdlog::warn("realtime listener: channel failed regex: {}", env_ch);
    return std::nullopt;
  }
  // Per ICD: prefer the envelope's channel field over the incoming
  // PG-wire channel (which is always the literal `plinth:realtime`
  // in 0.5.0). `channel` arg is passed in for future use / test
  // seams but the envelope is the source of truth.
  (void)channel;
  DispatchedEvent ev;
  ev.layer = std::move(layer);
  ev.channel = std::move(env_ch);
  ev.envelope = std::move(root);
  return ev;
}

auto dispatch(const DispatchedEvent& ev) -> void {
  std::lock_guard lock(handlers_mutex);
  for (const auto& h : handlers) {
    try {
      h(ev);
    } catch (const std::exception& e) {
      spdlog::warn("realtime listener: handler threw: {}", e.what());
    }
  }
}

// ── Thread body (connect + LISTEN + poll + dispatch loop) ──────────

auto open_listen_conn(const Config::Database& db_cfg) -> PGconn* {
  auto conninfo = build_conninfo(db_cfg);
  PGconn* conn = PQconnectdb(conninfo.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    spdlog::error("realtime listener: connect failed: {}",
                  PQerrorMessage(conn));
    PQfinish(conn);
    return nullptr;
  }
  PgResultPtr res{PQexec(conn, R"(LISTEN "plinth:realtime")"), PQclear};
  if (PQresultStatus(res.get()) != PGRES_COMMAND_OK) {
    spdlog::error("realtime listener: LISTEN failed: {}",
                  PQresultErrorMessage(res.get()));
    PQfinish(conn);
    return nullptr;
  }
  spdlog::info("realtime listener: subscribed to {}", WIRE_CHANNEL);
  return conn;
}

auto drain_notifications(PGconn* conn) -> void {
  if (PQconsumeInput(conn) == 0) {
    spdlog::warn("realtime listener: PQconsumeInput failed: {}",
                 PQerrorMessage(conn));
    return;
  }
  while (auto* n = PQnotifies(conn)) {
    std::string channel = n->relname == nullptr ? "" : n->relname;
    std::string payload = n->extra == nullptr ? "" : n->extra;
    PQfreemem(n);
    if (auto ev = parse_envelope(channel, payload); ev.has_value()) {
      dispatch(*ev);
    }
  }
}

// Wait up to `backoff_ms` for either a stop wake-fd signal or the
// backoff to expire. Returns early on wake. Caller should re-check
// stop_token after return.
auto wait_for_reconnect(int wake_fd, int backoff_ms) -> void {
  std::array<pollfd, 1> fds{};
  fds[0].fd = wake_fd;
  fds[0].events = POLLIN;
  int rc = ::poll(fds.data(), fds.size(), backoff_ms);
  if (rc > 0 && (fds[0].revents & POLLIN) != 0) {
    std::uint64_t discard = 0;
    static_cast<void>(::read(wake_fd, &discard, sizeof(discard)));
  }
}

auto run_listener(const std::stop_token& tok, const Config::Database& db_cfg,
                  int wake_fd, int backoff_ms) -> void {
  PGconn* conn = nullptr;
  while (!tok.stop_requested()) {
    if (conn == nullptr || PQstatus(conn) != CONNECTION_OK) {
      if (conn != nullptr) {
        PQfinish(conn);
        conn = nullptr;
      }
      conn = open_listen_conn(db_cfg);
      if (conn == nullptr) {
        wait_for_reconnect(wake_fd, backoff_ms);
        continue;
      }
      // 0.5.0 has no resync-on-reconnect hook — §OQ3 + ICD
      // Listener Subsystem / Threading & reconnect bullet 5
      // (reserved for 0.5.2+ broker).
    }

    std::array<pollfd, 2> fds{};
    fds[0].fd = PQsocket(conn);
    fds[0].events = POLLIN;
    fds[1].fd = wake_fd;
    fds[1].events = POLLIN;

    int rc = ::poll(fds.data(), fds.size(), POLL_TIMEOUT_MS);
    if (rc < 0) {
      continue; // EINTR — re-check stop token
    }
    if ((fds[1].revents & POLLIN) != 0) {
      std::uint64_t discard = 0;
      static_cast<void>(::read(wake_fd, &discard, sizeof(discard)));
      continue;
    }
    if ((fds[0].revents & POLLIN) != 0) {
      drain_notifications(conn);
    }
  }
  if (conn != nullptr) {
    PQfinish(conn);
  }
  ::close(wake_fd);
  spdlog::info("realtime listener: stopped");
}

} // namespace

// ── Public API ───────────────────────────────────────────────────────

auto register_handler(EventHandler h) -> void {
  std::lock_guard lock(handlers_mutex);
  handlers.push_back(std::move(h));
}

auto clear_handlers_for_test() -> void {
  std::lock_guard lock(handlers_mutex);
  handlers.clear();
}

auto start_listener(const Config::Database& db_cfg,
                    const Config::Realtime::Listener& listener_cfg) -> void {
  std::lock_guard lock(lifecycle_mutex);
  if (!listener_cfg.enabled) {
    spdlog::info("realtime listener: disabled by config");
    return;
  }
  if (listener_thread.has_value()) {
    return; // already running
  }
  int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (fd < 0) {
    spdlog::error(
        "realtime listener: eventfd() failed; listener will not start");
    return;
  }
  wakeup_fd = fd;
  auto backoff_ms = listener_cfg.reconnect_backoff_ms;
  {
    std::lock_guard exit_lock(listener_exit_mutex);
    listener_exited = false;
  }
  listener_thread.emplace([db_cfg, fd, backoff_ms](const std::stop_token& tok) {
    run_listener(tok, db_cfg, fd, backoff_ms);
    {
      std::lock_guard exit_lock(listener_exit_mutex);
      listener_exited = true;
    }
    listener_exit_cv.notify_all();
  });
}

auto stop_listener(std::chrono::milliseconds timeout) -> bool {
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
  listener_thread.reset(); // completion barrier makes this join immediate
  wakeup_fd = -1;
  return true;
}

auto apply_notification_for_test(std::string_view channel,
                                 std::string_view payload_json) -> bool {
  auto ev = parse_envelope(channel, payload_json);
  if (!ev.has_value()) {
    return false;
  }
  dispatch(*ev);
  return true;
}

} // namespace plinth::realtime
