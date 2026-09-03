#include "kernel/ws/js_stress.hpp"

#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/run_on_context.hpp"
#include "kernel/js/runtime_pool.hpp"
#include "kernel/ws/messages.hpp"

#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::ws {

namespace {

constexpr std::string_view JS_STRESS_SIG = "lh0:1:js_stress";

// process-lifetime singleton guarded by g_mu; lifecycle owned by
// init_js_stress_pool/shutdown_js_stress_pool.
std::mutex g_mu;
std::condition_variable g_drained;
std::shared_ptr<plinth::js::RuntimePool> g_pool;
std::size_t g_inflight = 0;
bool g_accepting = false;

class DispatchLease {
 public:
  explicit DispatchLease(std::shared_ptr<plinth::js::RuntimePool> pool_in)
      : pool(std::move(pool_in)) {}
  DispatchLease(const DispatchLease&) = delete;
  DispatchLease(DispatchLease&&) = delete;
  auto operator=(const DispatchLease&) -> DispatchLease& = delete;
  auto operator=(DispatchLease&&) -> DispatchLease& = delete;
  ~DispatchLease() {
    // Destroy the task's last possible pool reference before publishing the
    // drained count. A successful shutdown wait therefore means pool teardown
    // is also complete, not merely that the coroutine reached its last line.
    pool.reset();
    {
      std::lock_guard lock(g_mu);
      --g_inflight;
    }
    g_drained.notify_all();
  }

  std::shared_ptr<plinth::js::RuntimePool> pool;
};

auto make_call_result(const std::string& id, const Json::Value& value)
    -> Json::Value {
  Json::Value v;
  v["type"] = msg::CALL_RESULT;
  v["id"] = id;
  v["value"] = value;
  v["resolved_tier"] = "tier1";
  v["provider_type"] = "kernel";
  return v;
}

auto make_call_error(const std::string& id, std::string_view code,
                     std::string_view message) -> Json::Value {
  Json::Value v;
  v["type"] = msg::CALL_ERROR;
  v["id"] = id;
  v["code"] = std::string{code};
  v["message"] = std::string{message};
  return v;
}

auto send_if_connected(const drogon::WebSocketConnectionPtr& conn,
                       const Json::Value& frame) -> void {
  if (conn && conn->connected()) {
    conn->sendJson(frame);
  }
}

// Catch-all per ICD-LH-0.1 §3 — the EvalErrorKind is carried in the
// message field, not the code. Callers today (harness + tests) don't
// branch on kind; richer taxonomy lands if a caller needs it.
auto eval_error_code(plinth::js::EvalErrorKind /*kind*/) -> std::string_view {
  return "js_eval_error";
}

} // namespace

auto init_js_stress_pool(const plinth::Config& cfg) -> void {
  std::lock_guard<std::mutex> g{g_mu};
  if (g_pool) {
    return;
  }
  g_pool = std::make_shared<plinth::js::RuntimePool>(
      /*ext=*/nullptr, plinth::js::default_runtime_limits(), cfg,
      /*pool_size=*/-1, /*user=*/nullptr);
  g_accepting = true;
  spdlog::info("ws::js_stress: pool initialized");
}

auto shutdown_js_stress_pool(std::chrono::milliseconds timeout) -> bool {
  std::shared_ptr<plinth::js::RuntimePool> local;
  {
    std::lock_guard<std::mutex> g{g_mu};
    g_accepting = false;
    local = std::move(g_pool);
  }
  // Destroy outside the lock — pool teardown pumps pending JS jobs
  // which may reenter Drogon callbacks; keeping the mutex free
  // avoids any deadlock if a racing dispatch observes null g_pool
  // and returns before we finish destroying.
  local.reset();

  std::unique_lock lock(g_mu);
  bool drained =
      g_drained.wait_for(lock, timeout, [] { return g_inflight == 0; });
  auto remaining = g_inflight;
  lock.unlock();
  if (drained) {
    spdlog::info("ws::js_stress: pool shut down");
  } else {
    spdlog::error("ws::js_stress: shutdown timed out with {} dispatch(es)",
                  remaining);
  }
  return drained;
}

auto js_stress_inflight_count_for_test() -> std::size_t {
  std::lock_guard lock(g_mu);
  return g_inflight;
}

auto try_dispatch_js_stress(const drogon::WebSocketConnectionPtr& conn,
                            const std::string& call_id,
                            const std::string& signature,
                            const Json::Value& args, bool caller_is_admin)
    -> bool {
  if (signature != JS_STRESS_SIG) {
    return false;
  }

  if (!caller_is_admin) {
    send_if_connected(conn, make_call_error(call_id, "permission_denied",
                                            "kernel.admin required"));
    return true;
  }

  if (!args.isArray() || args.empty() || !args[0].isString()) {
    send_if_connected(conn,
                      make_call_error(call_id, "invalid_call",
                                      "js_stress: args[0] must be string"));
    return true;
  }

  std::string script = args[0].asString();
  std::weak_ptr<drogon::WebSocketConnection> weak_conn = conn;

  std::shared_ptr<DispatchLease> lease;
  {
    std::lock_guard lock(g_mu);
    if (!g_accepting || !g_pool) {
      send_if_connected(conn, make_call_error(call_id, "internal_error",
                                              "js_stress pool shut down"));
      return true;
    }
    lease = std::make_shared<DispatchLease>(g_pool);
    ++g_inflight;
  }

  // captures are owned values: weak_conn is a non-owning weak_ptr, call_id is a
  // string copy, script is moved, and lease keeps the pool alive while also
  // publishing completion to shutdown. No references to caller frame.
  drogon::async_run([weak_conn, call_id, lease,
                     script = std::move(script)]() -> drogon::Task<> {
    plinth::js::BridgeContext* bc = lease->pool->acquire();
    if (bc == nullptr) {
      auto locked = weak_conn.lock();
      send_if_connected(locked, make_call_error(call_id, "resource_exhausted",
                                                "js_stress pool saturated"));
      co_return;
    }

    auto result = co_await plinth::js::run_on_context(*bc, script);

    auto locked = weak_conn.lock();
    if (result.value.has_value()) {
      send_if_connected(locked, make_call_result(call_id, *result.value));
      lease->pool->release(bc);
    } else {
      const auto& err = result.value.error();
      send_if_connected(
          locked,
          make_call_error(call_id, eval_error_code(err.kind), err.message));
      lease->pool->destroy(bc);
    }
  });

  return true;
}

} // namespace plinth::ws
