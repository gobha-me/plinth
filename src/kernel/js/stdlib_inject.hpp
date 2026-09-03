// SPDX-License-Identifier: MIT
//
// Kernel standard-library injection into a QuickJS context. Each
// pooled runtime in RuntimePool receives the complete 0.3.2 synchronous
// surface (`log.*`, `config.get`, `crypto.*`) at creation time, before
// any JS code runs.
//
// See docs/icd/ICD-0.3.2-kernel-stdlib-sync.md §Function Registration
// Mechanism. Per-namespace registration functions live in
// src/kernel/js/stdlib/*.cpp; this header is the single seam the pool
// calls into.

#pragma once

#include <quickjs.h>

#include <string>
#include <string_view>

namespace plinth::js {

struct BridgeContext;

// Attaches a single host function as `<ns>.<name>` on globalThis.
// Creates the namespace object on first call, reuses it on subsequent
// ones. Non-owning — the supplied function pointer must outlive the
// context (file-scope C functions suffice).
auto inject_sync_fn(JSContext* ctx, const char* ns, const char* name,
                    JSCFunction* fn, int argc_hint) -> void;

// Registers the complete 0.3.2 synchronous surface on `ctx`. Called
// exactly once per runtime at pool-creation time (see
// RuntimePool::create_entry). The BridgeContext pointer must already
// be installed via JS_SetContextOpaque by the caller — every binding
// retrieves its BridgeContext from the context opaque slot.
auto inject_kernel_stdlib(JSContext* ctx) -> void;

// Per-namespace registrars (implementation detail; declared here so
// inject_kernel_stdlib can call them across translation units).
auto register_log(JSContext* ctx) -> void;
auto register_config(JSContext* ctx) -> void;
auto register_crypto(JSContext* ctx) -> void;
// 0.3.3 async surface — see ICD-0.3.3.
auto register_db(JSContext* ctx) -> void;
auto register_audit(JSContext* ctx) -> void;
// 0.3.4 cap.* surface — see ICD-0.3.4.
auto register_cap(JSContext* ctx) -> void;
// 0.5.0 pubsub.* surface — see ICD-0.5.0 §`pubsub.*` JS Stdlib.
auto register_pubsub(JSContext* ctx) -> void;

// ICD-0.5.2 §pubsub.subscribe JS Binding — helper exported so the
// PUBSUB_SUBSCRIBE dispatch arm in run_on_context.cpp can synthesize
// the unsubscribe JS function on the bc's owning loop (JSValue
// construction + JS_NewCFunctionData must happen where the runtime
// lives).
auto make_unsubscribe_function(JSContext* ctx, const std::string& channel)
    -> JSValue;

// ICD-0.5.2 §Security Constraint 2 — defense-in-depth RBAC re-check
// exported so the PUBSUB_SUBSCRIBE dispatch arm can re-validate the
// subscribe permission at registration time (between the inline
// classify-gate and the broker-registry mutation). Returns empty
// string on allow; a `pubsub.*` rejection code on deny — same return
// shape as the classify gate. Callers are responsible for audit
// emission + persistent_callbacks rollback on denial.
auto classify_pubsub_subscribe(std::string_view channel,
                               const BridgeContext& bc) -> std::string;

} // namespace plinth::js
