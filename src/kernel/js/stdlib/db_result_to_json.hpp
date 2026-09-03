// SPDX-License-Identifier: MIT
//
// ICD-0.5.3 §OID-Driven PG-Type → JS-Type Mapping.
//
// Converts a `drogon::orm::Result` (one PG query's rows) into the
// `{rows, row_count}` Json::Value the JS `db.query` / `db.exec`
// bindings surface. The 0.3.3 implementation lived inline in
// run_on_context.cpp as a string-parse heuristic; 0.5.3 extracts the
// seam here and promotes the heuristic to an OID-driven switch. The
// heuristic is retained behind `db.oid_mapping.enabled=false` as a
// deployment-ramp escape hatch.
//
// Identity assumption — PG OIDs are stable since PG 8.0. Plinth's
// supported PG version is ≥ 14 per the compose file; the built-in
// OIDs defined here are pinned.

#pragma once

#include <drogon/orm/Field.h>
#include <drogon/orm/Result.h>
#include <json/value.h>

namespace plinth::js::db {

// Set the global feature flag (ICD-0.5.3 §Config Surface
// `db.oid_mapping.enabled`). Called once from main.cpp during
// startup after `load_config`. Thread-safe; read via relaxed
// load from the result-conversion hot path.
auto set_oid_mapping_enabled(bool enabled) -> void;

// Snapshot of the feature flag for test introspection / loggers.
auto oid_mapping_enabled() -> bool;

// Convert a single field value to Json::Value. Reads `field.oid()`
// (added to Drogon via `third_party/drogon-patches/ftype-accessor.patch`)
// when the feature flag is live; otherwise falls back to the 0.3.3
// heuristic for legacy behavior.
auto field_to_json(const drogon::orm::Field& field) -> Json::Value;

// Convert a full Drogon result to `{rows: [...], row_count: N}` per
// ICD-0.3.3 §Result Shape. Always safe to call regardless of the
// feature flag — internally dispatches per-field.
auto result_to_json(const drogon::orm::Result& result) -> Json::Value;

} // namespace plinth::js::db
