#include "kernel/packages/rbac_test_runner.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/capabilities/resolution.hpp"
#include "kernel/capabilities/types.hpp"
#include "kernel/logging.hpp"
#include "kernel/rbac/ephemeral_user.hpp"
#include "kernel/rbac/rule_registrar.hpp"

#include <json/value.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::packages::rbac_test {

namespace {

using PgResultPtr = std::unique_ptr<PGresult, decltype(&PQclear)>;

struct WorkerState {
  bool finished = false;
};

struct Worker {
  std::shared_ptr<WorkerState> state;
  std::jthread thread;
};

class WorkerRegistry {
 public:
  auto start() -> bool {
    std::lock_guard lock(mu);
    reap_finished_locked();
    if (active != 0) {
      return false;
    }
    accepting = true;
    return true;
  }

  auto schedule(std::function<void(std::stop_token)> task) -> bool {
    std::lock_guard lock(mu);
    reap_finished_locked();
    if (!accepting) {
      return false;
    }

    auto state = std::make_shared<WorkerState>();
    workers.push_back(Worker{.state = state, .thread = {}});
    ++active;
    try {
      workers.back().thread = std::jthread(
          [this, state, task = std::move(task)](std::stop_token stop) mutable {
            try {
              task(stop);
            } catch (const std::exception& e) {
              spdlog::error("rbac_test worker: exception: {}", e.what());
            } catch (...) {
              spdlog::error("rbac_test worker: unknown exception");
            }
            finish(state);
          });
    } catch (...) {
      --active;
      workers.pop_back();
      throw;
    }
    return true;
  }

  auto shutdown(std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock(mu);
    accepting = false;
    for (auto& worker : workers) {
      worker.thread.request_stop();
    }
    bool drained =
        drained_cv.wait_for(lock, timeout, [this] { return active == 0; });
    if (drained) {
      reap_finished_locked();
    }
    return drained;
  }

  [[nodiscard]] auto active_count() const -> std::size_t {
    std::lock_guard lock(mu);
    return active;
  }

 private:
  auto finish(const std::shared_ptr<WorkerState>& state) -> void {
    {
      std::lock_guard lock(mu);
      state->finished = true;
      --active;
    }
    drained_cv.notify_all();
  }

  auto reap_finished_locked() -> void {
    auto it = workers.begin();
    while (it != workers.end()) {
      if (it->state->finished) {
        it = workers.erase(it);
      } else {
        ++it;
      }
    }
  }

