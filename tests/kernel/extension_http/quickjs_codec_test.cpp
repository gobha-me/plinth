// SPDX-License-Identifier: MIT
// Actual-engine value-codec component controls. Sources/host cancellation
// controls are synthetic; no installed generation, real handler driver,
// cancellation queue, HTTP dispatch or physical native settlement is claimed.
#include "kernel/extension_http/quickjs_codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <limits>
#include <malloc.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using namespace plinth::extension_http;
using namespace std::chrono_literals;
constexpr auto ID = "11111111-1111-1111-1111-111111111111";
constexpr auto OTHER = "22222222-2222-2222-2222-222222222222";
constexpr auto RESPONSE = "({schema:1,status:207,headers:[['x-test','first'],['"
                          "x-test','second']],body:new Uint8Array([0,255,7])})";
static_assert(!std::is_copy_constructible_v<ChargedNativeResponse>);
static_assert(!std::is_copy_constructible_v<QuickJsCodecTurn>);
struct Value {
  JSContext* context;
  JSValue value;
  Value(JSContext* context_in, JSValue value_in)
      : context(context_in), value(value_in) {}
  ~Value() { JS_FreeValue(context, value); }
  Value(const Value&) = delete;
  auto operator=(const Value&) -> Value& = delete;
};
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
  const bool was_null = value == nullptr;
  auto* result = std::realloc(value, size);
  if (result && was_null) ++static_cast<Allocations*>(opaque)->blocks;
  return result;
}
auto usable_size(const void* value) -> std::size_t {
  // Linux/glibc allocator observer; it does not modify the owned allocation.
  return value ? ::malloc_usable_size(const_cast<void*>(value)) : 0;
}
constexpr JSMallocFunctions ALLOCATOR{.js_calloc = allocate_zero,
                                      .js_malloc = allocate,
                                      .js_free = deallocate,
                                      .js_realloc = reallocate,
                                      .js_malloc_usable_size = usable_size};
