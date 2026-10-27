// SPDX-License-Identifier: MIT
// Actual-engine tests of the opt-in length-aware module hook ONLY. The small
// immutable source map is synthetic, not an installed artifact/generation or
// actual HTTP adapter. No existing runtime/caller is exercised by these tests.
#include "kernel/extension_http/codec.hpp"
#include "kernel/packages/restore_codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <quickjs.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <optional>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;
constexpr auto NAME_LIMIT = plinth::packages::restore::INVENTORY_PATH_MAX_BYTES;
struct Module {
  std::string_view name;
  std::string_view source;
};
constexpr std::array<Module, 3> SOURCES{{
    {"server/dep.js", "export const value = 41;"},
    {"server/nested/main.js",
     "import {value} from '../dep.js'; export {value};"},
    {"server/planet\xF0\x9F\xAA\x90.js", "export const value = 42;"},
}};
struct Allocations {
  std::size_t blocks = 0;
};
auto allocate(void* opaque, std::size_t size) -> void* {
  auto* value = std::malloc(size);
  if (value) ++static_cast<Allocations*>(opaque)->blocks;
  return value;
}
auto allocate_zero(void* opaque, std::size_t count, std::size_t size) -> void* {
  auto* value = std::calloc(count, size);
  if (value) ++static_cast<Allocations*>(opaque)->blocks;
  return value;
}
auto deallocate(void* opaque, void* value) -> void {
  if (value) --static_cast<Allocations*>(opaque)->blocks;
  std::free(value);
}
auto reallocate(void* opaque, void* value, std::size_t size) -> void* {
  if (size == 0) {
    deallocate(opaque, value);
    return nullptr;
  }
  auto* result = std::realloc(value, size);
  if (result && !value) ++static_cast<Allocations*>(opaque)->blocks;
  return result;
}
auto usable_size(const void* value) -> std::size_t {
  // Linux is the supported QuickJS platform. This glibc observer does not
  // mutate the owned malloc/calloc/realloc allocation despite its signature.
  return value ? ::malloc_usable_size(const_cast<void*>(value)) : 0;
}
constexpr JSMallocFunctions ALLOCATOR{.js_calloc = allocate_zero,
                                      .js_malloc = allocate,
                                      .js_free = deallocate,
                                      .js_realloc = reallocate,
                                      .js_malloc_usable_size = usable_size};
