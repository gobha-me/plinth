// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <libpq-fe.h>
#include <memory>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef PLINTH_BINARY_PATH
#error "PLINTH_BINARY_PATH must be defined by the build system"
#endif

extern char** environ;

using namespace std::chrono_literals;

namespace {

class TempTree {
 public:
  TempTree() {
    path = std::filesystem::temp_directory_path() /
           ("plinth_shutdown_" + std::to_string(::getpid()) + "_" +
            std::to_string(sequence++));
    std::filesystem::create_directories(path / "data");
    std::filesystem::create_directories(path / "staging");
  }

  ~TempTree() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  TempTree(const TempTree&) = delete;
  auto operator=(const TempTree&) -> TempTree& = delete;

  std::filesystem::path path;

 private:
  static inline unsigned int sequence = 0;
};

class ChildProcess {
 public:
  ChildProcess(std::vector<std::string> args,
               const std::filesystem::path& output_path) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
    REQUIRE(::posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, output_path.c_str(),
                O_CREAT | O_WRONLY | O_TRUNC, 0600) == 0);
    REQUIRE(::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                               STDERR_FILENO) == 0);
    int rc = ::posix_spawn(&pid, args.front().c_str(), &actions, nullptr,
                           argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    REQUIRE(rc == 0);
  }

  ~ChildProcess() {
    if (pid > 0) {
      (void)::kill(pid, SIGKILL);
      (void)::waitpid(pid, nullptr, 0);
    }
  }

  ChildProcess(const ChildProcess&) = delete;
  auto operator=(const ChildProcess&) -> ChildProcess& = delete;

  auto send_signal(int signal) const -> void {
    REQUIRE(::kill(pid, signal) == 0);
  }

  auto wait_for_exit(std::chrono::milliseconds timeout) -> std::optional<int> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      pid_t result = ::waitpid(pid, &status, WNOHANG);
      if (result == pid) {
        pid = -1;
        return status;
      }
      if (result < 0 && errno == EINTR) {
        continue;
      }
      REQUIRE(result == 0);
      std::this_thread::sleep_for(10ms);
    }
    return std::nullopt;
  }

 private:
  pid_t pid = -1;
};

