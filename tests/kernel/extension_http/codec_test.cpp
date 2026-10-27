#include "kernel/extension_http/codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace {
using namespace plinth::extension_http;
using Json = nlohmann::ordered_json;
constexpr auto ID = "01234567-89ab-cdef-0123-456789abcdef";
constexpr auto OTHER = "11234567-89ab-cdef-0123-456789abcdef";

template <typename T>
auto require_too_large(const CodecResult<T>& result) -> void {
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error() == CodecError::TOO_LARGE);
}

auto request() -> NativeRequest {
  return {{ID,
           "POST",
           {"/dav/file%3Fname", "/dav/file?name", "a=%FF&b=%00&c=%23&d=+?"},
           {{"content-type", "application/octet-stream"},
            {"x-note", "café 🪐"},
            {"x-note", "two"}}},
          {0, 255, 128, 13, 10}};
}
auto context() -> ContextData {
  return {ID,
          OTHER,
          ID,
          OTHER,
          100,
          4500000100ULL,
          "/dav/*",
          {},
          {ID, "notes", "1.2.3-beta.2+build.001", std::string(64, 'a'), OTHER}};
}
auto response() -> NativeResponse {
  return {207,
          {{"content-type", "application/octet-stream"},
           {"x-note", "first"},
           {"x-note", "second"}},
          {0, 255, 1}};
}
auto response_json() -> std::string {
  return R"({"schema":1,"status":207,"headers":[],"body_b64":"AP8B"})";
}
} // namespace

TEST_CASE("HTTP codec scalar validators retain literal bounds and types",
          "[extension_http][codec]") {
  REQUIRE(valid_uuid(ID));
  REQUIRE_FALSE(valid_uuid("01234567-89AB-cdef-0123-456789abcdef"));
  REQUIRE_FALSE(valid_uuid("0123456789abcdef0123456789abcdef"));
  REQUIRE(valid_hex256(std::string(64, 'f')));
  REQUIRE_FALSE(valid_hex256(std::string(64, 'F')));
  REQUIRE_FALSE(valid_hex256(std::string(63, 'a')));
  REQUIRE(parse_ns("0") == 0);
  REQUIRE(parse_ns("18446744073709551615") ==
          std::numeric_limits<std::uint64_t>::max());
  for (auto bad : {"", "00", "01", "+1", "-1", "1.0", "1e3", " 1",
                   "18446744073709551616"}) {
    INFO(bad);
    REQUIRE_FALSE(parse_ns(bad));
  }
  REQUIRE(valid_name("a0"));
  REQUIRE(valid_name(std::string(64, 'a')));
  for (auto bad : {"a", "A0", "0a", "a_b", "a.b"})
    REQUIRE_FALSE(valid_name(bad));
  REQUIRE_FALSE(valid_name(std::string(65, 'a')));
  for (auto good : {"0.0.0", "1.2.3-0", "1.2.3-beta.2+build.001"})
    REQUIRE(valid_version(good));
  REQUIRE(valid_version("1.2.3+" + std::string(250, 'x')));
  REQUIRE_FALSE(valid_version("1.2.3+" + std::string(251, 'x')));
  for (auto bad : {"1.2", "01.2.3", "1.02.3", "1.2.03", "1.2.3-01", "1.2.3-",
                   "1.2.3+", "1.2.3+a..b"})
    REQUIRE_FALSE(valid_version(bad));
  for (auto good : {"GET", "PROPFIND", "M-SEARCH", "X!9"})
    REQUIRE(valid_method(good));
  REQUIRE(valid_method(std::string(32, 'A')));
  REQUIRE_FALSE(valid_method(std::string(33, 'A')));
  for (auto bad : {"", "get", "Get", "9GET", "GET ", "GET/"})
    REQUIRE_FALSE(valid_method(bad));
  REQUIRE(valid_claim("/dav/*"));
  REQUIRE(valid_claim("/status.php"));
  REQUIRE(valid_claim("/" + std::string(255, 'a')));
  REQUIRE_FALSE(valid_claim("/" + std::string(256, 'a')));
  for (auto bad : {"/", "/*", "/dav/", "/dav//a", "/dav/../a", "/dav/*/a",
                   "/%64av", "/dav?x", "/dav#x"})
    REQUIRE_FALSE(valid_claim(bad));
}