struct Value {
  JSContext* context;
  JSValue value;
  Value(JSContext* context_in, JSValue value_in)
      : context(context_in), value(value_in) {}
  ~Value() { JS_FreeValue(context, value); }
  Value(const Value&) = delete;
  auto operator=(const Value&) -> Value& = delete;
};
struct Policy {
  std::size_t validations = 0, normalizations = 0, loads = 0;
  std::size_t base_length = 0, specifier_length = 0;
  bool saw_nul = false, saw_invalid_scalar = false;
  bool reject = false, throw_exception = false, pending_with_success = false;
  std::string last_base, last_specifier;
};
// Deliberately finite synthetic graph, not a replacement implementation of
// HttpModuleIndex. Actual adapter integration must resolve its captured index.
auto resolve(std::string_view base, std::string_view specifier)
    -> std::optional<std::string> {
  if (!base.starts_with("server/") || base.size() > NAME_LIMIT ||
      base.find('\0') != base.npos ||
      !plinth::extension_http::valid_utf8(base) ||
      specifier.size() > NAME_LIMIT || specifier.find('\0') != specifier.npos ||
      !plinth::extension_http::valid_utf8(specifier))
    return std::nullopt;
  std::string name;
  if (specifier.starts_with("./")) {
    name = std::string{base.substr(0, base.rfind('/') + 1)} +
           std::string{specifier.substr(2)};
  } else if (specifier == "../dep.js" && base == "server/nested/main.js") {
    name = "server/dep.js";
  } else
    return std::nullopt;
  for (const auto& module : SOURCES)
    if (module.name == name) return name;
  return std::nullopt;
}
auto validate(JSContext* context, const char* base, std::size_t base_length,
              const char* specifier, std::size_t specifier_length,
              void* opaque) noexcept -> int {
  try {
    auto& policy = *static_cast<Policy*>(opaque);
    ++policy.validations;
    policy.base_length = base_length;
    policy.specifier_length = specifier_length;
    if ((!base && base_length) || (!specifier && specifier_length)) {
      JS_ThrowTypeError(context, "invalid fixture name pointer");
      return -1;
    }
    const std::string_view base_view{base ? base : "", base_length};
    const std::string_view specifier_view{specifier ? specifier : "",
                                          specifier_length};
    policy.saw_nul = base_view.find('\0') != base_view.npos ||
                     specifier_view.find('\0') != specifier_view.npos;
    policy.saw_invalid_scalar =
        !plinth::extension_http::valid_utf8(base_view) ||
        !plinth::extension_http::valid_utf8(specifier_view);
    // Never retain arbitrarily large rejected input just to report it.
    if (base_length <= NAME_LIMIT) policy.last_base.assign(base_view);
    if (specifier_length <= NAME_LIMIT)
      policy.last_specifier.assign(specifier_view);
    if (policy.throw_exception || policy.pending_with_success) {
      JS_ThrowTypeError(context, "fixture hook rejection");
      return policy.pending_with_success ? 0 : -1;
    }
    if (policy.reject)
      return -1; // Engine supplies the fixed fallback exception.
    if (!resolve(base_view, specifier_view)) {
      JS_ThrowTypeError(context, "fixture import policy rejected name");
      return -1;
    }
    return 0;
  } catch (...) {
    JS_ThrowInternalError(context, "fixture hook allocation failure");
    return -1;
  }
}
auto normalize(JSContext* context, const char* base, const char* specifier,
               void* opaque) noexcept -> char* {
  try {
    ++static_cast<Policy*>(opaque)->normalizations;
    const auto name = resolve(base, specifier);
    if (name) return js_strdup(context, name->c_str());
    JS_ThrowTypeError(context, "fixture normalization rejected name");
    return nullptr;
  } catch (...) {
    JS_ThrowInternalError(context, "fixture normalization allocation failure");
    return nullptr;
  }
}
auto load(JSContext* context, const char* name, void* opaque) noexcept
    -> JSModuleDef* {
  ++static_cast<Policy*>(opaque)->loads;
  for (const auto& module : SOURCES) {
    if (module.name != name) continue;
    const auto compiled =
        JS_Eval(context, module.source.data(), module.source.size(), name,
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return nullptr;
    auto* result = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    JS_FreeValue(context, compiled);
    return result;
  }
  JS_ThrowReferenceError(context, "fixture module missing");
  return nullptr;
}
struct Engine {
  // tracker/policy/source bytes outlive every context/job and runtime callback.
  Allocations& allocations;
  Policy policy;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + 5s;
  JSRuntime* runtime;
  JSContext* context;
  explicit Engine(Allocations& allocations_in)
      : allocations(allocations_in),
        runtime(JS_NewRuntime2(&ALLOCATOR, &allocations)), context(nullptr) {
    if (!runtime) return;
    JS_SetMemoryLimit(runtime, std::size_t{16} * 1024 * 1024);
    JS_SetMaxStackSize(runtime, std::size_t{512} * 1024);
    JS_SetInterruptHandler(
        runtime,
        [](JSRuntime*, void* opaque) {
          return std::chrono::steady_clock::now() >=
                         static_cast<Engine*>(opaque)->deadline
                     ? 1
                     : 0;
        },
        this);
    context = JS_NewContext(runtime);
    JS_SetModuleLoaderFunc(runtime, normalize, load, &policy);
    JS_SetModuleNameValidationFunc(runtime, validate, &policy);
  }
  ~Engine() {
    if (context) JS_FreeContext(context);
    if (runtime) JS_FreeRuntime(runtime);
  }
  Engine(const Engine&) = delete;
  auto operator=(const Engine&) -> Engine& = delete;
  auto ready() const -> void {
    REQUIRE(runtime);
    REQUIRE(context);
  }
  auto evaluate(std::string_view source, const char* name = "server/http.js")
      -> JSValue {
    // Initial captured source is compiled directly with a canonical filename.
    // It never masquerades as a non-relative user import accepted by the hook.
    return JS_Eval(context, source.data(), source.size(), name,
                   JS_EVAL_TYPE_MODULE);
  }
  auto pump() -> void {
    std::size_t jobs = 0;
    while (JS_IsJobPending(runtime)) {
      REQUIRE(++jobs <= 128);
      REQUIRE(std::chrono::steady_clock::now() < deadline);
      JSContext* job_context = nullptr;
      REQUIRE(JS_ExecutePendingJob(runtime, &job_context) >= 0);
    }
  }
  auto fulfilled(JSValueConst value) -> void {
    REQUIRE_FALSE(JS_IsException(value));
    REQUIRE(JS_IsPromise(value));
    pump();
    REQUIRE(JS_PromiseState(context, value) == JS_PROMISE_FULFILLED);
  }
  auto rejected(JSValueConst value) -> void {
    if (JS_IsException(value)) {
      Value error{context, JS_GetException(context)};
      REQUIRE(JS_IsError(error.value));
    } else {
      REQUIRE(JS_IsPromise(value));
      pump();
      REQUIRE(JS_PromiseState(context, value) == JS_PROMISE_REJECTED);
      Value error{context, JS_PromiseResult(context, value)};
      REQUIRE(JS_IsError(error.value));
    }
    REQUIRE_FALSE(JS_HasException(context));
  }
  auto answer() -> int {
    Value global{context, JS_GetGlobalObject(context)};
    Value answer{context, JS_GetPropertyStr(context, global.value, "answer")};
    REQUIRE(JS_IsNumber(answer.value));
    int result = 0;
    REQUIRE(JS_ToInt32(context, &result, answer.value) == 0);
    return result;
  }
};
} // namespace

