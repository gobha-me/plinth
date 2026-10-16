#pragma once

#include "kernel/packages/restore_codec.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace plinth::packages::restore {

struct PolicyReview {
  std::string admin_user_id;
  std::string reviewed_at;
  std::string review_ref;
  std::string evidence_sha256;
};

struct PolicyProjection {
  std::string id;
  std::uint64_t version = 0;
};

struct PolicyField {
  std::string key;
  std::string meaning;
};

struct PolicyEntry {
  std::string application_id;
  std::string panel_id;
  // Normalized native capability identity, including minimal decimal version.
  std::string capability;
  PolicyProjection projection_contract;
  std::string artifact_digest;
  std::string schema_digest;
  std::vector<PolicyField> fields;
};

// Parsed syntax is NOT trusted deployment policy, persistence approval,
// reviewer eligibility, verified package ownership or an authority lease.
struct ParsedPolicy {
  std::string policy_revision;
  PolicyReview review;
  std::vector<PolicyEntry> entries;
};

// Raw JSON is required to detect escaped/duplicate member names before DOM
// construction. Result owns every string. No filesystem or runtime activation.
[[nodiscard]] auto parse_policy(std::string_view text)
    -> std::expected<ParsedPolicy, CodecError>;

} // namespace plinth::packages::restore
