// SPDX-License-Identifier: MIT
#pragma once

#include <libpq-fe.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace plinth::db {

// A thread-owned policy for synchronous startup and listener operations.
// Calls retain libpq's error-result contract, including during rollback and
// advisory-lock destructors. The startup owner checks the sticky failure at
// phase boundaries; error/audit paths cannot begin another blocking operation.
class OperationScope {
 public:
  explicit OperationScope(
      std::stop_token stop,
      std::chrono::milliseconds timeout = std::chrono::seconds{10})
      : previous(current), stop(stop), timeout(timeout) {
    current = this;
  }
  ~OperationScope() { current = previous; }
  OperationScope(const OperationScope&) = delete;
  auto operator=(const OperationScope&) -> OperationScope& = delete;

  auto checkpoint() const -> void {
    if (cancel_failed) {
      throw std::runtime_error(
          "PostgreSQL query cancellation was not confirmed");
    }
    if (stop.stop_requested()) {
      throw std::runtime_error("PostgreSQL startup operation cancelled");
    }
    if (failed) {
      throw std::runtime_error(
          "PostgreSQL startup operation deadline exceeded");
    }
  }

  // Listener reconnect loops can retry a timed-out connection. Main never
  // resets startup failure: startup must unwind rather than continue partially.
  auto retry() noexcept -> void { failed = false; }

  [[nodiscard]] auto cancellation_failed() const noexcept -> bool {
    return cancel_failed;
  }

  static auto checkpoint_current() -> void {
    if (current != nullptr) {
      current->checkpoint();
    }
  }

 private:
  friend struct Operation;
  static inline thread_local OperationScope* current = nullptr;
  OperationScope* previous;
  std::stop_token stop;
  std::chrono::milliseconds timeout;
  bool failed = false;
  bool cancel_failed = false;
};

struct Operation {
  OperationScope* scope = OperationScope::current;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() +
      (scope == nullptr ? std::chrono::milliseconds{0} : scope->timeout);

  [[nodiscard]] auto active() const noexcept -> bool {
    return scope != nullptr;
  }

  auto fail_cancellation() const noexcept -> void {
    scope->cancel_failed = true;
  }

  [[nodiscard]] auto interrupted() const noexcept -> bool {
    if (scope->stop.stop_requested() || scope->failed) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      scope->failed = true;
      return true;
    }
    return false;
  }

  [[nodiscard]] auto wait(PGconn* connection, short events) const -> bool {
    while (!interrupted()) {
      pollfd socket{.fd = PQsocket(connection), .events = events, .revents = 0};
      if (socket.fd < 0) {
        return false;
      }
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      int timeout = static_cast<int>(
          std::clamp(remaining.count(), decltype(remaining.count()){1},
                     decltype(remaining.count()){50}));
      int result = ::poll(&socket, 1, timeout);
      if (result > 0) {
        return true; // libpq consumes readiness, EOF, and socket errors.
      }
      if (result < 0 && errno != EINTR) {
        return false;
      }
    }
    return false;
  }
};

namespace detail {

inline auto cancel_query(PGconn* connection) -> bool;

inline auto finish_connect(PGconn* connection, const Operation& operation)
    -> bool {
  while (!operation.interrupted()) {
    switch (PQconnectPoll(connection)) {
      case PGRES_POLLING_OK: return true;
      case PGRES_POLLING_FAILED: return false;
      case PGRES_POLLING_READING:
        if (!operation.wait(connection, POLLIN)) {
          return false;
        }
        break;
      case PGRES_POLLING_WRITING:
        if (!operation.wait(connection, POLLOUT)) {
          return false;
        }
        break;
      case PGRES_POLLING_ACTIVE: break;
    }
  }
  return false;
}

// The connection owner alone interrupts the socket. It retains PGconn and
// descriptor ownership until its existing RAII guard calls PQfinish.
inline auto failed_result(PGconn* connection) -> PGresult* {
  if (connection != nullptr) {
    int socket = PQsocket(connection);
    if (socket >= 0) {
      (void)::shutdown(socket, SHUT_RDWR);
      (void)PQconsumeInput(connection);
    }
  }
  return PQmakeEmptyPGresult(connection, PGRES_FATAL_ERROR);
}

inline auto receive_result(PGconn* connection, const Operation& operation,
                           bool allow_cancel = true) -> PGresult* {
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  Result result{nullptr, PQclear};
  auto abort = [&] {
    if (allow_cancel && operation.interrupted() && !cancel_query(connection)) {
      operation.fail_cancellation();
    }
    return failed_result(connection);
  };
  for (;;) {
    if (operation.interrupted()) {
      return abort();
    }
    int flushing = PQflush(connection);
    if (flushing < 0) {
      return failed_result(connection);
    }
    if (flushing > 0) {
      if (!operation.wait(connection, POLLIN | POLLOUT) ||
          PQconsumeInput(connection) == 0) {
        return abort();
      }
      continue;
    }
    if (PQconsumeInput(connection) == 0) {
      return failed_result(connection);
    }
    if (PQisBusy(connection) != 0) {
      if (!operation.wait(connection, POLLIN)) {
        return abort();
      }
      continue;
    }
    Result next{PQgetResult(connection), PQclear};
    if (!next) {
      return result.release();
    }
    auto status = PQresultStatus(next.get());
    // COPY transfers require another protocol; callers of these SQL helpers
    // already reject COPY results. Do not wait indefinitely for COPY input.
    if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT ||
        status == PGRES_COPY_BOTH) {
      return next.release();
    }
    result = std::move(next);
  }
}

