// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace plinth::lifecycle {

struct ShutdownResult {
  bool clean = true;
  std::vector<std::string> completed_steps;
  std::string failed_step;
};

struct ShutdownHooks {
  std::function<bool(std::chrono::milliseconds)> close_ingress;
  std::function<bool(std::chrono::milliseconds)> stop_listeners;
  std::function<bool(std::chrono::milliseconds)> drain_async_tasks;
  std::function<bool(std::chrono::milliseconds)> drain_rbac_workers;
  std::function<bool(std::chrono::milliseconds)> drain_extension_dispatches;
  std::function<bool(std::chrono::milliseconds)> drain_js_stress_dispatches;
  std::function<bool(std::chrono::milliseconds)> flush_database_state;
  std::function<bool(std::chrono::milliseconds)> close_audit_gate;
  std::function<bool(std::chrono::milliseconds)> stop_drogon;
  std::function<void()> close_log_sinks;
};

// One process-owned shutdown graph. quiesce() is idempotent and safe when
// called concurrently; finish_after_drogon() closes log sinks only after the
// thread running app().run() has joined.
class ShutdownCoordinator {
 public:
  explicit ShutdownCoordinator(
      std::chrono::milliseconds timeout = std::chrono::seconds{40});
  ShutdownCoordinator(ShutdownHooks hooks, std::chrono::milliseconds timeout);

  ShutdownCoordinator(const ShutdownCoordinator&) = delete;
  ShutdownCoordinator(ShutdownCoordinator&&) = delete;
  auto operator=(const ShutdownCoordinator&) -> ShutdownCoordinator& = delete;
  auto operator=(ShutdownCoordinator&&) -> ShutdownCoordinator& = delete;

  // Installs process-wide HTTP and connection admission gates. Call once after
  // route registration and before app().run().
  auto install_ingress_gate() -> void;

  [[nodiscard]] auto accepting_ingress() const noexcept -> bool;

  // Runs the dependency graph through the Drogon stop request. A false result
  // is fail-closed: the graph stops at failed_step and does not destroy any
  // downstream dependency still usable by live work. A later call may retry.
  [[nodiscard]] auto quiesce() -> ShutdownResult;

  // Called by the owner of app().run() after that call returns and its thread
  // joins. Idempotent.
  auto finish_after_drogon() -> void;

 private:
  enum class State : unsigned char {
    RUNNING,
    QUIESCING,
    QUIESCED,
    FAILED,
    FINISHING,
    FINISHED,
  };

  [[nodiscard]] auto run_graph() -> ShutdownResult;
  [[nodiscard]] auto drain_http_requests(std::chrono::milliseconds timeout)
      -> bool;

  ShutdownHooks hooks;
  std::chrono::milliseconds timeout;
  std::atomic<bool> accepting{true};
  std::mutex requests_mu;
  std::condition_variable requests_cv;
  std::size_t active_requests = 0;
  std::mutex state_mu;
  std::condition_variable state_cv;
  State state = State::RUNNING;
  ShutdownResult last_result;
};

[[nodiscard]] auto production_shutdown_hooks() -> ShutdownHooks;

} // namespace plinth::lifecycle
