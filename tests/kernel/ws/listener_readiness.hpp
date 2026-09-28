#pragma once

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <trantor/net/EventLoop.h>
#include <utility>

namespace plinth::ws_test::detail {

struct ListenerReadinessSnapshot {
  bool ready{false};
  bool failed{false};
  bool cancelled{false};
  std::size_t acknowledgements{0};
  std::size_t pending{0};
  std::string error;
};

// Test-only listen acknowledgement. Acceptor invokes before_listen() on its
// owner immediately before listen(). queueInLoop (not runInLoop) therefore
// observes the socket only after that synchronous listen() has completed.
// The fixture drains these callbacks BEFORE coordinator/listener teardown:
// fd is borrowed, never closed here, and cannot be recycled under an ack.
class ListenerReadiness final
    : public std::enable_shared_from_this<ListenerReadiness> {
 public:
  ListenerReadiness(std::size_t owner_count, std::uint16_t expected_port)
      : expected_owners(owner_count), port(expected_port) {}

  auto before_listen(int fd, trantor::EventLoop* owner, std::size_t index)
      -> void {
    if (owner == nullptr || !owner->isInLoopThread() ||
        index >= expected_owners) {
      fail("invalid listener owner");
      return;
    }
    {
      std::lock_guard lock(mu);
      if (cancelled) {
        return;
      }
      if (!seen_indices.insert(index).second ||
          !seen_loops.insert(owner).second) {
        fail_locked("duplicate listener owner");
        return;
      }
      ++pending;
    }
    auto self = shared_from_this();
    try {
      owner->queueInLoop([self, fd, owner] {
        // Even cancelled callbacks settle their ownership before teardown.
        // A cancelled startup can never become ready via a late callback.
        std::string error;
        {
          std::lock_guard lock(self->mu);
          if (!self->cancelled) {
            if (!owner->isInLoopThread()) {
              error = "listen acknowledgement on wrong owner";
            } else {
              int accepting = 0;
              socklen_t size = sizeof(accepting);
              sockaddr_in address{};
              socklen_t address_size = sizeof(address);
              if (::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting,
                               &size) != 0 ||
                  size != sizeof(accepting) || accepting != 1) {
                error = "listener socket is not accepting";
              } else if (::getsockname(fd,
                                       reinterpret_cast<sockaddr*>(&address),
                                       &address_size) != 0 ||
                         address_size != sizeof(address) ||
                         address.sin_family != AF_INET ||
                         address.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                         ntohs(address.sin_port) != self->port) {
                error = "listener socket endpoint mismatch";
              }
            }
          }
          if (!error.empty()) {
            self->fail_locked(std::move(error));
          } else if (!self->cancelled && !self->failed) {
            ++self->acknowledgements;
          }
          --self->pending;
          self->cv.notify_all();
        }
      });
    } catch (...) {
      std::lock_guard lock(mu);
      --pending;
      fail_locked("could not queue listen acknowledgement");
    }
  }

  [[nodiscard]] auto wait_until(std::chrono::steady_clock::time_point deadline)
      -> bool {
    std::unique_lock lock(mu);
    if (!cv.wait_until(lock, deadline, [this] {
          return failed || cancelled || ready_locked();
        })) {
      cancelled = true;
      fail_locked("listener startup deadline expired");
    }
    return ready_locked();
  }

  auto cancel() -> void {
    std::lock_guard lock(mu);
    cancelled = true;
    cv.notify_all();
  }

  auto fail(std::string error) -> void {
    std::lock_guard lock(mu);
    fail_locked(std::move(error));
  }

  [[nodiscard]] auto drain_until(std::chrono::steady_clock::time_point deadline)
      -> bool {
    std::unique_lock lock(mu);
    return cv.wait_until(lock, deadline, [this] { return pending == 0; });
  }

  [[nodiscard]] auto snapshot() const -> ListenerReadinessSnapshot {
    std::lock_guard lock(mu);
    return {ready_locked(),   failed,  cancelled,
            acknowledgements, pending, error};
  }

 private:
  [[nodiscard]] auto ready_locked() const -> bool {
    return !failed && !cancelled && expected_owners != 0 &&
           acknowledgements == expected_owners;
  }

  auto fail_locked(std::string message) -> void {
    failed = true;
    if (error.empty()) {
      error = std::move(message);
    }
    cv.notify_all();
  }

  const std::size_t expected_owners;
  const std::uint16_t port;
  mutable std::mutex mu;
  std::condition_variable cv;
  std::set<std::size_t> seen_indices;
  std::set<trantor::EventLoop*> seen_loops;
  std::size_t acknowledgements{0};
  std::size_t pending{0};
  bool failed{false};
  bool cancelled{false};
  std::string error;
};

} // namespace plinth::ws_test::detail
