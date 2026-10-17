#pragma once

#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plinth::frontend {

enum class SurfaceKind { PRIMARY, FLOAT, TRAY };
// Raw byte excess and invalid/structural input are semantic refusals;
// allocation/length pressure is an operational unknown, never payload413.
enum class TypedRequestError { TOO_LARGE, INVALID, UNAVAILABLE };

namespace typed_request_detail {
// Shared by the actual parser and deterministic classification tests. This
// owns no mutable fault state and creates no authority/admission value.
template <typename Operation>
auto operational_boundary(Operation&& operation)
    -> decltype(std::forward<Operation>(operation)()) {
  try {
    return std::forward<Operation>(operation)();
  } catch (const std::bad_alloc&) {
    return std::unexpected(TypedRequestError::UNAVAILABLE);
  } catch (const std::length_error&) {
    return std::unexpected(TypedRequestError::UNAVAILABLE);
  }
}
} // namespace typed_request_detail

struct SurfaceLookup {
  std::string application_id;
  std::string panel_id;
  SurfaceKind kind;
  std::optional<std::string> expected_generation;
  std::optional<std::string> content_type;
  std::optional<std::string> capability;
};

// Exact raw envelope, including decoded duplicate-key detection before DOM
// construction. Syntax only: no authorization/readiness/reservation is granted.
[[nodiscard]] auto parse_surface_admission(std::string_view raw)
    -> std::expected<std::vector<SurfaceLookup>, TypedRequestError>;
[[nodiscard]] auto is_canonical_content_type(std::string_view value) -> bool;

} // namespace plinth::frontend
