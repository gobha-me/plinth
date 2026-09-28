#include "listener_readiness.hpp"
#include "ws_test_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>

namespace {

using plinth::ws_test::detail::ListenerReadiness;
using namespace std::chrono_literals;

class OwnedSocket {
 public:
  OwnedSocket() {
    descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
      throw std::runtime_error("readiness control could not create socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
      ::close(descriptor);
      throw std::runtime_error("readiness control could not bind socket");
    }
    socklen_t size = sizeof(address);
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address),
                      &size) != 0) {
      ::close(descriptor);
      throw std::runtime_error("readiness control could not inspect socket");
    }
    port = ntohs(address.sin_port);
  }
  ~OwnedSocket() { ::close(descriptor); }
  OwnedSocket(const OwnedSocket&) = delete;
  auto operator=(const OwnedSocket&) -> OwnedSocket& = delete;
  OwnedSocket(OwnedSocket&&) = delete;
  auto operator=(OwnedSocket&&) -> OwnedSocket& = delete;

  [[nodiscard]] auto fd() const -> int { return descriptor; }
  [[nodiscard]] auto bound_port() const -> std::uint16_t { return port; }

 private:
  int descriptor{-1};
  std::uint16_t port{0};
};

// Destroy this BEFORE its socket. Assertion unwinding still drains queued
// borrowed-FD callbacks and joins their owner before the descriptor is closed.
class OwnedLoop {
 public:
  explicit OwnedLoop(std::shared_ptr<ListenerReadiness> ready)
      : readiness(std::move(ready)) {}
  ~OwnedLoop() {
    readiness->cancel();
    auto completion = std::make_shared<std::promise<void>>();
    auto done = completion->get_future();
    // Same-producer FIFO settles every setup callback even if an assertion
    // unwinds before it has incremented the readiness pending count.
    target.getLoop()->queueInLoop([completion] { completion->set_value(); });
    target.run();
    if (done.wait_for(2s) != std::future_status::ready ||
        !readiness->drain_until(std::chrono::steady_clock::now() + 2s)) {
      std::terminate();
    }
    target.requestStop();
    if (!target.waitFor(2s)) {
      std::terminate();
    }
  }
  OwnedLoop(const OwnedLoop&) = delete;
  auto operator=(const OwnedLoop&) -> OwnedLoop& = delete;
  OwnedLoop(OwnedLoop&&) = delete;
  auto operator=(OwnedLoop&&) -> OwnedLoop& = delete;

  trantor::EventLoopThread target;

 private:
  std::shared_ptr<ListenerReadiness> readiness;
};

auto require_bounded_negative_controls() -> void {
  {
    // One successful physical socket does not satisfy two configured owners.
    OwnedSocket socket;
    auto readiness =
        std::make_shared<ListenerReadiness>(2, socket.bound_port());
    OwnedLoop owner(readiness);
    auto completed = std::make_shared<std::promise<int>>();
    auto done = completed->get_future();
    auto* loop = owner.target.getLoop();
    loop->queueInLoop([readiness, completed, loop, fd = socket.fd()] {
      readiness->before_listen(fd, loop, 0);
      completed->set_value(::listen(fd, 1));
    });
    owner.target.run();
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    REQUIRE(done.get() == 0);
    REQUIRE(readiness->drain_until(std::chrono::steady_clock::now() + 2s));
    REQUIRE(readiness->snapshot().acknowledgements == 1);
    REQUIRE_FALSE(readiness->snapshot().ready);
    REQUIRE_FALSE(readiness->wait_until(std::chrono::steady_clock::now()));
    REQUIRE(readiness->snapshot().failed);
    REQUIRE(readiness->snapshot().cancelled);
    REQUIRE_FALSE(readiness->snapshot().ready);
  }
  {
    // A queued ack on a bound-but-not-listening socket fails, not ready=true.
    OwnedSocket socket;
    auto readiness =
        std::make_shared<ListenerReadiness>(1, socket.bound_port());
    OwnedLoop owner(readiness);
    auto* loop = owner.target.getLoop();
    loop->queueInLoop([readiness, loop, fd = socket.fd()] {
      readiness->before_listen(fd, loop, 0);
    });
    owner.target.run();
    REQUIRE_FALSE(readiness->wait_until(std::chrono::steady_clock::now() + 2s));
    REQUIRE(readiness->drain_until(std::chrono::steady_clock::now() + 2s));
    REQUIRE(readiness->snapshot().failed);
    REQUIRE(readiness->snapshot().error == "listener socket is not accepting");
    REQUIRE(readiness->snapshot().acknowledgements == 0);
    REQUIRE_FALSE(readiness->snapshot().ready);
  }
  {
    // Cancellation precedes a real queued ack. Neither it nor a subsequent
    // before-listen callback can resurrect a cancelled startup.
    OwnedSocket socket;
    auto readiness =
        std::make_shared<ListenerReadiness>(1, socket.bound_port());
    OwnedLoop owner(readiness);
    auto completed = std::make_shared<std::promise<int>>();
    auto done = completed->get_future();
    auto* loop = owner.target.getLoop();
    loop->queueInLoop([readiness, completed, loop, fd = socket.fd()] {
      readiness->before_listen(fd, loop, 0);
      readiness->cancel();
      completed->set_value(::listen(fd, 1));
      readiness->before_listen(fd, loop, 0);
    });
    owner.target.run();
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    REQUIRE(done.get() == 0);
    REQUIRE(readiness->drain_until(std::chrono::steady_clock::now() + 2s));
    REQUIRE(readiness->snapshot().pending == 0);
    REQUIRE(readiness->snapshot().cancelled);
    REQUIRE(readiness->snapshot().acknowledgements == 0);
    REQUIRE_FALSE(readiness->wait_until(std::chrono::steady_clock::now()));
    REQUIRE_FALSE(readiness->snapshot().ready);
  }
}