class Socket {
 public:
  explicit Socket(int fd_in = -1) : fd(fd_in) {}
  ~Socket() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  Socket(const Socket&) = delete;
  auto operator=(const Socket&) -> Socket& = delete;
  Socket(Socket&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
  auto operator=(Socket&&) -> Socket& = delete;

  int fd;
};

auto test_port() -> std::uint16_t {
  Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  REQUIRE(socket.fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  REQUIRE(::bind(socket.fd, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  REQUIRE(::getsockname(socket.fd, reinterpret_cast<sockaddr*>(&address),
                        &length) == 0);
  return ntohs(address.sin_port);
}

auto connect_to(std::uint16_t port) -> Socket {
  Socket socket{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (socket.fd < 0) {
    return socket;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(socket.fd, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
    return Socket{};
  }
  timeval timeout{.tv_sec = 2, .tv_usec = 0};
  (void)::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
  (void)::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
  return socket;
}

auto send_all(int fd, std::string_view data) -> bool {
  while (!data.empty()) {
    ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

auto health_is_ready(std::uint16_t port) -> bool {
  auto socket = connect_to(port);
  if (socket.fd < 0) {
    return false;
  }
  constexpr std::string_view REQUEST =
      "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (!send_all(socket.fd, REQUEST)) {
    return false;
  }
  std::string response(512, '\0');
  ssize_t received = ::recv(socket.fd, response.data(), response.size(), 0);
  return received > 0 && response.substr(0, static_cast<std::size_t>(received))
                             .contains(" 200 ");
}

auto wait_for_health(std::uint16_t port, std::chrono::milliseconds timeout)
    -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (health_is_ready(port)) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

auto open_unauthenticated_websocket(std::uint16_t port) -> Socket {
  auto socket = connect_to(port);
  if (socket.fd < 0) {
    return socket;
  }
  const std::string request =
      "GET /ws/events HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  if (!send_all(socket.fd, request)) {
    return Socket{};
  }
  std::string response(1024, '\0');
  ssize_t received = ::recv(socket.fd, response.data(), response.size(), 0);
  if (received <= 0 || !response.substr(0, static_cast<std::size_t>(received))
                            .contains(" 101 ")) {
    return Socket{};
  }
  return socket;
}

auto required_env(const char* name) -> std::optional<std::string> {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string{value};
}

auto emit_realtime_burst() -> void {
  auto host = required_env("PLINTH_PG_HOST");
  auto port = required_env("PLINTH_PG_PORT");
  auto user = required_env("PLINTH_PG_USER");
  auto password = required_env("PLINTH_PG_PASSWORD");
  auto database = required_env("PLINTH_PG_DATABASE");
  REQUIRE(host.has_value());
  REQUIRE(port.has_value());
  REQUIRE(user.has_value());
  REQUIRE(password.has_value());
  REQUIRE(database.has_value());

  const char* keywords[] = {"host",     "port",   "user",
                            "password", "dbname", nullptr};
  const char* values[] = {host->c_str(),     port->c_str(),     user->c_str(),
                          password->c_str(), database->c_str(), nullptr};
  using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
  Connection connection{PQconnectdbParams(keywords, values, 0), PQfinish};
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
  Result result{PQexec(connection.get(),
                       "SELECT pg_notify('plinth:realtime', json_build_object("
                       "'layer', 'data', "
                       "'channel', 'plinth:data:lifecycle.signal', "
                       "'emitted_at', clock_timestamp()::text, "
                       "'test_sequence', series_value)::text) "
                       "FROM generate_series(1, 200) AS series_value"),
                PQclear};
  REQUIRE(PQresultStatus(result.get()) == PGRES_TUPLES_OK);
}

auto write_config(const TempTree& tree, std::uint16_t port,
                  bool valid_migrations) -> std::filesystem::path {
  auto host = required_env("PLINTH_PG_HOST");
  auto pg_port = required_env("PLINTH_PG_PORT");
  auto user = required_env("PLINTH_PG_USER");
  auto password = required_env("PLINTH_PG_PASSWORD");
  auto database = required_env("PLINTH_PG_DATABASE");
  REQUIRE(host.has_value());
  REQUIRE(pg_port.has_value());
  REQUIRE(user.has_value());
  REQUIRE(password.has_value());
  REQUIRE(database.has_value());

  nlohmann::json config = {
      {"database",
       {{"host", *host},
        {"port", std::stoi(*pg_port)},
        {"user", *user},
        {"password", *password},
        {"database", *database},
        {"pool_size", 8}}},
      {"migrations_dir", valid_migrations
                             ? std::string{CMAKE_SOURCE_DIR} + "/migrations"
                             : (tree.path / "missing-migrations").string()},
      {"dev_mode", true},
      {"listen_host", "127.0.0.1"},
      {"listen_port", port},
      {"node_id", "shutdown-process-test"},
      {"ws_auth_timeout_s", 30.0},
      {"ws_heartbeat_interval_s", 30.0},
      {"ws_heartbeat_timeout_s", 30.0},
      {"packages",
       {{"data_dir", (tree.path / "data").string()},
        {"staging_dir", (tree.path / "staging").string()}}},
      {"shell",
       {{"enabled", false},
        {"bundle_path",
         std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled"}}}};
  auto path = tree.path / "config.json";
  std::ofstream stream(path);
  REQUIRE(stream.good());
  stream << config.dump(2);
  REQUIRE(stream.good());
  return path;
}

auto read_text(const std::filesystem::path& path) -> std::string {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

auto require_clean_signal_shutdown(int signal) -> void {
  TempTree tree;
  auto port = test_port();
  auto config = write_config(tree, port, true);
  ChildProcess child{{PLINTH_BINARY_PATH, "serve", "--config", config.string()},
                     tree.path / "process.log"};
  REQUIRE(wait_for_health(port, 30s));
  auto websocket = open_unauthenticated_websocket(port);
  REQUIRE(websocket.fd >= 0);
  emit_realtime_burst();

  const auto started = std::chrono::steady_clock::now();
  child.send_signal(signal);
  auto status = child.wait_for_exit(15s);
  REQUIRE(status.has_value());
  REQUIRE(std::chrono::steady_clock::now() - started < 15s);
  REQUIRE(WIFEXITED(*status));
  REQUIRE(WEXITSTATUS(*status) == 0);
}

} // namespace

TEST_CASE("explicit config failures happen before service startup",
          "[integration][lifecycle][subprocess][config]") {
  TempTree tree;
  auto output = tree.path / "process.log";

  SECTION("missing file") {
    auto missing = tree.path / "missing.json";
    ChildProcess child{
        {PLINTH_BINARY_PATH, "serve", "--config", missing.string()}, output};
    auto status = child.wait_for_exit(2s);
    REQUIRE(status.has_value());
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 1);
    auto text = read_text(output);
    REQUIRE(text.contains("config.file_unreadable"));
    REQUIRE_FALSE(text.contains("starting..."));
  }

  SECTION("malformed file") {
    auto malformed = tree.path / "malformed.json";
    std::ofstream stream(malformed);
    stream << "{ not-json";
    stream.close();
    ChildProcess child{
        {PLINTH_BINARY_PATH, "serve", "--config", malformed.string()}, output};
    auto status = child.wait_for_exit(2s);
    REQUIRE(status.has_value());
    REQUIRE(WIFEXITED(*status));
    REQUIRE(WEXITSTATUS(*status) == 1);
    auto text = read_text(output);
    REQUIRE(text.contains("config.file_invalid"));
    REQUIRE_FALSE(text.contains("starting..."));
  }
}

TEST_CASE("production exits cleanly with an active WebSocket timer",
          "[integration][lifecycle][subprocess]") {
  if (!required_env("PLINTH_PG_HOST").has_value()) {
    SKIP("PostgreSQL environment is not configured");
  }

  SECTION("SIGTERM") {
    for (int attempt = 0; attempt < 5; ++attempt) {
      CAPTURE(attempt);
      require_clean_signal_shutdown(SIGTERM);
    }
  }
  SECTION("SIGINT") {
    for (int attempt = 0; attempt < 5; ++attempt) {
      CAPTURE(attempt);
      require_clean_signal_shutdown(SIGINT);
    }
  }
}

TEST_CASE("partial startup uses bounded coordinator teardown",
          "[integration][lifecycle][subprocess]") {
  if (!required_env("PLINTH_PG_HOST").has_value()) {
    SKIP("PostgreSQL environment is not configured");
  }

  TempTree tree;
  auto config = write_config(tree, test_port(), false);
  ChildProcess child{{PLINTH_BINARY_PATH, "serve", "--config", config.string()},
                     tree.path / "process.log"};
  auto status = child.wait_for_exit(10s);
  REQUIRE(status.has_value());
  REQUIRE(WIFEXITED(*status));
  REQUIRE(WEXITSTATUS(*status) == 1);
}
