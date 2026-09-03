#include <argparse/argparse.hpp>
#include <drogon/drogon.h>
#include <plinth/version.hpp>
#include <spdlog/spdlog.h>

#include "kernel/audit/handlers.hpp"
#include "kernel/auth/handlers.hpp"
#include "kernel/cap/api_cap.hpp"
#include "kernel/capabilities/bootstrap.hpp"
#include "kernel/capabilities/listener.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/config.hpp"
#include "kernel/db/bootstrap.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/frontend/api_frontend.hpp"
#include "kernel/groups/handlers.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/js/db_search_path.hpp"
#include "kernel/js/db_silent_audit.hpp"
#include "kernel/js/eval_guard.hpp"
#include "kernel/js/stdlib/db_result_to_json.hpp"
#include "kernel/lifecycle/shutdown_coordinator.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/handlers.hpp"
#include "kernel/packages/install_lifecycle.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/packages/validator.hpp"
#include "kernel/rbac/enforcement.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/shell/active_frontend.hpp"
#include "kernel/shell/firstboot.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/js_stress.hpp"
#include "kernel/ws/registration.hpp"

#include <json/value.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

auto register_healthz() -> void {
  drogon::app().registerHandler(
      "/healthz",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(
            Json::Value{Json::objectValue});
        resp->getJsonObject()->operator[]("status") = "ok";
        std::move(callback)(resp);
      },
      {drogon::Get});
}

auto resolve_kernel_url(const std::string& flag) -> std::optional<std::string> {
  if (!flag.empty()) {
    return flag;
  }
  const char* env_kernel = std::getenv("PLINTH_KERNEL_URL");
  // empty-string check on char*.
  if (env_kernel != nullptr && *env_kernel != '\0') {
    return std::string{env_kernel};
  }
  return std::nullopt;
}

auto stdout_supports_colour() -> bool {
  bool stdout_tty = ::isatty(STDOUT_FILENO) != 0;
  // already called it.
  const char* no_color = std::getenv("NO_COLOR");
  // empty-string check on char*.
  return stdout_tty && (no_color == nullptr || *no_color == '\0');
}

auto run_validate(const argparse::ArgumentParser& validate_cmd) -> int {
  auto path = validate_cmd.get<std::string>("path");
  auto json_mode = validate_cmd.get<bool>("--json");
  auto quiet = validate_cmd.get<bool>("--quiet");
  auto structure_only = validate_cmd.get<bool>("--structure-only");
  auto against_kernel = validate_cmd.get<bool>("--against-running-kernel");
  auto kernel_flag = validate_cmd.get<std::string>("--kernel");

  plinth::packages::ValidationConfig cfg;
  cfg.max_size_bytes =
      static_cast<std::size_t>(validate_cmd.get<std::uint64_t>("--max-size"));
  cfg.cross_file = !structure_only;
  cfg.against_running_kernel = against_kernel;
  cfg.kernel_url = resolve_kernel_url(kernel_flag);

  if (against_kernel && !cfg.kernel_url) {
    std::cerr << "--against-running-kernel requires --kernel <url> "
                 "or PLINTH_KERNEL_URL\n";
    return 1;
  }

  auto report = plinth::packages::validate(std::filesystem::path{path}, cfg);

  if (json_mode) {
    plinth::packages::render_json(report, std::filesystem::path{path},
                                  std::cout);
  } else {
    plinth::packages::render_text(
        report, std::filesystem::path{path}, std::cout,
        {.quiet = quiet, .colour = stdout_supports_colour()});
  }
  return report.disposition();
}

auto shutdown_signals() -> sigset_t {
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  return signals;
}

auto block_shutdown_signals() -> void {
  auto signals = shutdown_signals();
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
    throw std::runtime_error("failed to block SIGINT/SIGTERM");
  }
}

