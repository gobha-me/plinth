// SPDX-License-Identifier: MIT

#include "kernel/auth/crypto.hpp"
#include "kernel/config.hpp"
#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/db/connection_info.hpp"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <array>
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
#include <stdexcept>
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
               const std::filesystem::path& output_path,
               const plinth::Config::Database* database = nullptr) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> environment;
    std::vector<char*> envp;
    if (database != nullptr) {
      for (auto** entry = environ; *entry != nullptr; ++entry) {
        if (!std::string_view{*entry}.starts_with("PLINTH_PG_") &&
            !std::string_view{*entry}.starts_with("PLINTH_DEV_MODE=")) {
          environment.emplace_back(*entry);
        }
      }
      environment.emplace_back("PLINTH_DEV_MODE=false");
      environment.push_back("PLINTH_PG_HOST=" + database->host);
      environment.push_back("PLINTH_PG_PORT=" + std::to_string(database->port));
      environment.push_back("PLINTH_PG_USER=" + database->user);
      environment.push_back("PLINTH_PG_PASSWORD=" + database->password);
      environment.push_back("PLINTH_PG_DATABASE=" + database->database);
      for (auto& entry : environment) {
        envp.push_back(entry.data());
      }
      envp.push_back(nullptr);
    }

    posix_spawn_file_actions_t actions;
    REQUIRE(::posix_spawn_file_actions_init(&actions) == 0);
    REQUIRE(::posix_spawn_file_actions_addopen(
                &actions, STDOUT_FILENO, output_path.c_str(),
                O_CREAT | O_WRONLY | O_TRUNC, 0600) == 0);
    REQUIRE(::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO,
                                               STDERR_FILENO) == 0);
    int rc =
        ::posix_spawn(&pid, args.front().c_str(), &actions, nullptr,
                      argv.data(), database == nullptr ? environ : envp.data());
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

// Owns only uniquely named credentials and a database on the disposable
// PostgreSQL instance selected by the integration-test environment.
class CredentialDatabase {
 public:
  CredentialDatabase() {
    db.host = required_env("PLINTH_PG_HOST").value();
    db.port = static_cast<std::uint16_t>(
        std::stoi(required_env("PLINTH_PG_PORT").value()));
    db.user = required_env("PLINTH_PG_USER").value();
    db.password = required_env("PLINTH_PG_PASSWORD").value();
    db.database = required_env("PLINTH_PG_DATABASE").value();
    auto conninfo = plinth::db::connection_info(db) +
                    " connect_timeout=5 options='-c statement_timeout=5000'";
    admin.reset(PQconnectdb(conninfo.c_str()));
    REQUIRE(admin != nullptr);
    REQUIRE(PQstatus(admin.get()) == CONNECTION_OK);
    auto suffix =
        std::to_string(::getpid()) + "_" + std::to_string(sequence++) + "'\\";
    db.user = "plinth credential role " + suffix;
    db.database = "plinth credential db " + suffix;
  }

  ~CredentialDatabase() {
    if (database_created) {
      CHECK(
          exec("DROP DATABASE " + quote(db.database, false) + " WITH (FORCE)"));
    }
    if (role_created) {
      CHECK(exec("DROP ROLE " + quote(db.user, false)));
    }
  }

  CredentialDatabase(const CredentialDatabase&) = delete;
  auto operator=(const CredentialDatabase&) -> CredentialDatabase& = delete;

  auto create(std::string password) -> void {
    db.password = std::move(password);
    REQUIRE(exec("CREATE ROLE " + quote(db.user, false) +
                 " LOGIN SUPERUSER PASSWORD " + quote(db.password, true)));
    role_created = true;
    REQUIRE(exec("CREATE DATABASE " + quote(db.database, false) + " OWNER " +
                 quote(db.user, false)));
    database_created = true;

    using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
    auto conninfo = plinth::db::connection_info(db) + " connect_timeout=5";
    Connection authenticated{PQconnectdb(conninfo.c_str()), PQfinish};
    REQUIRE(authenticated != nullptr);
    REQUIRE(PQstatus(authenticated.get()) == CONNECTION_OK);
    // Trust authentication would hide password corruption, so it is not a
    // valid environment for this test. CI's PostgreSQL service uses SCRAM.
    REQUIRE(PQconnectionUsedPassword(authenticated.get()) == 1);
    auto incorrect = db;
    incorrect.password = "deliberately-incorrect-test-password";
    conninfo = plinth::db::connection_info(incorrect) + " connect_timeout=5";
    Connection rejected{PQconnectdb(conninfo.c_str()), PQfinish};
    REQUIRE(rejected != nullptr);
    REQUIRE(PQstatus(rejected.get()) == CONNECTION_BAD);
  }