TEST_CASE(
    "HTTP codec validates Unicode scalars without interpreting binary bytes",
    "[extension_http][codec]") {
  REQUIRE(valid_utf8("café 🪐"));
  REQUIRE(valid_utf8(std::string_view{"a\0b", 3}));
  for (const std::string& bad :
       {std::string{"\xc0\xaf"}, std::string{"\xed\xa0\x80"},
        std::string{"\xf4\x90\x80\x80"}, std::string{"\xe2\x82"},
        std::string{"\x80"}}) {
    REQUIRE_FALSE(valid_utf8(bad));
  }
  REQUIRE(decode_body64("AP+A") == Binary{0, 255, 128});
}

TEST_CASE("HTTP codec keeps captured path and opaque query separate",
          "[extension_http][codec]") {
  for (auto query :
       {"", "a=%FF", "a=%00", "a=%23", "a=+", "a=?later?", "a=1&a=2"}) {
    INFO(query);
    REQUIRE(validate_target("/x", "/x", query));
  }
  // /x and /x? intentionally arrive as these identical three values. There is
  // no fictitious raw-target accessor/delimiter bit in the pure interface.
  const Target absent{"/x", "/x", ""};
  const Target empty_query{"/x", "/x", ""};
  REQUIRE(absent == empty_query);
  for (const auto& pair : std::array<std::pair<std::string, std::string>, 7>{
           {{"/dav/a%20b", "/dav/a b"},
            {"/dav/a%3fb", "/dav/a?b"},
            {"/dav/a%25b", "/dav/a%b"},
            {"/dav/caf%C3%A9", "/dav/café"},
            {"/dav/%F0%9F%AA%90", "/dav/🪐"},
            {"/dav/", "/dav/"},
            {"/dav/literal", "/dav/literal"}}}) {
    INFO(pair.first);
    REQUIRE(validate_target(pair.first, pair.second, "?a=%FF+"));
  }
  // No encoded ASCII aliases, second decode, or decoded '?' routing pass.
  REQUIRE_FALSE(validate_target("/%64av/x", "/dav/x", ""));
  REQUIRE_FALSE(validate_target("/dav/%252f", "/dav//", ""));
  REQUIRE(validate_target("/dav/%252f", "/dav/%2f", ""));
  REQUIRE_FALSE(validate_target("/dav/x", "/dav/y", ""));
  REQUIRE_FALSE(validate_target("/dav/x%3Fy", "/dav/x", "y"));
  REQUIRE_FALSE(validate_target("/dav/x?y", "/dav/x?y", ""));
  for (auto query : {"a=%", "a=%0", "a=%GG", "a= ", "a=#", "a=é", "a=\t"})
    REQUIRE_FALSE(validate_target("/x", "/x", query));
  REQUIRE(validate_target("/" + std::string(8191, 'a'),
                          "/" + std::string(8191, 'a'),
                          std::string(8192, 'a')));
  require_too_large(validate_target("/x", "/x", std::string(8193, 'a')));
}

TEST_CASE("HTTP codec rejects malformed original and decoded paths together",
          "[extension_http][codec]") {
  for (auto path : {"", "x", "//x", "/x//y", "/x/./y", "/x/../y", "/x/.",
                    "/x/..", "/x\\y", "/x#y"}) {
    INFO(path);
    REQUIRE_FALSE(validate_target(path, path, ""));
  }
  for (const auto& pair : std::array<std::pair<std::string, std::string>, 11>{
           {{"/x%2fy", "/x/y"},
            {"/x%5cy", "/x\\y"},
            {"/x%00y", std::string{"/x\0y", 4}},
            {"/x%0ay", "/x\ny"},
            {"/x%7fy", "/x\x7fy"},
            {"/x%23y", "/x#y"},
            {"/x%GG", "/x%GG"},
            {"/x%", "/x%"},
            {"/%C0%AF", "/\xc0\xaf"},
            {"/%ED%A0%80", "/\xed\xa0\x80"},
            {"/%E2%82", "/\xe2\x82"}}}) {
    INFO(pair.first);
    REQUIRE_FALSE(validate_target(pair.first, pair.second, ""));
  }
  REQUIRE(validate_target("/", "/", ""));
}