[[noreturn]] auto emergency_shutdown_exit(std::string_view reason) -> void {
  plinth::log::shutdown();
  spdlog::critical("shutdown failed closed: {}", reason);
  if (auto logger = spdlog::default_logger(); logger != nullptr) {
    logger->flush();
  }
  spdlog::shutdown();
  std::_Exit(2);
}

class ShutdownDeadlineGuard {
 public:
  explicit ShutdownDeadlineGuard(std::chrono::milliseconds timeout)
      : watchdog([this, timeout] {
          std::unique_lock lock(mu);
          if (cv.wait_for(lock, timeout, [this] { return finished; })) {
            return;
          }
          lock.unlock();
          constexpr std::string_view message =
              "shutdown hard deadline exceeded; exiting fail-closed\n";
          (void)::write(STDERR_FILENO, message.data(), message.size());
          std::_Exit(2);
        }) {}

  ~ShutdownDeadlineGuard() {
    {
      std::lock_guard lock(mu);
      finished = true;
    }
    cv.notify_all();
  }

  ShutdownDeadlineGuard(const ShutdownDeadlineGuard&) = delete;
  auto operator=(const ShutdownDeadlineGuard&)
      -> ShutdownDeadlineGuard& = delete;

 private:
  std::mutex mu;
  std::condition_variable cv;
  bool finished = false;
  std::jthread watchdog;
};

auto bounded_quiesce(plinth::lifecycle::ShutdownCoordinator& coordinator)
    -> plinth::lifecycle::ShutdownResult {
  ShutdownDeadlineGuard hard_deadline{std::chrono::seconds{50}};
  return coordinator.quiesce();
}

auto run_drogon_until_shutdown(
    plinth::lifecycle::ShutdownCoordinator& coordinator)
    -> plinth::lifecycle::ShutdownResult {
  struct AppState {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::exception_ptr exception;
  } state;

  std::thread app_thread([&state] {
    try {
      drogon::app().run();
    } catch (...) {
      std::lock_guard lock(state.mu);
      state.exception = std::current_exception();
    }
    {
      std::lock_guard lock(state.mu);
      state.done = true;
    }
    state.cv.notify_all();
  });

  auto signals = shutdown_signals();
  std::optional<plinth::lifecycle::ShutdownResult> shutdown_result;
  for (;;) {
    {
      std::lock_guard lock(state.mu);
      if (state.done) {
        break;
      }
    }

    timespec wait{.tv_sec = 0, .tv_nsec = 100'000'000};
    int received = sigtimedwait(&signals, nullptr, &wait);
    if (received == SIGINT || received == SIGTERM) {
      auto startup_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds{10};
      std::unique_lock lock(state.mu);
      while (!state.done && !drogon::app().isRunning() &&
             std::chrono::steady_clock::now() < startup_deadline) {
        state.cv.wait_for(lock, std::chrono::milliseconds{10});
      }
      bool can_stop = state.done || drogon::app().isRunning();
      lock.unlock();
      if (!can_stop) {
        emergency_shutdown_exit("Drogon did not finish startup");
      }
      shutdown_result = bounded_quiesce(coordinator);
      if (!shutdown_result->clean) {
        emergency_shutdown_exit(shutdown_result->failed_step);
      }
      break;
    }
    if (received < 0 && errno != EAGAIN && errno != EINTR) {
      emergency_shutdown_exit("sigtimedwait failed");
    }
  }

  if (!shutdown_result.has_value()) {
    shutdown_result = bounded_quiesce(coordinator);
    if (!shutdown_result->clean) {
      emergency_shutdown_exit(shutdown_result->failed_step);
    }
  }

  {
    std::unique_lock lock(state.mu);
    if (!state.cv.wait_for(lock, std::chrono::seconds{10},
                           [&state] { return state.done; })) {
      emergency_shutdown_exit("Drogon event loops did not stop");
    }
  }
  app_thread.join();
  if (state.exception != nullptr) {
    std::rethrow_exception(state.exception);
  }
  return *shutdown_result;
}

