#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace plinth {

struct Config {
  struct Database {
    std::string host = "localhost";
    uint16_t port = 5432;
    std::string user = "plinth";
    std::string password = "plinth";
    std::string database = "plinth";
    // PG connection-pool size. Per ICD-0.3.3 §Back-Pressure the
    // recommended sizing formula is:
    //   pool_size >= runtime_pool_size * max_concurrent_async_ops
    // The 0.3.3 kernel defaults are runtime_pool_size = 4 (auto;
    // see RuntimePool ctor's resolve_pool_size) and
    // max_concurrent_async_ops = 8 (RuntimeLimits default), so
    // 4*8 = 32 is the minimum that keeps the bridge from
    // blocking when one extension fan-outs at the limit. Per
    // architect decision (2026-04-18) the default ships at 32;
    // operators with multiple extensions should bump in
    // config.json.example or via PLINTH_PG_POOL_SIZE.
    int pool_size = 32;
  };

  Database db;
  std::string migrations_dir = "./migrations";
  bool dev_mode = false;
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 8080;
  bool registration_enabled = false;
  std::string node_id = "node-1";

  // WebSocket connection lifecycle (per ICD-0.1.6).
  // Tests override these via JSON config to avoid 30+10s waits.
  double ws_auth_timeout_s = 5.0;
  double ws_heartbeat_interval_s = 30.0;
  double ws_heartbeat_timeout_s = 10.0;

  // GlassWorm Unicode scanner (per ICD-0.4.1). Secure-by-default:
  // enabled, threshold 50 (legitimate emoji rarely exceed a handful),
  // findings audited. Disabling is a global escape hatch only — there
  // is no per-extension override (DESIGN-glassworm-defense-v0x.md §7).
  bool security_unicode_scanner_enabled = true;
  std::size_t security_unicode_scanner_threshold = 50;
  bool security_unicode_scanner_log_findings = true;

  // Package install lifecycle (ICD-0.4.4). data_dir holds installed
  // extensions under `extensions/{name}/{version}/`; staging_dir is
  // scratch space during UPLOADING (unlinked on lifecycle exit).
  // max_package_size_mb caps uploaded zip size; libzip extraction
  // additionally rejects any zip whose uncompressed total exceeds
  // 2× this cap (zip-bomb guard).
  std::string packages_data_dir = "./data";
  std::string packages_staging_dir = "./data/staging";
  std::size_t packages_max_package_size_mb = 50;

  // ICD-0.4.5 §Atomic Swap T2 — drain window for in-flight capability
  // calls against the old version during upgrade. Default 5000 ms per
  // Maintainer-approved OQ #1; single timed condvar wait
  // (not poll-with-budget).
  std::size_t packages_upgrade_drain_timeout_ms = 5000;

  // ICD-0.5.0 §Config Surface — PG LISTEN/NOTIFY bridge.
  // ICD-0.5.1 §Config Surface — Coalescer substruct added.
  struct Realtime {
    struct Listener {
      bool enabled = true;
      int reconnect_backoff_ms = 1000;
    } listener;
    struct Notify {
      std::size_t max_payload_bytes = 8000;
    } notify;
    struct Coalescer {
      bool enabled = true;
      std::size_t window_ms = 50;
    } coalescer;
    // ICD-0.5.2 §Config Surface — WebSocket broker.
    struct Broker {
      bool enabled = true;
      std::size_t max_subscriptions_per_conn = 64;
      bool rbac_enforce = true;
    } broker;
    // ICD-0.5.4 §Config Surface — plinth.events writer + replay +
    // cursor cache + retention sweep. All bounds are hard-fails;
    // operator misconfig is immediate rather than silently coerced.
    // ICD-0.5.5 §10 extends with `seq.*`, `live_buffer_cap_per_subscription`,
    // `coalesce.emit_superseded_seqs`, and `debounce.*`.
    struct Events {
      bool enabled = true;
      std::size_t retention_seconds = 3600;        // [60, 604800]
      std::size_t cleanup_interval_ms = 300000;    // [10000, 3600000]
      std::size_t replay_max_rows_per_chunk = 500; // [10, 10000]
      std::size_t replay_max_total_rows = 10000;   // [100, 1000000]; >= chunk
      std::size_t write_queue_size = 10000;        // [100, 1000000]
      std::size_t shutdown_drain_ms = 5000;        // [100, 60000]
      std::size_t cursor_cache_ttl_ms = 1000;      // [0, 60000]
      std::size_t cursor_flush_threshold = 50;     // [1, 10000]
      std::size_t audit_window_ms = 60000;         // [1000, 3600000]

      // ICD-0.5.5 §10 — sequence-number generation strategy. OQ1
      // pinned to writer-first: the writer's INSERT ... RETURNING
      // seq is the canonical envelope-seq source. Modeled as an
      // enum class so the field stays trivially default-constructible
      // and `Config::Realtime::Events g_cfg;` static-storage
      // declarations in the realtime subsystems remain noexcept-
      // initializable (cert-err58-cpp). Future ICDs may add
      // `nextval_preallocate` or `independent_counter` variants
      // alongside `writer_returning`; the loader hard-fails any
      // string the enum doesn't cover.
      enum class SeqSource : std::uint8_t { WRITER_RETURNING };
      struct Seq {
        SeqSource source = SeqSource::WRITER_RETURNING;
        std::size_t gap_audit_window_ms = 60000; // [1000, 3600000]
      } seq;

      // ICD-0.5.5 §10 — per-(connection, subscription) live-frame
      // buffer cap during replay. On overflow the broker aborts the
      // in-flight replay and emits `resync reason=live_buffer_overflow`.
      // Default sized for ~12.8 s of full-rate emission per channel
      // (256 / (1 frame per 50 ms coalesce window)). Pin the actual
      // default after LH-3 exercises the path.
      std::size_t live_buffer_cap_per_subscription = 256; // [16, 65536]

