// SPDX-License-Identifier: MIT
// Pure codec/framing/credit controls; no socket, peer authentication, durable
// dispatch, clock-domain verification or physical adapter settlement proof.
#include "kernel/extension_http/frames.hpp"
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace {
using namespace plinth::extension_http;
using Json = nlohmann::ordered_json;
constexpr auto OWNER = "11111111-1111-1111-1111-111111111111";
constexpr auto ADAPTER = "22222222-2222-2222-2222-222222222222";
constexpr auto DISPATCH = "33333333-3333-3333-3333-333333333333";
constexpr auto RECEIPT = "44444444-4444-4444-4444-444444444444";
constexpr auto CLOCK = "55555555-5555-5555-5555-555555555555";
constexpr auto PACKAGE = "66666666-6666-6666-6666-666666666666";
constexpr auto USER = "77777777-7777-7777-7777-777777777777";
constexpr auto PAT = "88888888-8888-8888-8888-888888888888";
static_assert(!std::is_copy_constructible_v<OwnedFrame>);
static_assert(!std::is_copy_constructible_v<EncodedFrame>);
static_assert(!std::is_copy_constructible_v<FramePayload>);
struct Fixture {
  plinth::lifecycle::AsyncTaskRegistry process;
  CreditPool pool;
  std::shared_ptr<AdmissionLease> lease =
      pool.try_acquire("framesfixture", 0, process.try_acquire());
  // Test-owned input DTO/raw strings are reserved BEFORE construction. These
  // are not process/network transport counters or a measured allocator bound.
  std::optional<ExtraMemoryCharge> input =
      lease->reserve_extra(std::size_t{20} * 1024 * 1024);
  auto release() -> void {
    input.reset();
    lease.reset();
  }
};
auto extension() -> Extension {
  return {PACKAGE, "fixture", "1.0.0", std::string(64, 'a'), RECEIPT};
}
auto identity() -> FrameIdentity {
  return {OWNER, ADAPTER, DISPATCH, RECEIPT};
}
auto handshake() -> HandshakeIdentity {
  return {OWNER, ADAPTER, CLOCK, extension()};
}
auto call() -> Call {
  return {
      identity(),
      {{DISPATCH,
        "POST",
        {"/app/item", "/app/item", "x=%FF+?"},
        {{"x-test", "value"}}},
       {0, 127, 255}},
      {DISPATCH, OWNER, ADAPTER, CLOCK, 100, 4500, "/app/*", {}, extension()}};
}
auto payload(const EncodedFrame& frame) -> std::string {
  const auto bytes = frame.bytes();
  return {reinterpret_cast<const char*>(bytes.data() + 4), bytes.size() - 4};
}
auto canonical(const Frame& frame, Fixture& f) -> std::string {
  auto encoded = encode_frame(frame, f.lease);
  REQUIRE(encoded);
  return payload(*encoded);
}
auto reverse_objects(const Json& value) -> Json {
  if (value.is_object()) {
    Json result = Json::object();
    for (auto it = value.rbegin(); it != value.rend(); ++it)
      result[it.key()] = reverse_objects(it.value());
    return result;
  }
  if (value.is_array()) {
    Json result = Json::array();
    for (const auto& item : value)
      result.push_back(reverse_objects(item));
    return result;
  }
  return value;
}
auto keys(const Json& value) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (auto it = value.begin(); it != value.end(); ++it)
    names.push_back(it.key());
  return names;
}
auto require_invalid(std::string_view raw, Fixture& f) -> void {
  auto parsed = parse_frame_payload(raw, f.lease);
  REQUIRE_FALSE(parsed);
  REQUIRE(parsed.error() == FrameError::INVALID);
}
auto inject(std::string raw, std::string_view anchor, std::string_view fields)
    -> std::string {
  const auto position = raw.find(anchor);
  REQUIRE(position != std::string::npos);
  raw.insert(position + anchor.size(), fields);
  return raw;
}
auto header(std::uint32_t n) -> std::array<std::uint8_t, 4> {
  return {static_cast<std::uint8_t>(n >> 24U),
          static_cast<std::uint8_t>(n >> 16U),
          static_cast<std::uint8_t>(n >> 8U), static_cast<std::uint8_t>(n)};
}
} // namespace

