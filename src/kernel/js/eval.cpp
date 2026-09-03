// SPDX-License-Identifier: MIT
//
// Implementation of plinth::js::eval — see ICD-0.3.0-quickjs-vendoring.md.

#include "kernel/js/eval.hpp"

#include "kernel/js/eval_guard.hpp"
#include "kernel/js/runtime_config.hpp"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace plinth::js {

namespace {

// Per ICD §Memory Limit (Hard-Coded): 16 MiB, rigid in 0.3.0.
constexpr std::size_t MEMORY_LIMIT_BYTES = runtime_config::MAX_MEMORY_BYTES;

// Cap on JS→JSON recursion; protects against cyclic or pathologically
// deep structures. Well under anything a test will produce.
constexpr int MAX_CONVERSION_DEPTH = 64;

auto json_null() -> Json::Value {
  return Json::Value{Json::nullValue};
}

// Reads a string property from a JS object. Returns empty string on
// missing/non-string. Frees the intermediate JSValue.
auto read_string_prop(JSContext* ctx, JSValueConst obj, const char* name)
    -> std::string {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  std::string out;
  if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsException(v)) {
    const char* s = JS_ToCString(ctx, v);
    if (s != nullptr) {
      out.assign(s);
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, v);
  return out;
}

// Reads an integer property from a JS object. Returns 0 on missing/error.
auto read_int_prop(JSContext* ctx, JSValueConst obj, const char* name) -> int {
  JSValue v = JS_GetPropertyStr(ctx, obj, name);
  int out = 0;
  if (!JS_IsUndefined(v) && !JS_IsNull(v) && !JS_IsException(v)) {
    int32_t tmp = 0;
    if (JS_ToInt32(ctx, &tmp, v) == 0) {
      out = static_cast<int>(tmp);
    }
  }
  JS_FreeValue(ctx, v);
  return out;
}

// Extracts + classifies the pending exception on `ctx`. Must be called
// only after JS_IsException(value) returned true. Frees the exception.
auto extract_error(JSContext* ctx) -> EvalError {
  EvalError err{.kind = EvalErrorKind::RUNTIME_ERROR,
                .message = {},
                .line = 0,
                .column = 0};

  JSValue exc = JS_GetException(ctx);
  bool is_error_obj = JS_IsError(exc);

  std::string name;
  std::string msg;
  std::string stack;
  if (is_error_obj) {
    name = read_string_prop(ctx, exc, "name");
    msg = read_string_prop(ctx, exc, "message");
    stack = read_string_prop(ctx, exc, "stack");
    err.line = read_int_prop(ctx, exc, "lineNumber");
    err.column = read_int_prop(ctx, exc, "columnNumber");
  } else {
    // Non-Error throw (e.g. `throw "oops"`); coerce to string.
    const char* s = JS_ToCString(ctx, exc);
    if (s != nullptr) {
      msg.assign(s);
      JS_FreeCString(ctx, s);
    }
  }
  JS_FreeValue(ctx, exc);

  // Classification:
  //   InternalError + "out of memory" → MEMORY_LIMIT (see
  //     JS_ThrowOutOfMemory in quickjs.c).
  //   SyntaxError                     → SYNTAX_ERROR.
  //   anything else                   → RUNTIME_ERROR.
  if (name == "InternalError" &&
      msg.find("out of memory") != std::string::npos) {
    err.kind = EvalErrorKind::MEMORY_LIMIT;
    err.message = "out of memory: JS heap exceeded " +
                  std::to_string(MEMORY_LIMIT_BYTES) + " bytes";
    return err;
  }

  std::string combined;
  if (name.empty() && msg.empty()) {
    combined = "<unknown JS exception>";
  } else if (name.empty()) {
    combined = msg;
  } else {
    combined = name + ": " + msg;
  }
  if (!stack.empty()) {
    combined += "\n" + stack;
  }
  err.message = std::move(combined);
  err.kind = (name == "SyntaxError") ? EvalErrorKind::SYNTAX_ERROR
                                     : EvalErrorKind::RUNTIME_ERROR;
  return err;
}

struct ConvOutcome {
  Json::Value value = json_null();
  bool ok = true;
  EvalError error{};
};

auto conv_fail(std::string message) -> ConvOutcome {
  return ConvOutcome{.value = json_null(),
                     .ok = false,
                     .error = EvalError{.kind = EvalErrorKind::INTERNAL,
                                        .message = std::move(message),
                                        .line = 0,
                                        .column = 0}};
}

auto js_to_json(JSContext* ctx, JSValueConst v, int depth) -> ConvOutcome;

auto js_array_to_json(JSContext* ctx, JSValueConst arr, int depth)
    -> ConvOutcome {
  int64_t len = 0;
  if (JS_GetLength(ctx, arr, &len) < 0) {
    return conv_fail("js_to_json: failed to read array length");
  }
  ConvOutcome out{};
  out.value = Json::Value{Json::arrayValue};
  for (int64_t i = 0; i < len; ++i) {
    JSValue elem = JS_GetPropertyUint32(ctx, arr, static_cast<uint32_t>(i));
    if (JS_IsException(elem)) {
      JS_FreeValue(ctx, elem);
      return conv_fail("js_to_json: array element access threw");
    }
    ConvOutcome child = js_to_json(ctx, elem, depth + 1);
    JS_FreeValue(ctx, elem);
    if (!child.ok) {
      return child;
    }
    out.value.append(std::move(child.value));
  }
  return out;
}

auto js_plain_object_to_json(JSContext* ctx, JSValueConst obj, int depth)
    -> ConvOutcome {
  JSPropertyEnum* tab = nullptr;
  uint32_t tab_len = 0;
  if (JS_GetOwnPropertyNames(ctx, &tab, &tab_len, obj,
                             JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
    return conv_fail("js_to_json: JS_GetOwnPropertyNames failed");
  }
  ConvOutcome out{};
  out.value = Json::Value{Json::objectValue};
  for (uint32_t i = 0; i < tab_len; ++i) {
    // JS_GetOwnPropertyNames returns a heap-allocated C-array of
    // `tab_len` entries; indexing it is the documented QuickJS
    // iteration pattern. We free the whole block via
    // JS_FreePropertyEnum before returning.
    const char* key = JS_AtomToCString(ctx, tab[i].atom);
    JSValue val = JS_GetProperty(ctx, obj, tab[i].atom);
    if (key == nullptr || JS_IsException(val)) {
      if (key != nullptr) {
        JS_FreeCString(ctx, key);
      }
      JS_FreeValue(ctx, val);
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return conv_fail("js_to_json: property read failed");
    }
    ConvOutcome child = js_to_json(ctx, val, depth + 1);
    std::string key_copy{key};
    JS_FreeCString(ctx, key);
    JS_FreeValue(ctx, val);
    if (!child.ok) {
      JS_FreePropertyEnum(ctx, tab, tab_len);
      return child;
    }
    out.value[key_copy] = std::move(child.value);
  }
  JS_FreePropertyEnum(ctx, tab, tab_len);
  return out;
}

auto js_to_json(JSContext* ctx, JSValueConst v, int depth) -> ConvOutcome {
  if (depth > MAX_CONVERSION_DEPTH) {
    return conv_fail("js_to_json: max conversion depth exceeded");
  }
  if (JS_IsNull(v) || JS_IsUndefined(v)) {
    return ConvOutcome{};
  }
  if (JS_IsBool(v)) {
    int b = JS_ToBool(ctx, v);
    if (b < 0) {
      return conv_fail("js_to_json: bool coercion failed");
    }
    return ConvOutcome{.value = Json::Value{static_cast<bool>(b)}};
  }
  if (JS_IsNumber(v)) {
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
      int32_t i32 = 0;
      if (JS_ToInt32(ctx, &i32, v) == 0) {
        return ConvOutcome{.value = Json::Value{static_cast<Json::Int>(i32)}};
      }
    }
    double d = 0.0;
    if (JS_ToFloat64(ctx, &d, v) == 0) {
      return ConvOutcome{.value = Json::Value{d}};
    }
    return conv_fail("js_to_json: number coercion failed");
  }
  if (JS_IsString(v)) {
    const char* s = JS_ToCString(ctx, v);
    if (s == nullptr) {
      return conv_fail("js_to_json: string coercion failed");
    }
    ConvOutcome out{.value = Json::Value{std::string{s}}};
    JS_FreeCString(ctx, s);
    return out;
  }
  if (JS_IsArray(v)) {
    return js_array_to_json(ctx, v, depth);
  }
  if (JS_IsObject(v)) {
    return js_plain_object_to_json(ctx, v, depth);
  }
  return conv_fail("js_to_json: unsupported JS value type");
}

} // namespace

