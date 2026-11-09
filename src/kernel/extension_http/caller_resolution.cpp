// SPDX-License-Identifier: MIT
#include "kernel/extension_http/caller_resolution.hpp"

#include "kernel/db/operations.hpp"
#include "kernel/extension_http/caller_resolution_detail.hpp"

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <memory>
#include <string_view>
#include <utility>

namespace plinth::extension_http {
namespace {
// Covers the bounded native result, vector/string storage, SQL/parameter
// payloads and output snapshot. This is a feature payload charge, not an
// assertion about PostgreSQL's server-side work or total libpq allocator heap.
constexpr std::size_t CALLER_BYTES = 4ULL * 1024 * 1024;
constexpr auto PAT_CALLER_SQL = R"sql(
WITH caller AS (
  SELECT u.id, u.username FROM plinth.users u
  JOIN plinth.pats p ON p.user_id = u.id
  WHERE u.id = $1::uuid AND p.id = $2::uuid
    AND u.disabled_at IS NULL AND p.revoked_at IS NULL
    AND (p.expires_at IS NULL OR p.expires_at > statement_timestamp())
)
SELECT CASE WHEN octet_length(c.username) <= 256 THEN c.username END AS username,
       CASE WHEN octet_length(e.rule) <= 256 THEN e.rule END AS rule,
       e.rule IS NOT NULL AS has_rule
FROM caller c LEFT JOIN LATERAL (
  SELECT DISTINCT r.rule FROM plinth.groups g
  JOIN plinth.group_rules gr ON gr.group_id = g.id
  JOIN plinth.rbac_rules r ON r.id = gr.rule_id
  LEFT JOIN plinth.group_members gm ON gm.group_id = g.id AND gm.user_id = c.id
  WHERE (g.name = 'everyone' OR gm.user_id IS NOT NULL)
    AND r.orphaned_at IS NULL
  ORDER BY r.rule LIMIT 1025
) e ON TRUE LIMIT 1025
)sql";
constexpr auto PUBLIC_CALLER_SQL = R"sql(
SELECT ''::text AS username,
       CASE WHEN octet_length(e.rule) <= 256 THEN e.rule END AS rule,
       e.rule IS NOT NULL AS has_rule
FROM (VALUES (true)) caller(present) LEFT JOIN LATERAL (
  SELECT DISTINCT r.rule FROM plinth.groups g
  JOIN plinth.group_rules gr ON gr.group_id = g.id
  JOIN plinth.rbac_rules r ON r.id = gr.rule_id
  WHERE g.name = 'everyone' AND r.orphaned_at IS NULL
  ORDER BY r.rule LIMIT 1025
) e ON TRUE LIMIT 1025
)sql";

auto text_field(const PGresult& result, int row, int column)
    -> std::optional<std::string_view> {
  if (PQgetisnull(&result, row, column) != 0) return std::nullopt;
  const auto length = PQgetlength(&result, row, column);
  if (length < 0 ||
      static_cast<std::size_t>(length) > detail::MAX_CALLER_TEXT_BYTES)
    return std::nullopt;
  const std::string_view value{PQgetvalue(&result, row, column),
                               static_cast<std::size_t>(length)};
  if (value.find('\0') != std::string_view::npos || !valid_utf8(value))
    return std::nullopt;
  return value;
}
} // namespace

