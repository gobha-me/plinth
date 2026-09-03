#include "kernel/config.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>

namespace plinth {

namespace {

// Read an env var, returning empty string if unset
auto env(const char* name) -> std::string {
  // getenv is the correct C API for reading environment variables
  const char* val = std::getenv(name);
  return val != nullptr ? std::string(val) : std::string{};
}

auto apply_security(Config& cfg, const nlohmann::json& sec) -> void {
  if (!sec.contains("unicode_scanner") || !sec["unicode_scanner"].is_object()) {
    return;
  }
  const auto& us = sec["unicode_scanner"];
  if (us.contains("enabled")) {
    cfg.security_unicode_scanner_enabled = us["enabled"].get<bool>();
  }
  if (us.contains("threshold")) {
    cfg.security_unicode_scanner_threshold = us["threshold"].get<std::size_t>();
  }
  if (us.contains("log_findings")) {
    cfg.security_unicode_scanner_log_findings = us["log_findings"].get<bool>();
  }
}

auto apply_database(Config& cfg, const nlohmann::json& db) -> void {
  if (db.contains("host")) {
    cfg.db.host = db["host"].get<std::string>();
  }
  if (db.contains("port")) {
    cfg.db.port = db["port"].get<uint16_t>();
  }
  if (db.contains("user")) {
    cfg.db.user = db["user"].get<std::string>();
  }
  if (db.contains("password")) {
    cfg.db.password = db["password"].get<std::string>();
  }
  if (db.contains("database")) {
    cfg.db.database = db["database"].get<std::string>();
  }
  if (db.contains("pool_size")) {
    cfg.db.pool_size = db["pool_size"].get<int>();
  }
}

auto apply_ws(Config& cfg, const nlohmann::json& j) -> void {
  if (j.contains("ws_auth_timeout_s")) {
    cfg.ws_auth_timeout_s = j["ws_auth_timeout_s"].get<double>();
  }
  if (j.contains("ws_heartbeat_interval_s")) {
    cfg.ws_heartbeat_interval_s = j["ws_heartbeat_interval_s"].get<double>();
  }
  if (j.contains("ws_heartbeat_timeout_s")) {
    cfg.ws_heartbeat_timeout_s = j["ws_heartbeat_timeout_s"].get<double>();
  }
}

auto apply_packages(Config& cfg, const nlohmann::json& p) -> void {
  if (p.contains("data_dir")) {
    cfg.packages_data_dir = p["data_dir"].get<std::string>();
  }
  if (p.contains("staging_dir")) {
    cfg.packages_staging_dir = p["staging_dir"].get<std::string>();
  }
  if (p.contains("max_package_size_mb")) {
    cfg.packages_max_package_size_mb =
        p["max_package_size_mb"].get<std::size_t>();
  }
  if (p.contains("upgrade_drain_timeout_ms")) {
    cfg.packages_upgrade_drain_timeout_ms =
        p["upgrade_drain_timeout_ms"].get<std::size_t>();
  }
}

// ICD-0.5.0 §Config Surface + §Error Model — realtime block. Unlike
// the other apply_* helpers this one REJECTS on out-of-range values
// (propagating std::runtime_error to load_config's caller) because the
// ICD error taxonomy demands a hard fail at startup on invalid
// realtime config. Other apply_* helpers warn-and-default; this one is
// a deliberate new convention for the realtime block (noted in the
// 0.5.0 CHANGELOG entry).

auto apply_realtime_listener(Config& cfg, const nlohmann::json& l) -> void {
  if (l.contains("enabled")) {
    cfg.realtime.listener.enabled = l["enabled"].get<bool>();
  }
  if (l.contains("reconnect_backoff_ms")) {
    auto ms = l["reconnect_backoff_ms"].get<int>();
    if (ms < 100 || ms > 60000) {
      throw std::runtime_error(
          "config.realtime.listener.reconnect_backoff_ms_out_of_range: " +
          std::to_string(ms));
    }
    cfg.realtime.listener.reconnect_backoff_ms = ms;
  }
}

auto apply_realtime_notify(Config& cfg, const nlohmann::json& n) -> void {
  if (n.contains("max_payload_bytes")) {
    // Parse as signed so negative / zero values are rejected with
    // the ICD error code rather than nlohmann's type_error.
    auto bytes = n["max_payload_bytes"].get<long long>();
    if (bytes <= 0 || bytes > 8000) {
      throw std::runtime_error(
          "config.realtime.notify.max_payload_bytes_invalid: " +
          std::to_string(bytes));
    }
    cfg.realtime.notify.max_payload_bytes = static_cast<std::size_t>(bytes);
  }
}

// ICD-0.5.1 §Config loader extension.
auto apply_realtime_coalescer(Config& cfg, const nlohmann::json& c) -> void {
  if (c.contains("enabled")) {
    cfg.realtime.coalescer.enabled = c["enabled"].get<bool>();
  }
  if (c.contains("window_ms")) {
    auto ms = c["window_ms"].get<long long>();
    if (ms < 1 || ms > 10000) {
      throw std::runtime_error(
          "config.realtime.coalescer.window_ms_out_of_range: " +
          std::to_string(ms));
    }
    cfg.realtime.coalescer.window_ms = static_cast<std::size_t>(ms);
  }
}

// ICD-0.5.2 §Config Surface — broker block. Same hard-fail posture as
// the listener / notify / coalescer blocks: out-of-range values throw
// at startup. Unknown sub-keys are warn-logged + ignored (forward-compat).
auto apply_realtime_broker(Config& cfg, const nlohmann::json& b) -> void {
  if (b.contains("enabled")) {
    cfg.realtime.broker.enabled = b["enabled"].get<bool>();
  }
  if (b.contains("max_subscriptions_per_conn")) {
    auto n = b["max_subscriptions_per_conn"].get<long long>();
    if (n < 1 || n > 4096) {
      throw std::runtime_error(
          "config.realtime.broker.max_subscriptions_per_conn_out_of_range: " +
          std::to_string(n));
    }
    cfg.realtime.broker.max_subscriptions_per_conn =
        static_cast<std::size_t>(n);
  }
  if (b.contains("rbac_enforce")) {
    cfg.realtime.broker.rbac_enforce = b["rbac_enforce"].get<bool>();
    if (!cfg.realtime.broker.rbac_enforce) {
      spdlog::warn("realtime.broker.rbac_enforce=false — degrading to "
                   "ICD-0.1.6 admin-only posture. MUST be true in "
                   "production per ICD-0.5.2 §Security Constraints 4.");
    }
  }
}

// Hard-fail bound checker shared by `apply_realtime_events` and the
// ICD-0.5.5 §10 substruct loaders. Pulled out at file scope so the
// cognitive-complexity budget for `apply_realtime_events` stays under
// the readability-function-cognitive-complexity threshold (25) — the
// 0.5.5 additions push it past the limit otherwise.
namespace {
auto bound_realtime_events(const char* field, long long val, long long lo,
                           long long hi) -> void {
  if (val < lo || val > hi) {
    throw std::runtime_error(std::string{"config.realtime.events."} + field +
                             "_out_of_range: " + std::to_string(val));
  }
}

// ICD-0.5.5 §10 — `seq` substruct (writer-first OQ1 + gap-audit window).
auto apply_realtime_events_seq(Config& cfg, const nlohmann::json& s) -> void {
  if (s.contains("source")) {
    auto src = s["source"].get<std::string>();
    if (src == "writer_returning") {
      cfg.realtime.events.seq.source =
          Config::Realtime::Events::SeqSource::WRITER_RETURNING;
    } else {
      throw std::runtime_error("config.realtime.events.seq.source_unknown: '" +
                               src +
                               "' (only 'writer_returning' is "
                               "supported by ICD-0.5.5)");
    }
  }
  if (s.contains("gap_audit_window_ms")) {
    auto n = s["gap_audit_window_ms"].get<long long>();
    bound_realtime_events("seq.gap_audit_window_ms", n, 1000, 3600000);
    cfg.realtime.events.seq.gap_audit_window_ms = static_cast<std::size_t>(n);
  }
}

// ICD-0.5.5 §10 — `debounce` substruct (subscribe_ack advisory).
auto apply_realtime_events_debounce(Config& cfg, const nlohmann::json& d)
    -> void {
  if (d.contains("recommend_ms")) {
    auto n = d["recommend_ms"].get<long long>();
    bound_realtime_events("debounce.recommend_ms", n, 0, 60000);
    cfg.realtime.events.debounce.recommend_ms = static_cast<std::size_t>(n);
  }
  if (d.contains("jitter_max_ms")) {
    auto n = d["jitter_max_ms"].get<long long>();
    bound_realtime_events("debounce.jitter_max_ms", n, 0, 5000);
    cfg.realtime.events.debounce.jitter_max_ms = static_cast<std::size_t>(n);
  }
}
} // namespace

// ICD-0.5.4 §Config Surface — events block. Same hard-fail posture as
// the listener / notify / coalescer / broker blocks. Cross-field
// consistency: replay_max_total_rows MUST be >= replay_max_rows_per_chunk
// per ICD §Config Surface field semantics.
auto apply_realtime_events(Config& cfg, const nlohmann::json& e) -> void {
  auto bound = [](const char* field, long long val, long long lo,
                  long long hi) { bound_realtime_events(field, val, lo, hi); };
  if (e.contains("enabled")) {
    cfg.realtime.events.enabled = e["enabled"].get<bool>();
    if (!cfg.realtime.events.enabled) {
      spdlog::warn("realtime.events.enabled=false — plinth.events writer "
                   "DISABLED kernel-wide. Reconnect clients with since_seq "
                   "will receive {type:resync, reason:events_disabled}. "
                   "Production MUST set true per ICD-0.5.4.");
    }
  }
  if (e.contains("retention_seconds")) {
    auto n = e["retention_seconds"].get<long long>();
    bound("retention_seconds", n, 60, 604800);
    cfg.realtime.events.retention_seconds = static_cast<std::size_t>(n);
  }
  if (e.contains("cleanup_interval_ms")) {
    auto n = e["cleanup_interval_ms"].get<long long>();
    bound("cleanup_interval_ms", n, 10000, 3600000);
    cfg.realtime.events.cleanup_interval_ms = static_cast<std::size_t>(n);
  }
  if (e.contains("replay_max_rows_per_chunk")) {
    auto n = e["replay_max_rows_per_chunk"].get<long long>();
    bound("replay_max_rows_per_chunk", n, 10, 10000);
    cfg.realtime.events.replay_max_rows_per_chunk = static_cast<std::size_t>(n);
  }
  if (e.contains("replay_max_total_rows")) {
    auto n = e["replay_max_total_rows"].get<long long>();
    bound("replay_max_total_rows", n, 100, 1000000);
    cfg.realtime.events.replay_max_total_rows = static_cast<std::size_t>(n);
  }
  if (e.contains("write_queue_size")) {
    auto n = e["write_queue_size"].get<long long>();
    bound("write_queue_size", n, 100, 1000000);
    cfg.realtime.events.write_queue_size = static_cast<std::size_t>(n);
  }
  if (e.contains("shutdown_drain_ms")) {
    auto n = e["shutdown_drain_ms"].get<long long>();
    bound("shutdown_drain_ms", n, 100, 60000);
    cfg.realtime.events.shutdown_drain_ms = static_cast<std::size_t>(n);
  }
  if (e.contains("cursor_cache_ttl_ms")) {
    auto n = e["cursor_cache_ttl_ms"].get<long long>();
    bound("cursor_cache_ttl_ms", n, 0, 60000);
    cfg.realtime.events.cursor_cache_ttl_ms = static_cast<std::size_t>(n);
  }
  if (e.contains("cursor_flush_threshold")) {
    auto n = e["cursor_flush_threshold"].get<long long>();
    bound("cursor_flush_threshold", n, 1, 10000);
    cfg.realtime.events.cursor_flush_threshold = static_cast<std::size_t>(n);
  }
  if (e.contains("audit_window_ms")) {
    auto n = e["audit_window_ms"].get<long long>();
    bound("audit_window_ms", n, 1000, 3600000);
    cfg.realtime.events.audit_window_ms = static_cast<std::size_t>(n);
  }
  // ICD-0.5.5 §10 — substruct loaders pulled into helpers above to
  // stay under the readability-function-cognitive-complexity budget.
  if (e.contains("seq") && e["seq"].is_object()) {
    apply_realtime_events_seq(cfg, e["seq"]);
  }
  if (e.contains("live_buffer_cap_per_subscription")) {
    auto n = e["live_buffer_cap_per_subscription"].get<long long>();
    bound("live_buffer_cap_per_subscription", n, 16, 65536);
    cfg.realtime.events.live_buffer_cap_per_subscription =
        static_cast<std::size_t>(n);
  }
  if (e.contains("coalesce") && e["coalesce"].is_object()) {
    const auto& c = e["coalesce"];
    if (c.contains("emit_superseded_seqs")) {
      cfg.realtime.events.coalesce.emit_superseded_seqs =
          c["emit_superseded_seqs"].get<bool>();
    }
  }
  if (e.contains("debounce") && e["debounce"].is_object()) {
    apply_realtime_events_debounce(cfg, e["debounce"]);
  }
  // Cross-field consistency: total cap must be >= chunk cap.
  if (cfg.realtime.events.replay_max_total_rows <
      cfg.realtime.events.replay_max_rows_per_chunk) {
    throw std::runtime_error(
        "config.realtime.events.replay_max_total_rows_below_chunk: " +
        std::to_string(cfg.realtime.events.replay_max_total_rows) + " < " +
        std::to_string(cfg.realtime.events.replay_max_rows_per_chunk));
  }
}

// ICD-0.5.3 §Config loader extension — db.* runtime semantics. Hard-
// fail posture on the substructs (matches the realtime block's
// convention at 0.5.0/0.5.1/0.5.2). Unknown sub-keys are warn-logged +
// ignored for forward compat. Phase 1 only covers `oid_mapping`;
// phases 2-4 extend with `silent` / `search_path` / `batch`.
auto apply_db_oid_mapping(Config& cfg, const nlohmann::json& om) -> void {
  if (om.contains("enabled")) {
    cfg.db_bindings.oid_mapping.enabled = om["enabled"].get<bool>();
    if (!cfg.db_bindings.oid_mapping.enabled) {
      spdlog::warn("db.oid_mapping.enabled=false — falling back to the "
                   "0.3.3 string-parse heuristic. Intended as a "
                   "deployment-ramp escape hatch only; production MUST "
                   "set true per ICD-0.5.3 §OID-Driven PG-Type → JS-Type "
                   "Mapping §Feature flag.");
    }
  }
}

// ICD-0.5.3 §silent Flag §Rate-limited `db.silent.used` audit. Bound
// check `[1000, 3600000]` ms per §Config Surface field semantics.
auto apply_db_silent(Config& cfg, const nlohmann::json& s) -> void {
  if (s.contains("audit_window_ms")) {
    auto ms = s["audit_window_ms"].get<long long>();
    if (ms < 1000 || ms > 3600000) {
      throw std::runtime_error(
          "config.db.silent.audit_window_ms_out_of_range: " +
          std::to_string(ms));
    }
    cfg.db_bindings.silent.audit_window_ms = static_cast<std::size_t>(ms);
  }
}

// ICD-0.5.3 §Per-Op SET search_path Isolation §Config override.
// `enforce=false` is a production-diagnostic escape hatch; emit a
// startup warn log so operators see it in the boot sequence.
auto apply_db_search_path(Config& cfg, const nlohmann::json& s) -> void {
  if (s.contains("enforce")) {
    cfg.db_bindings.search_path.enforce = s["enforce"].get<bool>();
    if (!cfg.db_bindings.search_path.enforce) {
      spdlog::warn("db.search_path.enforce=false — per-op SET LOCAL "
                   "search_path wrapper DISABLED kernel-wide. Production "
                   "MUST set true per ICD-0.5.3 §Security Constraint 2.");
    }
  }
}

// ICD-0.5.3 §`db.batch()` §Field semantics — all four bounds are
// hard-fails; operator misconfig is immediate rather than silently
// coerced.
auto apply_db_batch(Config& cfg, const nlohmann::json& b) -> void {
  auto bound = [](const char* field, long long val, long long lo,
                  long long hi) {
    if (val < lo || val > hi) {
      throw std::runtime_error(std::string{"config.db.batch."} + field +
                               "_out_of_range: " + std::to_string(val));
    }
  };
  if (b.contains("max_ops_per_batch")) {
    auto n = b["max_ops_per_batch"].get<long long>();
    bound("max_ops_per_batch", n, 1, 100000);
    cfg.db_bindings.batch.max_ops_per_batch = static_cast<std::size_t>(n);
  }
  if (b.contains("max_concurrent_batches_per_bc")) {
    auto n = b["max_concurrent_batches_per_bc"].get<long long>();
    bound("max_concurrent_batches_per_bc", n, 1, 64);
    cfg.db_bindings.batch.max_concurrent_batches_per_bc =
        static_cast<std::size_t>(n);
  }
  if (b.contains("timeout_ms")) {
    auto n = b["timeout_ms"].get<long long>();
    bound("timeout_ms", n, 100, 600000);
    cfg.db_bindings.batch.timeout_ms = static_cast<std::size_t>(n);
  }
  if (b.contains("audit_window_ms")) {
    auto n = b["audit_window_ms"].get<long long>();
    bound("audit_window_ms", n, 1000, 3600000);
    cfg.db_bindings.batch.audit_window_ms = static_cast<std::size_t>(n);
  }
}

auto apply_db(Config& cfg, const nlohmann::json& d) -> void {
  if (d.contains("oid_mapping") && d["oid_mapping"].is_object()) {
    apply_db_oid_mapping(cfg, d["oid_mapping"]);
  }
  if (d.contains("silent") && d["silent"].is_object()) {
    apply_db_silent(cfg, d["silent"]);
  }
  if (d.contains("search_path") && d["search_path"].is_object()) {
    apply_db_search_path(cfg, d["search_path"]);
  }
  if (d.contains("batch") && d["batch"].is_object()) {
    apply_db_batch(cfg, d["batch"]);
  }
}

// ICD-0.6.0 §9 — frontend shell config. Soft-fail posture: invalid
// `root_redirect` values warn-and-default to `/app/` rather than
// throwing, matching the security/packages/ws blocks (the realtime/db
// blocks hard-fail because their bounds are load-bearing). `enabled`
// is a plain bool with no validation.
auto apply_shell(Config& cfg, const nlohmann::json& s) -> void {
  if (s.contains("enabled")) {
    cfg.shell.enabled = s["enabled"].get<bool>();
  }
  if (s.contains("root_redirect")) {
    auto rr = s["root_redirect"].get<std::string>();
    // ICD-0.6.0 §9.2 validation: single-segment with trailing slash.
    static const std::regex ROOT_REDIRECT_PATTERN{R"(^/[^/]+/$)"};
    if (std::regex_match(rr, ROOT_REDIRECT_PATTERN)) {
      cfg.shell.root_redirect = std::move(rr);
    } else {
      spdlog::warn("shell.root_redirect={} does not match ^/[^/]+/$ — "
                   "falling back to /app/ per ICD-0.6.0 §9.2",
                   rr);
    }
  }
  // ICD-0.6.1 §9.2 — pass-through; resolution to absolute path happens
  // at firstboot time (no filesystem probe at config load).
  if (s.contains("bundle_path")) {
    cfg.shell.bundle_path = s["bundle_path"].get<std::string>();
  }
}

auto apply_realtime(Config& cfg, const nlohmann::json& r) -> void {
  if (r.contains("listener") && r["listener"].is_object()) {
    apply_realtime_listener(cfg, r["listener"]);
  }
  if (r.contains("notify") && r["notify"].is_object()) {
    apply_realtime_notify(cfg, r["notify"]);
  }
  if (r.contains("coalescer") && r["coalescer"].is_object()) {
    apply_realtime_coalescer(cfg, r["coalescer"]);
  }
  if (r.contains("broker") && r["broker"].is_object()) {
    apply_realtime_broker(cfg, r["broker"]);
  }
  if (r.contains("events") && r["events"].is_object()) {
    apply_realtime_events(cfg, r["events"]);
  }
}

auto apply_json(Config& cfg, const nlohmann::json& j) -> void {
  if (j.contains("database") && j["database"].is_object()) {
    apply_database(cfg, j["database"]);
  }
  if (j.contains("migrations_dir")) {
    cfg.migrations_dir = j["migrations_dir"].get<std::string>();
  }
  if (j.contains("dev_mode")) {
    cfg.dev_mode = j["dev_mode"].get<bool>();
  }
  if (j.contains("listen_host")) {
    cfg.listen_host = j["listen_host"].get<std::string>();
  }
  if (j.contains("listen_port")) {
    cfg.listen_port = j["listen_port"].get<uint16_t>();
  }
  if (j.contains("registration_enabled")) {
    cfg.registration_enabled = j["registration_enabled"].get<bool>();
  }
  if (j.contains("node_id")) {
    cfg.node_id = j["node_id"].get<std::string>();
  }
  apply_ws(cfg, j);
  if (j.contains("security") && j["security"].is_object()) {
    apply_security(cfg, j["security"]);
  }
  if (j.contains("packages") && j["packages"].is_object()) {
    apply_packages(cfg, j["packages"]);
  }
  if (j.contains("realtime") && j["realtime"].is_object()) {
    apply_realtime(cfg, j["realtime"]);
  }
  if (j.contains("db") && j["db"].is_object()) {
    apply_db(cfg, j["db"]);
  }
  if (j.contains("shell") && j["shell"].is_object()) {
    apply_shell(cfg, j["shell"]);
  }
}

auto apply_env(Config& cfg) -> void {
  auto v = env("PLINTH_PG_HOST");
  if (!v.empty()) {
    cfg.db.host = v;
  }

  v = env("PLINTH_PG_PORT");
  if (!v.empty()) {
    cfg.db.port = static_cast<uint16_t>(std::stoi(v));
  }

  v = env("PLINTH_PG_USER");
  if (!v.empty()) {
    cfg.db.user = v;
  }

  v = env("PLINTH_PG_PASSWORD");
  if (!v.empty()) {
    cfg.db.password = v;
  }

  v = env("PLINTH_PG_DATABASE");
  if (!v.empty()) {
    cfg.db.database = v;
  }

  v = env("PLINTH_PG_POOL_SIZE");
  if (!v.empty()) {
    cfg.db.pool_size = std::stoi(v);
  }

  v = env("PLINTH_MIGRATIONS_DIR");
  if (!v.empty()) {
    cfg.migrations_dir = v;
  }

  v = env("PLINTH_DEV_MODE");
  if (!v.empty()) {
    cfg.dev_mode = (v == "1" || v == "true");
  }

  v = env("PLINTH_REGISTRATION_ENABLED");
  if (!v.empty()) {
    cfg.registration_enabled = (v == "1" || v == "true");
  }

  v = env("PLINTH_NODE_ID");
  if (!v.empty()) {
    cfg.node_id = v;
  }
}

} // namespace

auto load_config() -> Config {
  Config cfg;
  apply_env(cfg);
  return cfg;
}

auto load_config(const std::string& config_path) -> Config {
  Config cfg;

  // Layer 1: an explicitly requested JSON file is required and fail-closed.
  std::ifstream file(config_path);
  if (!file.is_open()) {
    throw std::runtime_error("config.file_unreadable: " + config_path);
  }

  try {
    auto j = nlohmann::json::parse(file);
    if (!j.is_object()) {
      throw std::runtime_error("config.root_not_object: " + config_path);
    }
    apply_json(cfg, j);
  } catch (const nlohmann::json::exception& e) {
    throw std::runtime_error("config.file_invalid: " + config_path + ": " +
                             e.what());
  }
  spdlog::debug("loaded config from {}", config_path);

  // Layer 2: Environment variable overrides
  apply_env(cfg);

  return cfg;
}

} // namespace plinth