class StartupOwner {
 public:
  StartupOwner()
      : control(std::make_shared<plinth::ws_test::ServerStartupControl>()) {
    plinth::ws_test::install_server_startup_control(control);
    auto completion = std::make_shared<std::promise<std::uint16_t>>();
    ready = completion->get_future().share();
    starter = std::thread([completion] {
      try {
        completion->set_value(plinth::ws_test::test_server_port());
      } catch (...) {
        completion->set_exception(std::current_exception());
      }
    });
  }
  ~StartupOwner() {
    control->release();
    if (starter.joinable()) {
      if (ready.wait_for(5s) != std::future_status::ready) {
        std::terminate();
      }
      starter.join();
    }
  }
  StartupOwner(const StartupOwner&) = delete;
  auto operator=(const StartupOwner&) -> StartupOwner& = delete;
  StartupOwner(StartupOwner&&) = delete;
  auto operator=(StartupOwner&&) -> StartupOwner& = delete;

  std::shared_ptr<plinth::ws_test::ServerStartupControl> control;
  std::shared_future<std::uint16_t> ready;
  std::thread starter;
};

} // namespace

TEST_CASE("WebSocket fixture waits for both owned listening sockets",
          "[ws][integration][isolated-ws-startup]") {
  // This named gate is mandatory: absent PG must fail, never SKIP green.
  REQUIRE(plinth::ws_test::pg_available());
  require_bounded_negative_controls();
  plinth::ws_test::reset_schema(plinth::ws_test::test_config().db);

  REQUIRE_FALSE(plinth::ws_test::server_startup_snapshot().thread_owned);
  StartupOwner owner;
  REQUIRE(
      owner.control->wait_held_until(std::chrono::steady_clock::now() + 5s));
  const auto held = owner.control->snapshot();
  REQUIRE(held.main_marker);
  REQUIRE(held.held_owners == 2);
  REQUIRE(held.nonaccepting_sockets == 2);
  REQUIRE_FALSE(held.failed);
  REQUIRE_FALSE(held.released);
  const auto pending = plinth::ws_test::server_startup_snapshot();
  REQUIRE(pending.thread_owned);
  // Original inline main-marker publication fails this physical-socket oracle
  // deterministically; no scheduling delay or failed-connect retry is needed.
  REQUIRE_FALSE(pending.ready);
  REQUIRE_FALSE(pending.failed);
  REQUIRE(pending.acknowledgements == 0);
  REQUIRE(pending.pending == 2);
  REQUIRE(owner.ready.wait_for(0s) == std::future_status::timeout);

  owner.control->release();
  REQUIRE(owner.ready.wait_for(5s) == std::future_status::ready);
  REQUIRE(owner.ready.get() == plinth::ws_test::test_config().listen_port);
  owner.starter.join();
  const auto listening = plinth::ws_test::server_startup_snapshot();
  REQUIRE(listening.thread_owned);
  REQUIRE(listening.ready);
  REQUIRE_FALSE(listening.failed);
  REQUIRE(listening.acknowledgements == 2);
  REQUIRE(listening.pending == 0);
  plinth::ws_test::WsTestClient client;
  REQUIRE(client.connect(2s));
}
