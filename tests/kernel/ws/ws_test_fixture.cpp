#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"
#include "kernel/cap/api_cap.hpp"
#include "kernel/capabilities/listener.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/lifecycle/shutdown_coordinator.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/handlers.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/js_stress.hpp"
#include "kernel/ws/registration.hpp"
#include "kernel/ws/subscriptions.hpp"

#include "../test_process.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <drogon/drogon.h>
#include <filesystem>
#include <future>
#include <json/reader.h>
#include <json/writer.h>
#include <stdexcept>
#include <unistd.h>

namespace plinth::ws_test {

namespace {

// Fixed test port. 0.1.6 integration tests assume only one binary can be
// running these tests at a time; a collision manifests as a bind error at
// startup. This keeps the harness simple and avoids parsing ephemeral
// ports back out of Drogon.
constexpr uint16_t TEST_PORT = 28099;

auto env(const char* name) -> const char* {
  return std::getenv(name);
}

} // namespace

auto pg_available() -> bool {
  if (env("PLINTH_PG_HOST") == nullptr) {
    return false;
  }
  auto cfg = test_config();
  auto conninfo = "host=" + cfg.db.host +
                  " port=" + std::to_string(cfg.db.port) +
                  " dbname=" + cfg.db.database + " user=" + cfg.db.user +
                  " password=" + cfg.db.password + " connect_timeout=3";
  PGconn* conn = PQconnectdb(conninfo.c_str());
  bool ok = (PQstatus(conn) == CONNECTION_OK);
  PQfinish(conn);
  return ok;
}

auto packages_data_dir() -> const std::filesystem::path& {
  namespace fs = std::filesystem;
  static const fs::path BASE = []() {
    auto root = fs::temp_directory_path() /
                ("plinth_http_test_" + std::to_string(::getpid()));
    fs::create_directories(root / "data");
    fs::create_directories(root / "staging");
    return root;
  }();
  return BASE;
}

auto packages_staging_dir() -> const std::filesystem::path& {
  namespace fs = std::filesystem;
  static const fs::path PATH = packages_data_dir() / "staging";
  return PATH;
}

auto test_config() -> plinth::Config {
  plinth::Config cfg;
  cfg.dev_mode = true;
  cfg.listen_host = "127.0.0.1";
  cfg.listen_port = TEST_PORT;
  cfg.node_id = "test-node";
  // Short timeouts so tests don't wait 30+ seconds.
  cfg.ws_auth_timeout_s = 1.0;
  cfg.ws_heartbeat_interval_s = 0.5;
  cfg.ws_heartbeat_timeout_s = 0.5;
  cfg.migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  cfg.registration_enabled = true;
  // 0.6.0.N HTTP fixture — point packages at a per-process tempdir so
  // POST /api/packages writes don't accumulate in the build tree, and
  // every test's reset_schema + clean clears them. The route
  // registration in start_test_server() captures these paths into the
  // handler's lambda; values must be stable across calls (they are —
  // the accessors are function-static).
  cfg.packages_data_dir = (packages_data_dir() / "data").string();
  cfg.packages_staging_dir = (packages_data_dir() / "staging").string();

  if (const auto* v = env("PLINTH_PG_HOST")) {
    cfg.db.host = v;
  }
  if (const auto* v = env("PLINTH_PG_PORT")) {
    cfg.db.port = static_cast<uint16_t>(std::stoi(v));
  }
  if (const auto* v = env("PLINTH_PG_USER")) {
    cfg.db.user = v;
  }
  if (const auto* v = env("PLINTH_PG_PASSWORD")) {
    cfg.db.password = v;
  }
  if (const auto* v = env("PLINTH_PG_DATABASE")) {
    cfg.db.database = v;
  }
  return cfg;
}

auto reset_schema(const plinth::Config::Database& db) -> void {
  auto migrations_dir = std::string{CMAKE_SOURCE_DIR} + "/migrations";
  plinth::db::bootstrap_schema(db, migrations_dir, /*dev_mode=*/true);
  plinth::groups::bootstrap_groups(db);
}

// ── TestPg ──────────────────────────────────────────────────────

TestPg::TestPg(const plinth::Config::Database& db) {
  auto conninfo = "host=" + db.host + " port=" + std::to_string(db.port) +
                  " dbname=" + db.database + " user=" + db.user +
                  " password=" + db.password;
  conn = PQconnectdb(conninfo.c_str());
}

TestPg::~TestPg() {
  if (conn != nullptr) {
    PQfinish(conn);
  }
}

auto TestPg::exec(const std::string& sql) const
    -> std::unique_ptr<PGresult, decltype(&PQclear)> {
  return {PQexec(conn, sql.c_str()), PQclear};
}

auto TestPg::exec_params(const std::string& sql,
                         const std::vector<std::string>& params) const
    -> std::unique_ptr<PGresult, decltype(&PQclear)> {
  std::vector<const char*> values;
  values.reserve(params.size());
  for (const auto& p : params) {
    values.push_back(p.c_str());
  }
  return {PQexecParams(conn, sql.c_str(), static_cast<int>(params.size()),
                       nullptr, values.data(), nullptr, nullptr, 0),
          PQclear};
}

auto insert_user(TestPg& pg, const std::string& username,
                 const std::string& password) -> std::string {
  auto hash = plinth::auth::hash_password(password);
  auto res =
      pg.exec_params("INSERT INTO plinth.users (username, password_hash) "
                     "VALUES ($1, $2) RETURNING id",
                     {username, hash});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_session(TestPg& pg, const std::string& user_id,
                    const std::string& raw_token) -> std::string {
  auto token_hash = plinth::auth::sha256_hex(raw_token);
  auto res = pg.exec_params("INSERT INTO plinth.sessions (user_id, token_hash) "
                            "VALUES ($1::uuid, $2) RETURNING id",
                            {user_id, token_hash});
  return PQgetvalue(res.get(), 0, 0);
}

auto insert_pat(TestPg& pg, const std::string& user_id,
                const std::string& raw_random) -> std::string {
  auto token_hash = plinth::auth::sha256_hex(raw_random);
  auto prefix = raw_random.substr(0, 8);
  auto res = pg.exec_params(
      "INSERT INTO plinth.pats (user_id, name, token_hash, token_prefix) "
      "VALUES ($1::uuid, $2, $3, $4) RETURNING id",
      {user_id, "test-pat", token_hash, prefix});
  return PQgetvalue(res.get(), 0, 0);
}

auto make_admin(TestPg& pg, const std::string& user_id) -> void {
  // bootstrap_groups has already run and created the admin group +
  // kernel.admin rule; just join the user.
  pg.exec_params("INSERT INTO plinth.group_members (group_id, user_id) "
                 "SELECT id, $1::uuid FROM plinth.groups WHERE name = 'admin' "
                 "ON CONFLICT DO NOTHING",
                 {user_id});
}

// ── Test server singleton ───────────────────────────────────────

namespace {

// Drogon cannot be started twice in the same process. We start it once on
// first use and leave it running until the test binary exits.
// singleton
std::once_flag g_server_once;
std::thread g_server_thread;
std::atomic<bool> g_server_ready{false};

auto start_test_server() -> void {
  // 0.6.3.N — `cfg` must outlive this function so the
  // `extensions::init_registry(cfg)` call below can stash a stable
  // pointer that `create_pool` reads on every later install event.
  // start_test_server is `std::call_once`-gated (g_server_once
  // above), so the static initialiser fires exactly once per
  // process. Pre-0.6.3.N this was a stack-local; without the lifetime
  // fix `cfg_ptr` dangled the moment we returned and the first
  // `POST /api/packages` SIGABRT'd in `create_pool`'s
  // `cfg_ptr->packages_data_dir` string ctor with `std::bad_alloc`.
  // above
  static plinth::Config cfg = test_config();
  if (!pg_available()) {
    throw std::runtime_error(
        "test_server_port() requires PG; call pg_available() first");
  }

  // NOTE: do NOT reset_schema here. Each test that needs fixtures calls
  // reset_schema() then inserts rows, *then* constructs WsTestClient —
  // which is what first triggers this function. A reset here runs after
  // those inserts and wipes them (CI hit this: sessions/PATs disappeared
  // between insert and the WS auth query). Tests that don't need data
  // don't query the DB, so missing schema is tolerated for them.

  plinth::log::init(cfg);
  plinth::log::set_node_id(cfg.node_id);

  drogon::app().createDbClient("postgresql", cfg.db.host, cfg.db.port,
                               cfg.db.database, cfg.db.user, cfg.db.password,
                               cfg.db.pool_size);
  auto shutdown = std::make_shared<plinth::lifecycle::ShutdownCoordinator>();
  plinth::test_process::register_shutdown([shutdown] {
    if (!g_server_ready.load()) {
      return;
    }
    auto result = shutdown->quiesce();
    if (!result.clean) {
      throw std::runtime_error("test server shutdown stopped at " +
                               result.failed_step);
    }
    if (g_server_thread.joinable()) {
      g_server_thread.join();
    }
    shutdown->finish_after_drogon();
  });
  if (!plinth::packages::rbac_test::start_async_workers()) {
    throw std::runtime_error("RBAC worker registry could not start");
  }

  // LH-0.1 process-lifetime pool for lh0:1:js_stress dispatch.
  plinth::ws::init_js_stress_pool(cfg);

  plinth::ws::register_ws_routes(cfg);
  // Share the test server with asset-server tests (ICD-0.4.4 I.13-I.15).
  // Drogon's app() is a process-wide singleton; the asset handler's
  // wildcard regex /ext/{name}/{version}/* does not collide with WS
  // routes (/ws/*). Per-test (name,version) mappings are added via
  // asset_server::register_routes() inside individual test cases.
  plinth::packages::asset_server::register_drogon_handler();

  // 0.6.0.N HTTP fixture (session 2): register the packages routes
  // (POST/GET /api/packages, GET/PATCH/DELETE /api/packages/{id}) so
  // `tests/kernel/packages/http_test_fixture.{hpp,cpp}` can drive
  // them via real HTTP through SessionFilter + RbacFilter (matching
  // production main.cpp:453-462). Tests not exercising packages get
  // an unused registration — no measurable cost.
  plinth::packages::register_package_routes({
      .db = cfg.db,
      .data_dir = cfg.packages_data_dir,
      .staging_dir = cfg.packages_staging_dir,
      .max_package_size_bytes =
          cfg.packages_max_package_size_mb * 1024ULL * 1024ULL,
      .upgrade_drain_timeout_ms = cfg.packages_upgrade_drain_timeout_ms,
  });

  // ICD-0.6.3 §5 — `POST /api/cap/{capability}` route registered here so
  // tests/kernel/cap/api_cap_test.cpp can drive it via real HTTP through
  // SessionFilter (no RbacFilter — the resolver enforces RBAC step 3 per
  // ICD-0.2.4). Mirrors the production main.cpp slot.
  plinth::cap::register_cap_routes(cfg.db);

  // 0.6.3.N — mirror production main.cpp:355,363 so tests dispatching
  // through `extensions::dispatch` (e.g. api_cap_test's
  // `POST /api/cap/shell.*`) hit a populated Tier 2 cache + extension
  // pool registry. Both calls are tolerant of a missing schema (warn
  // and proceed empty) — tests that don't seed packages get a no-op.
  // The process-level test owner invokes the production coordinator graph.
  plinth::capabilities::init_resolver(cfg.db);
  plinth::extensions::init_registry(cfg);
  shutdown->install_ingress_gate();

  drogon::app()
      .setLogPath("")
      .setLogLevel(trantor::Logger::kWarn)
      .addListener(cfg.listen_host, cfg.listen_port)
      .setThreadNum(2)
      .disableSigtermHandling();

  // Drogon signals "ready" from its main loop; capture it here.
  drogon::app().getLoop()->queueInLoop([]() { g_server_ready.store(true); });

  g_server_thread = std::thread([]() { drogon::app().run(); });

  // Wait up to 5s for the ready signal.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!g_server_ready.load() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!g_server_ready.load()) {
    throw std::runtime_error("Drogon test server failed to start");
  }
}

} // namespace

auto test_server_port() -> uint16_t {
  std::call_once(g_server_once, []() { start_test_server(); });
  return TEST_PORT;
}

// ── WsTestClient ────────────────────────────────────────────────

WsTestClient::WsTestClient() {
  auto port = test_server_port();
  auto host = "ws://127.0.0.1:" + std::to_string(port);
  client = drogon::WebSocketClient::newWebSocketClient(host);

  client->setMessageHandler([this](std::string&& message,
                                   const drogon::WebSocketClientPtr& /*c*/,
                                   const drogon::WebSocketMessageType& type) {
    if (type != drogon::WebSocketMessageType::Text) {
      return;
    }
    // Drogon's handler signature is `std::string&&`, so consume the
    // rvalue by moving it into a local before processing.
    auto msg = std::move(message);
    // ICD-0.5.5 §8 L.03/L.04 — drain pause path. Raw text goes
    // into `paused_raw` without parsing; `resume_drain` re-parses
    // and flushes. CV is NOT notified so `receive_json` waiters
    // see no new frames while paused.
    if (drain_paused.load(std::memory_order_acquire)) {
      std::lock_guard lock(mu);
      paused_raw.push_back(std::move(msg));
      return;
    }
    Json::Value root;
    Json::CharReaderBuilder builder;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    std::string errs;
    // Json::CharReader::parse requires (begin, end) char pointers —
    // the one-past-end `msg.data() + msg.size()` is well-defined
    // per std::string's contiguous-storage guarantee.
    if (!reader->parse(msg.data(), msg.data() + msg.size(), &root, &errs)) {
      return; // ignore non-JSON
    }
    std::lock_guard lock(mu);
    inbox.push_back(std::move(root));
    if (inspector) {
      inspector(inbox.back());
    }
    cv.notify_all();
  });

  client->setConnectionClosedHandler(
      [this](const drogon::WebSocketClientPtr& /*c*/) {
        std::lock_guard lock(mu);
        closed = true;
        cv.notify_all();
      });
}

WsTestClient::~WsTestClient() {
  if (!client) {
    return;
  }
  // drogon::WebSocketClientImpl::stop() invokes trantor operations
  // (invalidateTimer, tcp shutdown, queued sends) on the owning
  // event loop. Calling stop() from a non-loop thread occasionally
  // trips an exception that propagates through a noexcept boundary
  // and std::terminates the process — see
  // project_ws_flaky_segfault.md for the history. Queue the stop()
  // onto Drogon's loop thread and wait for it to finish so the
  // destructor-driven teardown path is always loop-threaded.
  auto* loop = drogon::app().getLoop();
  auto client_copy = client; // shared_ptr copy outlives `this`
  std::promise<void> done;
  auto done_fut = done.get_future();
  loop->queueInLoop([client_copy, &done]() {
    client_copy->stop();
    done.set_value();
  });
  done_fut.wait();
}

auto WsTestClient::connect(std::chrono::milliseconds timeout) -> bool {
  std::promise<bool> p;
  auto f = p.get_future();
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setPath("/ws/events");
  client->connectToServer(req,
                          [this, &p](drogon::ReqResult r,
                                     const drogon::HttpResponsePtr& /*resp*/,
                                     const drogon::WebSocketClientPtr& /*c*/) {
                            auto ok = (r == drogon::ReqResult::Ok);
                            {
                              std::lock_guard lock(mu);
                              connected = ok;
                            }
                            p.set_value(ok);
                          });
  if (f.wait_for(timeout) != std::future_status::ready) {
    return false;
  }
  return f.get();
}

auto WsTestClient::send_json(const Json::Value& v) -> void {
  if (!connected) {
    return;
  }
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  auto payload = Json::writeString(b, v);
  auto conn = client->getConnection();
  if (conn && conn->connected()) {
    conn->send(payload);
  }
}

auto WsTestClient::receive_json(std::chrono::milliseconds timeout)
    -> std::optional<Json::Value> {
  std::unique_lock lock(mu);
  if (cv.wait_for(lock, timeout,
                  [this]() { return !inbox.empty() || closed; })) {
    if (!inbox.empty()) {
      auto v = std::move(inbox.front());
      inbox.pop_front();
      return v;
    }
  }
  return std::nullopt;
}

auto WsTestClient::wait_for_close(std::chrono::milliseconds timeout) -> bool {
  std::unique_lock lock(mu);
  return cv.wait_for(lock, timeout, [this]() { return closed; });
}

auto WsTestClient::pause_drain() -> void {
  drain_paused.store(true, std::memory_order_release);
}

auto WsTestClient::resume_drain() -> void {
  std::deque<std::string> drained;
  {
    std::lock_guard lock(mu);
    drain_paused.store(false, std::memory_order_release);
    drained.swap(paused_raw);
  }
  // Re-parse + push each frame, holding the same ordering invariant
  // as the live message handler. We release `mu` between iterations
  // so a real-time arrival can interleave (acceptable per L.* test
  // coordination — producer is held by `apply_drain_for_test`).
  Json::CharReaderBuilder builder;
  for (auto& raw : drained) {
    Json::Value root;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    std::string errs;
    if (!reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs)) {
      continue;
    }
    std::lock_guard lock(mu);
    inbox.push_back(std::move(root));
    if (inspector) {
      inspector(inbox.back());
    }
    cv.notify_all();
  }
}

auto WsTestClient::is_drain_paused() const -> bool {
  return drain_paused.load(std::memory_order_acquire);
}

auto WsTestClient::set_frame_inspector(FrameInspector cb) -> void {
  std::lock_guard lock(mu);
  inspector = std::move(cb);
}

// ── live_buffer_cap_override wrappers ──────────────────────────────

auto set_live_buffer_cap_override(std::size_t cap) -> void {
  plinth::ws::test_seam::set_live_buffer_cap_override(cap);
}

auto clear_live_buffer_cap_override() -> void {
  plinth::ws::test_seam::clear_live_buffer_cap_override();
}

} // namespace plinth::ws_test
