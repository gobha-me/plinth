#pragma once

#include "kernel/packages/generation_capture.hpp"
#include "kernel/packages/name_operation.hpp"

#include <expected>
#include <string>

namespace plinth::packages::generation {

enum class BindingError {
  INVALID_CAPTURE,
  LOCK_NOT_OWNED,
  GENERATION_MISMATCH,
  DECLARATION_MISMATCH,
  LIMIT_EXCEEDED,
  UNAVAILABLE,
};

// Synchronous, bounded full registration binding on the caller's SAME locked
// PG session. Parses only captured bytes, never filesystem paths. Does not
// BEGIN/COMMIT, insert receipts, publish, grant permission or approve restore.
// Provenance comes from the exact stored package row, never a caller boolean.
// v1 binds immutable canonical positive uint64 v2-float ordinals as strings;
// legacy/nonfloat rows must retain null ordinals, without invented typedness.
// NEW facility profile only: <=4096 combined registration rows, <=1MiB raw
// manifest aggregate/SQL metadata/transcript each; raw depth32/nodes65536 with
// decoded duplicate refusal. These are NOT changes to installer acceptance.
// Reserves 16MiB of explicit working-copy capacity before allocating. Library
// DOM/libpq/framework overhead is not a total-process-memory bound.
//
// Digest v1: SHA256("plinth.generation.binding.v1" || NUL || canonical_json).
// Object fields profile/package_id/name/version/provenance, parsed manifest/
// capabilities/panels serialize() TEXT (panels null iff absent), registered
// panel/capability arrays of compact sorted-key JSON row TEXT. Panels sort by
// bytewise id; capabilities by their full compact row bytes. Outer encoding is
// restore::detail::canonical_json, unchanged depth6/nodes16384 limits. Logical
// ordinals are included; mutable state/readiness/grants are deliberately not.
// Unknown JSON numbers use existing DOM double semantics for float/float;
// integer/float comparisons instead require safe exact integral conversion.
// Raw captured checksum/artifact digest still bind exact source bytes. This is
// not an arbitrary-precision numeric codec, user-scope authority or admission.
[[nodiscard]] auto bind_registered_generation(
    name_operation::NameOperation& operation, const CapturedArtifact& artifact,
    Budget& budget) -> std::expected<std::string, BindingError>;

} // namespace plinth::packages::generation