auto detail::decode_http_caller(const PGresult& result,
                                const Principal& principal)
    -> std::expected<capabilities::UserContext, CallerResolutionFailure> {
  constexpr auto INVALID =
      std::unexpected(CallerResolutionFailure::UNAVAILABLE);
  const bool anonymous = principal.kind == Principal::Kind::ANONYMOUS;
  if ((anonymous &&
       (!principal.user_id.empty() || !principal.pat_id.empty())) ||
      (!anonymous &&
       (principal.kind != Principal::Kind::PAT ||
        !valid_uuid(principal.user_id) || !valid_uuid(principal.pat_id))))
    return INVALID;
  if (PQresultStatus(&result) != PGRES_TUPLES_OK || PQnfields(&result) != 3)
    return INVALID;
  constexpr std::array NAMES = {"username", "rule", "has_rule"};
  for (int column = 0; column < 3; ++column) {
    const auto* name = PQfname(&result, column);
    if (name == nullptr || std::string_view{name} != NAMES[column] ||
        PQfformat(&result, column) != 0)
      return INVALID;
  }
  const auto rows = PQntuples(&result);
  if (rows == 0) return std::unexpected(CallerResolutionFailure::REJECTED);
  if (rows < 0 || static_cast<std::size_t>(rows) > MAX_CALLER_RULES)
    return INVALID;
  auto username = text_field(result, 0, 0);
  if (!username || (anonymous ? !username->empty() : username->empty()))
    return INVALID;
  capabilities::UserContext user{.user_id = principal.user_id,
                                 .username = std::string{*username},
                                 .auth_type = anonymous ? "anonymous" : "pat",
                                 .effective_rules = {},
                                 .session_id = "",
                                 .ip_address = ""};
  user.effective_rules.reserve(static_cast<std::size_t>(rows));
  for (int row = 0; row < rows; ++row) {
    if (text_field(result, row, 0) != username) return INVALID;
    const auto has_rule = text_field(result, row, 2);
    const auto rule = text_field(result, row, 1);
    if (has_rule == "f") {
      if (rows != 1 || PQgetisnull(&result, row, 1) == 0) return INVALID;
    } else if (has_rule == "t") {
      if (!rule || rule->empty()) return INVALID;
      user.effective_rules.emplace_back(*rule);
    } else {
      return INVALID;
    }
  }
  return user;
}

ResolvedHttpCaller::ResolvedHttpCaller(
    HttpCallerSnapshot value_in, std::shared_ptr<AuthenticationOwner> owner_in,
    ExtraMemoryCharge allocation_in)
    : value(std::move(value_in)), owner(std::move(owner_in)),
      allocation(std::move(allocation_in)) {
}

auto ResolvedHttpCaller::snapshot() const -> std::optional<HttpCallerSnapshot> {
  if (!owner || !owner->current()) return std::nullopt;
  return value;
}

struct CallerResolutionRun {
  static auto resolve(AuthenticatedHttpPrincipal principal, PGconn& connection)
      -> CallerResolutionResult try {
    if (!principal.owner || !principal.owner->current())
      return std::unexpected(CallerResolutionFailure::RETIRED);
    auto allocation = principal.owner->reserve(CALLER_BYTES);
    if (!allocation) return std::unexpected(CallerResolutionFailure::BUSY);
    const auto started = std::chrono::steady_clock::now();
    const auto remaining = principal.owner->remaining_execution_ms();
    if (!remaining) return std::unexpected(CallerResolutionFailure::RETIRED);
    const auto identity = principal.snapshot();
    if (!identity) return std::unexpected(CallerResolutionFailure::RETIRED);
    // An existing transaction could provide an old identity/rule snapshot.
    // Never reuse it or change a caller's transaction policy implicitly.
    if (PQstatus(&connection) != CONNECTION_OK ||
        PQtransactionStatus(&connection) != PQTRANS_IDLE)
      return std::unexpected(CallerResolutionFailure::UNAVAILABLE);

    const auto budget = std::chrono::milliseconds{*remaining};
    db::OperationScope scope{principal.owner->operation_stop_token(),
                             started + budget, budget};
    scope.checkpoint();
    const bool anonymous = identity->kind == Principal::Kind::ANONYMOUS;
    const std::array parameters = {identity->user_id.c_str(),
                                   identity->pat_id.c_str()};
    using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
    Result result{
        db::exec_params(
            &connection, anonymous ? PUBLIC_CALLER_SQL : PAT_CALLER_SQL,
            anonymous ? 0 : 2, nullptr, anonymous ? nullptr : parameters.data(),
            nullptr, nullptr, 0),
        PQclear};
    scope.checkpoint();
    if (!principal.owner->current())
      return std::unexpected(CallerResolutionFailure::RETIRED);
    if (!result) return std::unexpected(CallerResolutionFailure::UNAVAILABLE);
    auto user = detail::decode_http_caller(*result, *identity);
    if (!user) return std::unexpected(user.error());
    if (!principal.owner->current())
      return std::unexpected(CallerResolutionFailure::RETIRED);
    return ResolvedHttpCaller{
        {*identity, std::move(*user)}, principal.owner, std::move(*allocation)};
  } catch (...) {
    return std::unexpected(principal.owner && !principal.owner->current()
                               ? CallerResolutionFailure::RETIRED
                               : CallerResolutionFailure::UNAVAILABLE);
  }
};

auto resolve_http_caller(AuthenticatedHttpPrincipal principal,
                         PGconn& connection) -> CallerResolutionResult {
  return CallerResolutionRun::resolve(std::move(principal), connection);
}
} // namespace plinth::extension_http
