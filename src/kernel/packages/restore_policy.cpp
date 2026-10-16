#include "kernel/packages/restore_policy.hpp"

#include "kernel/capabilities/parser.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/packages/detail/restore_json.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace plinth::packages::restore {
namespace {

using Json = nlohmann::json;

// Count value nodes (including containers) at their actual value depth before
// a DOM exists. Object names are tracked after JSON escape decoding, separately
// from value nodes. The caller bounds the complete raw input before this pass.
class PolicySax final : public nlohmann::json_sax<Json> {
 public:
  auto null() -> bool override { return value(); }
  auto boolean(bool /*unused*/) -> bool override { return value(); }
  auto number_integer(number_integer_t /*unused*/) -> bool override {
    return value();
  }
  auto number_unsigned(number_unsigned_t /*unused*/) -> bool override {
    return value();
  }
  auto number_float(number_float_t /*unused*/, const string_t& /*unused*/)
      -> bool override {
    return value();
  }
  auto string(string_t& /*unused*/) -> bool override { return value(); }
  auto binary(binary_t& /*unused*/) -> bool override {
    return fail(CodecError::INVALID_JSON);
  }
  auto start_object(std::size_t /*unused*/) -> bool override {
    return begin(true);
  }
  auto start_array(std::size_t /*unused*/) -> bool override {
    return begin(false);
  }
  auto key(string_t& name) -> bool override {
    if (frames.empty() || !frames.back().object) {
      return fail(CodecError::INVALID_JSON);
    }
    if (!frames.back().keys.insert(name).second) {
      return fail(CodecError::DUPLICATE_KEY);
    }
    return true;
  }
  auto end_object() -> bool override { return end(true); }
  auto end_array() -> bool override { return end(false); }
  auto parse_error(std::size_t /*unused*/, const std::string& /*unused*/,
                   const nlohmann::detail::exception& /*unused*/)
      -> bool override {
    return fail(CodecError::INVALID_JSON);
  }
  [[nodiscard]] auto failure() const -> CodecError {
    return error.value_or(CodecError::INVALID_JSON);
  }

 private:
  struct Frame {
    bool object = false;
    std::set<std::string> keys;
  };
  std::vector<Frame> frames;
  std::size_t nodes = 0;
  std::optional<CodecError> error;

  auto fail(CodecError code) -> bool {
    if (!error) {
      error = code;
    }
    return false;
  }
  auto value() -> bool {
    if (frames.size() >= JSON_MAX_DEPTH || nodes >= JSON_MAX_NODES) {
      return fail(CodecError::LIMIT_EXCEEDED);
    }
    ++nodes;
    return true;
  }
  auto begin(bool object) -> bool {
    if (!value()) {
      return false;
    }
    frames.push_back(Frame{.object = object, .keys = {}});
    return true;
  }
  auto end(bool object) -> bool {
    if (frames.empty() || frames.back().object != object) {
      return fail(CodecError::INVALID_JSON);
    }
    frames.pop_back();
    return true;
  }
};

auto exact_keys(const Json& value,
                std::initializer_list<std::string_view> names) -> bool {
  return value.is_object() && value.size() == names.size() &&
         std::ranges::all_of(names, [&](auto name) {
           return value.contains(std::string{name});
         });
}

auto text(const Json& value) -> std::optional<std::string_view> {
  if (!value.is_string()) {
    return std::nullopt;
  }
  return value.get_ref<const std::string&>();
}

auto valid_text(std::string_view value, std::size_t minimum,
                std::size_t maximum) -> bool {
  return value.size() >= minimum && value.size() <= maximum &&
         plinth::packages::detail::unicode_scalar_count(value).has_value();
}

auto bounded_text(const Json& value, std::size_t minimum, std::size_t maximum)
    -> std::optional<std::string_view> {
  auto result = text(value);
  if (!result || !valid_text(*result, minimum, maximum)) {
    return std::nullopt;
  }
  return result;
}

auto lower(char value) -> bool {
  return value >= 'a' && value <= 'z';
}
auto digit(char value) -> bool {
  return value >= '0' && value <= '9';
}
auto lower_hex(char value) -> bool {
  return digit(value) || (value >= 'a' && value <= 'f');
}

auto valid_id(std::string_view value, bool application) -> bool {
  if (value.size() < (application ? 2U : 1U) || value.size() > 64 ||
      !lower(value.front())) {
    return false;
  }
  return std::ranges::all_of(value.substr(1), [application](char byte) {
    return lower(byte) || digit(byte) || byte == '-' ||
           (!application && byte == '_');
  });
}

auto valid_field_key(std::string_view key) -> bool {
  return valid_text(key, 1, 64) && key.find('\0') == std::string_view::npos &&
         key != "__proto__" && key != "prototype" && key != "constructor";
}

auto valid_digest(std::string_view value) -> bool {
  return value.size() == 64 && std::ranges::all_of(value, lower_hex);
}

auto valid_uuid(std::string_view value) -> bool {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool separator =
        index == 8 || index == 13 || index == 18 || index == 23;
    if (separator ? value[index] != '-' : !lower_hex(value[index])) {
      return false;
    }
  }
  return true;
}