TEST_CASE(
    "HTTP Frame1 all eight variants roundtrip with canonical definition order",
    "[extension-http][http_frames]") {
  Fixture f;
  REQUIRE(f.input);
  const std::array<Frame, 8> frames{
      Hello{handshake()},
      HelloAck{handshake()},
      call(),
      Accepted{identity()},
      Entered{identity(), 101},
      Result{identity(),
             NativeResponse{201, {{"x-result", "ok"}}, {0, 128, 255}}},
      Cancel{identity(), CancelReason::CLIENT_GONE},
      Settled{identity(),
              {SettlementReason::COMPLETED, PeerEntryState::ENTERED, 101}}};
  constexpr std::array types{"hello",   "hello_ack", "call",   "accepted",
                             "entered", "result",    "cancel", "settled"};
  const std::array<std::vector<std::string>, 8> expected_keys{{
      {"schema", "type", "owner_epoch", "adapter_epoch", "clock_domain_id",
       "extension"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "clock_domain_id",
       "extension"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id", "request", "context"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id", "entered_at_ns"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id", "outcome"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id", "reason"},
      {"schema", "type", "owner_epoch", "adapter_epoch", "dispatch_id",
       "generation_receipt_id", "settlement"},
  }};
  for (std::size_t i = 0; i < frames.size(); ++i) {
    INFO(types[i]);
    auto encoded = encode_frame(frames[i], f.lease);
    REQUIRE(encoded);
    auto parsed = parse_framed_buffer(encoded->bytes(), f.lease);
    REQUIRE(parsed);
    REQUIRE(parsed->value() == frames[i]);
    auto again = encode_frame(parsed->value(), f.lease);
    REQUIRE(again);
    REQUIRE(std::ranges::equal(encoded->bytes(), again->bytes()));
    const auto raw = payload(*encoded);
    REQUIRE(raw.starts_with(std::string{"{\"schema\":1,\"type\":\""} +
                            types[i] + "\",\"owner_epoch\":\""));
    const auto original = Json::parse(raw);
    REQUIRE(keys(original) == expected_keys[i]);
    if (i == 2) {
      REQUIRE(keys(original["request"]) ==
              std::vector<std::string>{"schema", "request_id", "method", "path",
                                       "original_path", "query", "headers",
                                       "body_b64"});
      REQUIRE(keys(original["context"]) ==
              std::vector<std::string>{
                  "schema", "dispatch_id", "owner_epoch", "adapter_epoch",
                  "clock_domain_id", "admitted_at_ns", "execution_cutoff_ns",
                  "matched_prefix", "principal", "extension"});
    }
    if (i == 5)
      REQUIRE(keys(original["outcome"]) ==
              std::vector<std::string>{"kind", "response"});
    if (i == 7)
      REQUIRE(
          keys(original["settlement"]) ==
          std::vector<std::string>{"reason", "entry_state", "entered_at_ns"});
    auto received =
        parse_frame_payload(reverse_objects(original).dump(), f.lease);
    REQUIRE(received);
    REQUIRE(received->value() == frames[i]);
    if (i < 2) REQUIRE_FALSE(original.contains("dispatch_id"));
  }
  const auto accepted = canonical(Accepted{identity()}, f);
  REQUIRE(
      accepted ==
      std::string{"{\"schema\":1,\"type\":\"accepted\",\"owner_epoch\":\""} +
          OWNER + "\",\"adapter_epoch\":\"" + ADAPTER +
          "\",\"dispatch_id\":\"" + DISPATCH +
          "\",\"generation_receipt_id\":\"" + RECEIPT + "\"}");
}

