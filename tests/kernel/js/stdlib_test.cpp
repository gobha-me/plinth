// SPDX-License-Identifier: MIT
//
// Milestone tests for ICD-0.3.2-kernel-stdlib-sync §Milestone
// Criteria. Each TEST_CASE maps 1:1 to one of the four Tests groups
// in the ICD. The pattern is:
//   1. Construct a RuntimePool with a targeted Config.
//   2. Acquire a BridgeContext; eval JS that exercises the binding.
//   3. Assert on the Json::Value result OR the EvalError.
//   4. Release (not destroy) so the next SECTION gets a clean
//      globalThis — the pool re-injects the stdlib on release().

#include <catch2/catch_test_macros.hpp>

#include "kernel/config.hpp"
#include "kernel/js/bridge_context.hpp"
#include "kernel/js/eval.hpp"
#include "kernel/js/runtime_pool.hpp"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using plinth::Config;
using plinth::js::BridgeContext;
using plinth::js::default_runtime_limits;
using plinth::js::eval_on_context;
using plinth::js::EvalErrorKind;
using plinth::js::RuntimePool;

namespace {

// Minimal in-memory spdlog sink — captures every formatted message so
// the log tests can assert on what got emitted. Thread-safe; each test
// case instantiates its own pointer and wires it via a dedicated
// logger set as the spdlog default.
class CapturingSink : public spdlog::sinks::base_sink<std::mutex> {
 public:
  [[nodiscard]] auto messages() -> std::vector<std::string> {
    std::lock_guard<std::mutex> g(this->mutex_);
    return lines;
  }
  [[nodiscard]] auto contains(std::string_view needle) -> bool {
    std::lock_guard<std::mutex> g(this->mutex_);
    return std::ranges::any_of(lines, [&](const std::string& s) {
      return s.find(needle) != std::string::npos;
    });
  }

 protected:
  auto sink_it_(const spdlog::details::log_msg& m) -> void override {
    spdlog::memory_buf_t formatted;
    formatter_->format(m, formatted);
    lines.emplace_back(formatted.data(), formatted.size());
  }
  auto flush_() -> void override {}

 private:
  std::vector<std::string> lines;
};

// Swap in a capturing spdlog default logger for the duration of a
// test, restoring the previous default on destruction. All
// plinth::log::* calls route through spdlog::default_logger(), so
// this captures everything the bindings emit.
class ScopedDefaultLogger {
 public:
  explicit ScopedDefaultLogger(std::shared_ptr<CapturingSink> s)
      : sink(std::move(s)), previous(spdlog::default_logger()) {
    auto logger = std::make_shared<spdlog::logger>("stdlib_test", sink);
    logger->set_level(spdlog::level::trace);
    spdlog::set_default_logger(logger);
  }
  ~ScopedDefaultLogger() { spdlog::set_default_logger(previous); }
  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  auto operator=(const ScopedDefaultLogger&) -> ScopedDefaultLogger& = delete;
  ScopedDefaultLogger(ScopedDefaultLogger&&) = delete;
  auto operator=(ScopedDefaultLogger&&) -> ScopedDefaultLogger& = delete;

 private:
  std::shared_ptr<CapturingSink> sink;
  std::shared_ptr<spdlog::logger> previous;
};

auto make_pool(const Config& cfg, int pool_size = 1) -> RuntimePool {
  return {/*ext=*/nullptr, default_runtime_limits(), cfg, pool_size};
}

} // namespace

// ─── [0.3.2] Milestone Test 1 — log.* ────────────────────────────────

TEST_CASE("stdlib: log.* forwards to plinth::log at the right level",
          "[js][stdlib][log]") {
  auto sink = std::make_shared<CapturingSink>();
  ScopedDefaultLogger guard(sink);

  Config cfg{};
  auto pool = make_pool(cfg);

  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);

  SECTION("info level captured") {
    auto r = eval_on_context(*bc, "log.info('hello')");
    REQUIRE(r.has_value());
    REQUIRE(sink->contains("hello"));
  }
  SECTION("debug / warn / error all route correctly") {
    // spdlog default sink level defaults to info; bump the logger
    // to trace so debug messages land in the capture.
    spdlog::set_level(spdlog::level::trace);
    auto r = eval_on_context(
        *bc, "log.debug('d-msg'); log.warn('w-msg'); log.error('e-msg'); 1");
    REQUIRE(r.has_value());
    REQUIRE(sink->contains("d-msg"));
    REQUIRE(sink->contains("w-msg"));
    REQUIRE(sink->contains("e-msg"));
  }
  SECTION("ctx object appended as single-line JSON suffix") {
    auto r =
        eval_on_context(*bc, "log.info('hi', { extension_id: 'test', n: 7 })");
    REQUIRE(r.has_value());
    REQUIRE(sink->contains("hi ctx="));
    REQUIRE(sink->contains("\"extension_id\":\"test\""));
    REQUIRE(sink->contains("\"n\":7"));
  }
  SECTION("non-string message throws TypeError") {
    auto r = eval_on_context(*bc, "log.info(42)");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::RUNTIME_ERROR);
    REQUIRE(r.error().message.find("TypeError") != std::string::npos);
  }

  pool.destroy(bc);
}

