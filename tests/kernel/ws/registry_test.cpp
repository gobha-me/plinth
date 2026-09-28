#include "kernel/lifecycle/shutdown_coordinator.hpp"
#include "kernel/ws/connection_registry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <trantor/net/EventLoopThread.h>
#include <utility>

using plinth::ws::ConnectionRegistry;
using plinth::ws::RegistryKey;

// Pure-unit tests of the registry's map semantics (no Drogon required).
// End-to-end displacement behavior is covered in auth_test.cpp.

namespace {

// A fake connection pointer whose identity matters (nullptr for simplicity
// would collapse into a shared "not set" value; using distinct int pointers
// gives us comparable identities without a real WebSocketConnection).
auto fake_conn(int id) -> drogon::WebSocketConnectionPtr {
  // Construct a shared_ptr whose identity is the bit pattern of `id` cast
  // to a pointer — gives the registry test distinct comparable identities
  // without a real WebSocketConnection. The no-op deleter is intentional;
  // there is no storage to free.
  auto* raw = reinterpret_cast<drogon::WebSocketConnection*>(
      static_cast<std::uintptr_t>(id));
  return {raw, [](drogon::WebSocketConnection*) {}};
}

struct CloseObservation {
  std::atomic<unsigned> connected_calls{0};
  std::atomic<unsigned> shutdown_calls{0};
  std::atomic<unsigned> off_loop_calls{0};
  std::atomic<bool> disconnected{false};
  std::atomic<bool> destroyed{false};
  std::atomic<bool> destroyed_on_loop{false};
  drogon::CloseCode code{drogon::CloseCode::kNone};
  std::string reason;
  std::weak_ptr<drogon::WebSocketConnection> connection;
  std::weak_ptr<plinth::ws::ConnState> state;
};

// Real loop scheduling, with an atomic affinity oracle rather than an unsafe
// status read in the test double. Restoring the original inline close fails
// deterministically even on a machine where TSan cannot start.
class AffinityConnection final : public drogon::WebSocketConnection {
 public:
  AffinityConnection(trantor::EventLoop* owner_loop,
                     std::shared_ptr<CloseObservation> observation,
                     bool fail_close)
      : loop(owner_loop), observed(std::move(observation)),
        throws_on_close(fail_close) {}

  ~AffinityConnection() override {
    observed->destroyed_on_loop.store(loop->isInLoopThread());
    observed->destroyed.store(true);
  }

  auto connected() const -> bool override {
    observed->connected_calls.fetch_add(1);
    note_affinity();
    return !observed->disconnected.load();
  }
  auto disconnected() const -> bool override {
    note_affinity();
    return observed->disconnected.load();
  }
  auto shutdown(drogon::CloseCode code, const std::string& reason)
      -> void override {
    note_affinity();
    observed->shutdown_calls.fetch_add(1);
    if (throws_on_close) {
      throw std::runtime_error("owned close failure");
    }
    observed->code = code;
    observed->reason = reason;
  }
  auto send(const char*, std::uint64_t, drogon::WebSocketMessageType)
      -> void override {}
  auto send(std::string_view, drogon::WebSocketMessageType) -> void override {}
  auto sendJson(const Json::Value&, drogon::WebSocketMessageType)
      -> void override {}
  auto localAddr() const -> const trantor::InetAddress& override {
    return address;
  }
  auto peerAddr() const -> const trantor::InetAddress& override {
    return address;
  }
  auto forceClose() -> void override {
    note_affinity();
    observed->disconnected.store(true);
  }
  auto setPingMessage(const std::string&, const std::chrono::duration<double>&)
      -> void override {}
  auto disablePing() -> void override {}

 private:
  auto note_affinity() const -> void {
    if (!loop->isInLoopThread()) {
      observed->off_loop_calls.fetch_add(1);
    }
  }