  mutable std::mutex mu;
  std::condition_variable drained_cv;
  bool accepting = true;
  std::size_t active = 0;
  std::vector<Worker> workers;
};

auto worker_registry() -> WorkerRegistry& {
  static WorkerRegistry registry;
  return registry;
}

auto cancelled_failure() -> RbacTestFailure {
  return RbacTestFailure{.kind = "cancelled",
                         .message = "RBAC test cancelled during shutdown"};
}

auto uuid_v4() -> std::string {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  std::uint64_t a = rng();
  std::uint64_t b = rng();
  a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
  std::array<char, 37> buf{};
  // natural fit for UUID formatting.
  std::snprintf(
      buf.data(), buf.size(), "%08x-%04x-%04x-%04x-%012llx",
      static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF),
      static_cast<unsigned>(a & 0xFFFF), static_cast<unsigned>(b >> 48),
      static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
  return std::string{buf.data()};
}

auto format_iso(std::chrono::system_clock::time_point tp) -> std::string {
  auto t = std::chrono::system_clock::to_time_t(tp);
  std::tm gm{};
  (void)::gmtime_r(&t, &gm);
  std::array<char, 32> buf{};
  (void)std::strftime(buf.data(), buf.size(), "%Y-%m-%dT%H:%M:%SZ", &gm);
  return std::string{buf.data()};
}

auto conninfo_of(const plinth::Config::Database& db) -> std::string {
  return "host=" + db.host + " port=" + std::to_string(db.port) +
         " dbname=" + db.database + " user=" + db.user +
         " password=" + db.password + " connect_timeout=5";
}

struct PgGuard {
  PGconn* conn = nullptr;
  explicit PgGuard(const plinth::Config::Database& db) {
    conn = PQconnectdb(conninfo_of(db).c_str());
  }
  ~PgGuard() {
    if (conn != nullptr) {
      PQfinish(conn);
    }
  }
  PgGuard(const PgGuard&) = delete;
  auto operator=(const PgGuard&) -> PgGuard& = delete;
  PgGuard(PgGuard&&) = delete;
  auto operator=(PgGuard&&) -> PgGuard& = delete;
  [[nodiscard]] auto ok() const -> bool {
    return conn != nullptr && PQstatus(conn) == CONNECTION_OK;
  }
};

// ── Advisory lock (reuses install_lifecycle's per-name key) ────────

// Compute the additive union of rules granted to this user via any
// group membership — mirrors the SessionFilter / RbacFilter query in
// src/kernel/rbac/enforcement.cpp. The denied ephemeral user inherits
// `everyone` grants, so a poisoned "rule granted to everyone" scenario
// makes RBAC pass and flips `assert_deny` to FAIL.
auto effective_rules_for(PGconn* conn, std::string_view user_id)
    -> std::vector<std::string> {
  std::string id_s{user_id};
  std::array<const char*, 1> values = {id_s.c_str()};
  PgResultPtr res(
      PQexecParams(conn,
                   "SELECT DISTINCT r.rule FROM plinth.rbac_rules r "
                   "JOIN plinth.group_rules gr ON gr.rule_id = r.id "
                   "JOIN plinth.group_members gm ON gm.group_id = gr.group_id "
                   "WHERE gm.user_id = $1::uuid",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  std::vector<std::string> out;
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return out;
  }
  int n = PQntuples(res.get());
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    out.emplace_back(PQgetvalue(res.get(), i, 0));
  }
  return out;
}

auto try_acquire_name_lock(PGconn* conn, std::string_view name) -> bool {
  std::string seed = "plinth.packages." + std::string{name};
  std::array<const char*, 1> values = {seed.c_str()};
  PgResultPtr res(
      PQexecParams(conn, "SELECT pg_try_advisory_lock(hashtextextended($1, 0))",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) == 0) {
    return false;
  }
  const char* got = PQgetvalue(res.get(), 0, 0);
  return got != nullptr && std::strcmp(got, "t") == 0;
}

auto release_name_lock(PGconn* conn, std::string_view name) -> void {
  std::string seed = "plinth.packages." + std::string{name};
  std::array<const char*, 1> values = {seed.c_str()};
  PgResultPtr res(
      PQexecParams(conn, "SELECT pg_advisory_unlock(hashtextextended($1, 0))",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  (void)res;
}

// ── Package lookup ────────────────────────────────────────────────

struct PackageLookup {
  std::string id;
  std::string name;
  std::string version;
  std::string state;
};

auto load_package(PGconn* conn, std::string_view package_id)
    -> std::expected<PackageLookup, RbacTestFailure> {
  std::string id_s{package_id};
  std::array<const char*, 1> values = {id_s.c_str()};
  PgResultPtr res(PQexecParams(conn,
                               "SELECT id::text, name, version, state "
                               "FROM plinth.packages WHERE id = $1::uuid",
                               1, nullptr, values.data(), nullptr, nullptr, 0),
                  PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(RbacTestFailure{
        .kind = "db_error", .message = PQresultErrorMessage(res.get())});
  }
  if (PQntuples(res.get()) == 0) {
    return std::unexpected(RbacTestFailure{.kind = "package_not_found",
                                           .message = "no package row for id"});
  }
  PackageLookup p;
  p.id = PQgetvalue(res.get(), 0, 0);
  p.name = PQgetvalue(res.get(), 0, 1);
  p.version = PQgetvalue(res.get(), 0, 2);
  p.state = PQgetvalue(res.get(), 0, 3);
  if (p.state != "ACTIVE" && p.state != "ACTIVE_FLAGGED") {
    return std::unexpected(
        RbacTestFailure{.kind = "invalid_state",
                        .message = "package not in ACTIVE or ACTIVE_FLAGGED",
                        .detail = nlohmann::json{{"state", p.state}}});
  }
  return p;
}

// ── Capability dispatch with wall-clock cap ────────────────────────

enum class InvocationStatus : std::uint8_t { READY, TIMEOUT, CANCELLED };

struct InvocationCompletion {
  std::mutex mu;
  std::condition_variable cv;
  std::optional<plinth::capabilities::ResolveResult> result;
  std::exception_ptr exception;
  bool cancelled = false;
};

// Invokes `call_capability` on the shared owned-worker registry. A timed-out
// invocation may continue, but it remains visible to shutdown and is joined
// before the registry reports a successful drain.
auto invoke_with_timeout(plinth::capabilities::CapabilityCall call,
                         plinth::capabilities::UserContext ctx,
                         std::chrono::milliseconds timeout,
                         std::stop_token owner_stop)
    -> std::pair<InvocationStatus, plinth::capabilities::ResolveResult> {
  auto completion = std::make_shared<InvocationCompletion>();
  bool scheduled = worker_registry().schedule(
      [call = std::move(call), ctx = std::move(ctx),
       completion](std::stop_token worker_stop) mutable {
        try {
          if (!worker_stop.stop_requested()) {
            auto result = plinth::capabilities::call_capability(call, ctx);
            std::lock_guard lock(completion->mu);
            completion->result.emplace(std::move(result));
          } else {
            std::lock_guard lock(completion->mu);
            completion->cancelled = true;
          }
        } catch (...) {
          std::lock_guard lock(completion->mu);
          completion->exception = std::current_exception();
        }
        completion->cv.notify_all();
      });
  if (!scheduled) {
    return {InvocationStatus::CANCELLED, plinth::capabilities::ResolveResult{}};
  }

  std::stop_callback notify_on_stop(owner_stop, [completion] {
    std::lock_guard lock(completion->mu);
    completion->cv.notify_all();
  });
  std::unique_lock lock(completion->mu);
  bool settled = completion->cv.wait_for(lock, timeout, [&] {
    return completion->result.has_value() || completion->exception != nullptr ||
           completion->cancelled || owner_stop.stop_requested();
  });
  if (completion->result.has_value()) {
    return {InvocationStatus::READY, std::move(*completion->result)};
  }
  if (completion->exception != nullptr) {
    std::rethrow_exception(completion->exception);
  }
  if (completion->cancelled) {
    return {InvocationStatus::CANCELLED, plinth::capabilities::ResolveResult{}};
  }
  auto status = owner_stop.stop_requested() ? InvocationStatus::CANCELLED
                                            : InvocationStatus::TIMEOUT;
  if (!settled) {
    status = InvocationStatus::TIMEOUT;
  }
  return {status, plinth::capabilities::ResolveResult{}};
}

// ── Per-rule invocation → RuleOutcome ──────────────────────────────

struct InvokeInputs {
  std::string_view clause; // "assert_deny" | "assert_allow"
  std::string signature;   // "notes:1:edit"
  Json::Value args;        // typically empty
  plinth::capabilities::UserContext user_ctx;
  std::chrono::milliseconds timeout;
};

auto classify_outcome(std::string_view rule, const InvokeInputs& in,
                      InvocationStatus status,
                      const plinth::capabilities::ResolveResult& out)
    -> RuleOutcome {
  RuleOutcome r;
  r.rule = rule;
  r.clause = in.clause;
  r.expected = (in.clause == "assert_deny") ? "permission_denied" : "success";
  if (status == InvocationStatus::TIMEOUT) {
    r.actual = "timeout";
    r.passed = false;
    return r;
  }
  if (status == InvocationStatus::CANCELLED) {
    r.actual = "cancelled";
    r.passed = false;
    return r;
  }
  if (out.has_value()) {
    r.actual = "success";
    r.passed = (in.clause == "assert_allow");
    return r;
  }
  auto err = out.error();
  std::string code{plinth::capabilities::error_code(err)};
  r.actual = code;
  if (in.clause == "assert_deny") {
    r.passed =
        (err == plinth::capabilities::CapabilityError::PERMISSION_DENIED);
  } else {
    // assert_allow: PASS if anything other than PERMISSION_DENIED —
    // the extension may legitimately throw (e.g., invalid_argument
    // because the test did not provide live data) without the rule
    // being broken. DESIGN §0.4.7 distinguishes permission error
    // from any other error.
    r.passed =
        (err != plinth::capabilities::CapabilityError::PERMISSION_DENIED);
  }
  return r;
}

auto invoke_single(std::string_view rule, const InvokeInputs& in,
                   std::stop_token owner_stop) -> RuleOutcome {
  auto parsed = plinth::capabilities::parse_signature(in.signature);
  if (std::holds_alternative<plinth::capabilities::CapabilityError>(parsed)) {
    RuleOutcome r;
    r.rule = rule;
    r.clause = in.clause;
    r.expected = (in.clause == "assert_deny") ? "permission_denied" : "success";
    r.actual = "invalid_capability";
    r.passed = false;
    r.detail["signature"] = std::string{in.signature};
    return r;
  }
  plinth::capabilities::CapabilityCall call{
      .signature = std::string{in.signature},
      .args = in.args,
      .call_depth = 0,
  };
  auto [status, out] =
      invoke_with_timeout(call, in.user_ctx, in.timeout, owner_stop);
  auto r = classify_outcome(rule, in, status, out);
  r.detail["signature"] = call.signature;
  return r;
}

// Extract `call` string and optional `args` from a clause JSON object.
// The rbac.json test_contract shape is:
//   "test": {
//     "assert_deny":  {"call": "notes:1:edit"},
//     "assert_allow": {"call": "notes:1:edit"}
//   }
// Returns std::nullopt if the clause is absent / missing / non-object.
struct ClauseSpec {
  std::string signature;
  Json::Value args;
};

auto extract_clause(const nlohmann::json& contract, std::string_view clause)
    -> std::optional<ClauseSpec> {
  auto it = contract.find(clause);
  if (it == contract.end() || !it->is_object()) {
    return std::nullopt;
  }
  auto call_it = it->find("call");
  if (call_it == it->end() || !call_it->is_string()) {
    return std::nullopt;
  }
  ClauseSpec s;
  s.signature = call_it->get<std::string>();
  // optional forward-compatible args field
  auto args_it = it->find("args");
  if (args_it != it->end()) {
    // nlohmann::json → Json::Value via string round-trip is the
    // minimal-churn bridge (the two libraries don't share a type).
    Json::Value v;
    Json::Reader r;
    std::string dump = args_it->dump();
    (void)r.parse(dump, v);
    s.args = std::move(v);
  }
  return s;
}

// ── State update ──────────────────────────────────────────────────

auto write_result_update_state_and_release_lock(
    PGconn* conn, std::string_view package_id, std::string_view package_name,
    const nlohmann::json& report_json, bool overall_passed)
    -> std::expected<void, RbacTestFailure> {
  std::string id_s{package_id};
  std::string payload = report_json.dump();
  std::string flag = overall_passed ? "t" : "f";
  std::string lock_seed = "plinth.packages." + std::string{package_name};
  std::array<const char*, 4> values = {id_s.c_str(), payload.c_str(),
                                       flag.c_str(), lock_seed.c_str()};
  PgResultPtr res(
      PQexecParams(conn,
                   "WITH updated AS ("
                   "  UPDATE plinth.packages "
                   "     SET last_rbac_test_run_at = NOW(), "
                   "         last_rbac_test_result = $2::jsonb, "
                   "         state = CASE "
                   "                   WHEN $3::bool THEN "
                   "                     CASE state "
                   "                       WHEN 'ACTIVE_FLAGGED' THEN 'ACTIVE' "
                   "                       ELSE state "
                   "                     END "
                   "                   ELSE 'ACTIVE_FLAGGED' "
                   "                 END "
                   "   WHERE id = $1::uuid "
                   "     AND state IN ('ACTIVE', 'ACTIVE_FLAGGED') "
                   "   RETURNING 1"
                   ") "
                   "SELECT pg_advisory_unlock(hashtextextended($4, 0)), "
                   "       (SELECT COUNT(*) FROM updated)",
                   4, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK ||
      PQntuples(res.get()) != 1 ||
      std::strcmp(PQgetvalue(res.get(), 0, 0), "t") != 0 ||
      std::strcmp(PQgetvalue(res.get(), 0, 1), "1") != 0) {
    return std::unexpected(RbacTestFailure{
        .kind = "db_error", .message = PQresultErrorMessage(res.get())});
  }
  return {};
}

auto emit_rbac_test_audit(const plinth::packages::InstallerContext& ctx,
                          const RbacTestReport& report,
                          std::string_view triggered_by) -> void {
  Json::Value detail(Json::objectValue);
  detail["package_id"] = report.package_id;
  detail["package_name"] = report.package_name;
  detail["package_version"] = report.package_version;
  detail["run_id"] = report.run_id;
  detail["rule_count_passed"] = static_cast<Json::UInt>(report.passed.size());
  detail["rule_count_failed"] = static_cast<Json::UInt>(report.failed.size());
  detail["rule_count_skipped"] = static_cast<Json::UInt>(report.skipped.size());
  detail["duration_ms"] = static_cast<Json::Int64>(report.duration.count());
  detail["triggered_by"] = std::string{triggered_by};

  if (!report.overall_passed()) {
    Json::Value failures(Json::arrayValue);
    for (const auto& f : report.failed) {
      Json::Value e(Json::objectValue);
      e["rule"] = f.rule;
      e["clause"] = f.clause;
      e["expected"] = f.expected;
      e["actual"] = f.actual;
      failures.append(std::move(e));
    }
    detail["failures"] = std::move(failures);
  }

  const char* action = report.overall_passed() ? "packages.rbac_test_passed"
                                               : "packages.rbac_test_failed";
  plinth::log::audit_sync(ctx.db, action, detail);
}

// ── Rule iteration ────────────────────────────────────────────────

auto run_one_rule(PGconn* conn, std::string_view rule_name,
                  const nlohmann::json& contract,
                  const plinth::rbac::RunUserPair& pair,
                  std::chrono::milliseconds timeout, RbacTestReport& report,
                  std::stop_token owner_stop)
    -> std::expected<void, RbacTestFailure> {
  if (owner_stop.stop_requested()) {
    return std::unexpected(cancelled_failure());
  }
  auto grant =
      plinth::rbac::grant_rule_to_run_group(pair.run_id, rule_name, *conn);
  if (!grant) {
    return std::unexpected(
        RbacTestFailure{.kind = "setup_failed", .message = grant.error()});
  }

  // assert_deny — run as __test_denied_<run_id>. Effective rules are
  // the additive union of rules granted to any group the user is in
  // (typically just `everyone`) — matches the HTTP path's
  // SessionFilter query. A poisoned `everyone → rule` grant makes
  // the denied user's RBAC check pass and flips assert_deny to FAIL.
  if (auto spec = extract_clause(contract, "assert_deny")) {
    auto rules = effective_rules_for(conn, pair.denied_user_id);
    auto ctx_user = plinth::rbac::build_test_user_context(
        pair.denied_username, pair.denied_user_id, std::move(rules));
    auto out = invoke_single(rule_name,
                             InvokeInputs{
                                 .clause = "assert_deny",
                                 .signature = spec->signature,
                                 .args = spec->args,
                                 .user_ctx = std::move(ctx_user),
                                 .timeout = timeout,
                             },
                             owner_stop);
    if (out.actual == "cancelled") {
      return std::unexpected(cancelled_failure());
    }
    if (out.passed) {
      report.passed.push_back(std::move(out));
    } else {
      report.failed.push_back(std::move(out));
    }
  }

  // assert_allow — run as __test_allowed_<run_id>. Effective rules
  // are whatever the DB grants transitively: `everyone` memberships
  // plus the synthetic group's single-rule grant issued above. If
  // the capability's `rbac_rule` does not match the granted rule,
  // RBAC denies → assert_allow FAILS.
  if (auto spec = extract_clause(contract, "assert_allow")) {
    auto rules = effective_rules_for(conn, pair.allowed_user_id);
    auto ctx_user = plinth::rbac::build_test_user_context(
        pair.allowed_username, pair.allowed_user_id, std::move(rules));
    auto out = invoke_single(rule_name,
                             InvokeInputs{
                                 .clause = "assert_allow",
                                 .signature = spec->signature,
                                 .args = spec->args,
                                 .user_ctx = std::move(ctx_user),
                                 .timeout = timeout,
                             },
                             owner_stop);
    if (out.actual == "cancelled") {
      return std::unexpected(cancelled_failure());
    }
    if (out.passed) {
      report.passed.push_back(std::move(out));
    } else {
      report.failed.push_back(std::move(out));
    }
  }

  auto revoke =
      plinth::rbac::revoke_rule_from_run_group(pair.run_id, rule_name, *conn);
  if (!revoke) {
    return std::unexpected(
        RbacTestFailure{.kind = "setup_failed", .message = revoke.error()});
  }
  return {};
}

} // namespace

// ── JSON round-trip ───────────────────────────────────────────────

auto to_json(const RuleOutcome& o) -> nlohmann::json {
  return {
      {"rule", o.rule},     {"clause", o.clause}, {"expected", o.expected},
      {"actual", o.actual}, {"passed", o.passed}, {"detail", o.detail},
  };
}

auto rule_outcome_from_json(const nlohmann::json& j) -> RuleOutcome {
  RuleOutcome o;
  o.rule = j.value("rule", std::string{});
  o.clause = j.value("clause", std::string{});
  o.expected = j.value("expected", std::string{});
  o.actual = j.value("actual", std::string{});
  o.passed = j.value("passed", false);
  if (j.contains("detail")) {
    o.detail = j.at("detail");
  }
  return o;
}

auto to_json(const RbacTestReport& r) -> nlohmann::json {
  nlohmann::json passed = nlohmann::json::array();
  nlohmann::json failed = nlohmann::json::array();
  for (const auto& o : r.passed) {
    passed.push_back(to_json(o));
  }
  for (const auto& o : r.failed) {
    failed.push_back(to_json(o));
  }
  return {
      {"run_id", r.run_id},
      {"package_id", r.package_id},
      {"package_name", r.package_name},
      {"package_version", r.package_version},
      {"started_at", format_iso(r.started_at)},
      {"duration_ms", r.duration.count()},
      {"passed", passed},
      {"failed", failed},
      {"skipped", r.skipped},
  };
}

auto rbac_test_report_from_json(const nlohmann::json& j) -> RbacTestReport {
  RbacTestReport r;
  r.run_id = j.value("run_id", std::string{});
  r.package_id = j.value("package_id", std::string{});
  r.package_name = j.value("package_name", std::string{});
  r.package_version = j.value("package_version", std::string{});
  if (j.contains("duration_ms")) {
    r.duration =
        std::chrono::milliseconds{j.at("duration_ms").get<long long>()};
  }
  if (j.contains("passed")) {
    for (const auto& e : j.at("passed")) {
      r.passed.push_back(rule_outcome_from_json(e));
    }
  }
  if (j.contains("failed")) {
    for (const auto& e : j.at("failed")) {
      r.failed.push_back(rule_outcome_from_json(e));
    }
  }
  if (j.contains("skipped")) {
    r.skipped = j.at("skipped").get<std::vector<std::string>>();
  }
  return r;
}

// ── Primary entry ─────────────────────────────────────────────────

namespace {

// orchestrator: connect → lock → load → fetch rules → per-rule iterate →
// teardown → persist → audit. Splitting into helpers proliferates signatures
// threading PgGuard/lock/RunUserPair/report state without readability gain.
auto run_rbac_test_impl(std::string_view package_id,
                        const plinth::packages::InstallerContext& ctx,
                        std::string_view triggered_by,
                        std::string_view run_id_override,
                        std::stop_token owner_stop)
    -> std::expected<RbacTestReport, RbacTestFailure> {
  if (owner_stop.stop_requested()) {
    return std::unexpected(cancelled_failure());
  }
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    return std::unexpected(
        RbacTestFailure{.kind = "db_error", .message = "PG connect failed"});
  }

  auto pkg = load_package(pg.conn, package_id);
  if (!pkg) {
    return std::unexpected(pkg.error());
  }

  if (!try_acquire_name_lock(pg.conn, pkg->name)) {
    return std::unexpected(
        RbacTestFailure{.kind = "lock_failed",
                        .message = "advisory lock held by another install"
                                   " or RBAC test run"});
  }
  struct LockGuard {
    PGconn* conn;
    std::string name;
    LockGuard(PGconn* c, std::string n) : conn{c}, name{std::move(n)} {}
    ~LockGuard() {
      if (conn != nullptr) {
        release_name_lock(conn, name);
      }
    }
    auto dismiss() noexcept -> void { conn = nullptr; }
    LockGuard(const LockGuard&) = delete;
    auto operator=(const LockGuard&) -> LockGuard& = delete;
    LockGuard(LockGuard&&) = delete;
    auto operator=(LockGuard&&) -> LockGuard& = delete;
  };
  LockGuard lock_guard{pg.conn, pkg->name};

  if (owner_stop.stop_requested()) {
    return std::unexpected(cancelled_failure());
  }

  RbacTestReport report;
  report.run_id =
      run_id_override.empty() ? uuid_v4() : std::string{run_id_override};
  report.package_id = pkg->id;
  report.package_name = pkg->name;
  report.package_version = pkg->version;
  report.started_at = std::chrono::system_clock::now();

  auto rules =
      plinth::rbac::fetch_extension_rules_for_rbac_test(pkg->name, *pg.conn);
  if (!rules) {
    return std::unexpected(
        RbacTestFailure{.kind = "db_error", .message = rules.error()});
  }

  // If there are no rules at all we skip user creation entirely — the
  // report is trivially all-pass (empty failed set).
  bool any_with_contract = false;
  for (const auto& r : *rules) {
    if (r.test_contract.has_value()) {
      any_with_contract = true;
    } else {
      report.skipped.push_back(r.rule);
    }
  }

  if (any_with_contract) {
    auto users = plinth::rbac::create_run_users(report.run_id, *pg.conn);
    if (!users) {
      // partial rows; the outer error is what we return.
      (void)plinth::rbac::destroy_run_users(report.run_id, *pg.conn);
      return std::unexpected(
          RbacTestFailure{.kind = "setup_failed", .message = users.error()});
    }
    struct UserGuard {
      std::string run_id;
      PGconn* conn;
      UserGuard(std::string r, PGconn* c) : run_id{std::move(r)}, conn{c} {}
      ~UserGuard() {
        // propagate a std::expected error; cleanup is best-effort and the
        // reconciler sweep is the backstop.
        (void)plinth::rbac::destroy_run_users(run_id, *conn);
      }
      UserGuard(const UserGuard&) = delete;
      auto operator=(const UserGuard&) -> UserGuard& = delete;
      UserGuard(UserGuard&&) = delete;
      auto operator=(UserGuard&&) -> UserGuard& = delete;
    };
    UserGuard user_guard{report.run_id, pg.conn};

    for (const auto& rule : *rules) {
      if (owner_stop.stop_requested()) {
        return std::unexpected(cancelled_failure());
      }
      if (!rule.test_contract.has_value()) {
        continue;
      }
      auto rr = run_one_rule(pg.conn, rule.rule, *rule.test_contract, *users,
                             ctx.upgrade_drain_timeout_ms, report, owner_stop);
      if (!rr) {
        return std::unexpected(rr.error());
      }
    }
  }

  report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now() - report.started_at);

  if (owner_stop.stop_requested()) {
    return std::unexpected(cancelled_failure());
  }

  emit_rbac_test_audit(ctx, report, triggered_by);

  // Publish the completion timestamp and release the session lock in one SQL
  // statement. PostgreSQL does not expose the UPDATE until the autocommit
  // statement completes, while pg_advisory_unlock executes inside that same
  // statement. Therefore any observer that sees last_rbac_test_run_at also
  // knows the worker no longer owns the lifecycle lock.
  auto upd = write_result_update_state_and_release_lock(
      pg.conn, pkg->id, pkg->name, to_json(report), report.overall_passed());
  if (!upd) {
    return std::unexpected(upd.error());
  }
  lock_guard.dismiss();
  return report;
}

} // namespace

auto run_rbac_test(std::string_view package_id,
                   const plinth::packages::InstallerContext& ctx,
                   std::string_view triggered_by,
                   std::string_view run_id_override)
    -> std::expected<RbacTestReport, RbacTestFailure> {
  return run_rbac_test_impl(package_id, ctx, triggered_by, run_id_override,
                            std::stop_token{});
}

// ── CLI entry ─────────────────────────────────────────────────────

namespace {

// Resolve `extension_name` → package_id. Matches the uniq-by-name-
// active invariant: `plinth.packages WHERE name = $1 AND state IN
// ('ACTIVE', 'ACTIVE_FLAGGED')` is unique per `uniq_packages_name_active`
// partial index. Returns `package_not_found` if no row matches, or
// `invalid_state` if rows exist only in UPLOADING/VALIDATING/etc.
// (the CLI cannot Phase-B a package mid-install).
auto lookup_package_by_name(PGconn* conn, std::string_view name)
    -> std::expected<std::string, RbacTestFailure> {
  std::string name_s{name};
  std::array<const char*, 1> values = {name_s.c_str()};
  PgResultPtr res(
      PQexecParams(conn,
                   "SELECT id::text FROM plinth.packages "
                   "WHERE name = $1 AND state IN ('ACTIVE', 'ACTIVE_FLAGGED')",
                   1, nullptr, values.data(), nullptr, nullptr, 0),
      PQclear);
  if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
    return std::unexpected(RbacTestFailure{
        .kind = "db_error", .message = PQresultErrorMessage(res.get())});
  }
  int n = PQntuples(res.get());
  if (n == 0) {
    // Disambiguate: does a row exist for this name at all? If yes,
    // it's in a non-active state; otherwise the extension name is
    // unknown to the kernel.
    PgResultPtr any(
        PQexecParams(conn,
                     "SELECT state FROM plinth.packages WHERE name = $1 "
                     "ORDER BY installed_at DESC LIMIT 1",
                     1, nullptr, values.data(), nullptr, nullptr, 0),
        PQclear);
    if (PQresultStatus(any.get()) == PGRES_TUPLES_OK &&
        PQntuples(any.get()) > 0) {
      return std::unexpected(RbacTestFailure{
          .kind = "invalid_state",
          .message = "extension is not in ACTIVE or "
                     "ACTIVE_FLAGGED state",
          .detail = nlohmann::json{{"state", PQgetvalue(any.get(), 0, 0)},
                                   {"extension", name_s}}});
    }
    return std::unexpected(
        RbacTestFailure{.kind = "package_not_found",
                        .message = "no active package with that name",
                        .detail = nlohmann::json{{"extension", name_s}}});
  }
  if (n > 1) {
    // The uniq-by-name-active partial index prevents this in
    // practice; defensive coding per ICD §CLI Surface.
    return std::unexpected(
        RbacTestFailure{.kind = "invalid_state",
                        .message = "multiple active packages for name",
                        .detail = nlohmann::json{{"extension", name_s}}});
  }
  return std::string{PQgetvalue(res.get(), 0, 0)};
}

auto render_text_summary(const RbacTestReport& report, std::ostream& out)
    -> void {
  out << "RBAC test: " << report.package_name << " " << report.package_version
      << "\n";
  out << "run_id:   " << report.run_id << "\n";
  out << "duration: " << report.duration.count() << " ms\n";
  out << "passed:   " << report.passed.size() << "\n";
  out << "failed:   " << report.failed.size() << "\n";
  out << "skipped:  " << report.skipped.size() << "\n";
  if (!report.failed.empty()) {
    out << "\nFailures:\n";
    for (const auto& f : report.failed) {
      out << "  - rule=" << f.rule << " clause=" << f.clause
          << " expected=" << f.expected << " actual=" << f.actual << "\n";
    }
  }
  if (!report.skipped.empty()) {
    out << "\nSkipped (no test_contract):\n";
    for (const auto& s : report.skipped) {
      out << "  - " << s << "\n";
    }
  }
  out << "\n";
  if (report.overall_passed()) {
    out << "OK — all rules passed.\n";
  } else {
    out << "FAIL — " << report.failed.size() << " rule(s) failed.\n";
  }
}

} // namespace