// [0.3.3.3] ICD-0.3.2 §Security Constraint 5 — `log.*` MUST NOT
// auto-inject caller identity and MUST NOT overwrite caller-supplied
// keys. The kernel-computed provenance path belongs to the audit API
// (0.3.3), not to log. This test asserts the weaker log.* contract:
// whatever the caller puts in the ctx payload is emitted verbatim.
TEST_CASE("stdlib: log.* preserves caller-supplied ctx and does not inject "
          "kernel fields",
          "[js][stdlib][log][security]") {
  auto sink = std::make_shared<CapturingSink>();
  ScopedDefaultLogger guard(sink);

  Config cfg{};
  // Populate projection fields that a naive implementation might be
  // tempted to splice into the ctx (extension_id is forward-reserved;
  // node_id is the current projection surface).
  cfg.node_id = "kernel-node-real";
  auto pool = make_pool(cfg);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);

  // Caller supplies keys that kernel-side audit would compute. log.*
  // must preserve them verbatim and MUST NOT replace them with
  // BridgeContext-derived values.
  const auto* src = R"(
        log.info('auth', {
            extension_id: 'forged-ext',
            user_id: '00000000-0000-0000-0000-000000000000',
            node_id: 'attacker-node'
        })
    )";
  auto r = eval_on_context(*bc, src);
  REQUIRE(r.has_value());

  // All three caller-supplied values must appear verbatim.
  REQUIRE(sink->contains("\"extension_id\":\"forged-ext\""));
  REQUIRE(
      sink->contains("\"user_id\":\"00000000-0000-0000-0000-000000000000\""));
  REQUIRE(sink->contains("\"node_id\":\"attacker-node\""));

  // The real kernel node_id must NOT have been spliced into this log
  // line — log.* is not the non-forgeable identity path.
  REQUIRE_FALSE(sink->contains("kernel-node-real"));

  pool.destroy(bc);
}

// ─── [0.3.2] Milestone Test 2 — config.get ────────────────────────────

TEST_CASE("stdlib: config.get returns whitelisted keys and null otherwise",
          "[js][stdlib][config]") {
  Config cfg{};
  cfg.dev_mode = true;
  cfg.node_id = "test-node";
  cfg.listen_port = 9999;
  cfg.registration_enabled = false;
  cfg.ws_auth_timeout_s = 4.5;
  // secrets populated so we can assert they do NOT leak out:
  cfg.db.password = "super-secret";

  auto pool = make_pool(cfg);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);

  SECTION("whitelisted scalar keys") {
    auto r1 = eval_on_context(*bc, "config.get('node_id')");
    REQUIRE(r1.has_value());
    REQUIRE(r1->asString() == "test-node");

    auto r2 = eval_on_context(*bc, "config.get('dev_mode')");
    REQUIRE(r2.has_value());
    REQUIRE(r2->asBool() == true);

    auto r3 = eval_on_context(*bc, "config.get('listen_port')");
    REQUIRE(r3.has_value());
    REQUIRE(r3->asInt() == 9999);

    auto r4 = eval_on_context(*bc, "config.get('registration_enabled')");
    REQUIRE(r4.has_value());
    REQUIRE(r4->asBool() == false);

    auto r5 = eval_on_context(*bc, "config.get('ws.auth_timeout_s')");
    REQUIRE(r5.has_value());
    REQUIRE(r5->asDouble() == 4.5);
  }
  SECTION("unknown key returns null") {
    auto r = eval_on_context(*bc, "config.get('unknown_key')");
    REQUIRE(r.has_value());
    REQUIRE(r->isNull());
  }
  SECTION("secret-flagged keys are not in projection — return null") {
    // Neither the dotted form an extension might guess at, nor the
    // raw struct field, leak out. The projection is a whitelist.
    for (const char* key : {"db.password", "db.host", "db.user", "db.database",
                            "migrations_dir"}) {
      std::string js = std::string{"config.get('"} + key + "')";
      auto r = eval_on_context(*bc, js);
      REQUIRE(r.has_value());
      REQUIRE(r->isNull());
    }
  }
  SECTION("non-string key throws TypeError") {
    auto r = eval_on_context(*bc, "config.get(42)");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::RUNTIME_ERROR);
    REQUIRE(r.error().message.find("TypeError") != std::string::npos);
  }

  pool.destroy(bc);
}

// ─── [0.3.2] Milestone Test 3 — crypto.hash ──────────────────────────

