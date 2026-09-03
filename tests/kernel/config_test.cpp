#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "kernel/config.hpp"

// Helper: write a temp config file, return its path. Includes PID so
// parallel ctest invocations (ctest -jN) don't collide on the same path.
static auto write_temp_config(const nlohmann::json& j) -> std::string {
  static std::atomic<uint64_t> counter{0};
  auto path = "test_config_" + std::to_string(::getpid()) + "_" +
              std::to_string(counter.fetch_add(1)) + ".json";
  std::ofstream f(path);
  f << j.dump();
  f.close();
  return path;
}

// Helper: remove a temp file
static auto remove_file(const std::string& path) -> void {
  (void)std::remove(path.c_str());
}

// Helper: set or unset an env var
static auto set_env(const char* name, const char* value) -> void {
  if (value != nullptr) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
}

// Env vars that load_config reads — must be cleared for unit tests
static constexpr std::array<const char*, 10> PLINTH_ENV_VARS = {
    "PLINTH_PG_HOST",
    "PLINTH_PG_PORT",
    "PLINTH_PG_USER",
    "PLINTH_PG_PASSWORD",
    "PLINTH_PG_DATABASE",
    "PLINTH_PG_POOL_SIZE",
    "PLINTH_MIGRATIONS_DIR",
    "PLINTH_DEV_MODE",
    "PLINTH_REGISTRATION_ENABLED",
    "PLINTH_NODE_ID"};

// RAII guard: saves and clears PLINTH_* env vars, restores on destruction
struct EnvGuard {
  std::vector<std::pair<std::string, std::string>> saved;

  EnvGuard() {
    for (const auto* name : PLINTH_ENV_VARS) {
      const char* val = std::getenv(name);
      if (val != nullptr) {
        saved.emplace_back(name, val);
        unsetenv(name);
      }
    }
  }

  ~EnvGuard() {
    for (const auto& [name, val] : saved) {
      setenv(name.c_str(), val.c_str(), 1);
    }
  }

  EnvGuard(const EnvGuard&) = delete;
  auto operator=(const EnvGuard&) -> EnvGuard& = delete;
  EnvGuard(EnvGuard&&) = delete;
  auto operator=(EnvGuard&&) -> EnvGuard& = delete;
};

TEST_CASE("Config defaults are correct", "[config][unit]") {
  EnvGuard guard;

  auto cfg = plinth::load_config();

  REQUIRE(cfg.db.host == "localhost");
  REQUIRE(cfg.db.port == 5432);
  REQUIRE(cfg.db.user == "plinth");
  REQUIRE(cfg.db.password == "plinth");
  REQUIRE(cfg.db.database == "plinth");
  // 0.3.3 bumped pool_size default 4 → 32 to satisfy ICD-0.3.3
  // §Back-Pressure formula (runtime_pool_size * max_concurrent_async_ops
  // = 4 * 8 = 32). See config.hpp comment + config.json.example.
  REQUIRE(cfg.db.pool_size == 32);
  REQUIRE(cfg.migrations_dir == "./migrations");
  REQUIRE(cfg.dev_mode == false);
  REQUIRE(cfg.listen_host == "127.0.0.1");
  REQUIRE(cfg.listen_port == 8080);
  REQUIRE(cfg.registration_enabled == false);
  REQUIRE(cfg.node_id == "node-1");
}

TEST_CASE("Config loads from JSON file", "[config][unit]") {
  EnvGuard guard;

  nlohmann::json j = {{"database",
                       {{"host", "db.example.com"},
                        {"port", 5433},
                        {"user", "testuser"},
                        {"password", "testpass"},
                        {"database", "testdb"},
                        {"pool_size", 8}}},
                      {"migrations_dir", "/opt/migrations"},
                      {"dev_mode", true},
                      {"listen_host", "127.0.0.1"},
                      {"listen_port", 9090}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.db.host == "db.example.com");
  REQUIRE(cfg.db.port == 5433);
  REQUIRE(cfg.db.user == "testuser");
  REQUIRE(cfg.db.password == "testpass");
  REQUIRE(cfg.db.database == "testdb");
  REQUIRE(cfg.db.pool_size == 8);
  REQUIRE(cfg.migrations_dir == "/opt/migrations");
  REQUIRE(cfg.dev_mode == true);
  REQUIRE(cfg.listen_host == "127.0.0.1");
  REQUIRE(cfg.listen_port == 9090);
}