TEST_CASE("HTTP QuickJS hook preserves complete static escaped NUL before "
          "normalization",
          "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    Value result{engine.context,
                 engine.evaluate("import {value} from './dep.js\\u0000suffix'; "
                                 "globalThis.answer=value;")};
    engine.rejected(result.value);
    REQUIRE(engine.policy.validations == 1);
    REQUIRE(engine.policy.saw_nul);
    REQUIRE(engine.policy.base_length == 14);
    REQUIRE(engine.policy.last_base == "server/http.js");
    REQUIRE(engine.policy.specifier_length == 15);
    REQUIRE(engine.policy.last_specifier ==
            std::string{"./dep.js\0suffix", 15});
    REQUIRE(engine.policy.normalizations == 0);
    REQUIRE(engine.policy.loads == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE(
    "HTTP QuickJS hook preserves computed dynamic NUL in the actual import job",
    "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    Value result{engine.context,
                 engine.evaluate(
                     "const name='./dep.js'+String.fromCharCode(0)+'suffix'; "
                     "await import(name);")};
    engine.rejected(result.value);
    REQUIRE(engine.policy.validations == 1);
    REQUIRE(engine.policy.saw_nul);
    REQUIRE(engine.policy.base_length == 14);
    REQUIRE(engine.policy.last_base == "server/http.js");
    REQUIRE(engine.policy.specifier_length == 15);
    REQUIRE(engine.policy.last_specifier ==
            std::string{"./dep.js\0suffix", 15});
    REQUIRE(engine.policy.normalizations == 0);
    REQUIRE(engine.policy.loads == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS hook allows owned relative static dynamic and Unicode "
          "module bytes",
          "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    std::string source;
    SECTION("static nested graph") {
      source =
          "import {value} from './nested/main.js'; globalThis.answer=value+1;";
    }
    SECTION("dynamic relative graph") {
      source = "globalThis.answer=(await import('./dep.js')).value+1;";
    }
    SECTION("decoded Unicode name") {
      source = "import {value} from './planet\\u{1FA90}.js'; "
               "globalThis.answer=value;";
    }
    Value result{engine.context, engine.evaluate(source)};
    engine.fulfilled(result.value);
    REQUIRE(engine.answer() == 42);
    REQUIRE(engine.policy.validations >= 1);
    REQUIRE(engine.policy.normalizations == engine.policy.validations);
    REQUIRE(engine.policy.loads == engine.policy.validations);
    REQUIRE_FALSE(engine.policy.saw_nul);
    REQUIRE_FALSE(engine.policy.saw_invalid_scalar);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS hook rejects nonrelative oversized and nonscalar "
          "decoded names",
          "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    std::string source, owned_filename;
    const char* filename = "server/http.js";
    SECTION("bare static") {
      source = "import 'dep.js';";
    }
    SECTION("absolute dynamic") {
      source = "await import('/server/dep.js');";
    }
    SECTION("URL dynamic") {
      source = "await import('https://example.test/dep.js');";
    }
    SECTION("oversized computed") {
      source = "await import('./'+'x'.repeat(1023));";
    }
    SECTION("static lone surrogate") {
      source = "import './\\uD800.js';";
    }
    SECTION("dynamic lone surrogate") {
      source = "await import('./'+String.fromCharCode(0xD800)+'.js');";
    }
    SECTION("invalid kernel base fixture") {
      source = "import './dep.js';";
      filename = "outside/http.js";
    }
    SECTION("oversized kernel base fixture") {
      source = "import './dep.js';";
      owned_filename = "server/" + std::string(NAME_LIMIT - 6, 'x');
      filename = owned_filename.c_str();
    }
    Value result{engine.context, engine.evaluate(source, filename)};
    engine.rejected(result.value);
    REQUIRE(engine.policy.validations == 1);
    REQUIRE(engine.policy.normalizations == 0);
    REQUIRE(engine.policy.loads == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE(
    "HTTP QuickJS hook validates static and dynamic imports before cache hits",
    "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    Value initial{
        engine.context,
        engine.evaluate(
            "import {value} from './dep.js'; globalThis.answer=value+1;")};
    engine.fulfilled(initial.value);
    REQUIRE(engine.answer() == 42);
    REQUIRE(engine.policy.loads == 1);
    Value cached{engine.context, engine.evaluate("await import('./dep.js');",
                                                 "server/cache.js")};
    engine.fulfilled(cached.value);
    REQUIRE(engine.policy.validations == 2);
    REQUIRE(engine.policy.normalizations == 2);
    REQUIRE(engine.policy.loads == 1);
    std::string source;
    SECTION("static suffix must not hit loaded prefix") {
      source = "import './dep.js\\u0000suffix';";
    }
    SECTION("dynamic suffix must not hit loaded prefix") {
      source = "await import('./dep.js'+String.fromCharCode(0)+'suffix');";
    }
    SECTION("new policy denial on otherwise valid cache hit") {
      engine.policy.reject = true;
      source = "await import('./dep.js');";
    }
    Value denied{engine.context, engine.evaluate(source, "server/denied.js")};
    engine.rejected(denied.value);
    REQUIRE(engine.policy.validations == 3);
    REQUIRE(engine.policy.normalizations == 2);
    REQUIRE(engine.policy.loads == 1);
    REQUIRE(engine.answer() == 42);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE(
    "HTTP QuickJS hook exception rejection and clearing have owned cleanup",
    "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    SECTION("explicit pending exception") {
      engine.policy.throw_exception = true;
    }
    SECTION("pending exception despite success return") {
      engine.policy.pending_with_success = true;
    }
    SECTION("failure without pending exception") {
      engine.policy.reject = true;
    }
    Value result{engine.context, engine.evaluate("await import('./dep.js');")};
    engine.rejected(result.value);
    REQUIRE(engine.policy.validations == 1);
    REQUIRE(engine.policy.normalizations == 0);
    REQUIRE(engine.policy.loads == 0);
    JS_SetModuleNameValidationFunc(engine.runtime, nullptr, nullptr);
    Value unhooked{
        engine.context,
        engine.evaluate("globalThis.answer=(await import('./dep.js')).value+1;",
                        "server/unhooked.js")};
    engine.fulfilled(unhooked.value);
    REQUIRE(engine.answer() == 42);
    REQUIRE(engine.policy.validations == 1);
    REQUIRE(engine.policy.normalizations == 1);
    REQUIRE(engine.policy.loads == 1);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE(
    "HTTP QuickJS hook runtime owns queued import through context destruction",
    "[extension-http][http_module_boundary][quickjs]") {
  Allocations allocations;
  {
    Engine engine{allocations};
    engine.ready();
    Value result{engine.context, engine.evaluate("await import('./dep.js');")};
    REQUIRE_FALSE(JS_IsException(result.value));
    REQUIRE(JS_IsPromise(result.value));
    REQUIRE(JS_IsJobPending(engine.runtime));
    REQUIRE(engine.policy.validations == 0);
    // Deliberately do not pump: runtime/context free their actual queued job
    // while hook/source/allocator owners remain alive. This is not a host-task
    // cancellation or actual HTTP/runtime-adapter settlement claim.
  }
  REQUIRE(allocations.blocks == 0);
}