      // ICD-0.5.5 §10 — coalescer-side wire opt-ins. OFF by default
      // because most SDKs derive covered seqs from `coalesced_count`;
      // operators flip on when 0.6.3 SDK demands it (variable per-
      // envelope size cost).
      struct Coalesce {
        bool emit_superseded_seqs = false;
      } coalesce;

      // ICD-0.5.5 §10 — `subscribe_ack` advisory cadence. Defaults
      // match `architecture/03-data.md §3.4 items 3 and 5`. Both
      // fields are emitted once per subscribe_ack (OQ5 pin: no
      // per-frame republish).
      struct Debounce {
        std::size_t recommend_ms = 100; // [0, 60000]
        std::size_t jitter_max_ms = 50; // [0, 5000]
      } debounce;
    } events;
  };
  Realtime realtime;

  // ICD-0.5.3 §Config Surface — db.* runtime semantics. Distinct
  // from Config::Database (which holds PG connection params) — this
  // block is the JSON "db" block, not "database". 0.5.3 lands the
  // `oid_mapping` substruct; subsequent 0.5.3 phases add `silent`,
  // `search_path`, and `batch` substructs under this same struct.
  struct Db {
    // ICD-0.5.3 §OID-Driven PG-Type → JS-Type Mapping §Feature flag.
    // Default true → OID switch in `db_result_to_json`. Set false to
    // fall back to the 0.3.3 string-parse heuristic as a
    // deployment-ramp escape hatch. Targeted for removal one
    // milestone after 0.5.3 ships if no regressions surface.
    struct OidMapping {
      bool enabled = true;
    } oid_mapping;

    // ICD-0.5.3 §silent Flag §Rate-limited `db.silent.used` audit.
    // Aggregation window for the `db.silent.used` audit event. The
    // first silent-exec in a window emits an audit row with
    // count_in_window=1; subsequent silent execs from the same
    // extension inside the window bump an in-memory counter, and
    // on the first emission AFTER the window closes the aggregated
    // `count_in_window` lands in one rollup audit. Bound
    // `[1000, 3600000]` ms per ICD §Config Surface.
    struct Silent {
      std::size_t audit_window_ms = 60000;
    } silent;

    // ICD-0.5.3 §Per-Op SET search_path Isolation §Config override.
    // Default true → every extension-scope db.exec / db.query is
    // wrapped in BEGIN; SET LOCAL search_path TO ext_<name>,
    // plinth; user_sql; COMMIT; Set false to disable the wrapper
    // kernel-wide (deployment-ramp / perf-diagnostic escape hatch).
    // Warn-logged at config load when false. Production MUST set
    // true per §Security Constraint 2.
    struct SearchPath {
      bool enforce = true;
    } search_path;

    // ICD-0.5.3 §`db.batch()` §Config Surface. Bounds per §Field
    // semantics. `max_ops_per_batch` caps the number of db.exec /
    // db.query calls inside one batch callback — overflow rejects
    // `db.batch.quota_exceeded` synchronously (caller can swallow +
    // COMMIT with what succeeded, or throw to trigger ROLLBACK).
    // `max_concurrent_batches_per_bc` bounds parallel Promise.all-
    // style batch usage from a single bc; nested batches are
    // rejected outright per §Binding implementation step 2.
    // `timeout_ms` arms a `trantor::EventLoop::runAfter` on the
    // main loop at BEGIN finalize per ICD §B.06; on fire the
    // in-batch enqueue path + `__db_batch_commit__` reject with
    // `db.batch.timeout`. Emit audit aggregation windows per
    // §Rate-limited audit events.
    struct Batch {
      std::size_t max_ops_per_batch = 500;
      std::size_t max_concurrent_batches_per_bc = 4;
      std::size_t timeout_ms = 30000;
      std::size_t audit_window_ms = 60000;
    } batch;
  };
  Db db_bindings;

  // ICD-0.6.0 §9.1 — Frontend shell static-handler config. Default
  // ships the bundled shell at /app/* with `/` redirecting there.
  // Set `enabled = false` to deploy the kernel as a headless API
  // (BYO-frontend territory per architecture/06-frontend.md §6) —
  // both the `/` redirect and the `/app/*` handler skip registration.
  // `root_redirect` may be overridden to point `/` at a non-default
  // mount; validation is `^/[^/]+/$` (single-segment, trailing slash);
  // invalid values warn-fall back to `/app/`.
  struct Shell {
    bool enabled = true;
    std::string root_redirect = "/app/";
    // ICD-0.6.1 §9.1 — directory containing `shell.zip` consumed at
    // first-boot pre-flight. Empty = resolved from `/proc/self/exe`
    // (try `<bin>/share/plinth/bundled` first for dev layouts;
    // fall back to `<bin>/../share/plinth/bundled` for FHS install).
    // Non-empty absolute = used verbatim; non-empty relative = relative
    // to CWD. See `plinth::shell::resolve_bundle_path`.
    std::string bundle_path;
  };
  Shell shell;
};

// Load secure defaults and environment-variable overrides.
auto load_config() -> Config;

// Load the named JSON file, then apply environment-variable overrides.
// An explicitly named file must exist, be readable, and contain a JSON object.
// Env vars: PLINTH_PG_HOST, PLINTH_PG_PORT, PLINTH_PG_USER,
//           PLINTH_PG_PASSWORD, PLINTH_PG_DATABASE, PLINTH_PG_POOL_SIZE,
//           PLINTH_DEV_MODE, PLINTH_MIGRATIONS_DIR
auto load_config(const std::string& config_path) -> Config;

} // namespace plinth