TEST_CASE("Config partial JSON — missing fields keep defaults",
          "[config][unit]") {
  EnvGuard guard;

  nlohmann::json j = {{"database", {{"host", "custom-host"}}}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.db.host == "custom-host");
  REQUIRE(cfg.db.port == 5432);     // default preserved
  REQUIRE(cfg.db.user == "plinth"); // default preserved
}

TEST_CASE("Env vars override JSON config", "[config][unit]") {
  nlohmann::json j = {{"database", {{"host", "from-json"}, {"port", 1111}}}};

  auto path = write_temp_config(j);

  // Set env var overrides
  set_env("PLINTH_PG_HOST", "from-env");
  set_env("PLINTH_PG_PORT", "2222");

  auto cfg = plinth::load_config(path);

  // Clean up
  set_env("PLINTH_PG_HOST", nullptr);
  set_env("PLINTH_PG_PORT", nullptr);
  remove_file(path);

  REQUIRE(cfg.db.host == "from-env"); // env wins over json
  REQUIRE(cfg.db.port == 2222);       // env wins over json
}

TEST_CASE("PLINTH_PG_POOL_SIZE overrides the default", "[config][unit]") {
  EnvGuard guard;

  set_env("PLINTH_PG_POOL_SIZE", "64");
  auto cfg = plinth::load_config();
  set_env("PLINTH_PG_POOL_SIZE", nullptr);

  REQUIRE(cfg.db.pool_size == 64);
}

TEST_CASE("PLINTH_DEV_MODE env var", "[config][unit]") {
  set_env("PLINTH_DEV_MODE", "true");
  auto cfg = plinth::load_config();
  set_env("PLINTH_DEV_MODE", nullptr);

  REQUIRE(cfg.dev_mode == true);
}

TEST_CASE("packages.upgrade_drain_timeout_ms from JSON", "[config][unit]") {
  EnvGuard guard;

  nlohmann::json j = {{"packages", {{"upgrade_drain_timeout_ms", 12345}}}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.packages_upgrade_drain_timeout_ms == 12345);
}

TEST_CASE("packages.upgrade_drain_timeout_ms default is 5000",
          "[config][unit]") {
  EnvGuard guard;
  auto cfg = plinth::load_config();
  REQUIRE(cfg.packages_upgrade_drain_timeout_ms == 5000);
}

TEST_CASE("PLINTH_DEV_MODE=1 also works", "[config][unit]") {
  set_env("PLINTH_DEV_MODE", "1");
  auto cfg = plinth::load_config();
  set_env("PLINTH_DEV_MODE", nullptr);

  REQUIRE(cfg.dev_mode == true);
}

TEST_CASE("registration_enabled and node_id from JSON", "[config][unit]") {
  EnvGuard guard;

  nlohmann::json j = {{"registration_enabled", false}, {"node_id", "node-42"}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.registration_enabled == false);
  REQUIRE(cfg.node_id == "node-42");
}

TEST_CASE("PLINTH_REGISTRATION_ENABLED env var", "[config][unit]") {
  EnvGuard guard;

  set_env("PLINTH_REGISTRATION_ENABLED", "false");
  auto cfg = plinth::load_config();
  set_env("PLINTH_REGISTRATION_ENABLED", nullptr);

  REQUIRE(cfg.registration_enabled == false);
}

TEST_CASE("PLINTH_NODE_ID env var", "[config][unit]") {
  EnvGuard guard;

  set_env("PLINTH_NODE_ID", "node-env");
  auto cfg = plinth::load_config();
  set_env("PLINTH_NODE_ID", nullptr);

  REQUIRE(cfg.node_id == "node-env");
}

TEST_CASE("Explicitly missing config fails closed", "[config][unit]") {
  EnvGuard guard;

  REQUIRE_THROWS_WITH(
      plinth::load_config("__nonexistent__.json"),
      Catch::Matchers::ContainsSubstring("config.file_unreadable"));
}

TEST_CASE("Malformed JSON fails closed", "[config][unit]") {
  EnvGuard guard;

  auto path = std::string{"test_bad_json.json"};
  std::ofstream f(path);
  f << "{ not valid json }}}";
  f.close();

  REQUIRE_THROWS_WITH(
      plinth::load_config(path),
      Catch::Matchers::ContainsSubstring("config.file_invalid"));
  remove_file(path);
}

TEST_CASE("Non-object JSON config fails closed", "[config][unit]") {
  EnvGuard guard;

  auto path = write_temp_config(nlohmann::json::array({1, 2, 3}));
  REQUIRE_THROWS_WITH(
      plinth::load_config(path),
      Catch::Matchers::ContainsSubstring("config.root_not_object"));
  remove_file(path);
}