// libpq 16 exposes only blocking PQcancel. An authenticated control connection
// can cancel its own role's backend using the normal nonblocking protocol.
// One two-second deadline covers connect, cancel dispatch, and confirmation on
// the original connection. Never recursively cancel the control connection.
inline auto cancel_query(PGconn* connection) -> bool {
  OperationScope cleanup_scope{std::stop_token{}, std::chrono::seconds{2}};
  Operation cleanup;
  using Options = std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)>;
  Options options{PQconninfo(connection), PQconninfoFree};
  if (!options || PQbackendPID(connection) <= 0) {
    return false;
  }
  std::vector<const char*> keywords;
  std::vector<const char*> values;
  for (auto* option = options.get(); option->keyword != nullptr; ++option) {
    if (option->val != nullptr) {
      keywords.push_back(option->keyword);
      values.push_back(option->val);
    }
  }
  keywords.push_back("host");
  values.push_back(PQhost(connection));
  keywords.push_back("port");
  values.push_back(PQport(connection));
  keywords.push_back("password");
  values.push_back(PQpass(connection));
  keywords.push_back("options");
  values.push_back("-c statement_timeout=2000");
  // Reuse the resolved numeric address so cancellation does not resolve DNS.
  const char* address = PQhostaddr(connection);
  if (address != nullptr && *address != '\0') {
    keywords.push_back("hostaddr");
    values.push_back(address);
  }
  keywords.push_back(nullptr);
  values.push_back(nullptr);
  using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
  Connection control{PQconnectStartParams(keywords.data(), values.data(), 0),
                     PQfinish};
  if (!control || !finish_connect(control.get(), cleanup) ||
      PQsetnonblocking(control.get(), 1) != 0) {
    return false;
  }
  auto backend = std::to_string(PQbackendPID(connection));
  const char* parameter = backend.c_str();
  if (PQsendQueryParams(control.get(),
                        "SELECT pg_catalog.pg_cancel_backend($1::int)", 1,
                        nullptr, &parameter, nullptr, nullptr, 0) == 0) {
    return false;
  }
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  Result sent{receive_result(control.get(), cleanup, false), PQclear};
  if (PQresultStatus(sent.get()) != PGRES_TUPLES_OK ||
      PQntuples(sent.get()) != 1 ||
      std::string_view{PQgetvalue(sent.get(), 0, 0)} != "t") {
    return false;
  }
  Result completed{receive_result(connection, cleanup, false), PQclear};
  return !cleanup.interrupted() && PQstatus(connection) == CONNECTION_OK;
}

} // namespace detail

inline auto connect(const char* conninfo) -> PGconn* {
  Operation operation;
  if (!operation.active()) {
    return PQconnectdb(conninfo);
  }
  if (operation.interrupted()) {
    return nullptr;
  }
  using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
  Connection connection{PQconnectStart(conninfo), PQfinish};
  if (!connection) {
    return nullptr;
  }
  using Options = std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)>;
  Options options{PQconninfo(connection.get()), PQconninfoFree};
  if (options) {
    for (auto* option = options.get(); option->keyword != nullptr; ++option) {
      if (std::string_view{option->keyword} == "connect_timeout" &&
          option->val != nullptr) {
        std::string_view value{option->val};
        int seconds = 0;
        auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (parsed.ec == std::errc{} && seconds > 0) {
          operation.deadline =
              std::min(operation.deadline, std::chrono::steady_clock::now() +
                                               std::chrono::seconds{seconds});
        }
      }
    }
  }
  if (detail::finish_connect(connection.get(), operation) ||
      (!operation.interrupted() &&
       PQstatus(connection.get()) == CONNECTION_BAD)) {
    return connection.release();
  }
  return nullptr;
}

inline auto exec(PGconn* connection, const char* sql) -> PGresult* {
  Operation operation;
  if (!operation.active()) {
    return PQexec(connection, sql);
  }
  if (operation.interrupted() || PQsetnonblocking(connection, 1) != 0 ||
      PQsendQuery(connection, sql) == 0) {
    return detail::failed_result(connection);
  }
  return detail::receive_result(connection, operation);
}

inline auto exec_params(PGconn* connection, const char* sql, int count,
                        const Oid* types, const char* const* values,
                        const int* lengths, const int* formats,
                        int result_format) -> PGresult* {
  Operation operation;
  if (!operation.active()) {
    return PQexecParams(connection, sql, count, types, values, lengths, formats,
                        result_format);
  }
  if (operation.interrupted() || PQsetnonblocking(connection, 1) != 0 ||
      PQsendQueryParams(connection, sql, count, types, values, lengths, formats,
                        result_format) == 0) {
    return detail::failed_result(connection);
  }
  return detail::receive_result(connection, operation);
}

} // namespace plinth::db