  trantor::EventLoop* loop;
  std::shared_ptr<CloseObservation> observed;
  bool throws_on_close;
  trantor::InetAddress address;
};

class CloseFixture {
 public:
  ~CloseFixture() {
    // Assertion unwinding must start an intentionally withheld loop and join
    // all queued owners before either the registry or the loop is destroyed.
    target.run();
    if (!registry->release_connections(std::chrono::seconds{2})) {
      std::terminate();
    }
    target.requestStop();
    if (!target.waitFor(std::chrono::seconds{2})) {
      std::terminate();
    }
  }

  auto install(bool published_loop = true, bool fail_close = false)
      -> std::shared_ptr<CloseObservation> {
    auto observed = std::make_shared<CloseObservation>();
    auto conn = std::make_shared<AffinityConnection>(target.getLoop(), observed,
                                                     fail_close);
    auto state = std::make_shared<plinth::ws::ConnState>();
    state->loop = published_loop ? target.getLoop() : nullptr;
    conn->setContext(state);
    observed->connection = conn;
    observed->state = state;
    registry->register_connection(key, conn, state);
    return observed;
  }

  auto barrier() -> void {
    target.run();
    auto done = std::make_shared<std::promise<void>>();
    auto completed = done->get_future();
    target.getLoop()->queueInLoop([done] { done->set_value(); });
    REQUIRE(completed.wait_for(std::chrono::seconds{2}) ==
            std::future_status::ready);
    completed.get();
  }

  trantor::EventLoopThread target;
  std::shared_ptr<ConnectionRegistry> registry =
      std::make_shared<ConnectionRegistry>();
  const RegistryKey key{.auth_type = "session", .id = "close-owner"};
};

} // namespace

TEST_CASE("RegistryKey equality", "[ws][registry]") {
  RegistryKey a{.auth_type = "session", .id = "abc"};
  RegistryKey b{.auth_type = "session", .id = "abc"};
  RegistryKey c{.auth_type = "pat", .id = "abc"};
  REQUIRE(a == b);
  REQUIRE_FALSE(a == c);
}

TEST_CASE("register_connection on empty slot installs", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn = fake_conn(1);
  auto prior = reg.register_connection({.auth_type = "session", .id = "s1"},
                                       conn, nullptr);
  REQUIRE(prior.conn == nullptr);
  REQUIRE(reg.size() == 1);
}

TEST_CASE("register_connection on occupied slot returns displaced",
          "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn1 = fake_conn(1);
  auto conn2 = fake_conn(2);
  REQUIRE(reg.register_connection({.auth_type = "session", .id = "s1"}, conn1,
                                  nullptr)
              .conn == nullptr);
  auto displaced = reg.register_connection({.auth_type = "session", .id = "s1"},
                                           conn2, nullptr);
  REQUIRE(displaced.conn == conn1);
  REQUIRE(reg.size() == 1);
}

TEST_CASE("unregister_connection ignores stale entries", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn1 = fake_conn(1);
  auto conn2 = fake_conn(2);
  reg.register_connection({.auth_type = "session", .id = "s1"}, conn1, nullptr);
  reg.register_connection({.auth_type = "session", .id = "s1"}, conn2,
                          nullptr); // conn1 displaced

  // conn1 disconnects after being displaced — must NOT remove conn2.
  reg.unregister_connection({.auth_type = "session", .id = "s1"}, conn1);
  REQUIRE(reg.size() == 1);

  // conn2 disconnects normally — should be removed.
  reg.unregister_connection({.auth_type = "session", .id = "s1"}, conn2);
  REQUIRE(reg.size() == 0);
}

TEST_CASE("session and PAT keys with same id are distinct", "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn_s = fake_conn(1);
  auto conn_p = fake_conn(2);
  REQUIRE(reg.register_connection({.auth_type = "session", .id = "shared-id"},
                                  conn_s, nullptr)
              .conn == nullptr);
  REQUIRE(reg.register_connection({.auth_type = "pat", .id = "shared-id"},
                                  conn_p, nullptr)
              .conn == nullptr);
  REQUIRE(reg.size() == 2);
}

