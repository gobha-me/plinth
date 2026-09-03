// SPDX-License-Identifier: MIT
//
// Kernel stdlib injection — generic helpers. Per-namespace binding
// implementations live in `src/kernel/js/stdlib/*.cpp`. See
// ICD-0.3.2-kernel-stdlib-sync.md.

#include "kernel/js/stdlib_inject.hpp"

#include <initializer_list>

#include <quickjs.h>

namespace plinth::js {

auto inject_sync_fn(JSContext* ctx, const char* ns, const char* name,
                    JSCFunction* fn, int argc_hint) -> void {
  JSValue global = JS_GetGlobalObject(ctx);

  // Re-use an existing namespace object if one already lives on
  // globalThis (e.g. register_crypto running after register_log has
  // already created `log`). Create a fresh object otherwise.
  JSValue ns_obj = JS_GetPropertyStr(ctx, global, ns);
  if (JS_IsUndefined(ns_obj) || !JS_IsObject(ns_obj)) {
    JS_FreeValue(ctx, ns_obj);
    ns_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, global, ns, JS_DupValue(ctx, ns_obj));
  }

  JSValue jsfn = JS_NewCFunction(ctx, fn, name, argc_hint);
  JS_SetPropertyStr(ctx, ns_obj, name, jsfn);

  JS_FreeValue(ctx, ns_obj);
  JS_FreeValue(ctx, global);
}

// 0.3.5 (ICD-0.3.5 §N.43 + §Security Constraints 5). Delete
// `globalThis.eval` and `globalThis.Function` so extension script
// sources cannot construct a dynamic code path outside the kernel
// surface. DESIGN §9.1 specifies "`eval` disabled by default." Must
// run BEFORE the per-namespace registrars in case any stdlib binding
// relies on internal `eval` during setup (currently none do, but the
// ordering is a one-line invariant worth preserving).
auto disable_dynamic_code_entrypoints(JSContext* ctx) -> void {
  JSValue global = JS_GetGlobalObject(ctx);
  for (const char* name : {"eval", "Function"}) {
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DeleteProperty(ctx, global, atom, 0);
    JS_FreeAtom(ctx, atom);
  }
  JS_FreeValue(ctx, global);
}

auto inject_kernel_stdlib(JSContext* ctx) -> void {
  disable_dynamic_code_entrypoints(ctx);
  register_log(ctx);
  register_config(ctx);
  register_crypto(ctx);
  register_db(ctx);
  register_audit(ctx);
  register_cap(ctx);
  register_pubsub(ctx);
}

} // namespace plinth::js