auto run_cli_test_rbac(const CliTestRbacOptions& opts,
                       const plinth::packages::InstallerContext& ctx,
                       std::ostream& out, std::ostream& err) -> int {
  PgGuard pg(ctx.db);
  if (!pg.ok()) {
    err << "error: PG connect failed\n";
    return 2;
  }
  auto pid = lookup_package_by_name(pg.conn, opts.extension_name);
  if (!pid) {
    err << "error: " << pid.error().message;
    if (!pid.error().detail.empty()) {
      err << " (" << pid.error().detail.dump() << ")";
    }
    err << "\n";
    return 2;
  }
  auto report = run_rbac_test(*pid, ctx, "cli", opts.run_id);
  if (!report) {
    err << "error: " << report.error().kind << ": " << report.error().message
        << "\n";
    return 2;
  }
  if (opts.json_output) {
    out << to_json(*report).dump(2) << "\n";
  } else {
    render_text_summary(*report, out);
  }
  return report->overall_passed() ? 0 : 1;
}

// ── Owned asynchronous scheduler ──────────────────────────────────

auto start_async_workers() -> bool {
  return worker_registry().start();
}

auto shutdown_async_workers(std::chrono::milliseconds timeout) -> bool {
  bool drained = worker_registry().shutdown(timeout);
  if (!drained) {
    spdlog::error("rbac_test: shutdown timed out with {} worker(s)",
                  worker_registry().active_count());
  }
  return drained;
}

auto active_async_worker_count_for_test() -> std::size_t {
  return worker_registry().active_count();
}

auto schedule_rbac_test(std::string_view package_id,
                        const plinth::packages::InstallerContext& ctx,
                        std::string_view triggered_by) -> void {
  std::string pid{package_id};
  std::string trig{triggered_by};
  plinth::packages::InstallerContext ctx_copy =
      ctx; // value-copy; worker never references caller frame
  bool scheduled = worker_registry().schedule(
      [pid = std::move(pid), trig = std::move(trig),
       ctx_copy = std::move(ctx_copy)](std::stop_token stop) mutable {
        auto result = run_rbac_test_impl(pid, ctx_copy, trig, {}, stop);
        if (!result && result.error().kind != "cancelled") {
          spdlog::error("rbac_test: {}: {}", result.error().kind,
                        result.error().message);
        }
      });
  if (!scheduled) {
    spdlog::warn("rbac_test: scheduling rejected during shutdown");
  }
}

} // namespace plinth::packages::rbac_test