TEST_CASE("HTTP Frame1 closed outcomes cancellation and settlement enums "
          "remain exact",
          "[extension-http][http_frames]") {
  Fixture f;
  for (auto error :
       {AdapterFailure::HANDLER_FAILED, AdapterFailure::RESPONSE_INVALID,
        AdapterFailure::ADAPTER_UNAVAILABLE, AdapterFailure::RESOURCE_EXHAUSTED,
        AdapterFailure::EXECUTION_CANCELLED}) {
    const Frame expected = Result{identity(), error};
    auto parsed = parse_frame_payload(canonical(expected, f), f.lease);
    REQUIRE(parsed);
    REQUIRE(parsed->value() == expected);
  }
  for (auto reason :
       {CancelReason::DEADLINE, CancelReason::CLIENT_GONE,
        CancelReason::SHUTDOWN, CancelReason::GENERATION_RETIRED}) {
    const Frame expected = Cancel{identity(), reason};
    auto parsed = parse_frame_payload(canonical(expected, f), f.lease);
    REQUIRE(parsed);
    REQUIRE(parsed->value() == expected);
  }
  for (auto reason : {SettlementReason::COMPLETED, SettlementReason::CANCELLED,
                      SettlementReason::FAILED}) {
    for (bool entered : {false, true}) {
      const Frame expected = Settled{
          identity(),
          {reason,
           entered ? PeerEntryState::ENTERED : PeerEntryState::NOT_ENTERED,
           entered ? std::optional<std::uint64_t>{101} : std::nullopt}};
      auto parsed = parse_frame_payload(canonical(expected, f), f.lease);
      REQUIRE(parsed);
      REQUIRE(parsed->value() == expected);
    }
  }
  REQUIRE_FALSE(encode_frame(
      Result{identity(), static_cast<AdapterFailure>(99)}, f.lease));
  REQUIRE_FALSE(
      encode_frame(Cancel{identity(), static_cast<CancelReason>(99)}, f.lease));
  REQUIRE_FALSE(encode_frame(
      Settled{
          identity(),
          {static_cast<SettlementReason>(99), PeerEntryState::NOT_ENTERED, {}}},
      f.lease));
  REQUIRE_FALSE(encode_frame(
      Settled{identity(),
              {SettlementReason::FAILED, static_cast<PeerEntryState>(99), {}}},
      f.lease));
}

TEST_CASE("HTTP Frame1 raw decoded duplicate and unknown keys reject at every "
          "record role",
          "[extension-http][http_frames]") {
  Fixture f;
  const auto raw = canonical(call(), f);
  for (auto anchor : {"{", "\"request\":{", "\"context\":{", "\"principal\":{",
                      "\"extension\":{"}) {
    INFO(anchor);
    require_invalid(inject(raw, anchor, "\"extra\":0,"), f);
  }
  require_invalid(inject(raw, "{", "\"\\u0073chema\":1,"), f);
  require_invalid(inject(raw, "\"request\":{", "\"\\u0073chema\":1,"), f);
  require_invalid(inject(raw, "\"context\":{", "\"\\u0073chema\":1,"), f);
  require_invalid(
      inject(raw, "\"principal\":{", "\"\\u006bind\":\"anonymous\","), f);
  require_invalid(inject(raw, "\"extension\":{",
                         std::string{"\"package_id\":\""} + PACKAGE + "\","),
                  f);
  const auto response =
      canonical(Result{identity(), NativeResponse{200, {}, {}}}, f);
  for (auto anchor : {"\"outcome\":{", "\"response\":{"})
    require_invalid(inject(response, anchor, "\"extra\":0,"), f);
  require_invalid(
      inject(response, "\"outcome\":{", "\"\\u006bind\":\"response\","), f);
  const auto settled = canonical(Settled{identity(), {}}, f);
  require_invalid(inject(settled, "\"settlement\":{", "\"extra\":0,"), f);
  require_invalid(
      inject(settled, "\"settlement\":{", "\"reason\":\"completed\","), f);
  // The discriminant may be last, but a key legal only for another variant
  // must still fail its exact role before DOM construction.
  auto hello = Json::parse(canonical(Hello{handshake()}, f));
  hello["dispatch_id"] = DISPATCH;
  require_invalid(hello.dump(), f);
  auto outcome = Json::parse(response);
  outcome["outcome"]["code"] = "handler_failed";
  require_invalid(outcome.dump(), f);
}