TEST_CASE("for_each sees all registered connections", "[ws][registry]") {
  ConnectionRegistry reg;
  reg.register_connection({.auth_type = "session", .id = "s1"}, fake_conn(1),
                          nullptr);
  reg.register_connection({.auth_type = "session", .id = "s2"}, fake_conn(2),
                          nullptr);
  reg.register_connection({.auth_type = "pat", .id = "p1"}, fake_conn(3),
                          nullptr);

  int count = 0;
  reg.for_each([&count](const drogon::WebSocketConnectionPtr&) { ++count; });
  REQUIRE(count == 3);
}

TEST_CASE(
    "displacement retains target metadata after concurrent context release",
    "[ws][registry]") {
  ConnectionRegistry reg;
  trantor::EventLoopThread target;
  target.run();
  const RegistryKey key{.auth_type = "session", .id = "displacement-race"};
  auto first = fake_conn(1);
  auto second = fake_conn(2);
  auto context = std::make_shared<plinth::ws::ConnState>();
  context->loop = target.getLoop();
  std::weak_ptr<plinth::ws::ConnState> lifetime = context;
  reg.register_connection(key, first, context);

  // Hold the displaced caller back until the old connection's close path has
  // unregistered and released its context on another thread. The registry
  // entry returned by replacement must be sufficient to queue the close.
  std::latch replaced{1};
  std::jthread closed([&, context = std::move(context)]() mutable {
    replaced.wait();
    reg.unregister_connection(key, first);
    context.reset();
  });
  auto displaced = reg.register_connection(key, second, nullptr);
  replaced.count_down();
  closed.join();
  REQUIRE(reg.size() == 1);
  REQUIRE(displaced.conn == first);
  REQUIRE(displaced.loop == target.getLoop());
  REQUIRE_FALSE(lifetime.expired());

  auto acknowledged = std::make_shared<std::promise<bool>>();
  auto done = acknowledged->get_future();
  displaced.loop->queueInLoop([entry = std::move(displaced), acknowledged]() {
    acknowledged->set_value(entry.state != nullptr &&
                            trantor::EventLoop::getEventLoopOfCurrentThread() ==
                                entry.loop);
  });
  REQUIRE(done.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
  REQUIRE(done.get());
  target.getLoop()->quit();
}

TEST_CASE(
    "registry releases owners on their loop before acknowledging shutdown",
    "[ws][registry]") {
  trantor::EventLoopThread thread;
  auto* loop = thread.getLoop();
  ConnectionRegistry reg;
  auto released = std::make_shared<std::atomic<bool>>(false);
  auto on_owner_loop = std::make_shared<std::atomic<bool>>(false);
  // This identity-only pointer has a tracking deleter; never dereference it.
  auto* raw = reinterpret_cast<drogon::WebSocketConnection*>(std::uintptr_t{1});
  drogon::WebSocketConnectionPtr conn{
      raw, [released, on_owner_loop, loop](drogon::WebSocketConnection*) {
        on_owner_loop->store(loop->isInLoopThread());
        released->store(true);
      }};
  auto state = std::make_shared<plinth::ws::ConnState>();
  state->loop = loop;
  std::weak_ptr<plinth::ws::ConnState> state_owner = state;
  reg.register_connection({.auth_type = "session", .id = "owned"}, conn, state);
  conn.reset();
  state.reset();

  // The loop has not started. Both attempts must retain the pending release,
  // although the map is already empty after the first attempt.
  CHECK_FALSE(reg.release_connections(std::chrono::milliseconds{1}));
  CHECK_FALSE(reg.release_connections(std::chrono::milliseconds{1}));
  CHECK_FALSE(released->load());
  CHECK_FALSE(state_owner.expired());
  auto late = fake_conn(2);
  reg.register_connection({.auth_type = "session", .id = "late"}, late,
                          nullptr);
  CHECK(reg.size() == 0);

  thread.run();
  CHECK(reg.release_connections(std::chrono::seconds{5}));
  CHECK(released->load());
  CHECK(on_owner_loop->load());
  CHECK(state_owner.expired());
  CHECK(reg.release_connections(std::chrono::milliseconds{0}));
  loop->quit();
  thread.wait();
}

TEST_CASE("registry retains owners if their loop was not published",
          "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn = fake_conn(1);
  reg.register_connection({.auth_type = "session", .id = "invalid"}, conn,
                          nullptr);
  CHECK_FALSE(reg.release_connections(std::chrono::milliseconds{1}));
  CHECK(reg.size() == 1);
  CHECK(conn.use_count() == 2);
}

TEST_CASE("registry closes connections on owner loops with bounded retry",
          "[ws][registry][close-owner]") {
  using namespace std::chrono_literals;
  CloseFixture fixture;

  SECTION("withheld loop retains one close batch through timeout and retry") {
    auto observed = fixture.install();
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK(observed->connected_calls.load() == 0);
    CHECK(observed->shutdown_calls.load() == 0);
    CHECK(observed->off_loop_calls.load() == 0);
    CHECK_FALSE(observed->connection.expired());
    CHECK_FALSE(observed->state.expired());
    CHECK_FALSE(observed->destroyed.load());

    // Sealing and snapshotting are one admission transaction; no late entry
    // may bypass the retained close batch.
    fixture.registry->register_connection(
        {.auth_type = "session", .id = "late"}, fake_conn(2), nullptr);
    CHECK(fixture.registry->size() == 1);
    fixture.barrier();
    CHECK(fixture.registry->close_all_connections(2s));
    CHECK(fixture.registry->close_all_connections(0ms));
    CHECK(observed->connected_calls.load() == 1);
    CHECK(observed->shutdown_calls.load() == 1);
    CHECK(observed->off_loop_calls.load() == 0);
    CHECK(observed->code == drogon::CloseCode::kEndpointGone);
    CHECK(observed->reason == "server shutdown");
    CHECK_FALSE(observed->destroyed.load());
    CHECK(fixture.registry->release_connections(2s));
    CHECK(observed->connection.expired());
    CHECK(observed->state.expired());
    CHECK(observed->destroyed.load());
    CHECK(observed->destroyed_on_loop.load());
  }

  SECTION("concurrent unregister leaves last callback owners on their loop") {
    auto observed = fixture.install();
    auto conn = observed->connection.lock();
    REQUIRE(conn);
    // This callback is queued first, but cannot run until after close freezes
    // its owned entry. It models peer EOF, unregister and context release.
    fixture.target.getLoop()->queueInLoop(
        [conn = std::move(conn), registry = fixture.registry, key = fixture.key,
         observed]() mutable {
          observed->disconnected.store(true);
          conn->clearContext();
          registry->unregister_connection(key, conn);
          conn.reset();
        });
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK_FALSE(observed->connection.expired());
    CHECK_FALSE(observed->state.expired());
    fixture.barrier();
    REQUIRE(fixture.registry->close_all_connections(2s));
    CHECK(fixture.registry->size() == 0);
    CHECK(observed->connected_calls.load() == 1);
    CHECK(observed->shutdown_calls.load() == 0);
    CHECK(observed->off_loop_calls.load() == 0);
    CHECK(observed->connection.expired());
    CHECK(observed->state.expired());
    CHECK(observed->destroyed.load());
    CHECK(observed->destroyed_on_loop.load());
    CHECK(fixture.registry->close_all_connections(0ms));
  }

  SECTION("missing loop fails closed without accessing or dropping owners") {
    auto observed = fixture.install(false);
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    CHECK(fixture.registry->size() == 1);
    CHECK(observed->connected_calls.load() == 0);
    CHECK(observed->shutdown_calls.load() == 0);
    CHECK(observed->off_loop_calls.load() == 0);
    CHECK_FALSE(observed->connection.expired());
    CHECK_FALSE(observed->state.expired());

    // This deliberately invalid registry entry cannot use normal release.
    // The test knows the fake's real owner and removes it there explicitly.
    auto conn = observed->connection.lock();
    fixture.target.getLoop()->queueInLoop([conn = std::move(conn),
                                           registry = fixture.registry,
                                           key = fixture.key]() mutable {
      registry->unregister_connection(key, conn);
      conn->clearContext();
      conn.reset();
    });
    fixture.barrier();
    CHECK(observed->connection.expired());
    CHECK(observed->state.expired());
    CHECK(observed->destroyed_on_loop.load());
  }

  SECTION("ready exception remains a reported failure on every retry") {
    auto observed = fixture.install(true, true);
    CHECK_FALSE(fixture.registry->close_all_connections(1ms));
    fixture.barrier();
    CHECK_THROWS_WITH(fixture.registry->close_all_connections(2s),
                      "owned close failure");
    CHECK_THROWS_WITH(fixture.registry->close_all_connections(0ms),
                      "owned close failure");
    plinth::lifecycle::ShutdownHooks hooks;
    hooks.close_ingress =
        [registry = fixture.registry](std::chrono::milliseconds timeout) {
          return registry->close_all_connections(timeout);
        };
    plinth::lifecycle::ShutdownCoordinator coordinator{std::move(hooks), 20ms};
    auto failed = coordinator.quiesce();
    CHECK_FALSE(failed.clean);
    CHECK(failed.failed_step == "close_ingress");
    CHECK_FALSE(coordinator.quiesce().clean);
    CHECK(observed->connected_calls.load() == 1);
    CHECK(observed->shutdown_calls.load() == 1);
    CHECK(observed->off_loop_calls.load() == 0);
    CHECK_FALSE(observed->connection.expired());
    CHECK(fixture.registry->release_connections(2s));
    CHECK(observed->connection.expired());
    CHECK(observed->state.expired());
    CHECK(observed->destroyed_on_loop.load());
  }

  SECTION("coordinator cannot advance through an unacknowledged close") {
    auto observed = fixture.install();
    unsigned downstream_calls = 0;
    bool logging_closed = false;
    auto downstream = [&downstream_calls](std::chrono::milliseconds) {
      ++downstream_calls;
      return true;
    };
    plinth::lifecycle::ShutdownHooks hooks{
        .close_ingress =
            [registry = fixture.registry](std::chrono::milliseconds timeout) {
              return registry->close_all_connections(timeout);
            },
        .stop_listeners = downstream,
        .drain_async_tasks = downstream,
        .drain_rbac_workers = downstream,
        .drain_extension_dispatches = downstream,
        .drain_js_stress_dispatches = downstream,
        .flush_database_state = downstream,
        .close_audit_gate = downstream,
        .stop_drogon = downstream,
        .close_log_sinks = [&logging_closed] { logging_closed = true; },
    };
    plinth::lifecycle::ShutdownCoordinator coordinator{std::move(hooks), 20ms};
    auto failed = coordinator.quiesce();
    CHECK_FALSE(failed.clean);
    CHECK(failed.failed_step == "close_ingress");
    CHECK_FALSE(coordinator.accepting_ingress());
    CHECK(downstream_calls == 0);
    CHECK_FALSE(logging_closed);
    CHECK_FALSE(observed->destroyed.load());
    fixture.barrier();
    REQUIRE(coordinator.quiesce().clean);
    CHECK(downstream_calls == 8);
    coordinator.finish_after_drogon();
    CHECK(logging_closed);
    CHECK(observed->shutdown_calls.load() == 1);
    CHECK(observed->off_loop_calls.load() == 0);
  }
}