  plinth::Config::Database db;

 private:
  auto quote(const std::string& value, bool literal) const -> std::string {
    std::unique_ptr<char, decltype(&PQfreemem)> result{
        literal ? PQescapeLiteral(admin.get(), value.data(), value.size())
                : PQescapeIdentifier(admin.get(), value.data(), value.size()),
        PQfreemem};
    REQUIRE(result != nullptr);
    return result.get();
  }

  auto exec(const std::string& sql) const -> bool {
    std::unique_ptr<PGresult, decltype(&PQclear)> result{
        PQexec(admin.get(), sql.c_str()), PQclear};
    return PQresultStatus(result.get()) == PGRES_COMMAND_OK;
  }

  static inline unsigned int sequence = 0;
  std::unique_ptr<PGconn, decltype(&PQfinish)> admin{nullptr, PQfinish};
  bool role_created = false;
  bool database_created = false;
};

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
                  bool valid_migrations,
                  const std::string& isolated_database = {})
    -> std::filesystem::path {
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
        {"database", isolated_database.empty() ? *database : isolated_database},
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
       {{"enabled", !isolated_database.empty()},
        {"bundle_path",
         std::string{CMAKE_BINARY_DIR} + "/share/plinth/bundled"}}}};
  if (!isolated_database.empty()) {
    config["dev_mode"] = false;
    config["realtime"]["coalescer"]["window_ms"] = 10000;
  }
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

using PgConnection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using PgResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

auto open_database(const std::string& override_name = {}) -> PgConnection {
  auto host = required_env("PLINTH_PG_HOST");
  auto port = required_env("PLINTH_PG_PORT");
  auto user = required_env("PLINTH_PG_USER");
  auto password = required_env("PLINTH_PG_PASSWORD");
  auto database = required_env("PLINTH_PG_DATABASE");
  REQUIRE(host);
  REQUIRE(port);
  REQUIRE(user);
  REQUIRE(password);
  REQUIRE(database);
  const char* keywords[] = {"host",     "port",   "user",
                            "password", "dbname", nullptr};
  const char* values[] = {host->c_str(),
                          port->c_str(),
                          user->c_str(),
                          password->c_str(),
                          override_name.empty() ? database->c_str()
                                                : override_name.c_str(),
                          nullptr};
  PgConnection connection{PQconnectdbParams(keywords, values, 0), PQfinish};
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  return connection;
}

auto sql(PGconn* connection, const std::string& query,
         const std::vector<std::string>& parameters = {}) -> PgResult {
  std::vector<const char*> values;
  values.reserve(parameters.size());
  for (const auto& parameter : parameters) {
    values.push_back(parameter.c_str());
  }
  PgResult result{PQexecParams(connection, query.c_str(),
                               static_cast<int>(values.size()), nullptr,
                               values.data(), nullptr, nullptr, 0),
                  PQclear};
  const auto status = PQresultStatus(result.get());
  INFO(query);
  INFO(PQresultErrorMessage(result.get()));
  REQUIRE((status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK));
  return result;
}

class IsolatedDatabase {
 public:
  IsolatedDatabase()
      : admin(open_database()),
        name("plinth_shutdown_" + std::to_string(::getpid()) + "_" +
             std::to_string(sequence++)) {
    sql(admin.get(), "CREATE DATABASE " + name);
  }
  ~IsolatedDatabase() {
    // The name is generated entirely from fixed text and unsigned integers.
    PgResult result{PQexec(admin.get(),
                           ("DROP DATABASE " + name + " WITH (FORCE)").c_str()),
                    PQclear};
  }
  IsolatedDatabase(const IsolatedDatabase&) = delete;
  auto operator=(const IsolatedDatabase&) -> IsolatedDatabase& = delete;

