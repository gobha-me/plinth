#pragma once

#include "kernel/packages/panels_manifest.hpp"
#include "kernel/packages/restore_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace plinth::packages::restore {

enum class ArtifactKind : std::uint8_t { DIRECTORY = 1, REGULAR = 2 };

struct ArtifactRecord {
  ArtifactKind kind = ArtifactKind::REGULAR;
  std::string_view path;
  std::uint32_t mode = 0;
  std::string_view content;
};

// Inputs must remain alive AND unchanged throughout this synchronous call.
// Root is implicit; every other parent is an explicit directory record.
// This validates an inventory's encoding, NOT its completeness against a real
// package, physical link types, protected capture, or bytes actually served.
[[nodiscard]] auto artifact_digest(
    std::span<const ArtifactRecord> records,
    std::size_t content_byte_cap = INVENTORY_CONTENT_MAX_BYTES)
    -> std::expected<std::string, CodecError>;

// Defensive validation includes programmatically constructed schemas. These
// bytes/digests approve neither field meanings nor a persistence projection.
[[nodiscard]] auto canonical_schema(const PanelContextSchema& schema)
    -> std::expected<std::string, CodecError>;
[[nodiscard]] auto schema_digest(const PanelContextSchema& schema)
    -> std::expected<std::string, CodecError>;

} // namespace plinth::packages::restore