TEST_CASE("HTTP Frame1 rejects wrong primitive types missing data and "
          "malformed scalars",
          "[extension-http][http_frames]") {
  Fixture f;
  const auto original = Json::parse(canonical(call(), f));
  for (const auto& value : std::array<Json, 5>{true, 1.0, "1", nullptr, 2}) {
    auto j = original;
    j["schema"] = value;
    require_invalid(j.dump(), f);
  }
  for (const auto& value : std::array<Json, 4>{0, 1.5, true, "01"}) {
    auto j = original;
    j["context"]["admitted_at_ns"] = value;
    require_invalid(j.dump(), f);
  }
  for (auto text : {"+1", "01", "1e2", "18446744073709551616"}) {
    auto j = original;
    j["context"]["execution_cutoff_ns"] = text;
    require_invalid(j.dump(), f);
  }
  for (auto key : {"owner_epoch", "adapter_epoch", "dispatch_id",
                   "generation_receipt_id"}) {
    auto j = original;
    j[key] = "invalid";
    require_invalid(j.dump(), f);
  }
  auto missing = original;
  missing["request"].erase("query");
  require_invalid(missing.dump(), f);
  auto primitive = original;
  primitive["context"]["principal"] = Json::array();
  require_invalid(primitive.dump(), f);
  auto bad_status =
      Json::parse(canonical(Result{identity(), NativeResponse{}}, f));
  for (const auto& value :
       std::array<Json, 6>{true, 200.0, "200", 199, 600, nullptr}) {
    auto j = bad_status;
    j["outcome"]["response"]["status"] = value;
    require_invalid(j.dump(), f);
  }
  auto bad_enum =
      Json::parse(canonical(Cancel{identity(), CancelReason::DEADLINE}, f));
  bad_enum["reason"] = "future";
  require_invalid(bad_enum.dump(), f);
  auto settled = Json::parse(canonical(Settled{identity(), {}}, f));
  settled["settlement"]["entered_at_ns"] = "101";
  require_invalid(settled.dump(), f);
  settled["settlement"]["entry_state"] = "entered";
  settled["settlement"]["entered_at_ns"] = nullptr;
  require_invalid(settled.dump(), f);
}

TEST_CASE(
    "HTTP Frame1 call identity prefix and path equality are data checks only",
    "[extension-http][http_frames]") {
  Fixture f;
  auto source = call();
  REQUIRE(encode_frame(source, f.lease));
  SECTION("envelope owner") {
    source.identity.owner_epoch = USER;
  }
  SECTION("envelope adapter") {
    source.identity.adapter_epoch = USER;
  }
  SECTION("envelope dispatch") {
    source.identity.dispatch_id = USER;
  }
  SECTION("envelope generation") {
    source.identity.generation_receipt_id = USER;
  }
  SECTION("request dispatch") {
    source.request.data.request_id = USER;
  }
  SECTION("prefix does not match path") {
    source.context.matched_prefix = "/else/*";
  }
  SECTION("original decoded path mismatch") {
    source.request.data.target.original_path = "/app/else";
  }
  REQUIRE_FALSE(encode_frame(source, f.lease));
  // An internally shaped standalone timestamp/ack is NOT evidence of matching
  // an actual peer/clock/prior Entered/handshake or physical settled children.
  REQUIRE(encode_frame(
      Entered{identity(), std::numeric_limits<std::uint64_t>::max()}, f.lease));
  REQUIRE(encode_frame(HelloAck{handshake()}, f.lease));
}

TEST_CASE("HTTP Frame1 UTF8 body token encoding and illegal shape fail before "
          "DOM use",
          "[extension-http][http_frames]") {
  Fixture f;
  const auto raw = canonical(call(), f);
  auto invalid_utf8 = raw;
  invalid_utf8.insert(1, 1, static_cast<char>(0xff));
  require_invalid(invalid_utf8, f);
  auto lone = Json::parse(raw);
  lone["request"]["headers"] = Json::array({Json::array({"x", "marker"})});
  auto surrogate = lone.dump();
  const auto at = surrogate.find("marker");
  surrogate.replace(at, 6, "\\uD800");
  require_invalid(surrogate, f);
  auto escaped_body = raw;
  const auto body = escaped_body.find("AH//");
  REQUIRE(body != std::string::npos);
  escaped_body.replace(body, 1, "\\u0041");
  require_invalid(escaped_body, f);
  auto nonminimal = raw;
  nonminimal.replace(nonminimal.find("AH//"), 4, "AB==");
  require_invalid(nonminimal, f);
  require_invalid(inject(raw, "\"request\":{", "\"\\u0062ody_b64\":\"\","), f);
  require_invalid("[]", f);
  require_invalid("{}", f);
  require_invalid(raw + raw, f);
  // These violate the closed schema, so no valid depth8/node1024 boundary is
  // fabricated. The global ceilings do not grant an unknown extension bag.
  std::string deep(12, '[');
  deep += "0";
  deep.append(12, ']');
  require_invalid(deep, f);
  std::string nodes = "0";
  for (unsigned i = 0; i < 1024; ++i)
    nodes += ",0";
  require_invalid(inject(raw, "{", "\"extra\":[" + nodes + "],"), f);
}