struct Host {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  std::shared_ptr<AdmissionLease> admission =
      pool.try_acquire("codecfixture", 0, process.try_acquire());
  std::shared_ptr<EngineHeapReservation> heap;
  bool live = true;
  Host() {
    auto result =
        EngineHeapReservation::create(admission, std::size_t{16} * 1024 * 1024);
    REQUIRE(result);
    heap = std::move(*result);
  }
};
auto remaining(JSContext* context, JSValueConst, int, JSValueConst*)
    -> JSValue {
  return JS_NewInt32(context, 4500);
}
auto unsubscribe(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_UNDEFINED;
}
auto on_abort(JSContext* context, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_NewCFunction(context, unsubscribe, "fixtureUnsubscribe", 0);
}
auto not_aborted(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_FALSE;
}
auto no_reason(JSContext*, JSValueConst, int, JSValueConst*) -> JSValue {
  return JS_NULL;
}
struct Engine {
  Host& host;
  JSRuntime* runtime = nullptr;
  JSContext* context = nullptr;
  JSValue cancel = JS_UNDEFINED, remaining_function = JS_UNDEFINED;
  std::unique_ptr<QuickJsCodecTurn> turn;
  std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + 5s;
  Engine(Host& host_in, Allocations& allocations) : host(host_in) {
    runtime = JS_NewRuntime2(&ALLOCATOR, &allocations);
    if (!runtime) return;
    JS_SetMemoryLimit(runtime, host.heap->bytes());
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
    if (!context) return;
    // Synthetic native controls ONLY; no real abort/registration semantics.
    cancel = JS_NewObject(context);
    remaining_function =
        JS_NewCFunction(context, remaining, "fixtureRemaining", 0);
    if (JS_IsException(cancel) || JS_IsException(remaining_function)) return;
    const auto aborted_key = JS_NewAtom(context, "aborted");
    const auto reason_key = JS_NewAtom(context, "reason");
    if (aborted_key == JS_ATOM_NULL || reason_key == JS_ATOM_NULL) {
      JS_FreeAtom(context, aborted_key);
      JS_FreeAtom(context, reason_key);
      return;
    }
    const auto first = JS_DefinePropertyGetSet(
        context, cancel, aborted_key,
        JS_NewCFunction(context, not_aborted, "fixtureAborted", 0),
        JS_UNDEFINED, JS_PROP_ENUMERABLE);
    const auto second = JS_DefinePropertyGetSet(
        context, cancel, reason_key,
        JS_NewCFunction(context, no_reason, "fixtureReason", 0), JS_UNDEFINED,
        JS_PROP_ENUMERABLE);
    JS_FreeAtom(context, aborted_key);
    JS_FreeAtom(context, reason_key);
    if (first < 0 || second < 0 ||
        JS_DefinePropertyValueStr(
            context, cancel, "onAbort",
            JS_NewCFunction(context, on_abort, "fixtureOnAbort", 1),
            JS_PROP_ENUMERABLE) < 0 ||
        JS_PreventExtensions(context, cancel) < 0)
      return;
    auto made = QuickJsCodecTurn::create(*context, host.admission, host.heap,
                                         [&host_in] { return host_in.live; });
    if (made) turn = std::move(*made);
  }
  ~Engine() {
    turn.reset();
    if (context) {
      JS_FreeValue(context, cancel);
      JS_FreeValue(context, remaining_function);
      JS_FreeContext(context);
    }
    if (runtime) JS_FreeRuntime(runtime);
    // Host retains its heap reservation through actual runtime destruction.
  }
  Engine(const Engine&) = delete;
  auto operator=(const Engine&) -> Engine& = delete;
  auto ready() const -> void {
    REQUIRE(runtime);
    REQUIRE(context);
    REQUIRE(turn);
  }
  auto evaluate(std::string_view source) -> JSValue {
    return JS_Eval(context, source.data(), source.size(), "codec-fixture.js",
                   JS_EVAL_TYPE_GLOBAL);
  }
  auto controls() const -> BorrowedContextControls {
    return {cancel, remaining_function};
  }
  auto integer(std::string_view source) -> int {
    const Value result{context, evaluate(source)};
    REQUIRE_FALSE(JS_IsException(result.value));
    int number = 0;
    REQUIRE(JS_IsNumber(result.value));
    REQUIRE(JS_ToInt32(context, &number, result.value) == 0);
    return number;
  }
};
auto input() -> NativeRequest {
  return {{ID,
           "POST",
           {"/http/a%20b", "/http/a b", "x=%FF&y=+"},
           {{"x-first", "one"}, {"x-first", "two"}}},
          {0, 255, 7}};
}
auto context_data() -> ContextData {
  return {ID,
          OTHER,
          ID,
          OTHER,
          9007199254740993ULL,
          9007203754740993ULL,
          "/http/*",
          {Principal::Kind::PAT, ID, OTHER},
          {ID, "codecfixture", "1.2.3", std::string(64, 'a'), OTHER}};
}
auto response(const InspectedReturn& result) -> const NativeResponse& {
  REQUIRE(std::holds_alternative<ChargedNativeResponse>(result));
  return std::get<ChargedNativeResponse>(result).value();
}
auto rejected(Engine& engine, std::string_view source) -> void {
  const Value object{engine.context, engine.evaluate(source)};
  REQUIRE_FALSE(JS_IsException(object.value));
  auto decoded = engine.turn->inspect_return(object.value);
  REQUIRE_FALSE(decoded);
  REQUIRE_FALSE(JS_HasException(engine.context));
}
auto trap_counter(Engine& engine) -> int {
  return engine.integer("globalThis.traps");
}
} // namespace