TEST_CASE("stdlib: crypto.hash correctness + algorithm whitelist",
          "[js][stdlib][crypto]") {
  Config cfg{};
  auto pool = make_pool(cfg);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);

  SECTION("sha256 empty string vector") {
    auto r = eval_on_context(*bc, "crypto.hash('sha256', '')");
    REQUIRE(r.has_value());
    REQUIRE(r->asString() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934"
                             "ca495991b7852b855");
  }
  SECTION("sha512 'abc' known vector") {
    auto r = eval_on_context(*bc, "crypto.hash('sha512', 'abc')");
    REQUIRE(r.has_value());
    REQUIRE(r->asString() == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea2"
                             "0a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
                             "454d4423643ce80e2a9ac94fa54ca49f");
  }
  SECTION("Uint8Array input matches string input for same bytes") {
    auto r = eval_on_context(
        *bc, "crypto.hash('sha256', new Uint8Array([0x61, 0x62, 0x63]))");
    REQUIRE(r.has_value());
    // SHA-256("abc")
    REQUIRE(r->asString() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
                             "b410ff61f20015ad");
  }
  SECTION("weak / unknown algorithm throws RangeError") {
    auto r = eval_on_context(*bc, "crypto.hash('md5', 'x')");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::RUNTIME_ERROR);
    REQUIRE(r.error().message.find("RangeError") != std::string::npos);

    auto r2 = eval_on_context(*bc, "crypto.hash('sha1', 'x')");
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().message.find("RangeError") != std::string::npos);
  }
  SECTION("missing arg throws TypeError") {
    auto r = eval_on_context(*bc, "crypto.hash('sha256')");
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind == EvalErrorKind::RUNTIME_ERROR);
    REQUIRE(r.error().message.find("TypeError") != std::string::npos);
  }

  pool.destroy(bc);
}

// ─── [0.3.2] Milestone Test 4 — crypto.randomBytes + timingSafeEqual ──

TEST_CASE("stdlib: crypto.randomBytes + crypto.timingSafeEqual",
          "[js][stdlib][crypto]") {
  Config cfg{};
  auto pool = make_pool(cfg);
  BridgeContext* bc = pool.acquire();
  REQUIRE(bc != nullptr);

  SECTION("randomBytes returns Uint8Array of requested length") {
    auto r = eval_on_context(*bc, "const b = crypto.randomBytes(32);"
                                  "[b.constructor.name, b.length]");
    REQUIRE(r.has_value());
    REQUIRE(r->isArray());
    REQUIRE((*r)[0].asString() == "Uint8Array");
    REQUIRE((*r)[1].asInt() == 32);
  }
  SECTION("two independent calls produce different outputs") {
    auto r = eval_on_context(
        *bc, "const a = crypto.randomBytes(16);"
             "const b = crypto.randomBytes(16);"
             "let differ = false;"
             "for (let i=0;i<16;i++) if (a[i]!==b[i]) { differ=true; break; }"
             "differ");
    REQUIRE(r.has_value());
    REQUIRE(r->asBool() == true);
  }
  SECTION("out-of-range counts throw RangeError") {
    auto r1 = eval_on_context(*bc, "crypto.randomBytes(0)");
    REQUIRE_FALSE(r1.has_value());
    REQUIRE(r1.error().message.find("RangeError") != std::string::npos);

    auto r2 = eval_on_context(*bc, "crypto.randomBytes(5000)");
    REQUIRE_FALSE(r2.has_value());
    REQUIRE(r2.error().message.find("RangeError") != std::string::npos);
  }
  SECTION("timingSafeEqual returns true on equal buffers") {
    auto r =
        eval_on_context(*bc, "crypto.timingSafeEqual(new Uint8Array([1,2,3]),"
                             "                       new Uint8Array([1,2,3]))");
    REQUIRE(r.has_value());
    REQUIRE(r->asBool() == true);
  }
  SECTION("timingSafeEqual returns false on different-value buffers") {
    auto r =
        eval_on_context(*bc, "crypto.timingSafeEqual(new Uint8Array([1,2,3]),"
                             "                       new Uint8Array([1,2,4]))");
    REQUIRE(r.has_value());
    REQUIRE(r->asBool() == false);
  }
  SECTION("timingSafeEqual returns false on different-length buffers") {
    auto r =
        eval_on_context(*bc, "crypto.timingSafeEqual(new Uint8Array([1,2,3]),"
                             "                       new Uint8Array([1,2]))");
    REQUIRE(r.has_value());
    REQUIRE(r->asBool() == false);
  }

  pool.destroy(bc);
}

// ─── Ancillary — stdlib survives release/reacquire ───────────────────
//
// `release()` clears globalThis own props between uses; the pool must
// re-inject the stdlib so a reacquired context still has log/config/
// crypto available.

TEST_CASE("stdlib: survives release and reacquire", "[js][stdlib]") {
  Config cfg{};
  auto pool = make_pool(cfg);

  BridgeContext* bc1 = pool.acquire();
  REQUIRE(bc1 != nullptr);
  auto r1 = eval_on_context(*bc1, "typeof log.info");
  REQUIRE(r1.has_value());
  REQUIRE(r1->asString() == "function");
  pool.release(bc1);

  BridgeContext* bc2 = pool.acquire();
  REQUIRE(bc2 != nullptr);
  auto r2 = eval_on_context(*bc2, "typeof config.get");
  REQUIRE(r2.has_value());
  REQUIRE(r2->asString() == "function");
  auto r3 = eval_on_context(*bc2, "typeof crypto.hash");
  REQUIRE(r3.has_value());
  REQUIRE(r3->asString() == "function");
  pool.release(bc2);
}