TEST_CASE(
    "HTTP Frame1 encoded metadata uses a total payload whitespace ceiling",
    "[extension-http][http_frames]") {
  Fixture f;
  auto raw = canonical(Accepted{identity()}, f);
  raw.insert(1, MAX_ENCODED_METADATA_BYTES - raw.size(), ' ');
  REQUIRE(raw.size() == MAX_ENCODED_METADATA_BYTES);
  auto accepted = parse_frame_payload(raw, f.lease);
  REQUIRE(accepted);
  raw.insert(1, 1, ' ');
  auto rejected = parse_frame_payload(raw, f.lease);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error() == FrameError::TOO_LARGE);
  auto huge = std::string(MAX_WIRE_BYTES + 1, ' ');
  auto oversized = parse_frame_payload(huge, f.lease);
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error() == FrameError::TOO_LARGE);
}

TEST_CASE("HTTP Frame1 maximal legal call metadata is counted across all "
          "nested values",
          "[extension-http][http_frames]") {
  Fixture f;
  auto source = call();
  auto& r = source.request.data;
  auto& c = source.context;
  const auto prefix = std::string{"/"} + std::string(253, 'x');
  c.matched_prefix = prefix + "/*";
  r.target.path = prefix + "/" + std::string(8192 - prefix.size() - 1, 'y');
  r.target.original_path = r.target.path;
  r.target.query = std::string(8192, 'q');
  r.method = std::string(32, 'X');
  r.headers = Headers(4, {"x", std::string(8191, 'a')});
  c.principal = {Principal::Kind::PAT, USER, PAT};
  c.extension.name = std::string(64, 'a');
  c.extension.version = "1.0.0+" + std::string(250, 'a');
  c.admitted_at_ns = std::numeric_limits<std::uint64_t>::max() - 1;
  c.execution_cutoff_ns = std::numeric_limits<std::uint64_t>::max();
  REQUIRE(encode_frame(source, f.lease));
  const auto raw = canonical(source, f);
  auto parsed = parse_frame_payload(raw, f.lease);
  REQUIRE(parsed);
  REQUIRE(parsed->value() == Frame{source});
  // Conservative independent maxima: envelope148 +Request57412 +Context971.
  // 58531<65536, so there is no individually legal Request+Context overflow
  // positive. Adversarial extra string fields still fail their closed role.
  REQUIRE(148 + 57412 + 971 == 58531);
  REQUIRE(58531 < MAX_METADATA_BYTES);
  auto illegal = inject(
      raw, "{", "\"extra\":\"" + std::string(MAX_METADATA_BYTES, 'z') + "\",");
  REQUIRE_FALSE(parse_frame_payload(illegal, f.lease));
}