TEST_CASE("HTTP codec filters only validated request header tuples",
          "[extension_http][codec]") {
  const Headers input{{"x-note", "first"},
                      {"connection", " X-Private, keep-alive"},
                      {"x-private", "omit"},
                      {"cookie", "fake=fixture"},
                      {"authorization", "Bearer fake"},
                      {"proxy-authorization", "fake"},
                      {"x-plinth-csrf", "fake"},
                      {"host", "example.test"},
                      {"forwarded", "for=example"},
                      {"x-forwarded-for", "example"},
                      {"te", "trailers"},
                      {"upgrade", "websocket"},
                      {"x-note", "second"}};
  auto filtered = filter_request_headers(input);
  REQUIRE(filtered);
  REQUIRE(*filtered == Headers{{"x-note", "first"}, {"x-note", "second"}});
  auto r = request();
  r.data.headers = input;
  REQUIRE_FALSE(
      validate_request(r)); // Wire DTOs cannot smuggle intake headers.
  r.data.headers = *filtered;
  REQUIRE(validate_request(r));
  for (auto value : {"", "x-note,", ",x-note", "x note", "x:note"})
    REQUIRE_FALSE(filter_request_headers({{"connection", value}}));
  for (auto name : {"Content-Type", "x space", "x:bad", "", "é"})
    REQUIRE_FALSE(validate_headers({{name, "ok"}}));
  for (const auto& value :
       {std::string{"a\rb"}, std::string{"a\nb"}, std::string{"a\0b", 3},
        std::string{"\x7f"}, std::string{"\xff"}})
    REQUIRE_FALSE(validate_headers({{"x-note", value}}));
  REQUIRE(validate_headers({{"x-note", "tab\tand Unicode café 🪐"}}));
}

TEST_CASE(
    "HTTP codec header boundaries and response forbidden fields are exact",
    "[extension_http][codec]") {
  REQUIRE(validate_headers(Headers(64, {"x", ""})));
  require_too_large(validate_headers(Headers(65, {"x", ""})));
  REQUIRE(validate_headers({{std::string(128, 'x'), ""}}));
  require_too_large(validate_headers({{std::string(129, 'x'), ""}}));
  REQUIRE(validate_headers({{"x", std::string(8192, 'a')}}));
  require_too_large(validate_headers({{"x", std::string(8193, 'a')}}));
  Headers aggregate(4, {"x", std::string(8191, 'a')});
  REQUIRE(validate_headers(aggregate)); // Exactly32768 name+value bytes.
  aggregate.back().second.push_back('a');
  require_too_large(validate_headers(aggregate));
  for (auto name :
       {"set-cookie", "cookie", "authorization", "www-authenticate",
        "proxy-authenticate", "proxy-authorization", "authentication-info",
        "proxy-authentication-info", "connection", "keep-alive",
        "proxy-connection", "content-length", "transfer-encoding", "trailer",
        "te", "upgrade", "access-control-allow-origin",
        "access-control-allow-credentials"}) {
    INFO(name);
    auto r = response();
    r.headers = {{name, "fixture"}};
    REQUIRE_FALSE(validate_response(r));
  }
  auto r = response();
  r.status = 204;
  REQUIRE(validate_response(r)); // Shape precedes later bodyless serialization.
  r.status = 599;
  REQUIRE(validate_response(r));
  for (int status : {0, 101, 199, 600}) {
    r.status = status;
    REQUIRE_FALSE(validate_response(r));
  }
}

TEST_CASE("HTTP codec Body64 canonical padding retains arbitrary binary",
          "[extension_http][codec]") {
  const std::array<std::pair<Binary, std::string>, 5> fixtures{
      {{{}, ""},
       {{0}, "AA=="},
       {{255}, "/w=="},
       {{0, 255}, "AP8="},
       {{0, 255, 128}, "AP+A"}}};
  for (const auto& [bytes, encoded] : fixtures) {
    REQUIRE(encode_body64(bytes) == encoded);
    REQUIRE(decode_body64(encoded) == bytes);
  }
  for (auto invalid : {"A", "AA", "AAA", "AA===", "====", "AA=A",
                       "AB==", "AAB=", "AA==\n", " AA==", "AA-_", "AA==AAAA"}) {
    INFO(invalid);
    REQUIRE_FALSE(decode_body64(invalid));
  }
}

