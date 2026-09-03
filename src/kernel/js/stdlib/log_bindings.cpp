// SPDX-License-Identifier: MIT
//
// `log.*` host bindings — ICD-0.3.2 §Injected Surface / log.*.
//
// Each level (debug/info/warn/error) is a thin wrapper around the
// matching `plinth::log::*` template. `ctx` — when supplied — is
// serialized via `JS_JSONStringify` into a single-line JSON string
// and appended to the log line as `" ctx={...}"`. No identity fields
// are auto-injected; see ICD-0.3.2 §Security Constraint 5.

#include "kernel/js/stdlib_inject.hpp"
#include "kernel/logging.hpp"

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace plinth::js {

namespace {

// Coerces `args[0]` into a UTF-8 std::string and renders the optional
// `args[1]` ctx object into ` ctx={…}` (single-line JSON). Returns
// JS_EXCEPTION if the first arg is missing or the wrong type.
auto build_log_line(JSContext* ctx, std::span<const JSValue> args,
                    std::string& out, JSValue& err) -> bool {
  if (args.empty() || !JS_IsString(args[0])) {
    err = JS_ThrowTypeError(ctx, "log: expected string at arg 0");
    return false;
  }
  const char* msg = JS_ToCString(ctx, args[0]);
  if (msg == nullptr) {
    err = JS_EXCEPTION; // already thrown by QuickJS
    return false;
  }
  out.assign(msg);
  JS_FreeCString(ctx, msg);

  if (args.size() >= 2 && !JS_IsUndefined(args[1]) && !JS_IsNull(args[1])) {
    if (!JS_IsObject(args[1])) {
      err = JS_ThrowTypeError(ctx, "log: expected object at arg 1");
      return false;
    }
    JSValue json_str =
        JS_JSONStringify(ctx, args[1], JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json_str)) {
      err = JS_EXCEPTION;
      return false;
    }
    const char* s = JS_ToCString(ctx, json_str);
    if (s != nullptr) {
      out.append(" ctx=");
      out.append(s);
      JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, json_str);
  }
  return true;
}

// Wraps the raw QuickJS argv pointer into a std::span — the single
// place where pointer arithmetic lives. All `log_*` host functions
// hand their (argc, argv) to this helper before using.
auto make_arg_span(int argc, JSValue* argv) -> std::span<const JSValue> {
  return {argv, static_cast<std::size_t>(argc)};
}

auto log_debug(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  std::string line;
  JSValue err = JS_UNDEFINED;
  if (!build_log_line(ctx, make_arg_span(argc, argv), line, err)) {
    return err;
  }
  plinth::log::debug("{}", line);
  return JS_UNDEFINED;
}

auto log_info(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  std::string line;
  JSValue err = JS_UNDEFINED;
  if (!build_log_line(ctx, make_arg_span(argc, argv), line, err)) {
    return err;
  }
  plinth::log::info("{}", line);
  return JS_UNDEFINED;
}

auto log_warn(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  std::string line;
  JSValue err = JS_UNDEFINED;
  if (!build_log_line(ctx, make_arg_span(argc, argv), line, err)) {
    return err;
  }
  plinth::log::warn("{}", line);
  return JS_UNDEFINED;
}

auto log_error(JSContext* ctx, JSValue /*this_val*/, int argc, JSValue* argv)
    -> JSValue {
  std::string line;
  JSValue err = JS_UNDEFINED;
  if (!build_log_line(ctx, make_arg_span(argc, argv), line, err)) {
    return err;
  }
  plinth::log::error("{}", line);
  return JS_UNDEFINED;
}

} // namespace

auto register_log(JSContext* ctx) -> void {
  inject_sync_fn(ctx, "log", "debug", &log_debug, 2);
  inject_sync_fn(ctx, "log", "info", &log_info, 2);
  inject_sync_fn(ctx, "log", "warn", &log_warn, 2);
  inject_sync_fn(ctx, "log", "error", &log_error, 2);
}

} // namespace plinth::js