TEST_CASE(
    "HTTP QuickJS codec encodes exact frozen metadata and mutable local binary",
    "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    auto request = input();
    auto context = context_data();
    SECTION("PAT") {
    }
    SECTION("anonymous") {
      context.principal = {};
    }
    const Value handler{engine.context, engine.evaluate(R"JS((r,c)=>{
      const keys=(o,n)=>Reflect.ownKeys(o).length===n.length && n.every(k=>Object.hasOwn(o,k)&&Object.getOwnPropertyDescriptor(o,k).enumerable);
      const rk=['schema','request_id','method','path','original_path','query','headers','body'];
      const ck=['schema','dispatch_id','owner_epoch','adapter_epoch','clock_domain_id','admitted_at_ns','execution_cutoff_ns','matched_prefix','principal','extension','cancel','remaining_execution_ms'];
      const pk=c.principal.kind==='pat'?['kind','user_id','pat_id']:['kind'];
      const ek=['package_id','name','version','artifact_sha256','generation_receipt_id'];
      const ok=keys(r,rk)&&keys(c,ck)&&keys(c.principal,pk)&&keys(c.extension,ek)&&
        [r,c,r.headers,r.headers[0],r.headers[1],c.principal,c.extension].every(Object.isFrozen)&&
        r.schema===1&&c.schema===1&&r.request_id==='11111111-1111-1111-1111-111111111111'&&c.dispatch_id===r.request_id&&
        c.owner_epoch==='22222222-2222-2222-2222-222222222222'&&c.adapter_epoch===r.request_id&&c.clock_domain_id===c.owner_epoch&&
        (c.principal.kind==='anonymous'||(c.principal.user_id===r.request_id&&c.principal.pat_id===c.owner_epoch))&&
        r.method==='POST'&&r.path==='/http/a b'&&r.original_path==='/http/a%20b'&&r.query==='x=%FF&y=+'&&
        r.headers[0][0]==='x-first'&&r.headers[0][1]==='one'&&r.headers[1][1]==='two'&&
        c.admitted_at_ns==='9007199254740993'&&c.execution_cutoff_ns==='9007203754740993'&&c.matched_prefix==='/http/*'&&
        c.extension.package_id===r.request_id&&c.extension.generation_receipt_id===c.owner_epoch&&
        c.extension.name==='codecfixture'&&c.extension.version==='1.2.3'&&c.extension.artifact_sha256==='a'.repeat(64)&&
        !Object.isExtensible(c.cancel)&&c.cancel.aborted===false&&c.cancel.reason===null&&c.remaining_execution_ms()===4500&&
        r.body instanceof Uint8Array&&r.body.length===3&&r.body[0]===0&&r.body[1]===255&&r.body[2]===7;
      r.body[0]=99; return ok&&r.body[0]===99;
    })JS")};
    REQUIRE_FALSE(JS_IsException(handler.value));
    bool observed = false;
    auto encoded = engine.turn->with_encoded_call(
        request, context, engine.controls(),
        [&](JSValueConst r, JSValueConst c) -> QuickJsCodecResult<void> {
          JSValueConst args[] = {r, c};
          const Value result{
              engine.context,
              JS_Call(engine.context, handler.value, JS_UNDEFINED, 2, args)};
          if (JS_IsException(result.value)) {
            const Value error{engine.context, JS_GetException(engine.context)};
            return std::unexpected(QuickJsCodecError::ENGINE_FAILURE);
          }
          observed = JS_IsBool(result.value) &&
                     JS_ToBool(engine.context, result.value) == 1;
          return {};
        });
    REQUIRE(encoded);
    REQUIRE(observed);
    REQUIRE(request.body == Binary{0, 255, 7});
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec encodes binary empties and refuses invalid "
          "captured data",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    auto request = input();
    auto context = context_data();
    bool invalid = false;
    SECTION("empty body") {
      request.body.clear();
    }
    SECTION("credential header must already be removed") {
      request.data.headers.emplace_back("authorization", "secret-fixture");
      invalid = true;
    }
    SECTION("identity mismatch") {
      context.dispatch_id = OTHER;
      invalid = true;
    }
    SECTION("invalid UTF8 metadata") {
      request.data.headers[0].second = std::string(1, static_cast<char>(0xff));
      invalid = true;
    }
    SECTION("anonymous has no unused identity") {
      context.principal.kind = Principal::Kind::ANONYMOUS;
      invalid = true;
    }
    int calls = 0;
    std::size_t body_length = 999;
    auto result = engine.turn->with_encoded_call(
        request, context, engine.controls(),
        [&](JSValueConst r, JSValueConst) -> QuickJsCodecResult<void> {
          ++calls;
          const Value body{engine.context,
                           JS_GetPropertyStr(engine.context, r, "body")};
          const Value buffer{engine.context,
                             JS_GetTypedArrayBuffer(engine.context, body.value,
                                                    nullptr, &body_length,
                                                    nullptr)};
          return JS_IsException(buffer.value)
                     ? QuickJsCodecResult<void>{std::unexpected(
                           QuickJsCodecError::ENGINE_FAILURE)}
                     : QuickJsCodecResult<void>{};
        });
    if (invalid) {
      REQUIRE_FALSE(result);
      REQUIRE(calls == 0);
    } else {
      REQUIRE(result);
      REQUIRE(calls == 1);
      REQUIRE(body_length == 0);
    }
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec accepts semantic record order and literal "
          "repeated headers",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string source = RESPONSE;
    SECTION("permuted fields") {
      source = "({body:new "
               "Uint8Array([0,255,7]),headers:[['x-test','first'],['x-test','"
               "second']],status:207,schema:1})";
    }
    SECTION("unused inherited properties") {
      source = "Object.assign(Object.create({get unused(){throw "
               "Error('unused')}})," +
               std::string(RESPONSE) + ")";
    }
    const Value value{engine.context, engine.evaluate(source)};
    REQUIRE_FALSE(JS_IsException(value.value));
    auto decoded = engine.turn->inspect_return(value.value);
    REQUIRE(decoded);
    REQUIRE(response(*decoded) ==
            NativeResponse{
                207, {{"x-test", "first"}, {"x-test", "second"}}, {0, 255, 7}});
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec rejects proxies accessors symbols and "
          "nonenumerable extras without traps",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string transform;
    SECTION("root proxy") {
      transform = "return new Proxy(r,{ownKeys(){traps++;return "
                  "Reflect.ownKeys(r)},get(){traps++;throw Error()}})";
    }
    SECTION("revoked proxy") {
      transform = "let p=Proxy.revocable(r,{});p.revoke();return p.proxy";
    }
    SECTION("header proxy") {
      transform = "r.headers=new Proxy(r.headers,{get(){traps++;throw "
                  "Error()}});return r";
    }
    SECTION("pair proxy") {
      transform = "r.headers[0]=new Proxy(r.headers[0],{get(){traps++;throw "
                  "Error()}});return r";
    }
    SECTION("body proxy") {
      transform =
          "r.body=new Proxy(r.body,{get(){traps++;throw Error()}});return r";
    }
    SECTION("schema getter") {
      transform = "Object.defineProperty(r,'schema',{get(){traps++;return "
                  "1},enumerable:true});return r";
    }
    SECTION("pair getter") {
      transform = "Object.defineProperty(r.headers[0],'1',{get(){traps++;"
                  "return 'first'},enumerable:true});return r";
    }
    SECTION("own symbol") {
      transform = "r[Symbol('extra')]=1;return r";
    }
    SECTION("nonenumerable extra") {
      transform = "Object.defineProperty(r,'extra',{value:1});return r";
    }
    SECTION("nonenumerable required field") {
      transform = "Object.defineProperty(r,'status',{value:207,enumerable:"
                  "false});return r";
    }
    rejected(engine, "(()=>{globalThis.traps=0;let r=" + std::string(RESPONSE) +
                         ";" + transform + "})()");
    REQUIRE(trap_counter(engine) == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec rejects exotic and missing own fields without "
          "thenable coercion",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string expression;
    SECTION("date object") {
      expression = "Object.assign(new Date(),r)";
    }
    SECTION("array object") {
      expression = "Object.assign([],r)";
    }
    SECTION("function object") {
      expression = "Object.assign(()=>{},r)";
    }
    SECTION("inherited required data") {
      expression = "Object.create(r)";
    }
    SECTION("fake thenable") {
      expression = "Object.defineProperty(r,'then',{enumerable:true,get(){"
                   "traps++;throw Error()}})";
    }
    SECTION("symbol coercion") {
      expression =
          "Object.assign(r,{[Symbol.toPrimitive](){traps++;throw Error()}})";
    }
    rejected(engine,
             "(()=>{globalThis.traps=0;const r=" + std::string(RESPONSE) +
                 ";return " + expression + "})()");
    REQUIRE(trap_counter(engine) == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE(
    "HTTP QuickJS codec enforces dense exact header tuples and byte bounds",
    "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string headers;
    bool allowed = false;
    SECTION("hole") {
      headers = "new Array(1)";
    }
    SECTION("inherited element") {
      headers = "Object.assign(Object.create([['x','inherited']]),{length:1})";
    }
    SECTION("array with inherited hole") {
      headers =
          "(()=>{let a=new "
          "Array(1);Object.setPrototypeOf(a,[['x','inherited']]);return a})()";
    }
    SECTION("extra symbol") {
      headers = "Object.assign([],{[Symbol('x')]:1})";
    }
    SECTION("extra key") {
      headers = "Object.assign([],{extra:1})";
    }
    SECTION("wrong pair size") {
      headers = "[['x','one','two']]";
    }
    SECTION("pair hole") {
      headers = "[['x',,]]";
    }
    SECTION("non-string value") {
      headers = "[['x',7]]";
    }
    SECTION("sixty four pairs") {
      headers = "Array.from({length:64},()=>['x','a'.repeat(511)])";
      allowed = true;
    }
    SECTION("sixty five pairs") {
      headers = "Array.from({length:65},()=>['x','a'])";
    }
    SECTION("one byte over") {
      headers =
          "Array.from({length:64},(_,i)=>['x','a'.repeat(i===0?512:511)])";
    }
    const Value value{engine.context,
                      engine.evaluate("({schema:1,status:200,headers:" +
                                      headers + ",body:new Uint8Array()})")};
    REQUIRE_FALSE(JS_IsException(value.value));
    auto result = engine.turn->inspect_return(value.value);
    if (allowed) {
      REQUIRE(result);
      REQUIRE(response(*result).headers.size() == 64);
      REQUIRE(response(*result).headers[63].second == std::string(511, 'a'));
    } else
      REQUIRE_FALSE(result);
    REQUIRE_FALSE(JS_HasException(engine.context));
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec requires exact numeric and header scalar policy",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string change;
    SECTION("boolean schema") {
      change = "r.schema=true";
    }
    SECTION("string schema") {
      change = "r.schema='1'";
    }
    SECTION("boolean status") {
      change = "r.status=true";
    }
    SECTION("string status") {
      change = "r.status='200'";
    }
    SECTION("fraction") {
      change = "r.status=200.5";
    }
    SECTION("NaN") {
      change = "r.status=NaN";
    }
    SECTION("infinity") {
      change = "r.status=Infinity";
    }
    SECTION("BigInt") {
      change = "r.status=200n";
    }
    SECTION("below") {
      change = "r.status=199";
    }
    SECTION("above") {
      change = "r.status=600";
    }
    SECTION("coercive status") {
      change = "r.status={valueOf(){traps++;return 200}}";
    }
    SECTION("uppercase header") {
      change = "r.headers=[['X-Test','v']]";
    }
    SECTION("NUL header") {
      change = "r.headers=[['x','a\\u0000b']]";
    }
    SECTION("lone surrogate") {
      change = "r.headers=[['x','\\uD800']]";
    }
    SECTION("framing header") {
      change = "r.headers=[['content-length','1']]";
    }
    SECTION("cookie header") {
      change = "r.headers=[['set-cookie','a=b']]";
    }
    SECTION("CORS header") {
      change = "r.headers=[['access-control-allow-origin','*']]";
    }
    rejected(engine, "(()=>{globalThis.traps=0;let r=" + std::string(RESPONSE) +
                         ";" + change + ";return r})()");
    REQUIRE(trap_counter(engine) == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec snapshots exact binary slices independently of "
          "later mutation",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string body = "new Uint8Array(b,1,3)";
    SECTION("view slice") {
    }
    SECTION("regular ArrayBuffer") {
      body = "b";
    }
    const Value object{
        engine.context,
        engine.evaluate(
            "globalThis.fixtureBuffer=new "
            "Uint8Array([91,0,255,7,92]).buffer;(()=>{const "
            "b=fixtureBuffer;return {schema:1,status:200,headers:[],body:" +
            body + "}})()")};
    REQUIRE_FALSE(JS_IsException(object.value));
    auto result = engine.turn->inspect_return(object.value);
    REQUIRE(result);
    const auto expected =
        body == "b" ? Binary{91, 0, 255, 7, 92} : Binary{0, 255, 7};
    REQUIRE(response(*result).body == expected);
    const Value changed{
        engine.context,
        engine.evaluate("new Uint8Array(fixtureBuffer).fill(44)")};
    REQUIRE_FALSE(JS_IsException(changed.value));
    const Value global{engine.context, JS_GetGlobalObject(engine.context)};
    const Value buffer{
        engine.context,
        JS_GetPropertyStr(engine.context, global.value, "fixtureBuffer")};
    JS_DetachArrayBuffer(engine.context, buffer.value);
    REQUIRE(response(*result).body == expected);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec distinguishes empty detached shared and exact "
          "binary brands",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string body;
    bool allowed = false, detach = false;
    SECTION("empty ArrayBuffer") {
      body = "new ArrayBuffer(0)";
      allowed = true;
    }
    SECTION("empty Uint8Array") {
      body = "new Uint8Array()";
      allowed = true;
    }
    SECTION("clamped") {
      body = "new Uint8ClampedArray(2)";
    }
    SECTION("int8") {
      body = "new Int8Array(2)";
    }
    SECTION("data view") {
      body = "new DataView(new ArrayBuffer(2))";
    }
    SECTION("string") {
      body = "'binary'";
    }
    SECTION("shared buffer") {
      body = "new SharedArrayBuffer(2)";
    }
    SECTION("shared view") {
      body = "new Uint8Array(new SharedArrayBuffer(2))";
    }
    SECTION("out of bounds") {
      body = "(()=>{let b=new ArrayBuffer(4,{maxByteLength:8});let v=new "
             "Uint8Array(b,2,2);b.resize(1);return v})()";
    }
    SECTION("detached buffer") {
      body = "globalThis.fixtureBuffer=new ArrayBuffer(2)";
      detach = true;
    }
    SECTION("detached view") {
      body = "new Uint8Array(globalThis.fixtureBuffer=new ArrayBuffer(2))";
      detach = true;
    }
    SECTION("maximum") {
      body = "new Uint8Array(8388608)";
      allowed = true;
    }
    SECTION("one over maximum") {
      body = "new Uint8Array(8388609)";
    }
    const Value object{
        engine.context,
        engine.evaluate("({schema:1,status:200,headers:[],body:(" + body +
                        ")})")};
    REQUIRE_FALSE(JS_IsException(object.value));
    if (detach) {
      const Value global{engine.context, JS_GetGlobalObject(engine.context)};
      const Value buffer{
          engine.context,
          JS_GetPropertyStr(engine.context, global.value, "fixtureBuffer")};
      JS_DetachArrayBuffer(engine.context, buffer.value);
    }
    auto result = engine.turn->inspect_return(object.value);
    if (allowed) {
      REQUIRE(result);
      REQUIRE(response(*result).body.size() ==
              (body == "new Uint8Array(8388608)" ? MAX_BODY_BYTES : 0));
    } else
      REQUIRE_FALSE(result);
    REQUIRE_FALSE(JS_HasException(engine.context));
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec inspects only native promise slots without "
          "pumping or rejection coercion",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    std::string source;
    int state = 0;
    SECTION("fulfilled") {
      source = "Promise.resolve(" + std::string(RESPONSE) + ")";
      state = 1;
    }
    SECTION("pending") {
      source = "new Promise(()=>{})";
      state = 2;
    }
    SECTION("pending job remains unrun") {
      source = "Promise.resolve().then(()=>{globalThis.jobs++;return " +
               std::string(RESPONSE) + "})";
      state = 2;
    }
    SECTION("rejected reason") {
      source = "Promise.reject({toString(){traps++;throw Error()}})";
      state = 3;
    }
    SECTION("invalid fulfillment") {
      source = "Promise.resolve({get then(){traps++;return undefined}})";
      state = 0;
    }
    SECTION("proxy promise") {
      source = "new Proxy(Promise.resolve(" + std::string(RESPONSE) + "),{})";
      state = 0;
    }
    const Value object{
        engine.context,
        engine.evaluate("globalThis.traps=0;globalThis.jobs=0;" + source)};
    REQUIRE_FALSE(JS_IsException(object.value));
    // Promise.resolve can inspect user then during fixture preparation; reset
    // that counter before inspecting. The codec itself must not observe it.
    const Value reset{engine.context, engine.evaluate("globalThis.traps=0")};
    REQUIRE_FALSE(JS_IsException(reset.value));
    auto result = engine.turn->inspect_return(object.value);
    if (state == 0)
      REQUIRE_FALSE(result);
    else {
      REQUIRE(result);
      if (state == 1) REQUIRE(response(*result).body == Binary{0, 255, 7});
      if (state == 2)
        REQUIRE(std::holds_alternative<PendingNativePromise>(*result));
      if (state == 3)
        REQUIRE(std::holds_alternative<RejectedNativePromise>(*result));
    }
    REQUIRE(trap_counter(engine) == 0);
    REQUIRE(engine.integer("globalThis.jobs") == 0);
  }
  REQUIRE(allocations.blocks == 0);
}

TEST_CASE("HTTP QuickJS codec keeps copied response credits through actual "
          "engine destruction",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  Host host;
  std::optional<ChargedNativeResponse> copied;
  {
    Engine engine{host, allocations};
    engine.ready();
    const Value object{engine.context, engine.evaluate(RESPONSE)};
    REQUIRE_FALSE(JS_IsException(object.value));
    auto result = engine.turn->inspect_return(object.value);
    REQUIRE(result);
    copied.emplace(std::move(std::get<ChargedNativeResponse>(*result)));
  }
  REQUIRE(allocations.blocks == 0);
  host.heap.reset();
  host.admission.reset();
  REQUIRE(host.pool.snapshot().tasks == 1);
  REQUIRE(host.process.active_for_test() == 1);
  REQUIRE(copied->value().body == Binary{0, 255, 7});
  copied.reset();
  REQUIRE(host.pool.drain(std::chrono::steady_clock::now()));
  REQUIRE(host.process.active_for_test() == 0);
}

TEST_CASE("HTTP QuickJS codec refuses wrong budgets exhausted memory retired "
          "turns and thrown consumers",
          "[extension-http][http_js_codec]") {
  Allocations allocations;
  {
    Host host;
    Engine engine{host, allocations};
    engine.ready();
    SECTION("zero and unlimited budgets") {
      REQUIRE_FALSE(EngineHeapReservation::create(host.admission, 0));
      REQUIRE_FALSE(EngineHeapReservation::create(
          host.admission, std::numeric_limits<std::size_t>::max()));
    }
    SECTION("different admission") {
      auto other =
          host.pool.try_acquire("otherfixture", 0, host.process.try_acquire());
      REQUIRE(other);
      REQUIRE_FALSE(QuickJsCodecTurn::create(*engine.context, other, host.heap,
                                             [] { return true; }));
    }
    SECTION("retired before encoding") {
      host.live = false;
      int calls = 0;
      auto result = engine.turn->with_encoded_call(
          input(), context_data(), engine.controls(),
          [&](JSValueConst, JSValueConst) -> QuickJsCodecResult<void> {
            ++calls;
            return {};
          });
      REQUIRE_FALSE(result);
      REQUIRE(result.error() == QuickJsCodecError::RETIRED);
      REQUIRE(calls == 0);
    }
    SECTION("retired between encoding phases") {
      int checks = 0, calls = 0;
      auto turn =
          QuickJsCodecTurn::create(*engine.context, host.admission, host.heap,
                                   [&] { return checks++ < 2; });
      REQUIRE(turn);
      auto result = (*turn)->with_encoded_call(
          input(), context_data(), engine.controls(),
          [&](JSValueConst, JSValueConst) -> QuickJsCodecResult<void> {
            ++calls;
            return {};
          });
      REQUIRE_FALSE(result);
      REQUIRE(result.error() == QuickJsCodecError::RETIRED);
      REQUIRE(calls == 0);
    }
    SECTION("throwing consumer") {
      auto result = engine.turn->with_encoded_call(
          input(), context_data(), engine.controls(),
          [](JSValueConst, JSValueConst) -> QuickJsCodecResult<void> {
            throw std::runtime_error("fixture");
          });
      REQUIRE_FALSE(result);
      REQUIRE(result.error() == QuickJsCodecError::ENGINE_FAILURE);
      REQUIRE_FALSE(JS_HasException(engine.context));
    }
    SECTION("output reservation exhausted") {
      const Value object{engine.context, engine.evaluate(RESPONSE)};
      REQUIRE_FALSE(JS_IsException(object.value));
      auto charge = host.admission->reserve_extra(EXTRA_BYTES_PER_LEASE -
                                                  host.admission->extra_used() -
                                                  sizeof(ExtraMemoryCharge));
      REQUIRE(charge);
      auto result = engine.turn->inspect_return(object.value);
      REQUIRE_FALSE(result);
      REQUIRE(result.error() == QuickJsCodecError::RESOURCE_EXHAUSTED);
    }
    SECTION("actual engine allocation failure") {
      const Value object{engine.context, engine.evaluate(RESPONSE)};
      REQUIRE_FALSE(JS_IsException(object.value));
      JS_SetMemoryLimit(engine.runtime, 1);
      auto result = engine.turn->inspect_return(object.value);
      REQUIRE_FALSE(result);
      REQUIRE_FALSE(JS_HasException(engine.context));
    }
  }
  REQUIRE(allocations.blocks == 0);
}