TEST_CASE("HTTP codec wire DTOs are closed canonical values not authority",
          "[extension_http][codec]") {
  auto req = request();
  auto raw = serialize_wire_request(req);
  REQUIRE(raw);
  REQUIRE(raw->starts_with(
      R"({"schema":1,"request_id":"01234567-89ab-cdef-0123-456789abcdef","method":"POST","path":)"));
  REQUIRE(raw->find("café 🪐") != std::string::npos);
  REQUIRE(raw->ends_with(R"("body_b64":"AP+ADQo="})"));
  auto parsed = parse_wire_request(*raw);
  REQUIRE(parsed);
  REQUIRE(*parsed == req);
  REQUIRE(parse_wire_response(response_json()) ==
          NativeResponse{207, {}, {0, 255, 1}});
  auto res = response();
  auto res_raw = serialize_wire_response(res);
  REQUIRE(res_raw);
  REQUIRE(parse_wire_response(*res_raw) == res);
  // Receiver member order is not semantic order; arrays/repeated headers are.
  auto j = Json::parse(*raw);
  auto reordered = Json::object();
  for (auto it = j.rbegin(); it != j.rend(); ++it)
    reordered[it.key()] = it.value();
  REQUIRE(parse_wire_request(reordered.dump()) == req);
  for (const auto& key : {"schema", "request_id", "method", "path",
                          "original_path", "query", "headers", "body_b64"}) {
    auto missing = j;
    missing.erase(key);
    REQUIRE_FALSE(parse_wire_request(missing.dump()));
  }
  auto unknown = j;
  unknown["permission"] = true;
  REQUIRE_FALSE(parse_wire_request(unknown.dump()));
  for (const auto& value : {Json(true), Json(1.0), Json("1"), Json(nullptr)}) {
    auto bad = j;
    bad["schema"] = value;
    REQUIRE_FALSE(parse_wire_request(bad.dump()));
  }
  auto bad_header = j;
  bad_header["headers"] = Json::array({Json::array({"x", "v", "extra"})});
  REQUIRE_FALSE(parse_wire_request(bad_header.dump()));
  for (const auto& value :
       {Json(true), Json(207.0), Json("207"), Json(101), Json(600)}) {
    auto bad = Json::parse(response_json());
    bad["status"] = value;
    REQUIRE_FALSE(parse_wire_response(bad.dump()));
  }
}

TEST_CASE("HTTP codec rejects raw duplicates unknown keys and escaped Body64 "
          "before DOM loss",
          "[extension_http][codec]") {
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"status":200,"headers":[],"body_b64":""})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"st\u0061tus":200,"headers":[],"body_b64":""})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[],"body_b64":"","unknown":{}})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[],"body_b64":"","body_b64":"AA=="})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[],"body_b64":"\u0041A=="})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[],"body_b64":"\/w=="})"));
  REQUIRE(parse_wire_response(
      R"({"sch\u0065ma":1,"status":207,"headers":[],"body_b64":"AA=="})"));
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[["x","\ud800"]],"body_b64":""})"));
  auto invalid_utf8 = response_json();
  invalid_utf8.insert(1, "\xff");
  REQUIRE_FALSE(parse_wire_response(invalid_utf8));
  REQUIRE_FALSE(parse_wire_response(response_json() + "{}"));
  REQUIRE_FALSE(parse_wire_response("[" + response_json() + "]"));
  // Unknown nested keys fail before DOM; deeply nested arrays are outside the
  // closed Headers grammar, not permission to invent an extension bag.
  REQUIRE_FALSE(parse_wire_response(
      R"({"schema":1,"status":207,"headers":[[[[[[[[[[]]]]]]]]]],"body_b64":""})"));
  std::string many = R"({"schema":1,"status":207,"headers":[)";
  for (int i = 0; i < 400; ++i) {
    if (i != 0) many += ',';
    many += R"(["x",""])";
  }
  many += R"(],"body_b64":""})";
  require_too_large(parse_wire_response(many));
  auto metadata = Json::parse(response_json());
  for (int i = 0; i < 9; ++i)
    metadata["headers"].push_back(Json::array({"x", std::string(8192, 'a')}));
  require_too_large(parse_wire_response(metadata.dump()));
  auto exact = response_json();
  exact.append(MAX_ENCODED_METADATA_BYTES - exact.size() + 4, ' ');
  // Four body alphabet bytes are excluded from the encoded metadata budget.
  REQUIRE(parse_wire_response(exact));
  exact.push_back(' ');
  require_too_large(parse_wire_response(exact));
  require_too_large(parse_wire_response(std::string(MAX_WIRE_BYTES + 1, ' ')));
}