class ServeShutdownGuard {
 public:
  explicit ServeShutdownGuard(
      plinth::lifecycle::ShutdownCoordinator& coordinator_in)
      : coordinator(coordinator_in) {}

  ~ServeShutdownGuard() {
    if (!dismissed) {
      ShutdownDeadlineGuard hard_deadline{std::chrono::seconds{50}};
      auto result = coordinator.quiesce();
      if (!result.clean) {
        emergency_shutdown_exit(result.failed_step);
      }
      coordinator.finish_after_drogon();
    }
  }

  ServeShutdownGuard(const ServeShutdownGuard&) = delete;
  auto operator=(const ServeShutdownGuard&) -> ServeShutdownGuard& = delete;

  auto dismiss() noexcept -> void { dismissed = true; }

 private:
  plinth::lifecycle::ShutdownCoordinator& coordinator;
  bool dismissed = false;
};

} // namespace

auto main(int argc, char* argv[]) -> int {
  argparse::ArgumentParser program("plinth", plinth::VERSION);

  // ── serve subcommand ────────────────────────────────────
  argparse::ArgumentParser serve_cmd("serve");
  serve_cmd.add_description("Start the Plinth kernel");
  serve_cmd.add_argument("--config", "-c")
      .help("Path to a required JSON configuration file");
  serve_cmd.add_argument("--port", "-p")
      .default_value(0)
      .scan<'i', int>()
      .help("Listen port (overrides config)");
  serve_cmd.add_argument("--host")
      .default_value(std::string{})
      .help("Listen address (overrides config)");
  serve_cmd.add_argument("--dev")
      .default_value(false)
      .implicit_value(true)
      .help("Enable dev_mode (destructive schema reset on startup)");

  // ── validate subcommand ─────────────────────────────────
  argparse::ArgumentParser validate_cmd("validate");
  validate_cmd.add_description("Validate an extension package");
  validate_cmd.add_argument("path").help("Path to the extension directory");
  validate_cmd.add_argument("--max-size")
      .default_value(std::uint64_t{50ULL * 1024ULL * 1024ULL})
      .scan<'u', std::uint64_t>()
      .help("Maximum package size in bytes (default 50 MB)");
  validate_cmd.add_argument("--json")
      .default_value(false)
      .implicit_value(true)
      .help("Emit machine-readable JSON to stdout");
  validate_cmd.add_argument("--quiet")
      .default_value(false)
      .implicit_value(true)
      .help("Suppress text output; rely on exit code");
  auto& validate_mode = validate_cmd.add_mutually_exclusive_group();
  validate_mode.add_argument("--structure-only")
      .default_value(false)
      .implicit_value(true)
      .help("Run only R1..R6 (skip cross-file and runtime-state rules)");
  validate_mode.add_argument("--against-running-kernel")
      .default_value(false)
      .implicit_value(true)
      .help("Run runtime-state rules against a live kernel in addition "
            "to cross-file rules");
  validate_cmd.add_argument("--kernel")
      .default_value(std::string{})
      .help("Kernel URL for --against-running-kernel "
            "(overrides PLINTH_KERNEL_URL)");

  // ── test rbac subcommand (ICD-0.4.7 §CLI Surface) ───────
  // Two-level nest: `plinth test rbac <extension>`.
  argparse::ArgumentParser test_cmd("test");
  test_cmd.add_description("Test harness subcommands");
  argparse::ArgumentParser test_rbac_cmd("rbac");
  test_rbac_cmd.add_description(
      "Re-run RBAC integration tests for an installed extension");
  test_rbac_cmd.add_argument("extension")
      .help("Extension name (plinth.packages.name, must be ACTIVE or "
            "ACTIVE_FLAGGED)");
  test_rbac_cmd.add_argument("--config", "-c")
      .help("Path to a required JSON configuration file");
  test_rbac_cmd.add_argument("--json")
      .default_value(false)
      .implicit_value(true)
      .help("Emit machine-readable JSON to stdout");
  test_rbac_cmd.add_argument("--run-id")
      .default_value(std::string{})
      .help("Force a specific run_id (UUIDv4). Default: generated");
  test_cmd.add_subparser(test_rbac_cmd);

  // ── register subcommands ────────────────────────────────
  program.add_subparser(serve_cmd);
  program.add_subparser(validate_cmd);
  program.add_subparser(test_cmd);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    std::cerr << program;
    return 1;
  }

  try {
    // ── serve ───────────────────────────────────────────
    if (program.is_subcommand_used("serve")) {
      const bool has_config = serve_cmd.is_used("--config");
      const auto config_path = has_config
                                   ? serve_cmd.get<std::string>("--config")
                                   : std::string{"<defaults+environment>"};

      // Load config: JSON file → env vars
      auto cfg =
          has_config ? plinth::load_config(config_path) : plinth::load_config();

      // CLI overrides (layer 3)
      if (serve_cmd.get<bool>("--dev")) {
        cfg.dev_mode = true;
      }
      auto cli_port = serve_cmd.get<int>("--port");
      if (cli_port > 0) {
        cfg.listen_port = static_cast<uint16_t>(cli_port);
      }
      auto cli_host = serve_cmd.get<std::string>("--host");
      if (!cli_host.empty()) {
        cfg.listen_host = cli_host;
      }

      // Block before logging or any subsystem starts a thread. The service
      // owner below consumes these signals synchronously and invokes the
      // coordinator outside signal context.
      block_shutdown_signals();
      plinth::lifecycle::ShutdownCoordinator shutdown;
      ServeShutdownGuard shutdown_guard{shutdown};

      plinth::log::init(cfg);
      plinth::log::set_node_id(cfg.node_id);

      // ICD-0.4.1 — wire the scanner policy from loaded Config and
      // emit the one-shot `security.unicode_scanner_disabled`
      // audit if an operator turned the gate off.
      plinth::js::set_unicode_scanner_policy(
          cfg.security_unicode_scanner_enabled,
          cfg.security_unicode_scanner_threshold,
          cfg.security_unicode_scanner_log_findings);
      if (!cfg.security_unicode_scanner_enabled) {
        Json::Value detail{Json::objectValue};
        detail["config_origin"] =
            has_config ? "explicit_file" : "defaults+environment";
        plinth::log::audit("security.unicode_scanner_disabled", detail,
                           plinth::log::AuditCtx{});
      }

      spdlog::info("plinth {} starting...", plinth::VERSION);
      spdlog::info("config={} host={}:{} dev_mode={}", config_path,
                   cfg.listen_host, cfg.listen_port, cfg.dev_mode);

      // Database bootstrap
      plinth::db::bootstrap_schema(cfg.db, cfg.migrations_dir, cfg.dev_mode);
      plinth::groups::bootstrap_groups(cfg.db);

      // Register Drogon PG connection pool for runtime queries
      drogon::app().createDbClient("postgresql", cfg.db.host, cfg.db.port,
                                   cfg.db.database, cfg.db.user,
                                   cfg.db.password, cfg.db.pool_size);

      // Seed kernel RBAC rules + capabilities (per ICD-0.2.0 §Bootstrap).
      plinth::capabilities::bootstrap_kernel_capabilities(cfg.db);

      // Load the Tier 2 cache and register Tier 1 stub handlers
      // (per ICD-0.2.2 §Resolution Algorithm).
      plinth::capabilities::init_resolver(cfg.db);

      // ICD-0.5.0.3 §Lifecycle — spin up one RuntimePool per
      // installed-ACTIVE extension. Must run AFTER init_resolver
      // so the Tier 2 cache is coherent with the `plinth.packages`
      // scan this init performs, and BEFORE route registration
      // so dispatched extension calls land on a live pool from
      // first request.
      plinth::extensions::init_registry(cfg);

      // LH-0.1 — process-lifetime RuntimePool for the
      // lh0:1:js_stress diagnostic dispatch. See
      // docs/icd/ICD-LH-0.1-async-bridge-stress.md §5.
      plinth::ws::init_js_stress_pool(cfg);

      // Start the LISTEN/NOTIFY subscriber so registry mutations
      // on any node reach our Tier 2 cache (ICD-0.2.3). Stopped
      // below after app().run() returns.
      plinth::capabilities::start_notify_listener(cfg.db);

      // ICD-0.5.0 §Deterministic Teardown + §Startup placement
      // — per-node LISTEN subscriber for the realtime event bus.
      // Runs as a sibling to the 0.2.3 capability listener; see
      // ICD §Relationship to the 0.2.3 Capability Listener.
      plinth::realtime::start_listener(cfg.db, cfg.realtime.listener);

      // ICD-0.5.1 §Startup + shutdown wiring — spin up the PG
      // auto-event coalescer after the listener. The coalescer's
      // dedicated event-loop thread hosts the window timers; the shutdown
      // coordinator reverses the dependency order.
      plinth::realtime::CoalescerRegistry::instance().start(
          cfg.realtime.coalescer);

      // ICD-0.5.2 §Broker Subsystem → Lifecycle integration. As
      // of ICD-0.5.5 §5 the broker is no longer a peer listener
      // handler; `broker::start` only flips `broker_enabled` and
      // pulls config so the writer-downstream `broker::dispatch`
      // call from `events_writer::insert_envelope` knows it is
      // safe to fan out. Order is still listener → coalescer →
      // broker → events_writer (so the writer's call into the
      // broker sees `broker_enabled=true` from the start).
      plinth::realtime::broker::start(cfg.realtime.broker);

      // ICD-0.5.5 §5 §Topology pin — the events writer is the
      // sole listener handler. Inside `insert_envelope` it
      // INSERTs, stamps `ev.envelope["seq"]` from the RETURNING
      // result, calls `broker::dispatch` (which fans to WS + JS
      // subscribers and populates `ev.delivered_to_users`), then
      // advances the per-user cursor. The shutdown coordinator drains this
      // database-backed work before destroying Drogon.
      plinth::realtime::events_writer::start(cfg.realtime.events);

      // ICD-0.5.3 §OID-Driven PG-Type → JS-Type Mapping §Feature
      // flag — propagate `db.oid_mapping.enabled` into the kernel
      // JS-result converter so db.query / db.exec honor the
      // setting on the first query after startup.
      plinth::js::db::set_oid_mapping_enabled(
          cfg.db_bindings.oid_mapping.enabled);

      // ICD-0.5.3 §silent Flag §Rate-limited `db.silent.used`
      // audit — propagate the aggregation window so silent-exec
      // bursts coalesce under the expected config window.
      plinth::js::set_silent_audit_window_ms(
          cfg.db_bindings.silent.audit_window_ms);

      // ICD-0.5.3 §Per-Op SET search_path Isolation §Config
      // override. Propagate the enforce flag so db.exec /
      // db.query from extension-scope bcs wrap in BEGIN; SET
      // LOCAL search_path ...; user_sql; COMMIT.
      plinth::js::db::set_search_path_enforce(
          cfg.db_bindings.search_path.enforce);

      // ICD-0.5.3 §`db.batch()` §Config Surface — propagate
      // the audit aggregation window, the per-batch op quota,
      // and the §B.06 wall-clock deadline so both observability
      // and the synchronous reject paths match the operator's
      // config.
      plinth::js::set_batch_audit_window_ms(
          cfg.db_bindings.batch.audit_window_ms);
      plinth::js::set_batch_max_ops_per_batch(
          cfg.db_bindings.batch.max_ops_per_batch);
      plinth::js::set_batch_timeout_ms(cfg.db_bindings.batch.timeout_ms);

      // TODO: scheduler init
      // TODO: QuickJS runtime pool init

      register_healthz();
      plinth::auth::register_auth_routes(cfg.dev_mode,
                                         cfg.registration_enabled);
      plinth::auth::register_pat_routes();
      plinth::groups::register_group_routes();
      plinth::audit::register_audit_routes();
      plinth::ws::register_ws_routes(cfg);

      // ICD-0.4.4: package install lifecycle + asset serving.
      // The asset-server wildcard route is registered once; per-
      // (name, version) entries live in an in-memory route map
      // populated by restore_routes() for already-ACTIVE rows
      // and by install_package() for new installs.
      plinth::packages::asset_server::register_drogon_handler();
      plinth::packages::PackageRoutesConfig pkgs_cfg{
          .db = cfg.db,
          .data_dir = cfg.packages_data_dir,
          .staging_dir = cfg.packages_staging_dir,
          .max_package_size_bytes =
              cfg.packages_max_package_size_mb * 1024ULL * 1024ULL,
          .upgrade_drain_timeout_ms = cfg.packages_upgrade_drain_timeout_ms,
      };
      plinth::packages::register_package_routes(pkgs_cfg);
      // Bootstrap install path (ICD-0.4.4 slice B + ICD-0.6.1 §3.6):
      //   1) reconcile_in_flight_installs — recover rows left in
      //      any mid-install state by a previous crash.
      //   2) shell::ensure_bundled_shell_installed — first-boot the
      //      bundled shell from on-disk `<bundle_path>/shell.zip`
      //      when no ACTIVE bundled frontend exists. Failure here
      //      aborts bootstrap with an exit code per ICD-0.6.1 §3.5.
      //   3) asset_server::restore_routes — rebuild the in-memory
      //      (name,version) → on-disk-tree route map from every
      //      ACTIVE/ACTIVE_FLAGGED row (the shell just installed
      //      counts; register_asset_routes was called inside its
      //      ACTIVATING stage but restore_routes is idempotent).
      plinth::packages::InstallerContext bootstrap_ctx{
          .db = cfg.db,
          .caller_user_id = "",
          .data_dir = cfg.packages_data_dir,
          .staging_dir = cfg.packages_staging_dir,
          .max_package_size_bytes =
              cfg.packages_max_package_size_mb * 1024ULL * 1024ULL,
          .upgrade_drain_timeout_ms =
              std::chrono::milliseconds{cfg.packages_upgrade_drain_timeout_ms},
      };
      if (!plinth::packages::rbac_test::start_async_workers()) {
        throw std::runtime_error(
            "RBAC worker registry still draining from a prior lifecycle");
      }
      // ICD-0.4.5 §Security Constraint 4 — bootstrap refuses to
      // start if {data_dir} and {staging_dir} are on different
      // mountpoints. Atomic-swap rename(2) relies on the single-
      // filesystem guarantee.
      if (auto mp = plinth::packages::check_single_mountpoint(
              cfg.packages_data_dir, cfg.packages_staging_dir);
          !mp.has_value()) {
        spdlog::critical("bootstrap: mountpoint check failed: {}", mp.error());
        return 1;
      }
      plinth::packages::reconcile_in_flight_installs(bootstrap_ctx);
      if (auto fb =
              plinth::shell::ensure_bundled_shell_installed(cfg, bootstrap_ctx);
          !fb.has_value()) {
        spdlog::critical("shell::firstboot: aborting boot — kind={} message={}",
                         fb.error().kind_string(), fb.error().message);
        return fb.error().exit_code();
      }
      plinth::packages::asset_server::restore_routes(cfg.db,
                                                     cfg.packages_data_dir);

      // ICD-0.6.3 §5 — `POST /api/cap/{capability}` HTTP cap-
      // dispatch route. Browser-side `plinth.call(cap, args)`
      // posts here; the handler resolves auth via SessionFilter,
      // synthesises the resolver signature triple from the URL
      // parameter, populates effective_rules from plinth.group_rules
      // (mirroring RbacFilter's SQL), and co_awaits the resolver
      // path. Slots in alongside the other kernel `/api/*` routes
      // BEFORE the catch-all so it does not get shadowed.
      plinth::cap::register_cap_routes(cfg.db);

      // ICD-0.6.2 §6 — `/api/frontend/tokens.css` indirection.
      // 302 → `/ext/{active-frontend}/{version}/css/tokens.css`
      // when a single ACTIVE frontend exists; 503 with JSON
      // diagnostic otherwise. Slot is AFTER kernel `/api/*` and
      // `/ext/*` registrations (so the asset server resolves the
      // 302 target) and BEFORE the active-frontend catch-all
      // (so the catch-all does not shadow `/api/frontend/*`).
      plinth::frontend::register_api_frontend_routes(cfg.db);

      // ICD-0.6.1 §4.4 / §4.6 — register the active frontend's
      // `/` redirect + `<mount>(.*)` SPA-fallback handler AFTER
      // all `/api/*`, `/ext/*`, `/ws`, `/healthz` registrations
      // so the catch-all glob does not shadow kernel API surfaces.
      // Reads `plinth.packages` to resolve the active frontend's
      // mount + entry + installed `client/` directory. Replaces
      // ICD-0.6.0 §8.1's hardcoded `/app/*` static handler.
      plinth::shell::register_routes_for_active_frontend(cfg.shell, cfg.db,
                                                         cfg.packages_data_dir);

      shutdown.install_ingress_gate();
      drogon::app()
          .setLogPath("") // Drogon logging disabled — spdlog handles it
          .setLogLevel(trantor::Logger::kWarn)
          .addListener(cfg.listen_host, cfg.listen_port)
          .setThreadNum(std::thread::hardware_concurrency())
          .disableSigtermHandling();

      auto shutdown_result = run_drogon_until_shutdown(shutdown);
      if (!shutdown_result.clean) {
        emergency_shutdown_exit(shutdown_result.failed_step);
      }
      shutdown.finish_after_drogon();
      shutdown_guard.dismiss();
      return 0;
    }

    // ── validate ────────────────────────────────────────
    if (program.is_subcommand_used("validate")) {
      return run_validate(validate_cmd);
    }

    // ── test rbac ───────────────────────────────────────
    if (program.is_subcommand_used("test") &&
        test_cmd.is_subcommand_used("rbac")) {
      const bool has_config = test_rbac_cmd.is_used("--config");
      auto cfg =
          has_config
              ? plinth::load_config(test_rbac_cmd.get<std::string>("--config"))
              : plinth::load_config();
      plinth::packages::InstallerContext ctx{
          .db = cfg.db,
          .caller_user_id = "",
          .data_dir = cfg.packages_data_dir,
          .staging_dir = cfg.packages_staging_dir,
          .max_package_size_bytes =
              cfg.packages_max_package_size_mb * 1024ULL * 1024ULL,
          .upgrade_drain_timeout_ms =
              std::chrono::milliseconds{cfg.packages_upgrade_drain_timeout_ms},
      };
      plinth::packages::rbac_test::CliTestRbacOptions opts{
          .extension_name = test_rbac_cmd.get<std::string>("extension"),
          .json_output = test_rbac_cmd.get<bool>("--json"),
          .run_id = test_rbac_cmd.get<std::string>("--run-id"),
      };
      return plinth::packages::rbac_test::run_cli_test_rbac(
          opts, ctx, std::cout, std::cerr);
    }
  } catch (const std::exception& e) {
    if (auto logger = spdlog::default_logger(); logger != nullptr) {
      logger->critical("fatal: {}", e.what());
      logger->flush();
    } else {
      std::cerr << "fatal: " << e.what() << '\n';
    }
    return 1;
  }

  // No subcommand — print help
  std::cout << program;
  return 0;
}
