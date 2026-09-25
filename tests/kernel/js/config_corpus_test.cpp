// SPDX-License-Identifier: MIT
//
// Fixed-seed, bounded coverage of the production config.get registration.
// The expected projection is built here from a fake Config, without calling
// make_config_projection or reading the binding's lookup table.

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <json/value.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace std::chrono_literals;

namespace {

constexpr std::array<std::uint64_t, 4> SEEDS{
    0x1210'0000'0000'0001ULL, 0x1210'0000'0000'0026ULL,
    0x1210'0000'0000'00C4ULL, 0x1210'0000'0000'BEEFULL};
constexpr int CASES_PER_SEED = 48;
constexpr std::size_t MAX_KEY_CODEPOINTS = 96;
constexpr int MAX_SHRINK_ATTEMPTS = 24;
constexpr auto SHUTDOWN_BOUND = 250ms;

constexpr std::array<std::string_view, 8> ALLOWED{"dev_mode",
                                                  "node_id",
                                                  "listen_host",
                                                  "listen_port",
                                                  "registration_enabled",
                                                  "ws.auth_timeout_s",
                                                  "ws.heartbeat_interval_s",
                                                  "ws.heartbeat_timeout_s"};
// Explicitly reviewed negative keys cover the current operator-only Config
// groups and their JSON-path/source-field aliases. Adding a public projection
// key requires updating this denylist and the eight-key oracle deliberately.
constexpr auto EXCLUDED = std::to_array<std::string_view>({
    "database",
    "database.host",
    "database.port",
    "database.user",
    "database.password",
    "database.database",
    "database.pool_size",
    "db",
    "db.host",
    "db.port",
    "db.user",
    "db.password",
    "db.database",
    "db.pool_size",
    "migrations_dir",
    "bootstrap_token",
    "browser_origin",
    "ws_browser_origin",
    "ws",
    "ws.browser_origin",
    "registration",
    "registration.mode",
    "registration.max_accounts",
    "registration.source_attempts",
    "registration.subject_attempts",
    "registration.global_attempts",
    "registration.window_seconds",
    "registration.invite_ttl_seconds",
    "security",
    "security.unicode_scanner",
    "security.unicode_scanner.enabled",
    "security.unicode_scanner.threshold",
    "security.unicode_scanner.log_findings",
    "security_unicode_scanner_enabled",
    "security_unicode_scanner_threshold",
    "security_unicode_scanner_log_findings",
    "packages",
    "packages_data_dir",
    "packages_staging_dir",
    "packages_max_package_size_mb",
    "packages_upgrade_drain_timeout_ms",
    "packages.data_dir",
    "packages.staging_dir",
    "packages.max_package_size_mb",
    "packages.upgrade_drain_timeout_ms",
    "realtime",
    "realtime.listener",
    "realtime.listener.enabled",
    "realtime.listener.reconnect_backoff_ms",
    "realtime.notify",
    "realtime.notify.max_payload_bytes",
    "realtime.coalescer",
    "realtime.coalescer.enabled",
    "realtime.coalescer.window_ms",
    "realtime.broker",
    "realtime.broker.enabled",
    "realtime.broker.max_subscriptions_per_conn",
    "realtime.broker.rbac_enforce",
    "realtime.events",
    "realtime.events.enabled",
    "realtime.events.retention_seconds",
    "realtime.events.cleanup_interval_ms",
    "realtime.events.replay_max_rows_per_chunk",
    "realtime.events.replay_max_total_rows",
    "realtime.events.write_queue_size",
    "realtime.events.shutdown_drain_ms",
    "realtime.events.cursor_cache_ttl_ms",
    "realtime.events.cursor_flush_threshold",
    "realtime.events.audit_window_ms",
    "realtime.events.seq",
    "realtime.events.seq.source",
    "realtime.events.seq.gap_audit_window_ms",
    "realtime.events.live_buffer_cap_per_subscription",
    "realtime.events.coalesce",
    "realtime.events.coalesce.emit_superseded_seqs",
    "realtime.events.debounce",
    "realtime.events.debounce.recommend_ms",
    "realtime.events.debounce.jitter_max_ms",
    "db_bindings",
    "db_bindings.oid_mapping",
    "db_bindings.oid_mapping.enabled",
    "db_bindings.silent",
    "db_bindings.silent.audit_window_ms",
    "db_bindings.search_path",
    "db_bindings.search_path.enforce",
    "db_bindings.batch",
    "db_bindings.batch.max_ops_per_batch",
    "db_bindings.batch.max_concurrent_batches_per_bc",
    "db_bindings.batch.timeout_ms",
    "db_bindings.batch.audit_window_ms",
    "db.oid_mapping",
    "db.oid_mapping.enabled",
    "db.silent",
    "db.silent.audit_window_ms",
    "db.search_path",
    "db.search_path.enforce",
    "db.batch",
    "db.batch.max_ops_per_batch",
    "db.batch.max_concurrent_batches_per_bc",
    "db.batch.timeout_ms",
    "db.batch.audit_window_ms",
    "shell",
    "shell.enabled",
    "shell.root_redirect",
    "shell.bundle_path",
});

enum class Kind {
  exact,
  unknown,
  case_variant,
  prefix,
  nested,
  nul,
  secret,
  missing,
  wrong,
  extra
};

struct CorpusCase {
  Kind kind = Kind::exact;
  std::u32string key;
  int variant = 0;
};

auto next(std::uint64_t& state) -> std::uint64_t {
  state += 0x9e37'79b9'7f4a'7c15ULL;
  auto value = state;
  value = (value ^ (value >> 30U)) * 0xbf58'476d'1ce4'e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d0'49bb'1331'11ebULL;
  return value ^ (value >> 31U);
}

auto codepoints(std::string_view ascii) -> std::u32string {
  std::u32string result;
  for (const auto ch : ascii) {
    result.push_back(static_cast<unsigned char>(ch));
  }
  return result;
}

auto fake_config(std::uint64_t seed) -> plinth::Config {
  plinth::Config config{};
  config.dev_mode = (seed & 1U) != 0;
  config.node_id = "fake-node-" + std::to_string(seed % 1000U);
  config.listen_host = "192.0.2." + std::to_string(1U + seed % 200U);
  config.listen_port = static_cast<std::uint16_t>(10000U + seed % 50000U);
  config.registration.mode = seed % 3U == 0
                                 ? plinth::Config::Registration::Mode::OPEN
                                 : plinth::Config::Registration::Mode::INVITE;
  config.ws_auth_timeout_s = 1.25 + static_cast<double>(seed % 5U);
  config.ws_heartbeat_interval_s = 7.5 + static_cast<double>(seed % 7U);
  config.ws_heartbeat_timeout_s = 2.75 + static_cast<double>(seed % 3U);
  // Selected excluded fields use conspicuous synthetic values; the rest keep
  // harmless defaults. Every excluded key must return null either way.
  config.db.host = "excluded-db-host.invalid";
  config.db.port = 6543;
  config.db.user = "excluded-db-user";
  config.db.password = "fake-excluded-password";
  config.db.database = "excluded-db-name";
  config.db.pool_size = 123;
  config.migrations_dir = "excluded-migrations-dir";
  config.bootstrap_token = "fake-excluded-token";
  config.browser_origin = "https://excluded.example.invalid";
  config.ws_browser_origin = "https://excluded-alias.example.invalid";
  config.registration.max_accounts = 98765;
  config.packages_data_dir = "excluded-packages-dir";
  return config;
}

auto oracle(const plinth::Config& config, std::u32string_view key)
    -> Json::Value {
  if (key == U"dev_mode") {
    return config.dev_mode;
  }
  if (key == U"node_id") {
    return config.node_id;
  }
  if (key == U"listen_host") {
    return config.listen_host;
  }
  if (key == U"listen_port") {
    return config.listen_port;
  }
  if (key == U"registration_enabled") {
    return config.registration.mode == plinth::Config::Registration::Mode::OPEN;
  }
  if (key == U"ws.auth_timeout_s") {
    return config.ws_auth_timeout_s;
  }
  if (key == U"ws.heartbeat_interval_s") {
    return config.ws_heartbeat_interval_s;
  }
  if (key == U"ws.heartbeat_timeout_s") {
    return config.ws_heartbeat_timeout_s;
  }
  return Json::Value{};
}

auto generated_case(std::uint64_t seed, int index) -> CorpusCase {
  std::uint64_t state =
      seed ^ (static_cast<std::uint64_t>(index) * 0x9e37'79b9'7f4a'7c15ULL);
  auto item = CorpusCase{};
  item.variant = static_cast<int>(next(state) % 8U);
  item.key =
      codepoints(ALLOWED[static_cast<std::size_t>(index) % ALLOWED.size()]);
  if (index < static_cast<int>(ALLOWED.size())) {
    return item; // Every seed exercises all eight exact keys.
  }
  switch ((index - static_cast<int>(ALLOWED.size())) % 9) {
    case 0: {
      item.kind = Kind::unknown;
      item.key = U"unknown_";
      constexpr std::array<char32_t, 7> chars{
          U'a', U'Z', U'_', U'0', U'\u00e9', U'\u4e2d', U'\U0001f600'};
      const auto length = static_cast<std::size_t>(next(state) % 80U);
      for (std::size_t i = 0; i < length; ++i) {
        item.key.push_back(chars[next(state) % chars.size()]);
      }
      break;
    }
    case 1:
      item.kind = Kind::case_variant;
      item.key[0] -= U'a' - U'A';
      break;
    case 2:
      item.kind = Kind::prefix;
      item.key.insert(0, U"prefix.");
      break;
    case 3:
      item.kind = Kind::nested;
      item.key += U".nested";
      break;
    case 4:
      item.kind = Kind::nul;
      item.key.push_back(U'\0');
      item.key += U"suffix";
      break;
    case 5:
      item.kind = Kind::secret;
      item.key = codepoints(EXCLUDED[next(state) % EXCLUDED.size()]);
      break;
    case 6: item.kind = Kind::missing; break;
    case 7: item.kind = Kind::wrong; break;
    case 8: item.kind = Kind::extra; break;
    default: break;
  }
  return item;
}

auto js_string(std::u32string_view key) -> std::string {
  std::string script = "String.fromCodePoint(";
  for (std::size_t i = 0; i < key.size(); ++i) {
    if (i != 0) {
      script += ',';
    }
    script += std::to_string(static_cast<std::uint32_t>(key[i]));
  }
  script += ')';
  return script;
}

auto expression(const CorpusCase& item) -> std::string {
  if (item.kind == Kind::missing) {
    return "config.get()";
  }
  if (item.kind == Kind::wrong) {
    constexpr std::array<std::string_view, 8> WRONG{
        "null", "true",      "42",  "({})",
        "([])", "undefined", "0.5", "Symbol('key')"};
    return "config.get(" + std::string{WRONG.at(item.variant)} + ')';
  }
  auto call = "config.get(" + js_string(item.key);
  if (item.kind == Kind::extra) {
    call += ",new Proxy({}, {get() { throw Error('unused extra arg'); }})";
  }
  return call + ')';
}

auto script(const CorpusCase& item) -> std::string {
  return "(() => { try { const value = " + expression(item) +
         "; return {type: value === null ? 'null' : typeof value, value};"
         " } catch (error) { return {error: error.name}; } })()";
}

auto expected(const plinth::Config& config, const CorpusCase& item)
    -> Json::Value {
  Json::Value result(Json::objectValue);
  if (item.kind == Kind::missing || item.kind == Kind::wrong) {
    result["error"] = "TypeError";
    return result;
  }
  auto value = oracle(config, item.key);
  result["type"] = value.isNull()     ? "null"
                   : value.isBool()   ? "boolean"
                   : value.isString() ? "string"
                                      : "number";
  result["value"] = std::move(value);
  return result;
}

struct Failure {
  std::string message;
  std::string signature;
};

auto discrepancy(plinth::js::BridgeContext& context,
                 const plinth::Config& config, const CorpusCase& item)
    -> std::optional<Failure> {
  auto result = plinth::js::eval_on_context(context, script(item));
  const auto want = expected(config, item);
  if (!result.has_value()) {
    return Failure{
        .message = "host eval failed: " + result.error().message,
        .signature =
            "host_eval:" + std::to_string(static_cast<int>(item.kind)) + ':' +
            std::to_string(static_cast<int>(result.error().kind))};
  }
  if (*result != want) {
    return Failure{
        .message = "expected " + want.toStyledString() + " got " +
                   result->toStyledString(),
        .signature =
            "value_mismatch:" + std::to_string(static_cast<int>(item.kind)) +
            ':' + want.toStyledString() + ':' + result->toStyledString()};
  }
  return std::nullopt;
}

auto shrink(CorpusCase item, const plinth::Config& config,
            std::string_view signature) -> std::pair<CorpusCase, int> {
  int attempts = 0;
  // Only the generated unknown-key family can be shortened without changing
  // what the input means. Exact, excluded, case/prefix/path, and NUL keys
  // remain their own minimal semantic regressions.
  constexpr std::size_t UNKNOWN_PREFIX_LENGTH = 8;
  while (item.kind == Kind::unknown && attempts < MAX_SHRINK_ATTEMPTS &&
         item.key.size() > UNKNOWN_PREFIX_LENGTH) {
    auto candidate = item;
    candidate.key.resize(UNKNOWN_PREFIX_LENGTH +
                         (item.key.size() - UNKNOWN_PREFIX_LENGTH) / 2);
    ++attempts;
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    auto* context = pool.acquire();
    if (context == nullptr) {
      break;
    }
    auto failure = discrepancy(*context, config, candidate);
    pool.destroy(context);
    if (!pool.shutdown(SHUTDOWN_BOUND)) {
      break;
    }
    if (!failure.has_value() || failure->signature != signature) {
      break;
    }
    item = std::move(candidate);
  }
  return {std::move(item), attempts};
}

auto assert_case(plinth::js::BridgeContext& context,
                 const plinth::Config& config, const CorpusCase& item) -> void {
  REQUIRE(item.key.size() <= MAX_KEY_CODEPOINTS);
  auto failure = discrepancy(context, config, item);
  if (failure.has_value()) {
    auto [minimal, attempts] = shrink(item, config, failure->signature);
    INFO("expression=" << expression(item) << " signature="
                       << failure->signature << " shrink_attempts=" << attempts
                       << " minimal=" << expression(minimal));
    FAIL(failure->message);
  }
}

} // namespace

TEST_CASE("QuickJS config.get projection has a bounded deterministic corpus",
          "[js][stdlib][config][corpus]") {
  INFO("seed_count=" << SEEDS.size() << " cases_per_seed=" << CASES_PER_SEED
                     << " max_key_codepoints=" << MAX_KEY_CODEPOINTS
                     << " max_shrink_attempts=" << MAX_SHRINK_ATTEMPTS
                     << " shutdown_ms=" << SHUTDOWN_BOUND.count());
  for (const auto seed : SEEDS) {
    INFO("seed=" << seed);
    const auto config = fake_config(seed);
    plinth::js::RuntimePool pool(nullptr, plinth::js::default_runtime_limits(),
                                 config, 1);
    for (int index = 0; index < CASES_PER_SEED; ++index) {
      INFO("case=" << index);
      auto item = generated_case(seed, index);
      auto* context = pool.acquire();
      REQUIRE(context != nullptr);
      assert_case(*context, config, item);
      if (index % 3 == 0) {
        pool.destroy(context);
      } else {
        pool.release(context);
      }
      if (index % 8 == 7) {
        pool.rebuild();
      }
    }
    // Exhaustively check the named secret/operator exclusions and all wrong
    // JS types; the generated corpus adds varied lifecycle and key shapes.
    for (const auto excluded : EXCLUDED) {
      auto* context = pool.acquire();
      REQUIRE(context != nullptr);
      assert_case(
          *context, config,
          CorpusCase{.kind = Kind::secret, .key = codepoints(excluded)});
      pool.release(context);
    }
    for (int variant = 0; variant < 8; ++variant) {
      auto* context = pool.acquire();
      REQUIRE(context != nullptr);
      assert_case(*context, config,
                  CorpusCase{.kind = Kind::wrong, .variant = variant});
      pool.destroy(context);
    }
    REQUIRE(pool.active_count() == 0);
    auto* held = pool.acquire();
    REQUIRE(held != nullptr);
    REQUIRE_FALSE(pool.shutdown(25ms));
    pool.destroy(held);
    REQUIRE(pool.shutdown(SHUTDOWN_BOUND));
    REQUIRE(pool.acquire() == nullptr);
  }
}
