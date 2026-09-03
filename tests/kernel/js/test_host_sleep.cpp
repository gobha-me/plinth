// SPDX-License-Identifier: MIT
//
// Test-only host shim: exposes `globalThis.__host_sleep_ms__(n)` that
// sleeps for n ms with the BridgeContext CPU timer paused around the
// sleep. Used by runtime_pool_test.cpp to prove that wall-clock-only
// work does NOT trip the CPU-time guard.
//
// Built ONLY under `-DPLINTH_JS_TEST_SHIMS=ON`. See CMakeLists.txt. This
// file MUST NEVER leak into a production build — it sets a host function
// with no RBAC check, bypassing everything ICD-0.3.2 will set up.

#include "kernel/js/bridge_context.hpp"

#include <quickjs.h>

#include <chrono>
#include <thread>

namespace {

// Storage for "the current BridgeContext". QuickJS JSContext has its
// own opaque slot, but we only need the BridgeContext pointer on this
// shim's thread during eval — the test runs single-threaded per pool,
// so thread_local is sufficient and cheap.
// test-only shim; scoped thread_local
thread_local plinth::js::BridgeContext* g_current_bc = nullptr;

auto host_sleep_ms(JSContext* /*ctx*/, JSValueConst /*this_val*/, int argc,
                   JSValueConst* argv) -> JSValue {
  if (argc < 1) {
    return JS_NewInt32(nullptr, 0);
  }
  int32_t ms = 0;
  if (JS_ToInt32(g_current_bc->ctx, &ms, argv[0]) != 0) {
    return JS_EXCEPTION;
  }
  // Pause / resume the CPU timer around the real sleep so only
  // wall-clock time accrues, not CPU time.
  if (g_current_bc != nullptr) {
    g_current_bc->pause_cpu_timer();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{ms});
  if (g_current_bc != nullptr) {
    g_current_bc->resume_cpu_timer();
  }
  return JS_NewInt32(g_current_bc->ctx, ms);
}

} // namespace

auto register_host_sleep(plinth::js::BridgeContext& bc) -> void {
  g_current_bc = &bc;
  JSValue global = JS_GetGlobalObject(bc.ctx);
  JSValue fn = JS_NewCFunction(bc.ctx, host_sleep_ms, "__host_sleep_ms__", 1);
  JS_SetPropertyStr(bc.ctx, global, "__host_sleep_ms__", fn);
  JS_FreeValue(bc.ctx, global);
}
