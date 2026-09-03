// SPDX-License-Identifier: MIT

#include "kernel/lifecycle/shutdown_coordinator.hpp"

#include "kernel/capabilities/listener.hpp"
#include "kernel/extensions/runtime_registry.hpp"
#include "kernel/js/db_batch_audit.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/logging.hpp"
#include "kernel/packages/asset_server.hpp"
#include "kernel/packages/rbac_test_runner.hpp"
#include "kernel/realtime/broker.hpp"
#include "kernel/realtime/coalescer.hpp"
#include "kernel/realtime/events_writer.hpp"
#include "kernel/realtime/listener.hpp"
#include "kernel/ws/connection_registry.hpp"
#include "kernel/ws/js_stress.hpp"

#include <drogon/drogon.h>
#include <json/value.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>

namespace plinth::lifecycle {

namespace {

constexpr std::string_view TRACKED_REQUEST_ATTRIBUTE =
    "plinth.lifecycle.tracked_request";

auto remaining_until(std::chrono::steady_clock::time_point deadline)
    -> std::chrono::milliseconds {
  auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

auto production_shutdown_hooks() -> ShutdownHooks {
  return ShutdownHooks{
      .close_ingress =
          [](std::chrono::milliseconds timeout) {
            async_tasks().close_admission();
            plinth::packages::asset_server::cancel_all_registrations();
            auto& registry = plinth::ws::ConnectionRegistry::instance();
            if (!registry.cancel_all_timers(timeout)) {
              return false;
            }
            registry.close_all_connections();
            plinth::ws::ConnectionRegistry::initiate_shutdown();
            return true;
          },
      .stop_listeners =
          [](std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            if (!plinth::capabilities::stop_notify_listener(
                    remaining_until(deadline))) {
              return false;
            }
            return plinth::realtime::stop_listener(remaining_until(deadline));
          },
      .drain_async_tasks =
          [](std::chrono::milliseconds timeout) {
            return async_tasks().drain(timeout);
          },
      .drain_rbac_workers =
          [](std::chrono::milliseconds timeout) {
            return plinth::packages::rbac_test::shutdown_async_workers(timeout);
          },
      .drain_extension_dispatches =
          [](std::chrono::milliseconds timeout) {
            return plinth::extensions::shutdown_registry(timeout);
          },
      .drain_js_stress_dispatches =
          [](std::chrono::milliseconds timeout) {
            return plinth::ws::shutdown_js_stress_pool(timeout);
          },
      .flush_database_state =
          [](std::chrono::milliseconds timeout) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            if (!plinth::realtime::events_writer::stop(
                    remaining_until(deadline))) {
              return false;
            }
            plinth::js::discard_all_batches();
            plinth::realtime::broker::stop();
            return plinth::realtime::CoalescerRegistry::instance().shutdown(
                remaining_until(deadline));
          },
      .close_audit_gate =
          [](std::chrono::milliseconds) {
            plinth::log::shutdown();
            return true;
          },
      .stop_drogon =
          [](std::chrono::milliseconds) {
            drogon::app().quit();
            return true;
          },
      .close_log_sinks = [] { spdlog::shutdown(); },
  };
}

ShutdownCoordinator::ShutdownCoordinator(std::chrono::milliseconds timeout_in)
    : ShutdownCoordinator(production_shutdown_hooks(), timeout_in) {
}

ShutdownCoordinator::ShutdownCoordinator(ShutdownHooks hooks_in,
                                         std::chrono::milliseconds timeout_in)
    : hooks(std::move(hooks_in)), timeout(timeout_in) {
}

auto ShutdownCoordinator::install_ingress_gate() -> void {
  drogon::app().registerNewConnectionAdvice(
      [this](const trantor::InetAddress&, const trantor::InetAddress&) {
        return accepting_ingress();
      });
  drogon::app().registerSyncAdvice(
      [this](const drogon::HttpRequestPtr&) -> drogon::HttpResponsePtr {
        if (accepting_ingress()) {
          return nullptr;
        }
        Json::Value body{Json::objectValue};
        body["error"] = "server_shutting_down";
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(drogon::k503ServiceUnavailable);
        return response;
      });
  drogon::app().registerPreHandlingAdvice(
      [this](const drogon::HttpRequestPtr& request,
             drogon::AdviceCallback&& reject,
             drogon::AdviceChainCallback&& continue_request) {
        bool admitted = false;
        {
          std::lock_guard lock(requests_mu);
          if (accepting.load(std::memory_order_acquire)) {
            ++active_requests;
            request->attributes()->insert(
                std::string{TRACKED_REQUEST_ATTRIBUTE}, true);
            admitted = true;
          }
        }
        if (admitted) {
          continue_request();
          return;
        }
        Json::Value body{Json::objectValue};
        body["error"] = "server_shutting_down";
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(drogon::k503ServiceUnavailable);
        reject(response);
      });
  drogon::app().registerPostHandlingAdvice(
      [this](const drogon::HttpRequestPtr& request,
             const drogon::HttpResponsePtr&) {
        auto attributes = request->attributes();
        if (!attributes->get<bool>(std::string{TRACKED_REQUEST_ATTRIBUTE})) {
          return;
        }
        attributes->erase(std::string{TRACKED_REQUEST_ATTRIBUTE});
        {
          std::lock_guard lock(requests_mu);
          --active_requests;
        }
        requests_cv.notify_all();
      });
}

auto ShutdownCoordinator::accepting_ingress() const noexcept -> bool {
  return accepting.load(std::memory_order_acquire);
}

auto ShutdownCoordinator::drain_http_requests(
    std::chrono::milliseconds timeout_in) -> bool {
  std::unique_lock lock(requests_mu);
  return requests_cv.wait_for(lock, timeout_in,
                              [this] { return active_requests == 0; });
}

auto ShutdownCoordinator::run_graph() -> ShutdownResult {
  ShutdownResult result;
  auto deadline = std::chrono::steady_clock::now() + timeout;

  auto run_bounded =
      [&](std::string name,
          const std::function<bool(std::chrono::milliseconds)>& step) {
        try {
          if (!step(remaining_until(deadline))) {
            result.clean = false;
            result.failed_step = std::move(name);
            return false;
          }
          result.completed_steps.push_back(std::move(name));
          return true;
        } catch (const std::exception& e) {
          result.clean = false;
          result.failed_step = std::move(name);
          spdlog::error("shutdown step {} failed: {}", result.failed_step,
                        e.what());
          return false;
        } catch (...) {
          result.clean = false;
          result.failed_step = std::move(name);
          spdlog::error("shutdown step {} failed with an unknown exception",
                        result.failed_step);
          return false;
        }
      };

  accepting.store(false, std::memory_order_release);
  if (!run_bounded("close_ingress", hooks.close_ingress) ||
      !run_bounded("drain_http_requests",
                   [this](std::chrono::milliseconds timeout_in) {
                     return drain_http_requests(timeout_in);
                   }) ||
      !run_bounded("stop_listeners", hooks.stop_listeners) ||
      !run_bounded("drain_async_tasks", hooks.drain_async_tasks) ||
      !run_bounded("drain_rbac_workers", hooks.drain_rbac_workers) ||
      !run_bounded("drain_extension_dispatches",
                   hooks.drain_extension_dispatches) ||
      !run_bounded("drain_js_stress_dispatches",
                   hooks.drain_js_stress_dispatches) ||
      !run_bounded("flush_database_state", hooks.flush_database_state) ||
      !run_bounded("close_audit_gate", hooks.close_audit_gate) ||
      !run_bounded("stop_drogon", hooks.stop_drogon)) {
    return result;
  }
  return result;
}

auto ShutdownCoordinator::quiesce() -> ShutdownResult {
  std::unique_lock lock(state_mu);
  while (state == State::QUIESCING || state == State::FINISHING) {
    state_cv.wait(lock);
  }
  if (state == State::QUIESCED || state == State::FINISHED) {
    return last_result;
  }
  state = State::QUIESCING;
  lock.unlock();

  auto result = run_graph();

  lock.lock();
  last_result = result;
  state = result.clean ? State::QUIESCED : State::FAILED;
  lock.unlock();
  state_cv.notify_all();
  return result;
}

auto ShutdownCoordinator::finish_after_drogon() -> void {
  std::unique_lock lock(state_mu);
  while (state == State::QUIESCING || state == State::FINISHING) {
    state_cv.wait(lock);
  }
  if (state == State::FINISHED) {
    return;
  }
  if (state != State::QUIESCED) {
    lock.unlock();
    auto result = quiesce();
    if (!result.clean) {
      return;
    }
    lock.lock();
  }
  state = State::FINISHING;
  lock.unlock();
  try {
    hooks.close_log_sinks();
  } catch (...) {
    lock.lock();
    state = State::QUIESCED;
    lock.unlock();
    state_cv.notify_all();
    throw;
  }
  lock.lock();
  state = State::FINISHED;
  lock.unlock();
  state_cv.notify_all();
}

} // namespace plinth::lifecycle
