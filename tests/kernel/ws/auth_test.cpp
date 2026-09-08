#include "ws_test_fixture.hpp"

#include "kernel/auth/crypto.hpp"

#include <arpa/inet.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <drogon/HttpAppFramework.h>
#include <future>
#include <json/reader.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using plinth::ws_test::WsTestClient;

namespace {

// The connection address and HTTP Host must be independent: Drogon's client
// derives Host from its resolver input, which makes this rejection case depend
// on whether localhost resolves to IPv4 or IPv6. Own this raw socket and bound
// the entire exchange, including fragmented headers/frames, to one deadline.
auto require_mismatched_host_rejected(const std::string& origin,
                                      const std::string& forwarded_host,
                                      const std::string& cookie) -> void {
  const auto port = plinth::ws_test::test_server_port();
  struct Socket {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    ~Socket() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    Socket() = default;
    Socket(const Socket&) = delete;
    auto operator=(const Socket&) -> Socket& = delete;
  } socket;
  REQUIRE(socket.fd >= 0);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  auto ready = [&](short events) {
    for (;;) {
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      REQUIRE(remaining > 0ms);
      pollfd pending{.fd = socket.fd, .events = events, .revents = 0};
      const int result =
          ::poll(&pending, 1, static_cast<int>(remaining.count()));
      if (result < 0 && errno == EINTR) {
        continue;
      }
      REQUIRE(result > 0);
      REQUIRE((pending.revents & POLLNVAL) == 0);
      return;
    }
  };
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const int connection = ::connect(
      socket.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  REQUIRE((connection == 0 || errno == EINPROGRESS));
  ready(POLLOUT);
  int error = 0;
  socklen_t error_size = sizeof(error);
  REQUIRE(::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &error_size) ==
          0);
  REQUIRE(error == 0);
  auto send_all = [&](std::string_view data) {
    while (!data.empty()) {
      ready(POLLOUT);
      const auto sent =
          ::send(socket.fd, data.data(), data.size(), MSG_NOSIGNAL);
      if (sent < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      REQUIRE(sent > 0);
      data.remove_prefix(static_cast<std::size_t>(sent));
    }
  };
  auto receive_exact = [&](std::size_t length) {
    std::string data(length, '\0');
    std::size_t offset = 0;
    while (offset < length) {
      ready(POLLIN);
      const auto received =
          ::recv(socket.fd, data.data() + offset, length - offset, 0);
      if (received < 0 &&
          (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      REQUIRE(received > 0);
      offset += static_cast<std::size_t>(received);
    }
    return data;
  };
  send_all("GET /ws/events HTTP/1.1\r\nHost: mismatched.example\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
           "Sec-WebSocket-Version: 13\r\nOrigin: " +
           origin + "\r\nX-Forwarded-Host: " + forwarded_host +
           "\r\nX-Forwarded-Proto: https\r\nCookie: plinth_session=" + cookie +
           "\r\n\r\n");
  std::string response;
  while (!response.ends_with("\r\n\r\n")) {
    REQUIRE(response.size() < 16384);
    response +=
        receive_exact(1); // Preserve any frame coalesced with the upgrade.
  }
  REQUIRE(response.starts_with("HTTP/1.1 101 "));
  auto frame = [&](unsigned char opcode) {
    const auto header = receive_exact(2);
    REQUIRE(static_cast<unsigned char>(header[0]) == (0x80U | opcode));
    auto length =
        static_cast<std::size_t>(static_cast<unsigned char>(header[1]));
    REQUIRE(length < 128); // Server frames are unmasked.
    if (length == 126) {
      const auto extended = receive_exact(2);
      length =
          (static_cast<std::size_t>(static_cast<unsigned char>(extended[0]))
           << 8U) |
          static_cast<unsigned char>(extended[1]);
    } else {
      REQUIRE(length != 127); // This fixture accepts only bounded small frames.
    }
    return receive_exact(length);
  };
  const auto payload = frame(1);
  Json::Value message;
  Json::CharReaderBuilder builder;
  const auto reader =
      std::unique_ptr<Json::CharReader>(builder.newCharReader());
  std::string parse_error;
  REQUIRE(reader->parse(payload.data(), payload.data() + payload.size(),
                        &message, &parse_error));
  REQUIRE(message["type"].asString() == "error");
  REQUIRE(message["error"].asString() == "auth_failed");
  const auto closing = frame(8);
  REQUIRE(closing.size() <= 125);
  // A close frame alone must not hide a transport that remains open.
  for (;;) {
    ready(POLLIN);
    std::array<char, 1> byte{};
    const auto received = ::recv(socket.fd, byte.data(), byte.size(), 0);
    if (received < 0 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    REQUIRE(received == 0);
    break;
  }
}

// Build an auth message.
auto auth_msg(const std::string& token) -> Json::Value {
  Json::Value v;
  v["type"] = "auth";
  v["token"] = token;
  return v;
}

} // namespace

TEST_CASE("WS auth succeeds with a valid session token",
          "[ws][integration][auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "alice", "password-123");
  auto raw_token = plinth::auth::generate_token();
  auto session_id = plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient client;
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(raw_token));

  auto msg = client.receive_json(3s);
  REQUIRE(msg.has_value());
  REQUIRE((*msg)["type"].asString() == "connected");
  REQUIRE((*msg)["user"]["id"].asString() == user_id);
  REQUIRE((*msg)["user"]["username"].asString() == "alice");
  REQUIRE((*msg)["session_id"].asString() == session_id);
  REQUIRE((*msg)["node_id"].asString() == cfg.node_id);
}

TEST_CASE("WS auth succeeds with a valid PAT", "[ws][integration][auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "bob", "password-123");
  auto raw_random = plinth::auth::generate_token();
  auto pat_id = plinth::ws_test::insert_pat(pg, user_id, raw_random);
  auto full_token = "plinth_" + raw_random;

  WsTestClient client;
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg(full_token));

  auto msg = client.receive_json(3s);
  REQUIRE(msg.has_value());
  REQUIRE((*msg)["type"].asString() == "connected");
  REQUIRE((*msg)["user"]["id"].asString() == user_id);
  REQUIRE((*msg)["pat_id"].asString() == pat_id);
  REQUIRE((*msg)["session_id"].asString().empty());
}

TEST_CASE("WS auth fails with an invalid token", "[ws][integration][auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  WsTestClient client;
  REQUIRE(client.connect(2s));
  client.send_json(auth_msg("not-a-real-token"));

  auto err = client.receive_json(3s);
  REQUIRE(err.has_value());
  REQUIRE((*err)["type"].asString() == "error");
  REQUIRE((*err)["error"].asString() == "auth_failed");

  REQUIRE(client.wait_for_close(2s));
}

TEST_CASE("WS auth times out if no message within ws_auth_timeout_s",
          "[ws][integration][auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }

  WsTestClient client;
  REQUIRE(client.connect(2s));
  // Send nothing; test config sets auth_timeout_s = 1.0.
  auto err = client.receive_json(3s);
  REQUIRE(err.has_value());
  REQUIRE((*err)["type"].asString() == "error");
  REQUIRE((*err)["error"].asString() == "auth_timeout");

  REQUIRE(client.wait_for_close(2s));
}

TEST_CASE("WS client owns connection completion after its caller times out",
          "[ws][integration][auth][client-timeout]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  WsTestClient client;
  bool timed_out = false;
  {
    struct ReleaseLoop {
      std::shared_ptr<std::promise<void>> release =
          std::make_shared<std::promise<void>>();
      ~ReleaseLoop() { release->set_value(); }
    } barrier;
    auto entered = std::make_shared<std::promise<void>>();
    auto entered_future = entered->get_future();
    auto release = barrier.release->get_future();
    // Both callback state and the unconditional release guard outlive a failed
    // assertion. Never leave Drogon's owning loop blocked on a test failure.
    drogon::app().getLoop()->queueInLoop([entered, release = release.share()] {
      entered->set_value();
      release.wait();
    });
    REQUIRE(entered_future.wait_for(2s) == std::future_status::ready);
    timed_out = !client.connect(0ms);
  }
  REQUIRE(timed_out);
  // This real auth-timeout frame proves the delayed handshake callback ran
  // after connect() returned, before the client destructor joins its stop.
  auto message = client.receive_json(3s);
  REQUIRE(message.has_value());
  REQUIRE((*message)["type"].asString() == "error");
  REQUIRE((*message)["error"].asString() == "auth_timeout");
  REQUIRE(client.wait_for_close(2s));
}

TEST_CASE("WS second connection displaces first with same session",
          "[ws][integration][auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);

  plinth::ws_test::TestPg pg(cfg.db);
  auto user_id = plinth::ws_test::insert_user(pg, "carol", "password-123");
  auto raw_token = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user_id, raw_token);

  WsTestClient c1;
  REQUIRE(c1.connect(2s));
  c1.send_json(auth_msg(raw_token));
  auto ok1 = c1.receive_json(3s);
  REQUIRE(ok1.has_value());
  REQUIRE((*ok1)["type"].asString() == "connected");

  // Second connection with same token displaces the first.
  WsTestClient c2;
  REQUIRE(c2.connect(2s));
  c2.send_json(auth_msg(raw_token));
  auto ok2 = c2.receive_json(3s);
  REQUIRE(ok2.has_value());
  REQUIRE((*ok2)["type"].asString() == "connected");

  // c1 should receive `{error: "already_connected"}` then close.
  auto evicted = c1.receive_json(3s);
  REQUIRE(evicted.has_value());
  REQUIRE((*evicted)["type"].asString() == "error");
  REQUIRE((*evicted)["error"].asString() == "already_connected");
  REQUIRE(c1.wait_for_close(2s));
}

TEST_CASE(
    "WS browser authenticates HttpOnly upgrade cookie before token frames",
    "[ws][integration][auth][browser-auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws_test::TestPg pg(cfg.db);
  auto browser_user =
      plinth::ws_test::insert_user(pg, "browser", "password-123");
  auto native_user = plinth::ws_test::insert_user(pg, "native", "password-123");
  auto cookie = plinth::auth::generate_token();
  auto other = plinth::auth::generate_token();
  auto session = plinth::ws_test::insert_session(pg, browser_user, cookie);
  plinth::ws_test::insert_session(pg, native_user, other);
  const auto origin =
      "http://127.0.0.1:" + std::to_string(plinth::ws_test::test_server_port());

  WsTestClient client;
  REQUIRE(client.connect(
      2s, {{"Origin", origin}, {"Cookie", "plinth_session=" + cookie}}));
  // A second identity must not race either validation or the RBAC lookup.
  client.send_json(auth_msg(other));
  auto connected = client.receive_json(3s);
  REQUIRE(connected.has_value());
  REQUIRE((*connected)["type"].asString() == "connected");
  REQUIRE((*connected)["user"]["id"].asString() == browser_user);
  REQUIRE((*connected)["session_id"].asString() == session);
  client.send_json(auth_msg(other));
  auto next = client.receive_json(3s);
  REQUIRE(next.has_value());
  REQUIRE((*next)["type"].asString() == "ping");
}

TEST_CASE(
    "WS browser cookie requires an exact actual Origin and a valid session",
    "[ws][integration][auth][browser-auth]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws_test::TestPg pg(cfg.db);
  auto user =
      plinth::ws_test::insert_user(pg, "browser-denied", "password-123");
  auto cookie = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user, cookie);
  const auto host =
      "127.0.0.1:" + std::to_string(plinth::ws_test::test_server_port());
  std::string origin = "http://" + host;
  SECTION("cross-origin host") {
    origin = "http://example.invalid";
  }
  SECTION("scheme mismatch ignores forwarded proto") {
    origin = "https://" + host;
  }
  SECTION("opaque origin") {
    origin = "null";
  }
  SECTION("missing origin") {
    origin = "";
  }
  SECTION("invalid cookie") {
    cookie = "invalid-test-cookie";
  }
  SECTION("missing cookie") {
    cookie = "";
  }
  SECTION("expired session") {
    auto result = pg.exec(
        "UPDATE plinth.sessions SET expires_at = NOW() - INTERVAL '1 second'");
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  SECTION("revoked session") {
    auto result = pg.exec("UPDATE plinth.sessions SET revoked_at = NOW()");
    REQUIRE(PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  }
  WsTestClient client;
  REQUIRE(client.connect(2s, {{"Origin", origin},
                              {"X-Forwarded-Proto", "https"},
                              {"X-Forwarded-Host", host},
                              {"Cookie", "plinth_session=" + cookie}}));
  auto error = client.receive_json(3s);
  REQUIRE(error.has_value());
  REQUIRE((*error)["type"].asString() == "error");
  REQUIRE((*error)["error"].asString() == "auth_failed");
  REQUIRE(client.wait_for_close(2s));
}

TEST_CASE(
    "WS proxy cookie auth uses configured HTTPS origin and preserved Host",
    "[ws][integration][auth][browser-proxy]") {
  if (!plinth::ws_test::pg_available()) {
    SKIP("PG not available");
  }
  auto cfg = plinth::ws_test::test_config();
  if (cfg.ws_browser_origin.empty()) {
    SKIP("Run the isolated plinth_tests_ws_browser_proxy CTest process");
  }
  plinth::ws_test::reset_schema(cfg.db);
  plinth::ws_test::TestPg pg(cfg.db);
  auto user = plinth::ws_test::insert_user(pg, "browser-proxy", "password-123");
  auto cookie = plinth::auth::generate_token();
  plinth::ws_test::insert_session(pg, user, cookie);
  std::string origin = cfg.ws_browser_origin;
  auto host = origin.substr(origin.find("://") + 3);
  bool accepted = true;
  bool mismatched_host = false;
  SECTION("HTTPS public origin over HTTP proxy upstream") {
  }
  SECTION("cross-origin rejected") {
    origin = "https://evil.example";
    accepted = false;
  }
  SECTION("actual HTTP origin cannot override configured HTTPS origin") {
    origin = "http://" + host;
    accepted = false;
  }
  SECTION("forwarded host cannot repair mismatched actual Host") {
    mismatched_host = true;
    accepted = false;
  }
  if (mismatched_host) {
    require_mismatched_host_rejected(origin, host, cookie);
    return;
  }
  WsTestClient client;
  REQUIRE(client.connect(2s, {{"Origin", origin},
                              {"X-Forwarded-Host", "127.0.0.1:28099"},
                              {"X-Forwarded-Proto", "https"},
                              {"Cookie", "plinth_session=" + cookie}}));
  auto message = client.receive_json(3s);
  REQUIRE(message.has_value());
  if (accepted) {
    REQUIRE((*message)["type"].asString() == "connected");
    REQUIRE((*message)["user"]["id"].asString() == user);
  } else {
    REQUIRE((*message)["type"].asString() == "error");
    REQUIRE((*message)["error"].asString() == "auth_failed");
    REQUIRE(client.wait_for_close(2s));
  }
}
