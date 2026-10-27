// SPDX-License-Identifier: MIT
#include "kernel/extension_http/frames.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <type_traits>

namespace plinth::extension_http {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::size_t MIB = std::size_t{1024} * 1024;
constexpr std::size_t BODY64_LIMIT = ((MAX_BODY_BYTES + 2) / 3) * 4;
constexpr std::size_t DTO_BYTES = MAX_BODY_BYTES + std::size_t{256} * 1024;
constexpr std::size_t SERIAL_WORK_BYTES = 2 * MAX_WIRE_BYTES + 4 * MIB;
constexpr std::array<std::string_view, 8> TYPES{
    "hello",   "hello_ack", "call",   "accepted",
    "entered", "result",    "cancel", "settled"};
constexpr std::array<std::string_view, 5> ERRORS{
    "handler_failed", "response_invalid", "adapter_unavailable",
    "resource_exhausted", "execution_cancelled"};
constexpr std::array<std::string_view, 4> CANCELS{
    "deadline", "client_gone", "shutdown", "generation_retired"};
constexpr std::array<std::string_view, 3> SETTLEMENTS{"completed", "cancelled",
                                                      "failed"};
constexpr std::array<std::string_view, 2> ENTRIES{"not_entered", "entered"};
auto invalid() -> std::unexpected<FrameError> {
  return std::unexpected(FrameError::INVALID);
}
auto large() -> std::unexpected<FrameError> {
  return std::unexpected(FrameError::TOO_LARGE);
}
auto unavailable() -> std::unexpected<FrameError> {
  return std::unexpected(FrameError::RESOURCE_UNAVAILABLE);
}
auto map_error(CodecError e) -> std::unexpected<FrameError> {
  return e == CodecError::TOO_LARGE ? large() : invalid();
}
template <typename E, std::size_t N>
auto enum_name(E value, const std::array<std::string_view, N>& names)
    -> std::string_view {
  const auto index = static_cast<std::size_t>(value);
  return index < N ? names[index] : std::string_view{};
}
template <typename E, std::size_t N>
auto enum_value(const Json& value, const std::array<std::string_view, N>& names)
    -> std::optional<E> {
  if (!value.is_string()) return {};
  for (std::size_t i = 0; i < N; ++i)
    if (value == names[i]) return static_cast<E>(i);
  return {};
}
auto whitespace(char c) -> bool {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
auto hex(char c) -> int {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// Every legal key is ASCII. Decode bounded legal JSON escape spelling without
// constructing a DOM. Non-ASCII key data is unknown for every closed schema.
auto decoded_key(std::string_view token) -> std::optional<std::string> {
  std::string key;
  for (std::size_t i = 1; i + 1 < token.size(); ++i) {
    auto c = static_cast<unsigned char>(token[i]);
    if (c == '\\') {
      if (++i + 1 >= token.size()) return {};
      c = static_cast<unsigned char>(token[i]);
      if (c == 'u') {
        if (i + 4 >= token.size() - 1) return {};
        unsigned value = 0;
        for (unsigned j = 0; j < 4; ++j) {
          const auto digit = hex(token[++i]);
          if (digit < 0) return {};
          value = (value << 4U) | static_cast<unsigned>(digit);
        }
        if (value > 127) return {};
        c = static_cast<unsigned char>(value);
      } else if (c == 'b')
        c = '\b';
      else if (c == 'f')
        c = '\f';
      else if (c == 'n')
        c = '\n';
      else if (c == 'r')
        c = '\r';
      else if (c == 't')
        c = '\t';
      else if (c != '"' && c != '\\' && c != '/')
        return {};
    } else if (c < 32 || c > 127)
      return {};
    key.push_back(static_cast<char>(c));
  }
  return key;
}
// The lexical exemption recognizes DECODED body_b64 keys (including legal
// Unicode escapes); the SAX role pass below proves that the exemption occurred
// at exactly Request.body_b64 or Response.body_b64 in the legal Frame variant.
auto raw_bounds(std::string_view raw) -> FrameResult<void> {
  if (raw.empty()) return invalid();
  if (raw.size() > MAX_WIRE_BYTES) return large();
  if (!valid_utf8(raw)) return invalid();
  std::size_t body = 0;
  bool found = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '"') continue;
    const auto start = i;
    for (++i; i < raw.size() && raw[i] != '"'; ++i) {
      if (raw[i] == '\\' && ++i == raw.size()) return invalid();
    }
    if (i == raw.size()) return invalid();
    auto next = i + 1;
    while (next < raw.size() && whitespace(raw[next]))
      ++next;
    if (next == raw.size() || raw[next] != ':') continue;
    if (i - start > 128) return invalid();
    const auto key = decoded_key(raw.substr(start, i - start + 1));
    if (!key) return invalid();
    if (*key != "body_b64") continue;
    if (found) return invalid();
    found = true;
    ++next;
    while (next < raw.size() && whitespace(raw[next]))
      ++next;
    if (next == raw.size() || raw[next] != '"') return invalid();
    const auto body_start = ++next;
    while (next < raw.size() && raw[next] != '"') {
      if (raw[next] == '\\') return invalid();
      ++next;
    }
    if (next == raw.size()) return invalid();
    body = next - body_start;
    if (body > BODY64_LIMIT) return large();
    i = next;
  }
  if (raw.size() - body > MAX_ENCODED_METADATA_BYTES) return large();
  return {};
}
enum class Role {
  ROOT,
  REQUEST,
  RESPONSE,
  CONTEXT,
  PRINCIPAL,
  EXTENSION,
  OUTCOME,
  SETTLEMENT,
  HEADERS,
  PAIR
};
auto allowed(Role role, std::string_view key) -> bool {
  switch (role) {
    case Role::ROOT:
      return key == "schema" || key == "type" || key == "owner_epoch" ||
             key == "adapter_epoch" || key == "clock_domain_id" ||
             key == "extension" || key == "dispatch_id" ||
             key == "generation_receipt_id" || key == "request" ||
             key == "context" || key == "entered_at_ns" || key == "outcome" ||
             key == "reason" || key == "settlement";
    case Role::REQUEST:
      return key == "schema" || key == "request_id" || key == "method" ||
             key == "path" || key == "original_path" || key == "query" ||
             key == "headers" || key == "body_b64";
    case Role::RESPONSE:
      return key == "schema" || key == "status" || key == "headers" ||
             key == "body_b64";
    case Role::CONTEXT:
      return key == "schema" || key == "dispatch_id" || key == "owner_epoch" ||
             key == "adapter_epoch" || key == "clock_domain_id" ||
             key == "admitted_at_ns" || key == "execution_cutoff_ns" ||
             key == "matched_prefix" || key == "principal" ||
             key == "extension";
    case Role::PRINCIPAL:
      return key == "kind" || key == "user_id" || key == "pat_id";
    case Role::EXTENSION:
      return key == "package_id" || key == "name" || key == "version" ||
             key == "artifact_sha256" || key == "generation_receipt_id";
    case Role::OUTCOME:
      return key == "kind" || key == "response" || key == "code";
    case Role::SETTLEMENT:
      return key == "reason" || key == "entry_state" || key == "entered_at_ns";
    case Role::HEADERS:
    case Role::PAIR: return false;
  }
  return false;
}
class Preflight final : public nlohmann::json_sax<Json> {
  struct Scope {
    Role role;
    std::vector<std::string> keys;
    std::string key, discriminator;
    std::size_t count = 0;
  };
  std::vector<Scope> stack;
  std::size_t nodes = 0, metadata = 0;
  bool root_seen = false;
  auto node() -> bool {
    if (stack.size() + 1 > 8 || nodes == 1024) {
      error = FrameError::TOO_LARGE;
      return false;
    }
    ++nodes;
    return true;
  }
  auto exact(const Scope& s,
             std::initializer_list<std::string_view> fields) const -> bool {
    return s.keys.size() == fields.size() &&
           std::ranges::all_of(fields, [&](auto key) {
             return std::ranges::find(s.keys, key) != s.keys.end();
           });
  }
  auto closed(const Scope& s) const -> bool {
    switch (s.role) {
      case Role::ROOT:
        if (s.discriminator == "hello" || s.discriminator == "hello_ack")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "clock_domain_id", "extension"});
        if (s.discriminator == "call")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "dispatch_id", "generation_receipt_id", "request",
                           "context"});
        if (s.discriminator == "accepted")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "dispatch_id", "generation_receipt_id"});
        if (s.discriminator == "entered")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "dispatch_id", "generation_receipt_id",
                           "entered_at_ns"});
        if (s.discriminator == "result")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "dispatch_id", "generation_receipt_id", "outcome"});
        if (s.discriminator == "cancel")
          return exact(s, {"schema", "type", "owner_epoch", "adapter_epoch",
                           "dispatch_id", "generation_receipt_id", "reason"});
        if (s.discriminator == "settled")
          return exact(s,
                       {"schema", "type", "owner_epoch", "adapter_epoch",
                        "dispatch_id", "generation_receipt_id", "settlement"});
        return false;
      case Role::REQUEST:
        return exact(s, {"schema", "request_id", "method", "path",
                         "original_path", "query", "headers", "body_b64"});
      case Role::RESPONSE:
        return exact(s, {"schema", "status", "headers", "body_b64"});
      case Role::CONTEXT:
        return exact(s, {"schema", "dispatch_id", "owner_epoch",
                         "adapter_epoch", "clock_domain_id", "admitted_at_ns",
                         "execution_cutoff_ns", "matched_prefix", "principal",
                         "extension"});
      case Role::PRINCIPAL:
        return s.discriminator == "anonymous"
                   ? exact(s, {"kind"})
                   : s.discriminator == "pat" &&
                         exact(s, {"kind", "user_id", "pat_id"});
      case Role::EXTENSION:
        return exact(s, {"package_id", "name", "version", "artifact_sha256",
                         "generation_receipt_id"});
      case Role::OUTCOME:
        return s.discriminator == "response"
                   ? exact(s, {"kind", "response"})
                   : s.discriminator == "error" && exact(s, {"kind", "code"});
      case Role::SETTLEMENT:
        return exact(s, {"reason", "entry_state", "entered_at_ns"});
      case Role::HEADERS: return true;
      case Role::PAIR: return s.count == 2;
    }
    return false;
  }
  template <typename Number> auto integer(Number value) -> bool {
    if (!node() || stack.empty()) return false;
    const auto& s = stack.back();
    if ((s.role == Role::ROOT || s.role == Role::REQUEST ||
         s.role == Role::RESPONSE || s.role == Role::CONTEXT) &&
        s.key == "schema")
      return value == 1;
    return s.role == Role::RESPONSE && s.key == "status" && value >= 200 &&
           value <= 599;
  }

 public:
  FrameError error = FrameError::INVALID;
  auto null() -> bool override {
    return node() && !stack.empty() && stack.back().role == Role::SETTLEMENT &&
           stack.back().key == "entered_at_ns";
  }
  auto boolean(bool) -> bool override { return false; }
  auto number_integer(number_integer_t v) -> bool override {
    return integer(v);
  }
  auto number_unsigned(number_unsigned_t v) -> bool override {
    return integer(v);
  }
  auto number_float(number_float_t, const string_t&) -> bool override {
    return false;
  }
  auto string(string_t& text) -> bool override {
    if (!node() || stack.empty()) return false;
    auto& s = stack.back();
    if (s.role == Role::HEADERS || s.key == "schema" ||
        (s.role == Role::RESPONSE && s.key == "status"))
      return false;
    if (s.role == Role::PAIR) {
      if (++s.count > 2) return false;
    } else if (s.key == "headers" || s.key == "request" || s.key == "context" ||
               s.key == "principal" || s.key == "extension" ||
               s.key == "outcome" || s.key == "response" ||
               s.key == "settlement")
      return false;
    const bool body = (s.role == Role::REQUEST || s.role == Role::RESPONSE) &&
                      s.key == "body_b64";
    if (!body) {
      if (text.size() > MAX_METADATA_BYTES - metadata) {
        error = FrameError::TOO_LARGE;
        return false;
      }
      metadata += text.size();
    }
    if ((s.role == Role::ROOT && s.key == "type") ||
        ((s.role == Role::OUTCOME || s.role == Role::PRINCIPAL) &&
         s.key == "kind"))
      s.discriminator = text;
    return true;
  }
  auto binary(binary_t&) -> bool override { return false; }
  auto start_object(std::size_t) -> bool override {
    if (!node()) return false;
    Role role = Role::ROOT;
    if (stack.empty()) {
      if (root_seen) return false;
      root_seen = true;
    } else {
      const auto& s = stack.back();
      if (s.role == Role::ROOT && s.key == "request")
        role = Role::REQUEST;
      else if (s.role == Role::ROOT && s.key == "context")
        role = Role::CONTEXT;
      else if ((s.role == Role::ROOT || s.role == Role::CONTEXT) &&
               s.key == "extension")
        role = Role::EXTENSION;
      else if (s.role == Role::CONTEXT && s.key == "principal")
        role = Role::PRINCIPAL;
      else if (s.role == Role::ROOT && s.key == "outcome")
        role = Role::OUTCOME;
      else if (s.role == Role::ROOT && s.key == "settlement")
        role = Role::SETTLEMENT;
      else if (s.role == Role::OUTCOME && s.key == "response")
        role = Role::RESPONSE;
      else
        return false;
    }
    stack.push_back({role, {}, {}, {}, 0});
    return true;
  }
  auto end_object() -> bool override {
    if (stack.empty() || !closed(stack.back())) return false;
    stack.pop_back();
    return true;
  }
  auto key(string_t& key) -> bool override {
    if (stack.empty()) return false;
    auto& s = stack.back();
    if (!allowed(s.role, key) || std::ranges::find(s.keys, key) != s.keys.end())
      return false;
    s.keys.push_back(key);
    s.key = key;
    return true;
  }
  auto start_array(std::size_t) -> bool override {
    if (!node() || stack.empty()) return false;
    auto& s = stack.back();
    Role role = Role::HEADERS;
    if ((s.role == Role::REQUEST || s.role == Role::RESPONSE) &&
        s.key == "headers") {
    } else if (s.role == Role::HEADERS) {
      if (++s.count > MAX_HEADER_PAIRS) {
        error = FrameError::TOO_LARGE;
        return false;
      }
      role = Role::PAIR;
    } else
      return false;
    stack.push_back({role, {}, {}, {}, 0});
    return true;
  }
  auto end_array() -> bool override {
    if (stack.empty() || !closed(stack.back())) return false;
    stack.pop_back();
    return true;
  }
  auto parse_error(std::size_t, const std::string&,
                   const nlohmann::detail::exception&) -> bool override {
    return false;
  }
};
auto preflight(std::string_view raw) -> FrameResult<void> {
  if (auto valid = raw_bounds(raw); !valid) return valid;
  Preflight reader;
  if (!Json::sax_parse(raw.begin(), raw.end(), &reader))
    return std::unexpected(reader.error);
  return {};
}
auto valid_extension(const Extension& e) -> bool {
  return valid_uuid(e.package_id) && valid_name(e.name) &&
         valid_version(e.version) && valid_hex256(e.artifact_sha256) &&
         valid_uuid(e.generation_receipt_id);
}
auto valid_identity(const FrameIdentity& i) -> bool {
  return valid_uuid(i.owner_epoch) && valid_uuid(i.adapter_epoch) &&
         valid_uuid(i.dispatch_id) && valid_uuid(i.generation_receipt_id);
}
auto valid_identity(const HandshakeIdentity& i) -> bool {
  return valid_uuid(i.owner_epoch) && valid_uuid(i.adapter_epoch) &&
         valid_uuid(i.clock_domain_id) && valid_extension(i.extension);
}
auto extension_json(const Extension& e) -> Json {
  return {{"package_id", e.package_id},
          {"name", e.name},
          {"version", e.version},
          {"artifact_sha256", e.artifact_sha256},
          {"generation_receipt_id", e.generation_receipt_id}};
}
auto extension_data(const Json& e) -> Extension {
  return {e["package_id"].get<std::string>(), e["name"].get<std::string>(),
          e["version"].get<std::string>(),
          e["artifact_sha256"].get<std::string>(),
          e["generation_receipt_id"].get<std::string>()};
}
auto identity_data(const Json& j) -> FrameIdentity {
  return {j["owner_epoch"].get<std::string>(),
          j["adapter_epoch"].get<std::string>(),
          j["dispatch_id"].get<std::string>(),
          j["generation_receipt_id"].get<std::string>()};
}
auto extension_metadata(const Extension& e) -> std::size_t {
  return e.package_id.size() + e.name.size() + e.version.size() +
         e.artifact_sha256.size() + e.generation_receipt_id.size();
}
auto context_metadata(const ContextData& c) -> std::size_t {
  return c.dispatch_id.size() + c.owner_epoch.size() + c.adapter_epoch.size() +
         c.clock_domain_id.size() + std::to_string(c.admitted_at_ns).size() +
         std::to_string(c.execution_cutoff_ns).size() +
         c.matched_prefix.size() +
         (c.principal.kind == Principal::Kind::PAT
              ? 3 + c.principal.user_id.size() + c.principal.pat_id.size()
              : 9) +
         extension_metadata(c.extension);
}
auto headers_metadata(const Headers& h) -> std::size_t {
  std::size_t n = 0;
  for (const auto& [key, value] : h)
    n += key.size() + value.size();
  return n;
}
auto metadata_size(const Frame& f) -> std::size_t {
  return std::visit(
      [&](const auto& value) -> std::size_t {
        using T = std::decay_t<decltype(value)>;
        const auto type = TYPES[f.index()].size();
        const auto& i = value.identity;
        if constexpr (std::is_same_v<T, Hello> || std::is_same_v<T, HelloAck>)
          return type + i.owner_epoch.size() + i.adapter_epoch.size() +
                 i.clock_domain_id.size() + extension_metadata(i.extension);
        else {
          const auto common = type + i.owner_epoch.size() +
                              i.adapter_epoch.size() + i.dispatch_id.size() +
                              i.generation_receipt_id.size();
          if constexpr (std::is_same_v<T, Call>) {
            const auto& r = value.request.data;
            return common + r.request_id.size() + r.method.size() +
                   r.target.path.size() + r.target.original_path.size() +
                   r.target.query.size() + headers_metadata(r.headers) +
                   context_metadata(value.context);
          } else if constexpr (std::is_same_v<T, Entered>)
            return common + std::to_string(value.entered_at_ns).size();
          else if constexpr (std::is_same_v<T, Result>) {
            if (const auto* r = std::get_if<NativeResponse>(&value.outcome))
              return common + 8 + headers_metadata(r->headers);
            return common + 5 +
                   enum_name(std::get<AdapterFailure>(value.outcome), ERRORS)
                       .size();
          } else if constexpr (std::is_same_v<T, Cancel>)
            return common + enum_name(value.reason, CANCELS).size();
          else if constexpr (std::is_same_v<T, Settled>)
            return common +
                   enum_name(value.settlement.reason, SETTLEMENTS).size() +
                   enum_name(value.settlement.entry_state, ENTRIES).size() +
                   (value.settlement.entered_at_ns
                        ? std::to_string(*value.settlement.entered_at_ns).size()
                        : 0);
          else
            return common;
        }
      },
      f);
}
auto read_frame(const Json& j) -> FrameResult<Frame> {
  const auto& type = j["type"];
  if (type == "hello" || type == "hello_ack") {
    HandshakeIdentity i{j["owner_epoch"].get<std::string>(),
                        j["adapter_epoch"].get<std::string>(),
                        j["clock_domain_id"].get<std::string>(),
                        extension_data(j["extension"])};
    if (type == "hello") return Frame{Hello{std::move(i)}};
    return Frame{HelloAck{std::move(i)}};
  }
  auto i = identity_data(j);
  if (type == "call") {
    auto r = parse_wire_request(j["request"].dump());
    if (!r) return map_error(r.error());
    auto c = parse_context(j["context"].dump());
    if (!c) return map_error(c.error());
    return Frame{Call{std::move(i), std::move(*r), std::move(*c)}};
  }
  if (type == "accepted") return Frame{Accepted{std::move(i)}};
  if (type == "entered") {
    auto ns = parse_ns(j["entered_at_ns"].get_ref<const std::string&>());
    if (!ns) return map_error(ns.error());
    return Frame{Entered{std::move(i), *ns}};
  }
  if (type == "result") {
    const auto& out = j["outcome"];
    if (out["kind"] == "response") {
      auto r = parse_wire_response(out["response"].dump());
      if (!r) return map_error(r.error());
      return Frame{Result{std::move(i), std::move(*r)}};
    }
    auto error = enum_value<AdapterFailure>(out["code"], ERRORS);
    if (!error) return invalid();
    return Frame{Result{std::move(i), *error}};
  }
  if (type == "cancel") {
    auto reason = enum_value<CancelReason>(j["reason"], CANCELS);
    if (!reason) return invalid();
    return Frame{Cancel{std::move(i), *reason}};
  }
  if (type == "settled") {
    const auto& s = j["settlement"];
    auto reason = enum_value<SettlementReason>(s["reason"], SETTLEMENTS);
    auto entry = enum_value<PeerEntryState>(s["entry_state"], ENTRIES);
    if (!reason || !entry) return invalid();
    std::optional<std::uint64_t> entered;
    if (!s["entered_at_ns"].is_null()) {
      auto n = parse_ns(s["entered_at_ns"].get_ref<const std::string&>());
      if (!n) return map_error(n.error());
      entered = *n;
    }
    return Frame{Settled{std::move(i), {*reason, *entry, entered}}};
  }
  return invalid();
}
// This scope destroys root/nested DOM and serializer temporaries before the
// final SAX backstop, avoiding simultaneous bulk strings from both phases.
auto write_frame(const Frame& f) -> FrameResult<std::string> {
  Json j{{"schema", 1}, {"type", TYPES[f.index()]}};
  auto emitted = std::visit(
      [&](const auto& v) -> FrameResult<void> {
        using T = std::decay_t<decltype(v)>;
        const auto& i = v.identity;
        j["owner_epoch"] = i.owner_epoch;
        j["adapter_epoch"] = i.adapter_epoch;
        if constexpr (std::is_same_v<T, Hello> || std::is_same_v<T, HelloAck>) {
          j["clock_domain_id"] = i.clock_domain_id;
          j["extension"] = extension_json(i.extension);
        } else {
          j["dispatch_id"] = i.dispatch_id;
          j["generation_receipt_id"] = i.generation_receipt_id;
          if constexpr (std::is_same_v<T, Call>) {
            auto request = serialize_wire_request(v.request);
            if (!request) return map_error(request.error());
            j["request"] = Json::parse(*request);
            auto context = serialize_context(v.context);
            if (!context) return map_error(context.error());
            j["context"] = Json::parse(*context);
          } else if constexpr (std::is_same_v<T, Entered>)
            j["entered_at_ns"] = std::to_string(v.entered_at_ns);
          else if constexpr (std::is_same_v<T, Result>) {
            if (const auto* r = std::get_if<NativeResponse>(&v.outcome)) {
              auto response = serialize_wire_response(*r);
              if (!response) return map_error(response.error());
              j["outcome"] = {{"kind", "response"},
                              {"response", Json::parse(*response)}};
            } else
              j["outcome"] = {
                  {"kind", "error"},
                  {"code",
                   enum_name(std::get<AdapterFailure>(v.outcome), ERRORS)}};
          } else if constexpr (std::is_same_v<T, Cancel>)
            j["reason"] = enum_name(v.reason, CANCELS);
          else if constexpr (std::is_same_v<T, Settled>)
            j["settlement"] = {
                {"reason", enum_name(v.settlement.reason, SETTLEMENTS)},
                {"entry_state", enum_name(v.settlement.entry_state, ENTRIES)},
                {"entered_at_ns",
                 v.settlement.entered_at_ns
                     ? Json(std::to_string(*v.settlement.entered_at_ns))
                     : Json(nullptr)}};
        }
        return {};
      },
      f);
  if (!emitted) return std::unexpected(emitted.error());
  return j.dump();
}
auto validate_data(const Frame& f) -> FrameResult<void> try {
  if (f.valueless_by_exception()) return invalid();
  auto valid = std::visit(
      [](const auto& v) -> FrameResult<void> {
        using T = std::decay_t<decltype(v)>;
        if (!valid_identity(v.identity)) return invalid();
        if constexpr (std::is_same_v<T, Call>) {
          if (auto valid = validate_call_data(v.request, v.context); !valid)
            return map_error(valid.error());
          const auto& i = v.identity;
          const auto& c = v.context;
          if (i.owner_epoch != c.owner_epoch ||
              i.adapter_epoch != c.adapter_epoch ||
              i.dispatch_id != c.dispatch_id ||
              i.generation_receipt_id != c.extension.generation_receipt_id)
            return invalid();
        } else if constexpr (std::is_same_v<T, Result>) {
          if (const auto* r = std::get_if<NativeResponse>(&v.outcome)) {
            if (auto valid = validate_response(*r); !valid)
              return map_error(valid.error());
          } else if (enum_name(std::get<AdapterFailure>(v.outcome), ERRORS)
                         .empty())
            return invalid();
        } else if constexpr (std::is_same_v<T, Cancel>) {
          if (enum_name(v.reason, CANCELS).empty()) return invalid();
        } else if constexpr (std::is_same_v<T, Settled>) {
          const auto& s = v.settlement;
          if (enum_name(s.reason, SETTLEMENTS).empty() ||
              enum_name(s.entry_state, ENTRIES).empty() ||
              (s.entry_state == PeerEntryState::ENTERED) !=
                  s.entered_at_ns.has_value())
            return invalid();
        }
        return {};
      },
      f);
  if (!valid) return valid;
  if (metadata_size(f) > MAX_METADATA_BYTES) return large();
  return {};
} catch (...) {
  return unavailable();
}
} // namespace
auto decode_frame_length(std::array<std::uint8_t, 4> h)
    -> FrameResult<std::uint32_t> {
  const auto n = (std::uint32_t{h[0]} << 24U) | (std::uint32_t{h[1]} << 16U) |
                 (std::uint32_t{h[2]} << 8U) | std::uint32_t{h[3]};
  if (n == 0) return invalid();
  if (n > MAX_WIRE_BYTES) return large();
  return n;
}
FramePayload::FramePayload(Binary b, std::shared_ptr<AdmissionLease> a,
                           ExtraMemoryCharge c)
    : admission(std::move(a)), charge(std::move(c)), buffer(std::move(b)) {
}
auto FramePayload::operator=(FramePayload&& other) noexcept -> FramePayload& {
  if (this != &other) {
    buffer = std::move(other.buffer);
    charge = std::move(other.charge);
    admission = std::move(other.admission);
  }
  return *this;
}
auto FramePayload::writable() noexcept -> std::span<std::uint8_t> {
  return buffer;
}
auto FramePayload::bytes() const noexcept -> std::span<const std::uint8_t> {
  return buffer;
}
OwnedFrame::OwnedFrame(Frame f, std::shared_ptr<AdmissionLease> a,
                       ExtraMemoryCharge c)
    : admission(std::move(a)), charge(std::move(c)), frame(std::move(f)) {
}
auto OwnedFrame::operator=(OwnedFrame&& other) noexcept -> OwnedFrame& {
  if (this != &other) {
    frame = std::move(other.frame);
    charge = std::move(other.charge);
    admission = std::move(other.admission);
  }
  return *this;
}
auto OwnedFrame::value() const noexcept -> const Frame& {
  return frame;
}
EncodedFrame::EncodedFrame(Binary b, std::shared_ptr<AdmissionLease> a,
                           ExtraMemoryCharge c)
    : admission(std::move(a)), charge(std::move(c)), buffer(std::move(b)) {
}
auto EncodedFrame::operator=(EncodedFrame&& other) noexcept -> EncodedFrame& {
  if (this != &other) {
    buffer = std::move(other.buffer);
    charge = std::move(other.charge);
    admission = std::move(other.admission);
  }
  return *this;
}
auto EncodedFrame::bytes() const noexcept -> std::span<const std::uint8_t> {
  return buffer;
}
auto allocate_frame_payload(std::array<std::uint8_t, 4> header,
                            std::shared_ptr<AdmissionLease> admission)
    -> FrameResult<FramePayload> try {
  const auto n = decode_frame_length(header);
  if (!n) return std::unexpected(n.error());
  if (!admission) return unavailable();
  auto charge = admission->reserve_extra(*n);
  if (!charge) return unavailable();
  return FramePayload{Binary(*n), std::move(admission), std::move(*charge)};
} catch (...) {
  return unavailable();
}
auto parse_frame_payload(std::string_view raw,
                         std::shared_ptr<AdmissionLease> admission)
    -> FrameResult<OwnedFrame> try {
  if (raw.empty()) return invalid();
  if (raw.size() > MAX_WIRE_BYTES) return large();
  if (!admission) return unavailable();
  auto work = admission->reserve_extra(3 * raw.size() + 12 * MIB);
  if (!work) return unavailable();
  auto storage = admission->reserve_extra(DTO_BYTES);
  if (!storage) return unavailable();
  if (auto checked = preflight(raw); !checked)
    return std::unexpected(checked.error());
  const auto j = Json::parse(raw, nullptr, false);
  auto frame = read_frame(j);
  if (!frame) return std::unexpected(frame.error());
  if (auto valid = validate_data(*frame); !valid)
    return std::unexpected(valid.error());
  return OwnedFrame{std::move(*frame), std::move(admission),
                    std::move(*storage)};
} catch (...) {
  return unavailable();
}
auto parse_framed_buffer(std::span<const std::uint8_t> raw,
                         std::shared_ptr<AdmissionLease> admission)
    -> FrameResult<OwnedFrame> {
  if (raw.size() < 4) return invalid();
  const auto n = decode_frame_length({raw[0], raw[1], raw[2], raw[3]});
  if (!n) return std::unexpected(n.error());
  if (raw.size() - 4 != *n) return invalid();
  return parse_frame_payload(
      {reinterpret_cast<const char*>(raw.data() + 4), *n},
      std::move(admission));
}
auto encode_frame(const Frame& f, std::shared_ptr<AdmissionLease> admission)
    -> FrameResult<EncodedFrame> try {
  if (!admission) return unavailable();
  auto work = admission->reserve_extra(SERIAL_WORK_BYTES);
  if (!work) return unavailable();
  auto storage = admission->reserve_extra(MAX_WIRE_BYTES + 4);
  if (!storage) return unavailable();
  if (auto valid = validate_data(f); !valid)
    return std::unexpected(valid.error());
  auto encoded = write_frame(f);
  if (!encoded) return std::unexpected(encoded.error());
  const auto& raw = *encoded;
  if (auto checked = preflight(raw); !checked)
    return std::unexpected(checked.error());
  const auto n = static_cast<std::uint32_t>(raw.size());
  Binary bytes(raw.size() + 4);
  bytes[0] = static_cast<std::uint8_t>(n >> 24U);
  bytes[1] = static_cast<std::uint8_t>(n >> 16U);
  bytes[2] = static_cast<std::uint8_t>(n >> 8U);
  bytes[3] = static_cast<std::uint8_t>(n);
  std::ranges::copy(raw, bytes.begin() + 4);
  return EncodedFrame{std::move(bytes), std::move(admission),
                      std::move(*storage)};
} catch (...) {
  return unavailable();
}
} // namespace plinth::extension_http
