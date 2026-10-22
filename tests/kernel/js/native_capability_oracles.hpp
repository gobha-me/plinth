// SPDX-License-Identifier: MIT
#pragma once

#include <json/value.h>

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>

namespace native_capability_corpus {

namespace detail {

// These descriptors are independent literals, not serialized JS programs or
// results produced by a production converter. JSON numeric storage tags are
// deliberately not part of the JavaScript number/code-unit contract.
inline auto units(std::initializer_list<int> values) -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const int value : values) {
    result.append(value);
  }
  return result;
}

inline auto null_value() -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "null";
  return result;
}

inline auto boolean(bool value) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "boolean";
  result["value"] = value;
  return result;
}

inline auto number(double value, bool safe) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "number";
  result["value"] = value;
  result["safe"] = safe;
  result["finite"] = true;
  return result;
}

inline auto string(std::initializer_list<int> code_units) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "string";
  result["units"] = units(code_units);
  return result;
}

inline auto array(std::initializer_list<Json::Value> items) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "array";
  result["items"] = Json::Value{Json::arrayValue};
  for (const auto& item : items) {
    result["items"].append(item);
  }
  return result;
}

inline auto entry(std::initializer_list<int> key, const Json::Value& value)
    -> Json::Value {
  Json::Value result{Json::objectValue};
  result["key"] = units(key);
  result["value"] = value;
  return result;
}

inline auto object(std::initializer_list<Json::Value> entries) -> Json::Value {
  Json::Value result{Json::objectValue};
  result["type"] = "object";
  result["entries"] = Json::Value{Json::arrayValue};
  for (const auto& item : entries) {
    result["entries"].append(item);
  }
  return result;
}

inline auto null_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return null_value();
    case 1: return object({entry({118}, null_value())});
    case 2: return array({null_value()});
    case 3: return object({entry({118}, array({null_value()}))});
    default: throw std::out_of_range("native capability null seed");
  }
}

inline auto boolean_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return boolean(false);
    case 1: return boolean(true);
    case 2: return array({boolean(false), boolean(true)});
    case 3:
      return object(
          {entry({102}, boolean(false)), entry({116}, boolean(true))});
    default: throw std::out_of_range("native capability boolean seed");
  }
}

inline auto integer_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return number(0.0, true);
    case 1: return number(2147483648.0, true);
    case 2: return number(-9007199254740991.0, true);
    case 3: return number(9007199254740991.0, true);
    default: throw std::out_of_range("native capability integer seed");
  }
}

inline auto fraction_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return number(-0.5, false);
    case 1: return number(0.5, false);
    case 2: return number(1.25, false);
    case 3: return number(-1024.125, false);
    default: throw std::out_of_range("native capability fraction seed");
  }
}

inline auto string_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return string({});
    case 1: return string({65, 0, 66});
    case 2: return string({233, 20013});
    case 3: return string({55357, 56960});
    default: throw std::out_of_range("native capability string seed");
  }
}

inline auto member_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return object({entry({}, number(1.0, true))});
    case 1:
      return object({
          entry({108, 101, 102, 116, 0, 114, 105, 103, 104, 116},
                number(2.0, true)),
          entry({108, 101, 102, 116}, number(3.0, true)),
      });
    case 2:
      return object(
          {entry({233}, number(4.0, true)), entry({20013}, number(5.0, true))});
    case 3:
      return object(
          {entry({55357, 56960}, number(6.0, true)),
           entry({113, 117, 111, 116, 101, 34, 92}, number(7.0, true))});
    default: throw std::out_of_range("native capability member seed");
  }
}

inline auto array_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return array({});
    case 1:
      return array(
          {null_value(), boolean(false), number(0.0, true), string({})});
    case 2:
      return array({number(2147483648.0, true), number(0.5, false),
                    string({65, 0, 66}), boolean(true)});
    case 3:
      return array({array({number(1.0, true)}), array({}),
                    object({entry({107}, number(2.0, true))})});
    default: throw std::out_of_range("native capability array seed");
  }
}

inline auto tree_data(std::size_t seed) -> Json::Value {
  switch (seed) {
    case 0: return object({});
    case 1:
      return object(
          {entry({97}, array({number(1.0, true),
                              object({entry({98}, string({120}))})}))});
    case 2:
      return array(
          {object({entry({107}, array({boolean(true), null_value()}))}),
           object({entry({122}, object({}))})});
    case 3:
      return object({
          entry({108, 101, 102, 116},
                object({entry({110}, number(1.0, true))})),
          entry({114, 105, 103, 104, 116},
                object({entry({110}, number(2.0, true))})),
          entry({116, 97, 105, 108}, array({})),
      });
    default: throw std::out_of_range("native capability tree seed");
  }
}

inline auto has_members(const Json::Value& value,
                        std::initializer_list<const char*> names) -> bool {
  if (!value.isObject() ||
      static_cast<std::size_t>(value.size()) != names.size()) {
    return false;
  }
  for (const auto* name : names) {
    if (!value.isMember(name)) {
      return false;
    }
  }
  return true;
}

inline auto valid_units(const Json::Value& value) -> bool {
  if (!value.isArray()) {
    return false;
  }
  for (const auto& unit : value) {
    if (!unit.isNumeric()) {
      return false;
    }
    const double numeric = unit.asDouble();
    if (!std::isfinite(numeric) || numeric < 0.0 || numeric > 65535.0 ||
        std::trunc(numeric) != numeric) {
      return false;
    }
  }
  return true;
}