TEST_CASE("HTTP Frame1 eight MiB binary and escaped body key roundtrip without "
          "quota reset",
          "[extension-http][http_frames]") {
  Fixture f;
  std::string raw;
  {
    NativeResponse response{200, {}, Binary(MAX_BODY_BYTES)};
    for (std::size_t i = 0; i < response.body.size(); ++i)
      response.body[i] = static_cast<std::uint8_t>(i % 251);
    Frame source = Result{identity(), std::move(response)};
    auto encoded = encode_frame(source, f.lease);
    REQUIRE(encoded);
    raw = payload(*encoded);
    // Source DTO and encoded buffer leave scope before parser workspace.
  }
  const auto position = raw.find("\"body_b64\"");
  REQUIRE(position != std::string::npos);
  {
    auto editing = f.lease->reserve_extra(MAX_WIRE_BYTES);
    REQUIRE(editing);
    std::string escaped;
    escaped.reserve(raw.size() + 5);
    escaped.append(raw, 0, position);
    escaped += "\"\\u0062ody_b64\"";
    escaped.append(raw, position + 10, std::string::npos);
    raw.swap(escaped);
  }
  {
    auto parsed = parse_frame_payload(raw, f.lease);
    REQUIRE(parsed);
    const auto& body =
        std::get<NativeResponse>(std::get<Result>(parsed->value()).outcome)
            .body;
    REQUIRE(body.size() == MAX_BODY_BYTES);
    REQUIRE(body.front() == 0);
    REQUIRE(body[127] == 127);
    REQUIRE(body.back() ==
            static_cast<std::uint8_t>((MAX_BODY_BYTES - 1) % 251));
    for (std::size_t i = 0; i < body.size(); ++i)
      if (body[i] != static_cast<std::uint8_t>(i % 251))
        FAIL("binary byte association changed");
  }
  // Body is exempted once; escaped key spelling still counts as metadata.
  auto editing = f.lease->reserve_extra(MAX_WIRE_BYTES);
  REQUIRE(editing);
  {
    constexpr std::string_view extra = "\"body_b64\":\"\",";
    const auto object = raw.find("\"response\":{");
    REQUIRE(object != std::string::npos);
    const auto insertion = object + 12;
    std::string duplicate;
    duplicate.reserve(raw.size() + extra.size());
    duplicate.append(raw, 0, insertion);
    duplicate += extra;
    duplicate.append(raw, insertion, std::string::npos);
    auto rejected = parse_frame_payload(duplicate, f.lease);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error() == FrameError::INVALID);
  }
  {
    auto oversized = raw;
    const auto padding = oversized.rfind('=');
    REQUIRE(padding != std::string::npos);
    oversized[padding] = 'A';
    // Same encoded length, one extra decoded byte; body-size validation must
    // not rely on the lexical base64 ceiling alone.
    auto rejected = parse_frame_payload(oversized, f.lease);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error() == FrameError::TOO_LARGE);
  }
}

TEST_CASE("HTTP Frame1 body plus one and malformed canonical base64 reject",
          "[extension-http][http_frames]") {
  Fixture f;
  Frame huge =
      Result{identity(), NativeResponse{200, {}, Binary(MAX_BODY_BYTES + 1)}};
  auto rejected = encode_frame(huge, f.lease);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error() == FrameError::TOO_LARGE);
  const auto raw =
      canonical(Result{identity(), NativeResponse{200, {}, {0}}}, f);
  for (auto token : {"AB==", "AA=", "AA===", "A A=", "AA__", "AA==\\n"}) {
    auto bad = raw;
    bad.replace(bad.find("AA=="), 4, token);
    require_invalid(bad, f);
  }
}

TEST_CASE("HTTP Frame1 BE lengths are checked before allocation and complete "
          "buffers are exact",
          "[extension-http][http_frames]") {
  Fixture f;
  const auto before = f.lease->extra_used();
  REQUIRE(*decode_frame_length({0, 1, 2, 3}) == 66051);
  REQUIRE_FALSE(decode_frame_length(header(0)));
  REQUIRE(*decode_frame_length(header(1)) == 1);
  REQUIRE(*decode_frame_length(header(MAX_WIRE_BYTES)) == MAX_WIRE_BYTES);
  REQUIRE_FALSE(decode_frame_length(header(MAX_WIRE_BYTES + 1)));
  REQUIRE_FALSE(
      decode_frame_length(header(std::numeric_limits<std::uint32_t>::max())));
  REQUIRE_FALSE(allocate_frame_payload(header(0), f.lease));
  REQUIRE_FALSE(allocate_frame_payload(header(MAX_WIRE_BYTES + 1), f.lease));
  REQUIRE(f.lease->extra_used() == before);
  {
    auto maximum = allocate_frame_payload(header(MAX_WIRE_BYTES), f.lease);
    REQUIRE(maximum);
    REQUIRE(maximum->bytes().size() == MAX_WIRE_BYTES);
  }
  REQUIRE(f.lease->extra_used() == before);
  auto encoded = encode_frame(Accepted{identity()}, f.lease);
  REQUIRE(encoded);
  auto bytes = encoded->bytes();
  for (std::size_t n : {0U, 1U, 2U, 3U})
    REQUIRE_FALSE(parse_framed_buffer(bytes.first(n), f.lease));
  REQUIRE_FALSE(parse_framed_buffer(bytes.first(bytes.size() - 1), f.lease));
  Binary trailing(bytes.begin(), bytes.end());
  trailing.push_back(0);
  REQUIRE_FALSE(parse_framed_buffer(trailing, f.lease));
  auto buffer =
      allocate_frame_payload({bytes[0], bytes[1], bytes[2], bytes[3]}, f.lease);
  REQUIRE(buffer);
  std::ranges::copy(bytes.subspan(4), buffer->writable().begin());
  auto received = parse_frame_payload(
      {reinterpret_cast<const char*>(buffer->bytes().data()),
       buffer->bytes().size()},
      f.lease);
  REQUIRE(received);
  REQUIRE(received->value() == Frame{Accepted{identity()}});
}