TEST_CASE(
    "HTTP codec context uses exact scalar unions and captured value equality",
    "[extension_http][codec]") {
  auto c = context();
  auto raw = serialize_context(c);
  REQUIRE(raw);
  REQUIRE(parse_context(*raw) == c);
  REQUIRE(validate_call_data(request(), c));
  c.principal = {Principal::Kind::PAT, ID, OTHER};
  raw = serialize_context(c);
  REQUIRE(raw);
  REQUIRE(parse_context(*raw) == c);
  auto j = Json::parse(*raw);
  auto wrong = j;
  wrong["principal"]["kind"] = "anonymous";
  REQUIRE_FALSE(parse_context(wrong.dump()));
  wrong = j;
  wrong["principal"]["permission"] = "kernel.admin";
  REQUIRE_FALSE(parse_context(wrong.dump()));
  wrong = j;
  wrong["extension"]["validated"] = true;
  REQUIRE_FALSE(parse_context(wrong.dump()));
  for (auto field : {"admitted_at_ns", "execution_cutoff_ns"}) {
    wrong = j;
    wrong[field] = 100;
    REQUIRE_FALSE(parse_context(wrong.dump()));
    wrong[field] = "0100";
    REQUIRE_FALSE(parse_context(wrong.dump()));
  }
  for (auto field : {"schema", "dispatch_id", "owner_epoch", "adapter_epoch",
                     "clock_domain_id", "admitted_at_ns", "execution_cutoff_ns",
                     "matched_prefix", "principal", "extension"}) {
    wrong = j;
    wrong.erase(field);
    REQUIRE_FALSE(parse_context(wrong.dump()));
  }
  auto duplicate = *raw;
  const auto start = duplicate.find(R"("kind":"pat")");
  REQUIRE(start != std::string::npos);
  duplicate.insert(start, R"("kind":"anonymous",)");
  REQUIRE_FALSE(parse_context(duplicate));
  c.execution_cutoff_ns = c.admitted_at_ns;
  REQUIRE(validate_context(
      c)); // Equal times are valid data, already expired at entry.
  --c.execution_cutoff_ns;
  REQUIRE_FALSE(validate_context(c));
  c = context();
  c.dispatch_id = OTHER;
  REQUIRE_FALSE(validate_call_data(request(), c));
  c = context();
  c.matched_prefix = "/davx/*";
  REQUIRE_FALSE(validate_call_data(request(), c));
  c = context();
  c.principal.user_id = ID;
  REQUIRE_FALSE(validate_context(c));
  // No pure DTO assertion can attest a principal, epoch, clock or receipt.
}

TEST_CASE("HTTP codec preserves the exact eight MiB binary boundary",
          "[extension_http][codec]") {
  Binary body(MAX_BODY_BYTES);
  for (std::size_t i = 0; i < body.size(); ++i)
    body[i] = static_cast<std::uint8_t>(i % 256);
  {
    auto encoded = encode_body64(body);
    REQUIRE(encoded);
    REQUIRE(encoded->size() == 11184812);
    auto decoded = decode_body64(*encoded);
    REQUIRE(decoded);
    REQUIRE(std::ranges::equal(*decoded, body));
    REQUIRE(encode_body64(*decoded) == *encoded);
  }
  NativeResponse r{200, {}, std::move(body)};
  {
    auto raw = serialize_wire_response(r);
    REQUIRE(raw);
    REQUIRE(raw->size() < MAX_WIRE_BYTES);
    auto parsed = parse_wire_response(*raw);
    REQUIRE(parsed);
    REQUIRE(std::ranges::equal(parsed->body, r.body));
  }
  auto req = request();
  req.body = std::move(r.body);
  {
    auto raw = serialize_wire_request(req);
    REQUIRE(raw);
    auto parsed = parse_wire_request(*raw);
    REQUIRE(parsed);
    REQUIRE(std::ranges::equal(parsed->body, req.body));
    REQUIRE(parsed->data == req.data);
  }
  req.body.push_back(0);
  require_too_large(encode_body64(req.body));
  require_too_large(validate_request(req));
  require_too_large(serialize_wire_request(req));
  r.body = std::move(req.body);
  require_too_large(serialize_wire_response(r));
}