inline auto equal_units(const Json::Value& actual, const Json::Value& expected)
    -> bool {
  if (!valid_units(actual) || !valid_units(expected) ||
      actual.size() != expected.size()) {
    return false;
  }
  for (Json::ArrayIndex index = 0; index < expected.size(); ++index) {
    if (actual[index].asDouble() != expected[index].asDouble()) {
      return false;
    }
  }
  return true;
}

inline auto valid_number(const Json::Value& value) -> bool {
  if (!has_members(value, {"type", "value", "safe", "finite"}) ||
      !value["value"].isNumeric() || !value["safe"].isBool() ||
      !value["finite"].isBool() || !value["finite"].asBool()) {
    return false;
  }
  const double numeric = value["value"].asDouble();
  if (!std::isfinite(numeric)) {
    return false;
  }
  constexpr double MAX_SAFE_INTEGER = 9007199254740991.0;
  const bool safe =
      std::trunc(numeric) == numeric && std::abs(numeric) <= MAX_SAFE_INTEGER;
  return value["safe"].asBool() == safe;
}

inline auto valid_entries(const Json::Value& entries) -> bool {
  if (!entries.isArray()) {
    return false;
  }
  for (Json::ArrayIndex index = 0; index < entries.size(); ++index) {
    const auto& item = entries[index];
    if (!has_members(item, {"key", "value"}) || !valid_units(item["key"])) {
      return false;
    }
    for (Json::ArrayIndex prior = 0; prior < index; ++prior) {
      if (equal_units(entries[prior]["key"], item["key"])) {
        return false;
      }
    }
  }
  return true;
}

inline auto find_entry(const Json::Value& entries, const Json::Value& key)
    -> const Json::Value* {
  for (const auto& item : entries) {
    if (equal_units(item["key"], key)) {
      return &item;
    }
  }
  return nullptr;
}

inline auto matches_descriptor(const Json::Value& actual,
                               const Json::Value& expected) -> bool;

inline auto matches_array(const Json::Value& actual,
                          const Json::Value& expected) -> bool {
  if (!has_members(actual, {"type", "items"}) ||
      !has_members(expected, {"type", "items"}) || !actual["items"].isArray() ||
      !expected["items"].isArray() ||
      actual["items"].size() != expected["items"].size()) {
    return false;
  }
  for (Json::ArrayIndex index = 0; index < expected["items"].size(); ++index) {
    if (!matches_descriptor(actual["items"][index], expected["items"][index])) {
      return false;
    }
  }
  return true;
}

inline auto matches_object(const Json::Value& actual,
                           const Json::Value& expected) -> bool {
  if (!has_members(actual, {"type", "entries"}) ||
      !has_members(expected, {"type", "entries"}) ||
      !valid_entries(actual["entries"]) ||
      !valid_entries(expected["entries"]) ||
      actual["entries"].size() != expected["entries"].size()) {
    return false;
  }
  for (const auto& item : actual["entries"]) {
    const auto* wanted = find_entry(expected["entries"], item["key"]);
    if (wanted == nullptr ||
        !matches_descriptor(item["value"], (*wanted)["value"])) {
      return false;
    }
  }
  return true;
}

inline auto matches_descriptor(const Json::Value& actual,
                               const Json::Value& expected) -> bool {
  if (!actual.isObject() || !expected.isObject() ||
      !actual["type"].isString() || !expected["type"].isString() ||
      actual["type"].asString() != expected["type"].asString()) {
    return false;
  }
  const auto type = expected["type"].asString();
  if (type == "null") {
    return has_members(actual, {"type"}) && has_members(expected, {"type"});
  }
  if (type == "boolean") {
    return has_members(actual, {"type", "value"}) &&
           has_members(expected, {"type", "value"}) &&
           actual["value"].isBool() && expected["value"].isBool() &&
           actual["value"].asBool() == expected["value"].asBool();
  }
  if (type == "number") {
    return valid_number(actual) && valid_number(expected) &&
           actual["value"].asDouble() == expected["value"].asDouble() &&
           actual["safe"].asBool() == expected["safe"].asBool();
  }
  if (type == "string") {
    return has_members(actual, {"type", "units"}) &&
           has_members(expected, {"type", "units"}) &&
           equal_units(actual["units"], expected["units"]);
  }
  if (type == "array") {
    return matches_array(actual, expected);
  }
  if (type == "object") {
    return matches_object(actual, expected);
  }
  return false;
}

} // namespace detail

// Families R00-R07, exactly four independently specified seeds each. Bad
// ledger indices are a fixture error, not a successful null observation.
inline auto expected_data(std::size_t family, std::size_t seed) -> Json::Value {
  if (seed >= 4) {
    throw std::out_of_range("native capability data seed");
  }
  switch (family) {
    case 0: return detail::null_data(seed);
    case 1: return detail::boolean_data(seed);
    case 2: return detail::integer_data(seed);
    case 3: return detail::fraction_data(seed);
    case 4: return detail::string_data(seed);
    case 5: return detail::member_data(seed);
    case 6: return detail::array_data(seed);
    case 7: return detail::tree_data(seed);
    default: throw std::out_of_range("native capability data family");
  }
}

// Arrays are ordered. Object entries are an exact unique UTF-16 key set, not
// an enumeration-order promise. Unexpected descriptor fields fail closed.
inline auto matches_tree(const Json::Value& actual, const Json::Value& expected)
    -> bool {
  return detail::matches_descriptor(actual, expected);
}

} // namespace native_capability_corpus