auto valid_revision(std::string_view value) -> bool {
  if (value.empty() || value.size() > 20 || value.front() < '1' ||
      value.front() > '9' || !std::ranges::all_of(value, digit)) {
    return false;
  }
  std::uint64_t parsed = 0;
  const auto* end = value.data() + value.size();
  auto result = std::from_chars(value.data(), end, parsed);
  return result.ec == std::errc{} && result.ptr == end;
}

auto positive_integer(const Json& value, std::uint64_t maximum)
    -> std::optional<std::uint64_t> {
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number >= 1 && number <= maximum) {
      return number;
    }
  } else if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number >= 1 && static_cast<std::uint64_t>(number) <= maximum) {
      return static_cast<std::uint64_t>(number);
    }
  }
  return std::nullopt;
}

struct Date {
  int year = 0;
  int month = 0;
  int day = 0;
};

auto days_in_month(int year, int month) -> int {
  constexpr std::array DAYS{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return month == 2 && leap ? 29 : DAYS[static_cast<std::size_t>(month - 1)];
}

auto previous_day(Date& date) -> void {
  if (date.day > 1) {
    --date.day;
    return;
  }
  if (--date.month == 0) {
    date.month = 12;
    --date.year;
  }
  date.day = days_in_month(date.year, date.month);
}

auto next_day(Date& date) -> void {
  if (date.day < days_in_month(date.year, date.month)) {
    ++date.day;
    return;
  }
  date.day = 1;
  if (++date.month == 13) {
    date.month = 1;
    ++date.year;
  }
}

auto decimal_at(std::string_view value, std::size_t position, std::size_t width)
    -> std::optional<int> {
  if (position > value.size() || width > value.size() - position) {
    return std::nullopt;
  }
  int number = 0;
  for (char byte : value.substr(position, width)) {
    if (!digit(byte)) {
      return std::nullopt;
    }
    number = number * 10 + (byte - '0');
  }
  return number;
}

auto offset_at(std::string_view value, std::size_t position)
    -> std::optional<int> {
  if (position >= value.size()) {
    return std::nullopt;
  }
  if (value[position] == 'Z' || value[position] == 'z') {
    return position + 1 == value.size() ? std::optional{0} : std::nullopt;
  }
  if (value.size() - position != 6 ||
      (value[position] != '+' && value[position] != '-') ||
      value[position + 3] != ':') {
    return std::nullopt;
  }
  auto hours = decimal_at(value, position + 1, 2);
  auto minutes = decimal_at(value, position + 4, 2);
  if (!hours || !minutes || *hours > 23 || *minutes > 59) {
    return std::nullopt;
  }
  return (*hours * 60 + *minutes) * (value[position] == '+' ? 1 : -1);
}

auto timestamp_offset(std::string_view value) -> std::optional<int> {
  std::size_t position = 19;
  if (value[position] == '.') {
    const auto first = ++position;
    while (position < value.size() && digit(value[position])) {
      ++position;
    }
    if (position == first) {
      return std::nullopt;
    }
  }
  return offset_at(value, position);
}

auto potential_leap_minute(Date date, int minutes) -> bool {
  // Offsets are at most 23:59, so at most one day adjustment is needed.
  // Intermediate year -1/10000 is harmless arithmetic, not an expanded input
  // year grammar. This checks possible UTC position, not IERS occurrence.
  if (minutes < 0) {
    previous_day(date);
    minutes += 24 * 60;
  } else if (minutes >= 24 * 60) {
    next_day(date);
    minutes -= 24 * 60;
  }
  return minutes == 23 * 60 + 59 &&
         date.day == days_in_month(date.year, date.month);
}

auto valid_timestamp(std::string_view value) -> bool {
  if (value.size() < 20 || value.size() > 32 ||
      std::ranges::any_of(
          value,
          [](char byte) { return static_cast<unsigned char>(byte) > 0x7F; }) ||
      value[4] != '-' || value[7] != '-' ||
      (value[10] != 'T' && value[10] != 't') || value[13] != ':' ||
      value[16] != ':') {
    return false;
  }
  auto year = decimal_at(value, 0, 4);
  auto month = decimal_at(value, 5, 2);
  auto day = decimal_at(value, 8, 2);
  auto hour = decimal_at(value, 11, 2);
  auto minute = decimal_at(value, 14, 2);
  auto second = decimal_at(value, 17, 2);
  auto offset = timestamp_offset(value);
  if (!year || !month || !day || !hour || !minute || !second || !offset ||
      *month < 1 || *month > 12 || *day < 1 ||
      *day > days_in_month(*year, *month) || *hour > 23 || *minute > 59 ||
      *second > 60) {
    return false;
  }
  return *second < 60 || potential_leap_minute(
                             Date{.year = *year, .month = *month, .day = *day},
                             *hour * 60 + *minute - *offset);
}

auto parse_review(const Json& value) -> std::optional<PolicyReview> {
  if (!exact_keys(value, {"admin_user_id", "reviewed_at", "review_ref",
                          "evidence_sha256"})) {
    return std::nullopt;
  }
  auto user = text(value["admin_user_id"]);
  auto timestamp = text(value["reviewed_at"]);
  auto reference = bounded_text(value["review_ref"], 0, 256);
  auto evidence = text(value["evidence_sha256"]);
  if (!user || !valid_uuid(*user) || !timestamp ||
      !valid_timestamp(*timestamp) || !reference || !evidence ||
      !valid_digest(*evidence)) {
    return std::nullopt;
  }
  return PolicyReview{.admin_user_id = std::string{*user},
                      .reviewed_at = std::string{*timestamp},
                      .review_ref = std::string{*reference},
                      .evidence_sha256 = std::string{*evidence}};
}

auto parse_projection(const Json& value) -> std::optional<PolicyProjection> {
  if (!exact_keys(value, {"id", "version"})) {
    return std::nullopt;
  }
  auto id = text(value["id"]);
  constexpr std::uint64_t MAX_SAFE_INTEGER = 9'007'199'254'740'991;
  auto version = positive_integer(value["version"], MAX_SAFE_INTEGER);
  if (!id || !valid_id(*id, false) || !version) {
    return std::nullopt;
  }
  return PolicyProjection{.id = std::string{*id}, .version = *version};
}

auto parse_fields(const Json& value)
    -> std::optional<std::vector<PolicyField>> {
  if (!value.is_array() || value.size() > 16) {
    return std::nullopt;
  }
  std::vector<PolicyField> result;
  result.reserve(value.size());
  std::set<std::string> keys;
  for (const auto& field : value) {
    if (!exact_keys(field, {"key", "meaning"})) {
      return std::nullopt;
    }
    auto key = text(field["key"]);
    auto meaning = bounded_text(field["meaning"], 1, 256);
    if (!key || !valid_field_key(*key) || !meaning ||
        !keys.insert(std::string{*key}).second) {
      return std::nullopt;
    }
    result.push_back(PolicyField{.key = std::string{*key},
                                 .meaning = std::string{*meaning}});
  }
  return result;
}

auto normalized_capability(const Json& value) -> std::optional<std::string> {
  auto signature = bounded_text(value, 1, 256);
  if (!signature) {
    return std::nullopt;
  }
  auto parsed = capabilities::parse_signature(*signature);
  const auto* parts = std::get_if<capabilities::ParsedSignature>(&parsed);
  if (parts == nullptr) {
    return std::nullopt;
  }
  return parts->namespace_ + ":" + std::to_string(parts->version) + ":" +
         parts->function;
}

auto parse_entry(const Json& value) -> std::expected<PolicyEntry, CodecError> {
  if (!exact_keys(value, {"application_id", "panel_id", "capability",
                          "projection_contract", "artifact_digest",
                          "schema_digest", "fields"})) {
    return std::unexpected(CodecError::INVALID_POLICY);
  }
  // Measure the original decoded entry, never a normalized capability alias.
  auto encoded = detail::canonical_json(value, POLICY_ENTRY_MAX_BYTES);
  if (!encoded) {
    // Preflight already established JSON grammar/UTF-8. Unsupported encoding
    // values (notably floating-point tokens) are policy-shape failures here.
    return std::unexpected(encoded.error() == CodecError::INVALID_JSON
                               ? CodecError::INVALID_POLICY
                               : encoded.error());
  }
  auto application = text(value["application_id"]);
  auto panel = text(value["panel_id"]);
  auto capability = normalized_capability(value["capability"]);
  auto artifact = text(value["artifact_digest"]);
  auto schema = text(value["schema_digest"]);
  auto projection = parse_projection(value["projection_contract"]);
  auto fields = parse_fields(value["fields"]);
  if (!application || !valid_id(*application, true) || !panel ||
      !valid_id(*panel, false) || !capability || !artifact ||
      !valid_digest(*artifact) || !schema || !valid_digest(*schema) ||
      !projection || !fields) {
    return std::unexpected(CodecError::INVALID_POLICY);
  }
  return PolicyEntry{.application_id = std::string{*application},
                     .panel_id = std::string{*panel},
                     .capability = std::move(*capability),
                     .projection_contract = std::move(*projection),
                     .artifact_digest = std::string{*artifact},
                     .schema_digest = std::string{*schema},
                     .fields = std::move(*fields)};
}

auto parse_document(const Json& value)
    -> std::expected<ParsedPolicy, CodecError> {
  if (!exact_keys(value,
                  {"format_version", "policy_revision", "review", "entries"}) ||
      positive_integer(value["format_version"], 1) != 1) {
    return std::unexpected(CodecError::INVALID_POLICY);
  }
  auto revision = text(value["policy_revision"]);
  auto review = parse_review(value["review"]);
  const auto& entries = value["entries"];
  if (!revision || !valid_revision(*revision) || !review ||
      !entries.is_array() || entries.size() > POLICY_MAX_ENTRIES) {
    return std::unexpected(CodecError::INVALID_POLICY);
  }
  ParsedPolicy result{.policy_revision = std::string{*revision},
                      .review = std::move(*review),
                      .entries = {}};
  result.entries.reserve(entries.size());
  using Binding = std::tuple<std::string, std::string, std::string, std::string,
                             std::string>;
  std::set<Binding> bindings;
  for (const auto& entry : entries) {
    auto parsed = parse_entry(entry);
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    if (!bindings
             .emplace(parsed->application_id, parsed->panel_id,
                      parsed->capability, parsed->artifact_digest,
                      parsed->schema_digest)
             .second) {
      return std::unexpected(CodecError::CONFLICTING_BINDING);
    }
    result.entries.push_back(std::move(*parsed));
  }
  return result;
}

} // namespace

auto parse_policy(std::string_view input)
    -> std::expected<ParsedPolicy, CodecError> {
  if (input.size() > POLICY_MAX_BYTES) {
    return std::unexpected(CodecError::TOO_LARGE);
  }
  try {
    PolicySax preflight;
    if (!Json::sax_parse(input.begin(), input.end(), &preflight)) {
      return std::unexpected(preflight.failure());
    }
    // Only bounded, duplicate-free raw JSON reaches DOM construction.
    return parse_document(Json::parse(input.begin(), input.end()));
  } catch (const Json::exception&) {
    return std::unexpected(CodecError::INVALID_JSON);
  } catch (const std::bad_alloc&) {
    return std::unexpected(CodecError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(CodecError::LIMIT_EXCEEDED);
  }
}

} // namespace plinth::packages::restore