auto eval(std::string_view src) -> std::expected<Json::Value, EvalError> {
  JSRuntime* rt = JS_NewRuntime();
  if (rt == nullptr) {
    return std::unexpected(EvalError{.kind = EvalErrorKind::INTERNAL,
                                     .message = "JS_NewRuntime failed",
                                     .line = 0,
                                     .column = 0});
  }
  JS_SetMemoryLimit(rt, MEMORY_LIMIT_BYTES);

  JSContext* ctx = JS_NewContext(rt);
  if (ctx == nullptr) {
    JS_FreeRuntime(rt);
    return std::unexpected(EvalError{.kind = EvalErrorKind::INTERNAL,
                                     .message = "JS_NewContext failed",
                                     .line = 0,
                                     .column = 0});
  }

  std::expected<Json::Value, EvalError> result = Json::Value{Json::nullValue};

  // ICD-0.4.1 Layer 2 — pre-`JS_Eval` GlassWorm gate. Scan the host-
  // submitted source bytes before the runtime sees them.
  if (auto err = pre_eval_scan(src, "<eval>"); err.has_value()) {
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return std::unexpected(std::move(*err));
  }

  JSValue ev =
      JS_Eval(ctx, src.data(), src.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(ev)) {
    result = std::unexpected(extract_error(ctx));
  } else {
    ConvOutcome conv = js_to_json(ctx, ev, 0);
    if (conv.ok) {
      result = std::move(conv.value);
    } else {
      result = std::unexpected(std::move(conv.error));
    }
  }
  JS_FreeValue(ctx, ev);

  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  return result;
}

} // namespace plinth::js