  PgConnection admin;
  std::string name;

 private:
  static inline unsigned int sequence = 0;
};

auto receive_exact(int fd, std::size_t length) -> std::string {
  if (fd < 0) {
    FAIL("cannot receive a WebSocket frame from an invalid socket");
    return {};
  }
  std::string data(length, '\0');
  std::size_t offset = 0;
  while (offset < length) {
    auto received = ::recv(fd, data.data() + offset, length - offset, 0);
    REQUIRE(received > 0);
    offset += static_cast<std::size_t>(received);
  }
  return data;
}

auto send_frame(int fd, const nlohmann::json& value) -> void {
  const auto body = value.dump();
  REQUIRE(body.size() <= 65535);
  std::string frame{static_cast<char>(0x81)};
  if (body.size() < 126) {
    frame.push_back(static_cast<char>(0x80U | body.size()));
  } else {
    frame.push_back(static_cast<char>(0xfe));
    frame.push_back(static_cast<char>((body.size() >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(body.size() & 0xffU));
  }
  constexpr std::array<unsigned char, 4> MASK{0x21, 0x43, 0x65, 0x07};
  for (auto byte : MASK) {
    frame.push_back(static_cast<char>(byte));
  }
  for (std::size_t i = 0; i < body.size(); ++i) {
    frame.push_back(static_cast<char>(static_cast<unsigned char>(body[i]) ^
                                      MASK[i % MASK.size()]));
  }
  REQUIRE(send_all(fd, frame));
}

auto receive_frame(int fd) -> nlohmann::json {
  auto header = receive_exact(fd, 2);
  REQUIRE(static_cast<unsigned char>(header[0]) == 0x81);
  auto length = static_cast<std::size_t>(static_cast<unsigned char>(header[1]));
  REQUIRE(length < 128); // server frames are unmasked
  if (length == 126) {
    auto extended = receive_exact(fd, 2);
    length = (static_cast<std::size_t>(static_cast<unsigned char>(extended[0]))
              << 8U) |
             static_cast<unsigned char>(extended[1]);
  }
  REQUIRE(length < 65536);
  return nlohmann::json::parse(receive_exact(fd, length));
}

auto authenticated_socket(std::uint16_t port, const std::string& token)
    -> Socket {
  auto socket = open_unauthenticated_websocket(port);
  REQUIRE(socket.fd >= 0);
  send_frame(socket.fd, {{"type", "auth"}, {"token", token}});
  REQUIRE(receive_frame(socket.fd).at("type") == "connected");
  return socket;
}

auto delete_preference(std::uint16_t port, const std::string& token,
                       const std::string& key) -> void {
  auto socket = connect_to(port);
  REQUIRE(socket.fd >= 0);
  const auto body = nlohmann::json{{"args", {{"key", key}}}}.dump();
  const auto request =
      "POST /api/cap/shell.preferences.set HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Connection: close\r\nContent-Type: application/json\r\nCookie: "
      "plinth_session=" +
      token + "\r\nContent-Length: " + std::to_string(body.size()) +
      "\r\n\r\n" + body;
  REQUIRE(send_all(socket.fd, request));
  std::string response;
  std::array<char, 4096> buffer{};
  while (true) {
    auto received = ::recv(socket.fd, buffer.data(), buffer.size(), 0);
    REQUIRE(received >= 0);
    if (received == 0) {
      break;
    }
    response.append(buffer.data(), static_cast<std::size_t>(received));
  }
  REQUIRE(response.starts_with("HTTP/1.1 200 "));
  auto separator = response.find("\r\n\r\n");
  REQUIRE(separator != std::string::npos);
  const auto value = nlohmann::json::parse(response.substr(separator + 4));
  REQUIRE(value.at("ok") == true);
  REQUIRE(value.at("value").at("deleted") == true);
}

auto scalar(PGconn* connection, const std::string& query,
            const std::vector<std::string>& parameters = {}) -> std::int64_t {
  auto result = sql(connection, query, parameters);
  REQUIRE(PQntuples(result.get()) == 1);
  return std::stoll(PQgetvalue(result.get(), 0, 0));
}

template <typename Predicate>
auto wait_for_condition(Predicate predicate, std::chrono::milliseconds timeout)
    -> bool {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

auto require_durable_shutdown(int signal, bool accepted_websocket_work)
    -> void {
  IsolatedDatabase database;
  TempTree tree;
  auto port = test_port();
  auto config = write_config(tree, port, true, database.name);
  plinth::Config::Database child_database;
  child_database.host = *required_env("PLINTH_PG_HOST");
  child_database.port =
      static_cast<std::uint16_t>(std::stoi(*required_env("PLINTH_PG_PORT")));
  child_database.user = *required_env("PLINTH_PG_USER");
  child_database.password = *required_env("PLINTH_PG_PASSWORD");
  child_database.database = database.name;
  ChildProcess child{{PLINTH_BINARY_PATH, "serve", "--config", config.string()},
                     tree.path / "process.log",
                     &child_database};
  REQUIRE(wait_for_health(port, 30s));
  auto connection = open_database(database.name);
  auto user = sql(connection.get(),
                  "INSERT INTO plinth.users (username, password_hash) "
                  "VALUES ('shutdown_owner', 'unused') RETURNING id::text");
  const std::string user_id{PQgetvalue(user.get(), 0, 0)};
  const std::string token = "fake-shutdown-session-token";
  sql(connection.get(),
      "INSERT INTO plinth.sessions (user_id, token_hash) VALUES ($1::uuid, $2)",
      {user_id, plinth::auth::sha256_hex(token)});
  sql(connection.get(),
      "INSERT INTO plinth.group_members (group_id, user_id) "
      "SELECT id, $1::uuid FROM plinth.groups WHERE name = 'admin'",
      {user_id});
  sql(connection.get(),
      "INSERT INTO ext_shell.user_preferences (user_id, key, value) "
      "VALUES ($1::uuid, 'shutdown_control', '1'), ($1::uuid, "
      "'shutdown_target', '2')",
      {user_id});
  // A real control deletion proves that this process subscribed and persists
  // coalescer events before the final open-window case begins.
  delete_preference(port, token, "shutdown_control");
  constexpr auto EVENT_CHANNEL = "plinth:data:ext_shell.user_preferences";
  REQUIRE(wait_for_condition(
      [&] {
        return scalar(connection.get(),
                      "SELECT count(*) FROM plinth.events WHERE channel = $1",
                      {EVENT_CHANNEL}) == 1;
      },
      15s));
  const auto control_seq = scalar(
      connection.get(), "SELECT max(seq) FROM plinth.events WHERE channel = $1",
      {EVENT_CHANNEL});

  if (accepted_websocket_work) {
    // The blocking transaction caches PostgreSQL statistics snapshots. Use an
    // independent autocommit observer so every admission poll sees new work.
    auto observer = open_database(database.name);
    auto websocket = authenticated_socket(port, token);
    sql(connection.get(), "BEGIN");
    sql(connection.get(),
        "LOCK TABLE ext_shell.user_preferences IN SHARE MODE");
    send_frame(websocket.fd, {{"type", "call"},
                              {"id", "final-write"},
                              {"signature", "shell:1:preferences.set"},
                              {"args", {{"key", "shutdown_target"}}}});
    // PostgreSQL's lock wait proves that the WS call entered the production
    // extension before the signal. Release it only after signaling shutdown.
    const bool admitted = wait_for_condition(
        [&] {
          return scalar(observer.get(),
                        "SELECT count(*) FROM pg_stat_activity "
                        "WHERE datname = current_database() AND pid <> "
                        "pg_backend_pid() "
                        "AND wait_event_type = 'Lock' AND query LIKE 'DELETE "
                        "FROM ext_shell.user_preferences%'") > 0;
        },
        3s);
    if (admitted) {
      child.send_signal(signal);
    }
    sql(connection.get(), "ROLLBACK");
    INFO(read_text(tree.path / "process.log"));
    REQUIRE(admitted);
  } else {
    delete_preference(port, token, "shutdown_target");
    child.send_signal(signal);
  }
  auto status = child.wait_for_exit(15s);
  INFO(read_text(tree.path / "process.log"));
  REQUIRE(status);
  REQUIRE(WIFEXITED(*status));
  REQUIRE(WEXITSTATUS(*status) == 0);
  REQUIRE(scalar(connection.get(),
                 "SELECT count(*) FROM ext_shell.user_preferences "
                 "WHERE user_id = $1::uuid",
                 {user_id}) == 0);
  const auto final_seq = scalar(
      connection.get(), "SELECT max(seq) FROM plinth.events WHERE channel = $1",
      {EVENT_CHANNEL});
  REQUIRE(final_seq > control_seq);
  REQUIRE(scalar(connection.get(),
                 "SELECT count(*) FROM plinth.events "
                 "WHERE channel = $1 AND seq > $2::bigint AND payload->'ops' "
                 "@> '[{\"op\":\"delete\",\"count\":1}]'::jsonb",
                 {EVENT_CHANNEL, std::to_string(control_seq)}) == 1);

  ChildProcess restarted{
      {PLINTH_BINARY_PATH, "serve", "--config", config.string()},
      tree.path / "restarted.log",
      &child_database};
  REQUIRE(wait_for_health(port, 30s));
  auto replay = authenticated_socket(port, token);
  send_frame(replay.fd, {{"type", "subscribe"},
                         {"channels", {EVENT_CHANNEL}},
                         {"since_seq", control_seq}});
  int final_events = 0;
  bool completed = false;
  for (int i = 0; i < 10 && !completed; ++i) {
    auto frame = receive_frame(replay.fd);
    if (frame.at("type") == "replay") {
      REQUIRE(frame.at("envelope").at("seq") == final_seq);
      ++final_events;
    }
    completed = frame.at("type") == "replay_done";
  }
  REQUIRE(completed);
  REQUIRE(final_events == 1);
  restarted.send_signal(signal);
  auto restarted_status = restarted.wait_for_exit(15s);
  REQUIRE(restarted_status);
  REQUIRE(WIFEXITED(*restarted_status));
  REQUIRE(WEXITSTATUS(*restarted_status) == 0);
  REQUIRE(scalar(connection.get(),
                 "SELECT count(*) FROM plinth.events WHERE channel = $1 AND "
                 "seq > $2::bigint",
                 {EVENT_CHANNEL, std::to_string(control_seq)}) == 1);
}

auto request_http(std::uint16_t port, std::string_view request) -> std::string {
  auto socket = connect_to(port);
  if (socket.fd < 0 || !send_all(socket.fd, request)) {
    throw std::runtime_error("failed to send subprocess HTTP request");
  }
  std::array<char, 4096> buffer{};
  std::string response;
  for (;;) {
    ssize_t received = ::recv(socket.fd, buffer.data(), buffer.size(), 0);
    if (received < 0) {
      throw std::runtime_error("failed to receive subprocess HTTP response");
    }
    if (received == 0) {
      return response;
    }
    response.append(buffer.data(), static_cast<std::size_t>(received));
    REQUIRE(response.size() <= 65536);
  }
}

auto require_authenticated_runtime_queries(std::uint16_t port) -> void {
  auto frontend = request_http(
      port, "GET /api/frontend/tokens.css HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n");
  REQUIRE(frontend.contains(" 302 "));
  REQUIRE(frontend.contains("/ext/shell/"));

  // The missing-user response requires a successful query on Drogon's pool.
  // A broken pool connection would instead time out or return a server error.
  constexpr std::string_view BODY =
      R"({"username":"credential-test-missing","password":"fake-password"})";
  auto login = request_http(
      port, "POST /api/auth/login HTTP/1.1\r\nHost: 127.0.0.1\r\n"
            "Connection: close\r\nContent-Type: application/json\r\n"
            "Content-Length: " +
                std::to_string(BODY.size()) + "\r\n\r\n" + std::string{BODY});
  REQUIRE(login.contains(" 401 "));
  REQUIRE(login.contains("invalid_credentials"));
}

auto require_authenticated_listener(const plinth::Config::Database& db,
                                    int boot) -> void {
  using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
  auto conninfo = plinth::db::connection_info(db) +
                  " connect_timeout=5 options='-c statement_timeout=5000'";
  Connection connection{PQconnectdb(conninfo.c_str()), PQfinish};
  REQUIRE(connection != nullptr);
  REQUIRE(PQstatus(connection.get()) == CONNECTION_OK);
  auto marker = std::to_string(boot);
  auto payload = nlohmann::json{
      {"layer", "system"},
      {"channel", "plinth:system:credential.test"},
      {"marker",
       marker}}.dump();
  const std::array<const char*, 1> payload_params{payload.c_str()};
  const std::array<const char*, 1> marker_params{marker.c_str()};
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
    Result sent{PQexecParams(
                    connection.get(), "SELECT pg_notify('plinth:realtime', $1)",
                    1, nullptr, payload_params.data(), nullptr, nullptr, 0),
                PQclear};
    REQUIRE(PQresultStatus(sent.get()) == PGRES_TUPLES_OK);
    Result found{
        PQexecParams(connection.get(),
                     "SELECT 1 FROM plinth.events WHERE channel = "
                     "'plinth:system:credential.test' AND payload->>'marker' "
                     "= $1 LIMIT 1",
                     1, nullptr, marker_params.data(), nullptr, nullptr, 0),
        PQclear};
    REQUIRE(PQresultStatus(found.get()) == PGRES_TUPLES_OK);
    if (PQntuples(found.get()) > 0) {
      return;
    }
    std::this_thread::sleep_for(25ms);
  }
  FAIL("production listener did not persist the credential-test notification");
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

TEST_CASE("production shutdown persists and replays the last extension write",
          "[integration][lifecycle][subprocess][realtime][shutdown]") {
  if (!required_env("PLINTH_PG_HOST")) {
    SKIP("PostgreSQL environment is not configured");
  }
  SECTION("SIGINT after an HTTP commit") {
    require_durable_shutdown(SIGINT, false);
  }
  SECTION("SIGTERM after an HTTP commit") {
    require_durable_shutdown(SIGTERM, false);
  }
  SECTION("SIGINT during an accepted WebSocket write") {
    require_durable_shutdown(SIGINT, true);
  }
  SECTION("SIGTERM during an accepted WebSocket write") {
    require_durable_shutdown(SIGTERM, true);
  }
}

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

TEST_CASE("production preserves PostgreSQL credentials across boot and restart",
          "[integration][lifecycle][subprocess][conninfo]") {
  if (!required_env("PLINTH_PG_HOST").has_value()) {
    SKIP("PostgreSQL environment is not configured");
  }

  // The last value has whitespace but no ordinary space, exercising Drogon's
  // pool quoting separately from Plinth's direct libpq connection strings.
  const std::array<std::string, 3> passwords{"fake space password",
                                             "fake\\backslash'quote",
                                             "fake\twhite\nspace\r\f\v"};
  for (std::size_t variant = 0; variant < passwords.size(); ++variant) {
    CAPTURE(variant);
    CredentialDatabase database;
    database.create(passwords[variant]);
    TempTree tree;
    auto port = test_port();
    auto config_path = write_config(tree, port, true);
    auto config = nlohmann::json::parse(read_text(config_path));
    config["dev_mode"] = false;
    {
      std::ofstream stream(config_path);
      REQUIRE(stream.good());
      stream << config.dump(2);
      REQUIRE(stream.good());
    }
    for (int boot = 0; boot < 2; ++boot) {
      CAPTURE(boot);
      auto log_path = tree.path / ("boot-" + std::to_string(boot) + ".log");
      ChildProcess child{
          {PLINTH_BINARY_PATH, "serve", "--config", config_path.string()},
          log_path,
          &database.db};
      REQUIRE(wait_for_health(port, 30s));
      require_authenticated_runtime_queries(port);
      require_authenticated_listener(database.db, boot);
      child.send_signal(SIGTERM);
      auto status = child.wait_for_exit(15s);
      REQUIRE(status.has_value());
      REQUIRE(WIFEXITED(*status));
      REQUIRE(WEXITSTATUS(*status) == 0);
      REQUIRE_FALSE(read_text(log_path).contains(database.db.password));
    }
  }
}