TEST_CASE("HTTP Frame1 admission and partial reservation failure restore exact "
          "credits",
          "[extension-http][http_frames]") {
  Fixture f;
  const auto raw = canonical(Accepted{identity()}, f);
  REQUIRE_FALSE(parse_frame_payload(raw, {}));
  REQUIRE_FALSE(encode_frame(Accepted{identity()}, {}));
  REQUIRE_FALSE(allocate_frame_payload(header(1), {}));
  const auto work = 3 * raw.size() + std::size_t{12} * 1024 * 1024 +
                    sizeof(ExtraMemoryCharge);
  auto held =
      f.lease->reserve_extra(EXTRA_BYTES_PER_LEASE - f.lease->extra_used() -
                             work - 512 - sizeof(ExtraMemoryCharge));
  REQUIRE(held);
  const auto before = f.lease->extra_used();
  auto failure = parse_frame_payload(raw, f.lease);
  REQUIRE_FALSE(failure);
  REQUIRE(failure.error() == FrameError::RESOURCE_UNAVAILABLE);
  REQUIRE(f.lease->extra_used() == before);
  auto exhausted =
      f.lease->reserve_extra(work + 512 - sizeof(ExtraMemoryCharge));
  REQUIRE(exhausted);
  auto payload_failure = allocate_frame_payload(header(1), f.lease);
  REQUIRE_FALSE(payload_failure);
  REQUIRE(payload_failure.error() == FrameError::RESOURCE_UNAVAILABLE);
  auto output_failure = encode_frame(Accepted{identity()}, f.lease);
  REQUIRE_FALSE(output_failure);
  REQUIRE(output_failure.error() == FrameError::RESOURCE_UNAVAILABLE);
}

TEST_CASE("HTTP Frame1 returned buffers and DTOs retain process credits "
          "through destruction",
          "[extension-http][http_frames]") {
  Fixture f;
  SECTION("payload owner") {
    auto buffer = allocate_frame_payload(header(1), f.lease);
    REQUIRE(buffer);
    f.release();
    REQUIRE(f.pool.snapshot().tasks == 1);
    REQUIRE(f.process.active_for_test() == 1);
    REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
    {
      auto moved = std::move(*buffer);
      REQUIRE(moved.bytes().size() == 1);
    }
  }
  SECTION("parsed owner") {
    auto parsed =
        parse_frame_payload(canonical(Accepted{identity()}, f), f.lease);
    REQUIRE(parsed);
    f.release();
    REQUIRE(f.pool.snapshot().tasks == 1);
    REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
    {
      auto moved = std::move(*parsed);
      REQUIRE(std::holds_alternative<Accepted>(moved.value()));
    }
  }
  SECTION("encoded owner") {
    auto encoded = encode_frame(Accepted{identity()}, f.lease);
    REQUIRE(encoded);
    f.release();
    REQUIRE(f.pool.snapshot().tasks == 1);
    REQUIRE_FALSE(f.pool.drain(std::chrono::steady_clock::now()));
    {
      auto moved = std::move(*encoded);
      REQUIRE(moved.bytes().size() > 4);
    }
  }
  REQUIRE(f.pool.drain(std::chrono::steady_clock::now()));
  REQUIRE(f.process.active_for_test() == 0);
}
