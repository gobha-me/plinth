#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::extension_http {

inline constexpr std::size_t MAX_BODY_BYTES = 8 * 1024 * 1024;
inline constexpr std::size_t MAX_WIRE_BYTES = 12 * 1024 * 1024;
inline constexpr std::size_t MAX_METADATA_BYTES = 65536;
inline constexpr std::size_t MAX_ENCODED_METADATA_BYTES = 524288;
inline constexpr std::size_t MAX_TARGET_BYTES = 8192;
inline constexpr std::size_t MAX_HEADER_PAIRS = 64;
inline constexpr std::size_t MAX_HEADER_BYTES = 32768;

enum class CodecError { INVALID, TOO_LARGE };
template <typename T> using CodecResult = std::expected<T, CodecError>;
using Binary = std::vector<std::uint8_t>;
using Headers = std::vector<std::pair<std::string, std::string>>;

// These are values, never authentication, provenance, a claim receipt, an
// adapter binding or permission. Schema is fixed at 1, not caller-selectable.
struct Target {
  std::string original_path;
  std::string path;
  std::string query;
  auto operator==(const Target&) const -> bool = default;
};
struct RequestData {
  std::string request_id;
  std::string method;
  Target target;
  Headers headers;
  auto operator==(const RequestData&) const -> bool = default;
};
struct Principal {
  enum class Kind { ANONYMOUS, PAT };
  Kind kind = Kind::ANONYMOUS;
  std::string user_id;
  std::string pat_id;
  auto operator==(const Principal&) const -> bool = default;
};
struct Extension {
  std::string package_id;
  std::string name;
  std::string version;
  std::string artifact_sha256;
  std::string generation_receipt_id;
  auto operator==(const Extension&) const -> bool = default;
};
struct ContextData {
  std::string dispatch_id;
  std::string owner_epoch;
  std::string adapter_epoch;
  std::string clock_domain_id;
  std::uint64_t admitted_at_ns = 0;
  std::uint64_t execution_cutoff_ns = 0;
  std::string matched_prefix;
  Principal principal;
  Extension extension;
  auto operator==(const ContextData&) const -> bool = default;
};
struct NativeRequest {
  RequestData data;
  Binary body;
  auto operator==(const NativeRequest&) const -> bool = default;
};
struct NativeResponse {
  int status = 200;
  Headers headers;
  Binary body;
  auto operator==(const NativeResponse&) const -> bool = default;
};

[[nodiscard]] auto valid_utf8(std::string_view text) -> bool;
[[nodiscard]] auto valid_uuid(std::string_view text) -> bool;
[[nodiscard]] auto valid_hex256(std::string_view text) -> bool;
[[nodiscard]] auto parse_ns(std::string_view text)
    -> CodecResult<std::uint64_t>;
[[nodiscard]] auto valid_name(std::string_view text) -> bool;
[[nodiscard]] auto valid_version(std::string_view text) -> bool;
[[nodiscard]] auto valid_method(std::string_view text) -> bool;
[[nodiscard]] auto valid_claim(std::string_view text) -> bool;

// Inputs are three supported, separately captured framework values. This does
// not invent a full original target or distinguish /x from /x?. Decoded '?' is
// path data; query is NEVER decoded. No routing/authority lookup is performed.
[[nodiscard]] auto validate_target(std::string_view original_path,
                                   std::string_view path,
                                   std::string_view query) -> CodecResult<void>;
[[nodiscard]] auto validate_headers(const Headers& headers)
    -> CodecResult<void>;
// Validates before removing credentials/hop-by-hop/Connection-nominated names;
// retains the original order and repeated permitted headers.
[[nodiscard]] auto filter_request_headers(const Headers& headers)
    -> CodecResult<Headers>;
[[nodiscard]] auto validate_request(const NativeRequest& request)
    -> CodecResult<void>;
[[nodiscard]] auto validate_response(const NativeResponse& response)
    -> CodecResult<void>;
[[nodiscard]] auto validate_context(const ContextData& context)
    -> CodecResult<void>;
// Additional pure equality/matching check, not receipt/clock/readiness proof.
[[nodiscard]] auto validate_call_data(const NativeRequest& request,
                                      const ContextData& context)
    -> CodecResult<void>;

[[nodiscard]] auto encode_body64(const Binary& body)
    -> CodecResult<std::string>;
[[nodiscard]] auto decode_body64(std::string_view text) -> CodecResult<Binary>;

// Closed WireRequest1/WireResponse1/ContextData1 JSON only, not Frame1/socket
// sequencing or JS objects. Raw bytes <=12MiB, depth<=8, nodes<=1024, decoded
// metadata strings<=64KiB, non-body encoded bytes<=512KiB are checked BEFORE
// DOM construction. Decoded duplicate/unknown keys reject in the SAX prepass.
// Body64 JSON escapes reject; no DOM-only API can assert raw duplicate absence.
[[nodiscard]] auto parse_wire_request(std::string_view raw)
    -> CodecResult<NativeRequest>;
[[nodiscard]] auto parse_wire_response(std::string_view raw)
    -> CodecResult<NativeResponse>;
[[nodiscard]] auto parse_context(std::string_view raw)
    -> CodecResult<ContextData>;
[[nodiscard]] auto serialize_wire_request(const NativeRequest& request)
    -> CodecResult<std::string>;
[[nodiscard]] auto serialize_wire_response(const NativeResponse& response)
    -> CodecResult<std::string>;
[[nodiscard]] auto serialize_context(const ContextData& context)
    -> CodecResult<std::string>;

// Callers reserve owner/copy credits BEFORE invoking allocating functions.
// Largest input is 12MiB; canonical encoded body <=11184812 bytes, decoded body
// <=8MiB. SAX and DOM passes are sequential; returned DTO/string owns its
// bytes. These payload bounds are not a total heap bound (JSON/container
// overhead), a feature-admission counter, or proof that an async task has
// settled.

} // namespace plinth::extension_http
