#include "kernel/ws/connection_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <trantor/net/EventLoopThread.h>

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
  REQUIRE(prior == nullptr);
  REQUIRE(reg.size() == 1);
}

TEST_CASE("register_connection on occupied slot returns displaced",
          "[ws][registry]") {
  ConnectionRegistry reg;
  auto conn1 = fake_conn(1);
  auto conn2 = fake_conn(2);
  REQUIRE(reg.register_connection({.auth_type = "session", .id = "s1"}, conn1,
                                  nullptr) == nullptr);
  auto displaced = reg.register_connection({.auth_type = "session", .id = "s1"},
                                           conn2, nullptr);
  REQUIRE(displaced == conn1);
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
                                  conn_s, nullptr) == nullptr);
  REQUIRE(reg.register_connection({.auth_type = "pat", .id = "shared-id"},
                                  conn_p, nullptr) == nullptr);
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