// ── ICD-0.5.0 §Config Surface ────────────────────────────────────────

TEST_CASE("Realtime config defaults", "[config][unit][realtime]") {
  EnvGuard guard;
  auto cfg = plinth::load_config();

  REQUIRE(cfg.realtime.listener.enabled == true);
  REQUIRE(cfg.realtime.listener.reconnect_backoff_ms == 1000);
  REQUIRE(cfg.realtime.notify.max_payload_bytes == 8000);
}

TEST_CASE("Realtime config from JSON", "[config][unit][realtime]") {
  EnvGuard guard;

  nlohmann::json j = {
      {"realtime",
       {{"listener", {{"enabled", false}, {"reconnect_backoff_ms", 250}}},
        {"notify", {{"max_payload_bytes", 4096}}}}}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.realtime.listener.enabled == false);
  REQUIRE(cfg.realtime.listener.reconnect_backoff_ms == 250);
  REQUIRE(cfg.realtime.notify.max_payload_bytes == 4096);
}

TEST_CASE("realtime.listener.reconnect_backoff_ms out of range rejects",
          "[config][unit][realtime]") {
  EnvGuard guard;

  SECTION("below 100") {
    nlohmann::json j = {
        {"realtime", {{"listener", {{"reconnect_backoff_ms", 50}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "reconnect_backoff_ms_out_of_range"));
    remove_file(path);
  }

  SECTION("above 60000") {
    nlohmann::json j = {
        {"realtime", {{"listener", {{"reconnect_backoff_ms", 120000}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "reconnect_backoff_ms_out_of_range"));
    remove_file(path);
  }
}

TEST_CASE("realtime.notify.max_payload_bytes invalid rejects",
          "[config][unit][realtime]") {
  EnvGuard guard;

  SECTION("zero") {
    nlohmann::json j = {{"realtime", {{"notify", {{"max_payload_bytes", 0}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(
        plinth::load_config(path),
        Catch::Matchers::ContainsSubstring("max_payload_bytes_invalid"));
    remove_file(path);
  }

  SECTION("negative") {
    nlohmann::json j = {
        {"realtime", {{"notify", {{"max_payload_bytes", -1}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(
        plinth::load_config(path),
        Catch::Matchers::ContainsSubstring("max_payload_bytes_invalid"));
    remove_file(path);
  }

  SECTION("above 8000") {
    nlohmann::json j = {
        {"realtime", {{"notify", {{"max_payload_bytes", 10000}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(
        plinth::load_config(path),
        Catch::Matchers::ContainsSubstring("max_payload_bytes_invalid"));
    remove_file(path);
  }
}

// ── ICD-0.5.5 §10 — seq + live_buffer + coalesce + debounce ───────────

TEST_CASE("Realtime events 0.5.5 config defaults",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  auto cfg = plinth::load_config();

  REQUIRE(cfg.realtime.events.seq.source ==
          plinth::Config::Realtime::Events::SeqSource::WRITER_RETURNING);
  REQUIRE(cfg.realtime.events.seq.gap_audit_window_ms == 60000);
  REQUIRE(cfg.realtime.events.live_buffer_cap_per_subscription == 256);
  REQUIRE(cfg.realtime.events.coalesce.emit_superseded_seqs == false);
  REQUIRE(cfg.realtime.events.debounce.recommend_ms == 100);
  REQUIRE(cfg.realtime.events.debounce.jitter_max_ms == 50);
}

TEST_CASE("Realtime events 0.5.5 config from JSON",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  nlohmann::json j = {
      {"realtime",
       {{"events",
         {{"seq",
           {{"source", "writer_returning"}, {"gap_audit_window_ms", 30000}}},
          {"live_buffer_cap_per_subscription", 1024},
          {"coalesce", {{"emit_superseded_seqs", true}}},
          {"debounce", {{"recommend_ms", 250}, {"jitter_max_ms", 0}}}}}}}};

  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);

  REQUIRE(cfg.realtime.events.seq.gap_audit_window_ms == 30000);
  REQUIRE(cfg.realtime.events.live_buffer_cap_per_subscription == 1024);
  REQUIRE(cfg.realtime.events.coalesce.emit_superseded_seqs == true);
  REQUIRE(cfg.realtime.events.debounce.recommend_ms == 250);
  REQUIRE(cfg.realtime.events.debounce.jitter_max_ms == 0);
}

TEST_CASE("realtime.events.seq.source rejects unknown variants",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  nlohmann::json j = {
      {"realtime",
       {{"events", {{"seq", {{"source", "nextval_preallocate"}}}}}}}};
  auto path = write_temp_config(j);
  REQUIRE_THROWS_WITH(plinth::load_config(path),
                      Catch::Matchers::ContainsSubstring("seq.source_unknown"));
  remove_file(path);
}

TEST_CASE("realtime.events.seq.gap_audit_window_ms out of range rejects",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  SECTION("below 1000") {
    nlohmann::json j = {
        {"realtime", {{"events", {{"seq", {{"gap_audit_window_ms", 500}}}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "seq.gap_audit_window_ms_out_of_range"));
    remove_file(path);
  }
  SECTION("above 3600000") {
    nlohmann::json j = {
        {"realtime",
         {{"events", {{"seq", {{"gap_audit_window_ms", 7200000}}}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "seq.gap_audit_window_ms_out_of_range"));
    remove_file(path);
  }
}

TEST_CASE("realtime.events.live_buffer_cap_per_subscription out of range "
          "rejects",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  SECTION("below 16") {
    nlohmann::json j = {
        {"realtime", {{"events", {{"live_buffer_cap_per_subscription", 8}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "live_buffer_cap_per_subscription_out_of_range"));
    remove_file(path);
  }
  SECTION("above 65536") {
    nlohmann::json j = {
        {"realtime",
         {{"events", {{"live_buffer_cap_per_subscription", 131072}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "live_buffer_cap_per_subscription_out_of_range"));
    remove_file(path);
  }
}

TEST_CASE("realtime.events.debounce.* out of range rejects",
          "[config][unit][realtime][events]") {
  EnvGuard guard;
  SECTION("recommend_ms above 60000") {
    nlohmann::json j = {
        {"realtime", {{"events", {{"debounce", {{"recommend_ms", 120000}}}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "debounce.recommend_ms_out_of_range"));
    remove_file(path);
  }
  SECTION("jitter_max_ms above 5000") {
    nlohmann::json j = {
        {"realtime", {{"events", {{"debounce", {{"jitter_max_ms", 10000}}}}}}}};
    auto path = write_temp_config(j);
    REQUIRE_THROWS_WITH(plinth::load_config(path),
                        Catch::Matchers::ContainsSubstring(
                            "debounce.jitter_max_ms_out_of_range"));
    remove_file(path);
  }
}

// ── ICD-0.6.0 §9 — frontend shell config ─────────────────────────────

TEST_CASE("Shell config defaults", "[config][unit][shell]") {
  EnvGuard guard;
  auto cfg = plinth::load_config();
  REQUIRE(cfg.shell.enabled == true);
  REQUIRE(cfg.shell.root_redirect == "/app/");
}

TEST_CASE("Shell config from JSON", "[config][unit][shell]") {
  EnvGuard guard;
  nlohmann::json j = {
      {"shell", {{"enabled", false}, {"root_redirect", "/console/"}}}};
  auto path = write_temp_config(j);
  auto cfg = plinth::load_config(path);
  remove_file(path);
  REQUIRE(cfg.shell.enabled == false);
  REQUIRE(cfg.shell.root_redirect == "/console/");
}

TEST_CASE("Shell.root_redirect invalid pattern falls back to /app/",
          "[config][unit][shell]") {
  EnvGuard guard;
  SECTION("missing trailing slash") {
    nlohmann::json j = {{"shell", {{"root_redirect", "/console"}}}};
    auto path = write_temp_config(j);
    auto cfg = plinth::load_config(path);
    remove_file(path);
    REQUIRE(cfg.shell.root_redirect == "/app/");
  }
  SECTION("missing leading slash") {
    nlohmann::json j = {{"shell", {{"root_redirect", "console/"}}}};
    auto path = write_temp_config(j);
    auto cfg = plinth::load_config(path);
    remove_file(path);
    REQUIRE(cfg.shell.root_redirect == "/app/");
  }
  SECTION("nested segments") {
    nlohmann::json j = {{"shell", {{"root_redirect", "/a/b/"}}}};
    auto path = write_temp_config(j);
    auto cfg = plinth::load_config(path);
    remove_file(path);
    REQUIRE(cfg.shell.root_redirect == "/app/");
  }
  SECTION("empty string") {
    nlohmann::json j = {{"shell", {{"root_redirect", ""}}}};
    auto path = write_temp_config(j);
    auto cfg = plinth::load_config(path);
    remove_file(path);
    REQUIRE(cfg.shell.root_redirect == "/app/");
  }
}
