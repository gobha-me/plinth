// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/js/execution_lifetime.hpp"
#include "native_capability_fixture.hpp"

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace native_capability_corpus::ownership {

namespace fixture = native_capability_corpus::fixture;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Generation = std::weak_ptr<plinth::js::ExecutionLifetime>;

inline auto same_generation(const Generation& a, const Generation& b) -> bool {
  return !a.owner_before(b) && !b.owner_before(a);
}

template <typename Predicate>
auto until(Predicate&& predicate, const char* failure,
           std::chrono::milliseconds budget = 2000ms) -> void {
  const auto deadline = Clock::now() + budget;
  while (Clock::now() < deadline) {
    if (predicate()) return;
    std::this_thread::yield();
  }
  fixture::demand(false, failure);
}

struct Snapshot {
  bool visited = false;
  bool cancelled = false;
  bool cap_only = true;
  int next = 0;
  int concurrent = 0;
  int detached = 0;
  std::vector<int> callbacks;
  std::vector<int> pending;
};

inline auto fence() -> void {
  auto promise = std::make_shared<std::promise<void>>();
  auto ready = promise->get_future();
  drogon::app().getLoop()->queueInLoop([promise] { promise->set_value(); });
  fixture::demand(ready.wait_for(2s) == std::future_status::ready,
                  "ownership observer owner-loop fence timed out");
  ready.get();
}

inline auto snapshot(Generation weak) -> Snapshot {
  auto promise = std::make_shared<std::promise<Snapshot>>();
  auto ready = promise->get_future();
  drogon::app().getLoop()->queueInLoop([weak, promise] {
    try {
      Snapshot result;
      {
        auto lifetime = weak.lock();
        if (lifetime) {
          result.visited = lifetime->visit([&](plinth::js::BridgeContext& bc) {
            result.cancelled = bc.cancelled.load(std::memory_order_acquire);
            result.next = bc.next_callback_id;
            result.concurrent = bc.concurrent_async_ops;
            result.detached =
                bc.inflight_detached.load(std::memory_order_acquire);
            for (const auto& [id, callback] : bc.callbacks) {
              result.callbacks.push_back(id);
              result.cap_only =
                  result.cap_only && callback.ns_for_cancellation == "cap";
            }
            for (const auto& op : bc.pending_ops) {
              result.pending.push_back(op.callback_id);
              result.cap_only = result.cap_only &&
                                op.type == plinth::js::AsyncOp::Type::CAP_CALL;
            }
            std::ranges::sort(result.callbacks);
          });
        }
      } // No strong generation/accepted task lease survives publication.
      promise->set_value(std::move(result));
    } catch (...) {
      promise->set_exception(std::current_exception());
    }
  });
  fixture::demand(ready.wait_for(2s) == std::future_status::ready,
                  "ownership generation snapshot timed out");
  return ready.get();
}

struct Event {
  Generation generation;
  std::string extension;
  unsigned entered = 0;
  unsigned finished = 0;
  int admitted = -1;
  std::optional<plinth::js::EvalErrorKind> error = std::nullopt;
};

// This recorder owns ordinary metadata only. In particular no recorded event
// holds an ExecutionLifetime/AsyncTaskLease alive during the drain checks.
class Observations {
  struct State {
    std::mutex mutex;
    std::vector<Event> events;
    bool failed = false;
  };
  std::shared_ptr<State> state = std::make_shared<State>();
  bool closed = false;

 public:
  Observations() {
    state->events.reserve(32);
    plinth::js::set_execution_observer_for_test(
        [shared = state](
            const plinth::js::ExecutionObservation& observation) noexcept {
          try {
            int admitted = -1;
            if (observation.stage ==
                plinth::js::ExecutionObservationStage::FINISH) {
              auto lifetime = observation.execution.lock();
              if (lifetime) {
                static_cast<void>(
                    lifetime->visit([&](plinth::js::BridgeContext& bc) {
                      admitted = bc.next_callback_id;
                    }));
              }
            } // Release temporary strong generation before
              // recording/publishing.
            std::lock_guard lock(shared->mutex);
            auto found =
                std::ranges::find_if(shared->events, [&](const Event& event) {
                  return same_generation(event.generation,
                                         observation.execution);
                });
            if (found == shared->events.end()) {
              shared->events.push_back(
                  {observation.execution, observation.extension_name});
              found = std::prev(shared->events.end());
            }
            if (observation.stage ==
                plinth::js::ExecutionObservationStage::ENTER) {
              ++found->entered;
            } else {
              ++found->finished;
              found->admitted = admitted;
              found->error = observation.error_kind;
            }
          } catch (...) {
            std::lock_guard lock(shared->mutex);
            shared->failed = true;
          }
        });
  }
  Observations(const Observations&) = delete;
  auto operator=(const Observations&) -> Observations& = delete;
  ~Observations() {
    if (!closed) {
      try {
        close();
      } catch (...) {
        std::terminate();
      }
    }
  }
  auto caller() const -> Generation {
    std::lock_guard lock(state->mutex);
    for (const auto& event : state->events) {
      if (event.extension.empty() && event.entered == 1)
        return event.generation;
    }
    return {};
  }
  auto close() -> void {
    if (closed) return;
    plinth::js::set_execution_observer_for_test({});
    fence();
    closed = true;
  }
  auto events() const -> std::vector<Event> {
    std::lock_guard lock(state->mutex);
    fixture::demand(!state->failed, "ownership observation capture failed");
    return state->events;
  }
};

struct Domain {
  std::string extension;
  std::string schema;
  std::string role;
  std::string legacy;
  std::filesystem::path handlers;
  bool attempted = false;
};

// Four extension pools share one registry owner, not four Providers competing
// for the process-global registry. Each actual pool owns its private DB domain.
class Providers {
  struct Gate {
    unsigned key;
    bool held;
  };
  std::filesystem::path base;
  bool registry_owned = false;
  bool closed = false;
  std::vector<Gate> gates;
  unsigned lock_namespace = 0;

 public:
  plinth::Config config;
  fixture::Control control;
  std::array<Domain, 4> domains;

  Providers()
      : config(plinth::async_bridge_test::test_config()), control(config.db) {
    static std::atomic<unsigned> sequence{0};
    const auto serial = sequence.fetch_add(1);
    const auto process = getpid();
    fixture::demand(process > 0 && process <= 100000000 && serial < 16,
                    "ownership task gate namespace exceeds INT4 bound");
    lock_namespace = 100000000U + static_cast<unsigned>(process) * 16U + serial;
    config.db.pool_size = 1;
    const auto prefix = "capol" + std::to_string(getpid()) + "r" +
                        std::to_string(serial) + "t" +
                        std::to_string(Clock::now().time_since_epoch().count());
    base = std::filesystem::temp_directory_path() / prefix;
    fixture::demand(std::filesystem::create_directory(base),
                    "ownership scratch path already exists");
    try {
      config.packages_data_dir = base.string();
      for (std::size_t index = 0; index < domains.size(); ++index) {
        auto& domain = domains[index];
        domain.extension = prefix + "d" + std::to_string(index);
        fixture::demand(domain.extension.size() <= 54,
                        "ownership extension identity too long");
        domain.schema = "ext_" + domain.extension;
        domain.role = plinth::db::extension_role_name(config.db.database,
                                                      domain.extension);
        domain.legacy = domain.schema + "_role";
        fixture::demand(
            control.scalar("SELECT to_regnamespace('" + domain.schema +
                           "') IS NULL") == "t" &&
                control.scalar(
                    "SELECT count(*) FROM pg_roles WHERE rolname IN ('" +
                    domain.role + "','" + domain.legacy + "')") == "0" &&
                control.scalar("SELECT count(*) FROM "
                               "plinth.extension_database_credentials WHERE "
                               "extension_name='" +
                               domain.extension + "'") == "0",
            "ownership fixture refuses existing identity");
        domain.handlers =
            base / "extensions" / domain.extension / "1.0.0/server/handlers";
        std::filesystem::create_directories(domain.handlers);
        std::ofstream main_file(domain.handlers.parent_path() / "main.js");
        main_file << "// owned O/L fixture\n";
        fixture::demand(main_file.good(), "ownership main source write failed");
        main_file.close();
        std::filesystem::create_directory_symlink(
            "1.0.0", base / "extensions" / domain.extension / "active");
      }
      registry_owned = true;
      plinth::extensions::init_registry(config);
      for (auto& domain : domains) {
        fixture::demand(plinth::extensions::create_pool(domain.extension),
                        "ownership provider pool unavailable");
        domain.attempted = true;
        static_cast<void>(
            control.query("SELECT plinth.provision_extension_database('" +
                          domain.extension + "')"));
        fixture::demand(
            control.scalar(
                "SELECT role_name FROM plinth.extension_database_credentials "
                "WHERE extension_name='" +
                domain.extension + "'") == domain.role &&
                domain.role != config.db.user,
            "ownership restricted identity mismatch");
        static_cast<void>(control.query(
            "CREATE TABLE " + domain.schema +
            ".receipt(marker integer PRIMARY KEY, pid integer NOT NULL, login "
            "text NOT NULL, session_login text NOT NULL, extension text NOT "
            "NULL, caller text NOT NULL, depth integer NOT NULL, code text NOT "
            "NULL, message text NOT NULL, detail text NOT NULL)"));
        static_cast<void>(control.query("ALTER TABLE " + domain.schema +
                                        ".receipt OWNER TO \"" + domain.role +
                                        "\""));
      }
    } catch (...) {
      if (!finish()) std::terminate();
      throw;
    }
  }
  Providers(const Providers&) = delete;
  auto operator=(const Providers&) -> Providers& = delete;
  ~Providers() {
    if (!closed) {
      try {
        if (!finish()) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  auto signature(unsigned domain, std::string_view function) const
      -> std::string {
    fixture::demand(domain < domains.size(), "ownership domain index");
    return "capol" + std::to_string(domain) + ":1:" + std::string{function};
  }
  auto stage(unsigned index, std::string_view function,
             const std::string& source) -> void {
    fixture::demand(index < domains.size() && source.size() < 16384 &&
                        !function.empty() &&
                        function.find_first_not_of(
                            "abcdefghijklmnopqrstuvwxyz0123456789_") ==
                            std::string_view::npos,
                    "ownership handler source/name bound");
    std::ofstream file(domains[index].handlers /
                       (std::string{function} + ".js"));
    file << source;
    fixture::demand(file.good(), "ownership handler source write failed");
    plinth::capabilities::CachedCapability entry{
        .signature = signature(index, function),
        .provider_type = "extension",
        .extension_name = domains[index].extension,
        .scope = "instance",
        .user_id = {},
        .rbac_rule = "capnative.call",
        .enabled = true};
    plinth::capabilities::seed_tier2_cache_for_test(entry);
  }
  auto write(unsigned index, unsigned marker, bool evidence = false) const
      -> std::string {
    fixture::demand(index < domains.size() && marker < 100000,
                    "ownership receipt identity bound");
    return "const receipt=await db.exec('INSERT INTO " + domains[index].schema +
           ".receipt VALUES (" + std::to_string(marker) +
           ",pg_backend_pid(),current_user,session_user,$1,$2,($3::text)::"
           "integer,$4,$5,$6)',[ctx.extension,ctx.user.id,String(ctx.call_"
           "depth)," +
           (evidence ? "args.code,args.message,args.detail" : "'','',''") +
           "],{silent:true});if(receipt.row_count!==1)throw new "
           "Error('ownership receipt acknowledgement');";
  }
  auto gate_sql(unsigned key) const -> std::string {
    return "SELECT pg_advisory_xact_lock(" + std::to_string(lock_namespace) +
           "," + std::to_string(key) + ")";
  }
  auto hold(unsigned key) -> void {
    fixture::demand(
        std::ranges::none_of(gates,
                             [&](const Gate& gate) { return gate.key == key; }),
        "ownership duplicate gate key");
    gates.push_back({key, false});
    fixture::demand(control.scalar("SELECT pg_try_advisory_lock(" +
                                   std::to_string(lock_namespace) + "," +
                                   std::to_string(key) + ")") == "t",
                    "ownership gate collision");
    gates.back().held = true;
  }
  auto unlock(unsigned key) -> void {
    const auto found = std::ranges::find_if(
        gates, [&](const Gate& gate) { return gate.key == key; });
    fixture::demand(found != gates.end() && found->held,
                    "ownership unlock without owned gate");
    fixture::demand(control.scalar("SELECT pg_advisory_unlock(" +
                                   std::to_string(lock_namespace) + "," +
                                   std::to_string(key) + ")") == "t",
                    "ownership unlock acknowledgement missing");
    found->held = false;
  }
  auto unlock_owned() -> void {
    for (auto& gate : gates) {
      if (gate.held) unlock(gate.key);
    }
  }
  auto waiter(unsigned index, unsigned key) -> bool {
    return control.scalar(
               "SELECT count(*) FROM pg_locks l JOIN pg_stat_activity a ON "
               "a.pid=l.pid WHERE l.locktype='advisory' AND NOT l.granted AND "
               "l.classid=" +
               std::to_string(lock_namespace) +
               " AND l.objid=" + std::to_string(key) +
               " AND l.objsubid=2 AND a.usename='" + domains[index].role +
               "' AND a.query LIKE '%" + gate_sql(key) + "%'") == "1";
  }
  auto entries(unsigned index, unsigned marker) -> unsigned {
    return static_cast<unsigned>(std::stoul(control.scalar(
        "SELECT count(*) FROM " + domains[index].schema +
        ".receipt WHERE marker=" + std::to_string(marker) + " AND login='" +
        domains[index].role + "' AND session_login=login AND extension='" +
        domains[index].extension +
        "' AND caller='11111111-1111-1111-1111-111111111187' AND depth=1")));
  }
  auto detail(unsigned index, unsigned marker, std::string_view field)
      -> std::string {
    fixture::demand(field == "code" || field == "message" || field == "detail",
                    "ownership receipt field");
    return control.scalar("SELECT " + std::string{field} + " FROM " +
                          domains[index].schema +
                          ".receipt WHERE marker=" + std::to_string(marker));
  }
  auto total() -> unsigned {
    unsigned result = 0;
    for (const auto& domain : domains)
      result += static_cast<unsigned>(std::stoul(control.scalar(
          "SELECT count(*) FROM " + domain.schema + ".receipt")));
    return result;
  }
  auto finish() -> bool {
    if (closed) return true;
    unlock_owned();
    if (!plinth::lifecycle::async_tasks().drain(5s) ||
        !plinth::log::wait_for_audits(5s))
      return false;
    if (registry_owned) {
      if (!plinth::extensions::shutdown_registry(5s)) return false;
      registry_owned = false;
      plinth::capabilities::clear_resolver_for_test();
    }
    for (auto& domain : domains) {
      if (!domain.attempted) continue;
      if (!plinth::packages::drop_schema_and_migrations(domain.extension,
                                                        control.raw()))
        return false;
      fixture::demand(
          control.scalar("SELECT to_regnamespace('" + domain.schema +
                         "') IS NULL") == "t" &&
              control.scalar(
                  "SELECT count(*) FROM plinth.extension_database_credentials "
                  "WHERE extension_name='" +
                  domain.extension + "'") == "0" &&
              control.scalar(
                  "SELECT count(*) FROM pg_roles WHERE rolname IN ('" +
                  domain.role + "','" + domain.legacy + "')") == "0",
          "ownership provider identity cleanup incomplete");
      domain.attempted = false;
    }
    std::filesystem::remove_all(base);
    closed = !std::filesystem::exists(base);
    return closed;
  }
};

// The context, pool, immutable source and promise are retained through this
// joined worker. Unwind releases physical gates BEFORE waiting for completion;
// no destructor can silently detach or destroy a running JavaScript owner.
class Run {
  Providers& providers;
  std::promise<plinth::js::EvalResult> promise;
  std::future<plinth::js::EvalResult> ready = promise.get_future();
  std::thread worker;

 public:
  Run(Providers& owners, plinth::js::BridgeContext& bc, std::string source)
      : providers(owners),
        worker([&bc, owned_source = std::move(source), this] {
          try {
            promise.set_value(drogon::sync_wait(
                plinth::js::run_on_context(bc, owned_source)));
          } catch (...) {
            promise.set_exception(std::current_exception());
          }
        }) {}
  Run(const Run&) = delete;
  auto operator=(const Run&) -> Run& = delete;
  ~Run() {
    if (worker.joinable()) {
      try {
        providers.unlock_owned();
        if (ready.wait_for(12s) != std::future_status::ready) std::terminate();
        worker.join();
      } catch (...) {
        std::terminate();
      }
    }
  }
  auto finish(std::chrono::milliseconds budget = 8000ms)
      -> plinth::js::EvalResult {
    fixture::demand(ready.wait_for(budget) == std::future_status::ready,
                    "ownership driver completion timed out");
    worker.join();
    return ready.get();
  }
};

inline auto pristine(const plinth::js::BridgeContext& bc,
                     bool abandoned = false) -> bool {
  const auto& batch = bc.batch_state;
  return !bc.execution_lifetime.load(std::memory_order_acquire) &&
         bc.pending_ops.empty() && bc.retired_pending_ops.empty() &&
         bc.callbacks.empty() && bc.persistent_callbacks.empty() &&
         bc.concurrent_async_ops == 0 &&
         (abandoned
              ? bc.inflight_detached.load(std::memory_order_acquire) >= 0
              : bc.inflight_detached.load(std::memory_order_acquire) == 0) &&
         batch.depth == 0 && batch.scope_id == 0 && !batch.pinned_conn &&
         !batch.rollback_owner && batch.timer_id == trantor::InvalidTimerId &&
         batch.timer_loop == nullptr;
}

class Caller {
  plinth::Config config;
  bool closed = false;

 public:
  plinth::js::RuntimePool pool;
  plinth::js::BridgeContext* context = nullptr;
  explicit Caller(const plinth::Config& input, int fanout = 8)
      : config(input), pool(nullptr, profile(fanout), config, 1, &identity()) {
    context = pool.acquire();
    fixture::demand(context != nullptr, "ownership caller unavailable");
  }
  Caller(const Caller&) = delete;
  auto operator=(const Caller&) -> Caller& = delete;
  ~Caller() {
    if (!closed) {
      try {
        if (!fixture::stopped(finish())) std::terminate();
      } catch (...) {
        std::terminate();
      }
    }
  }
  static auto identity() -> const plinth::capabilities::UserContext& {
    static const auto value = fixture::user();
    return value;
  }
  static auto profile(int fanout) -> plinth::js::RuntimeLimits {
    auto limits = plinth::js::default_runtime_limits();
    limits.max_concurrent_async_ops = fanout;
    return limits;
  }
  auto destroy() -> void {
    fixture::demand(context != nullptr && !context->execution_lifetime.load(
                                              std::memory_order_acquire),
                    "ownership destroy attempted before retirement");
    pool.destroy(context);
    context = nullptr;
    fixture::demand(pool.active_count() == 0,
                    "ownership destroy did not return the lease");
  }
  auto acquire() -> void {
    fixture::demand(context == nullptr, "ownership acquire with held context");
    context = pool.acquire();
    fixture::demand(context != nullptr,
                    "ownership replacement caller unavailable");
  }
  auto finish() -> fixture::Shutdown {
    if (context != nullptr) destroy();
    const bool first = pool.shutdown(5s);
    const bool second = pool.shutdown(0ms);
    fixture::Shutdown result{first, second, pool.active_count(),
                             pool.free_count()};
    closed = fixture::stopped(result);
    return result;
  }
};

inline auto invocation(const Providers& providers, unsigned domain,
                       std::string_view function,
                       std::string_view argument = "null") -> std::string {
  return "cap.call('" + providers.signature(domain, function) + "'," +
         std::string{argument} + ")";
}

} // namespace native_capability_corpus::ownership
