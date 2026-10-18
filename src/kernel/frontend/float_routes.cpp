#include "kernel/frontend/float_routes.hpp"

#include "kernel/auth/middleware.hpp"
#include "kernel/browser_origin.hpp"
#include "kernel/frontend/admin_defaults_contract.hpp"
#include "kernel/frontend/float_layout_contract.hpp"
#include "kernel/frontend/float_layout_service.hpp"
#include "kernel/frontend/surface_protocol.hpp"
#include "kernel/frontend/typed_requests.hpp"
#include "kernel/lifecycle/async_task_registry.hpp"
#include "kernel/packages/detail/launcher_validation.hpp"
#include "kernel/shell/active_frontend.hpp"
#include "kernel/shell/platform_graph.hpp"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/utils/HttpConstraint.h>
#include <json/writer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace plinth::frontend {
namespace {
using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
enum class Route {
  CATALOG,
  ADMISSION,
  READ,
  COMMIT,
  TYPED,
  PLATFORM,
  ADMIN_READ,
  ADMIN_PUT,
  ADMIN_DELETE
};
struct Definition {
  Route route;
  std::string_view path;
  bool regex;
};
constexpr std::array DEFINITIONS{
    Definition{Route::CATALOG, "/api/frontend/surfaces", false},
    Definition{Route::ADMISSION, "/api/frontend/surfaces/admission", false},
    Definition{Route::READ, "/api/frontend/float-layout", false},
    Definition{Route::COMMIT, "/api/frontend/float-layout", false},
    Definition{Route::TYPED, "^/_plinth/typed/.*$", true},
    Definition{Route::PLATFORM, "^/_plinth/platform/.*$", true},
    Definition{Route::ADMIN_READ, "/api/frontend/handler-defaults/admin",
               false},
    Definition{Route::ADMIN_PUT, "/api/frontend/handler-defaults/admin", false},
    Definition{Route::ADMIN_DELETE, "/api/frontend/handler-defaults/admin",
               false}};

auto asset(Route route) -> bool {
  return route == Route::TYPED || route == Route::PLATFORM;
}
auto mutation(Route route) -> bool {
  return route == Route::ADMISSION || route == Route::COMMIT ||
         route == Route::ADMIN_PUT || route == Route::ADMIN_DELETE;
}
auto admin(Route route) -> bool {
  return route == Route::ADMIN_READ || route == Route::ADMIN_PUT ||
         route == Route::ADMIN_DELETE;
}
auto maximum(Route route) -> std::size_t {
  if (admin(route)) return admin_defaults::MAX_RESPONSE_BYTES;
  return route == Route::READ     ? 131072
         : route == Route::COMMIT ? 2048
                                  : 1048576;
}
auto constraints(Route route) -> std::vector<drogon::internal::HttpConstraint> {
  if (route == Route::PLATFORM) return {drogon::Get, drogon::Head};
  if (route == Route::TYPED)
    return {drogon::Get, drogon::Head, "plinth::auth::SessionFilter"};
  if (route == Route::ADMISSION)
    return {drogon::Post, "plinth::auth::SessionFilter",
            "plinth::auth::CsrfFilter"};
  if (route == Route::COMMIT || route == Route::ADMIN_PUT)
    return {drogon::Put, "plinth::auth::SessionFilter",
            "plinth::auth::CsrfFilter"};
  if (route == Route::ADMIN_DELETE)
    return {drogon::Delete, "plinth::auth::SessionFilter",
            "plinth::auth::CsrfFilter"};
  return {drogon::Get, "plinth::auth::SessionFilter"};
}
auto harden(const drogon::HttpResponsePtr& response, bool changes) -> void {
  response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Vary", changes ? "Origin, Cookie, Authorization"
                                      : "Cookie, Authorization");
  response->addHeader("X-Content-Type-Options", "nosniff");
}
auto admission_error(TypedRequestError error) -> std::string_view {
  switch (error) {
    case TypedRequestError::TOO_LARGE: return "body_too_large";
    case TypedRequestError::INVALID: return "invalid_argument";
    case TypedRequestError::UNAVAILABLE: return "storage_unavailable";
  }
  return "storage_unavailable";
}

auto commit_error(layout_contract::Error error) -> std::string_view {
  switch (error) {
    case layout_contract::Error::TOO_LARGE: return "body_too_large";
    case layout_contract::Error::INVALID:
    case layout_contract::Error::STRUCTURAL_LIMIT: return "invalid_argument";
    case layout_contract::Error::LIMIT_EXCEEDED: return "storage_unavailable";
  }
  return "storage_unavailable";
}

auto error_status(std::string_view code, bool is_asset)
    -> drogon::HttpStatusCode {
  if (code == "not_authenticated") return drogon::k401Unauthorized;
  if (is_asset) {
    if (code == "range_not_satisfiable")
      return drogon::k416RequestedRangeNotSatisfiable;
    if (code == "asset_not_found" || code == "permission_denied" ||
        code == "invalid_argument")
      return drogon::k404NotFound;
    return drogon::k503ServiceUnavailable;
  }
  if (code == "permission_denied") return drogon::k403Forbidden;
  if (code == "stale_binding") return drogon::k409Conflict;
  if (code == "invalid_argument") return drogon::k400BadRequest;
  if (code == "body_too_large" || code == "payload_too_large")
    return drogon::k413RequestEntityTooLarge;
  return drogon::k503ServiceUnavailable;
}
auto error_response(std::string_view code, bool is_asset, bool changes)
    -> drogon::HttpResponsePtr {
  const auto status = error_status(code, is_asset);
  const auto response = drogon::HttpResponse::newHttpResponse();
  response->setStatusCode(status);
  harden(response, changes);
  // No caller/server error strings, SQL state, package existence or private
  // policy data crosses this boundary. The service code is mapped, not echoed.
  switch (status) {
    case drogon::k400BadRequest:
      response->setBody(R"({"error":"invalid_request"})");
      break;
    case drogon::k401Unauthorized:
      response->setBody(R"({"error":"not_authenticated"})");
      break;
    case drogon::k403Forbidden:
      response->setBody(R"({"error":"permission_denied"})");
      break;
    case drogon::k404NotFound:
      response->setBody(R"({"error":"not_found"})");
      break;
    case drogon::k409Conflict:
      response->setBody(R"({"error":"stale_binding"})");
      break;
    case drogon::k413RequestEntityTooLarge:
      response->setBody(R"({"error":"body_too_large"})");
      break;
    case drogon::k416RequestedRangeNotSatisfiable:
      response->setBody(R"({"error":"range_not_satisfiable"})");
      break;
    default: response->setBody(R"({"error":"service_unavailable"})"); break;
  }
  return response;
}

auto decode_query_part(std::string_view raw) -> std::optional<std::string> {
  const auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::string value;
  value.reserve(raw.size());
  for (std::size_t index = 0; index < raw.size(); ++index) {
    if (raw[index] == '%') {
      if (index + 2 >= raw.size()) return std::nullopt;
      const int high = hex(raw[index + 1]), low = hex(raw[index + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      value.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else
      value.push_back(raw[index] == '+' ? ' ' : raw[index]);
  }
  if (value.find('\0') != std::string::npos ||
      !packages::detail::unicode_scalar_count(value))
    return std::nullopt;
  return value;
}
auto parse_query(std::string_view raw)
    -> std::optional<FloatLayoutService::CatalogQuery> {
  if (raw.empty() || raw.size() > 16384) return std::nullopt;
  std::map<std::string, std::string> fields;
  for (std::size_t begin = 0; begin <= raw.size();) {
    const auto amp = raw.find('&', begin);
    const auto item =
        raw.substr(begin, amp == std::string_view::npos ? amp : amp - begin);
    const auto equal = item.find('=');
    if (equal == std::string_view::npos || equal == 0 ||
        item.find('=', equal + 1) != std::string_view::npos)
      return std::nullopt;
    const auto key = decode_query_part(item.substr(0, equal)),
               value = decode_query_part(item.substr(equal + 1));
    if (!key || !value ||
        (key != "kind" && key != "limit" && key != "content_type" &&
         key != "cursor" && key != "include_admin_default") ||
        !fields.emplace(*key, *value).second || fields.size() > 5)
      return std::nullopt;
    if (amp == std::string_view::npos) break;
    begin = amp + 1;
  }
  const auto kind = fields.find("kind");
  if (kind == fields.end()) return std::nullopt;
  FloatLayoutService::CatalogQuery result;
  if (kind->second == "float")
    result.filters.kind = SurfaceKind::FLOAT;
  else if (kind->second == "primary")
    result.filters.kind = SurfaceKind::PRIMARY;
  else if (kind->second == "tray")
    result.filters.kind = SurfaceKind::TRAY;
  else
    return std::nullopt;
  if (const auto found = fields.find("limit"); found != fields.end()) {
    const auto& value = found->second;
    std::size_t limit = 0;
    const auto parsed =
        std::from_chars(value.data(), value.data() + value.size(), limit);
    if (value.empty() || value.front() == '0' || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || limit < 1 || limit > 64)
      return std::nullopt;
    result.filters.limit = limit;
  }
  if (const auto found = fields.find("content_type"); found != fields.end()) {
    if (result.filters.kind != SurfaceKind::FLOAT ||
        !is_canonical_content_type(found->second))
      return std::nullopt;
    result.filters.content_type = found->second;
  }
  if (const auto found = fields.find("include_admin_default");
      found != fields.end()) {
    if (found->second != "0" && found->second != "1") return std::nullopt;
    result.filters.include_admin_default = found->second == "1";
    if (result.filters.kind != SurfaceKind::FLOAT &&
        result.filters.include_admin_default)
      return std::nullopt;
    if (result.filters.include_admin_default && !result.filters.content_type)
      return std::nullopt;
  }
  if (const auto found = fields.find("cursor"); found != fields.end()) {
    if (found->second.empty() || found->second.size() > 4096)
      return std::nullopt;
    result.cursor = found->second;
  }
  return result;
}
auto trusted_origin(const drogon::HttpRequestPtr& request,
                    std::string_view configured) -> std::optional<std::string> {
  if (!configured.empty()) {
    if (configured.size() > 2048 || !valid_browser_origin(configured))
      return std::nullopt;
    return std::string{configured};
  }
  const auto& host = request->getHeader("Host");
  if (host.empty() || host.size() > 2040) return std::nullopt;
  auto origin =
      std::string{request->isOnSecureConnection() ? "https://" : "http://"} +
      host;
  if (!valid_browser_origin(origin)) return std::nullopt;
  return origin;
}
auto asset_selector(const drogon::HttpRequestPtr& request, bool platform)
    -> std::expected<protocol::AssetSelector, protocol::Error> {
  if (!request->query().empty())
    return std::unexpected(protocol::Error::INVALID);
  auto result = protocol::parse_asset_selector(request->getOriginalPath());
  if (!result || (result->kind == protocol::AssetKind::PLATFORM) != platform)
    return std::unexpected(protocol::Error::INVALID);
  return result;
}
auto admin_read_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::ReadRequest, TypedRequestError> {
  if (!body.empty()) return std::unexpected(TypedRequestError::INVALID);
  return admin_defaults::parse_read_query(query);
}
auto admin_put_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::PutRequest, TypedRequestError> {
  if (!query.empty()) return std::unexpected(TypedRequestError::INVALID);
  return admin_defaults::parse_put(body);
}
auto admin_delete_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::DeleteRequest, TypedRequestError> {
  if (!query.empty()) return std::unexpected(TypedRequestError::INVALID);
  return admin_defaults::parse_delete(body);
}

class Reply {
 public:
  Reply(Callback callback, std::shared_ptr<FloatLayoutService> service,
        std::shared_ptr<lifecycle::AsyncTaskLease> accepted, bool changes,
        bool is_asset, std::size_t cap)
      : callback(std::move(callback)), service(std::move(service)),
        accepted(std::move(accepted)), changes(changes), is_asset(is_asset),
        cap(cap), fallback(error_response({}, is_asset, changes)) {}
  auto send(const drogon::HttpResponsePtr& response) -> void {
    if (delivered.exchange(true)) return;
    // Mark BEFORE invoking callback. If it throws, service's completion
    // boundary can mark sticky delivery failure; neither this owner nor a catch
    // retries.
    callback(response);
  }
  auto fail(std::string_view code) -> void {
    if (delivered.load()) return;
    drogon::HttpResponsePtr response;
    try {
      response = error_response(code, is_asset, changes);
    } catch (...) {
      response = fallback;
    }
    send(response);
  }
  auto json(FloatLayoutService::Result result) -> void {
    if (delivered.load()) return;
    if (!result) {
      fail(result.error().code);
      return;
    }
    auto response = fallback;
    try {
      const auto platform = shell::current_platform();
      if (platform && !platform->profile().empty() && result->isObject()) {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        builder["emitUTF8"] = true;
        auto body = Json::writeString(builder, *result);
        if (body.size() <= cap) {
          response = drogon::HttpResponse::newHttpResponse();
          response->setStatusCode(drogon::k200OK);
          harden(response, changes);
          response->addHeader("X-Plinth-Platform-Profile", platform->profile());
          response->setBody(std::move(body));
        }
      }
    } catch (...) {
      response = fallback;
    }
    send(response);
  }
  auto binary(FloatLayoutService::AssetResult result) -> void {
    if (delivered.load()) return;
    if (!result || !*result) {
      fail(result ? "storage_unavailable" : result.error().code);
      return;
    }
    send(*result); // Actual stream serializer/physical owner stays untouched.
  }
  auto management(FloatLayoutService::AdminResult result) -> void {
    // Keep the complete success/ACK owner alive through serialization and the
    // one-shot callback return/unwind. Never obtain a later global profile to
    // label rows or cursors from the admitted frame.
    if (delivered.load()) return;
    if (!result) {
      fail(result.error().code);
      return;
    }
    auto response = fallback;
    try {
      if (result->body().isObject() && !result->profile().empty()) {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        builder["emitUTF8"] = true;
        auto body = Json::writeString(builder, result->body());
        if (body.size() <= cap) {
          response = drogon::HttpResponse::newHttpResponse();
          response->setStatusCode(drogon::k200OK);
          harden(response, changes);
          response->addHeader("X-Plinth-Platform-Profile",
                              std::string{result->profile()});
          response->setBody(std::move(body));
        }
      }
    } catch (...) {
      response = fallback;
    }
    // This final nonblocking check happens after potentially allocating body
    // serialization, immediately before handoff. The retained counted guard
    // excludes normal lifecycle cutover through callback return. Refusal can
    // follow committed DML; it is not rollback or safe retry permission.
    if (response != fallback) {
      std::expected<void, js::PromiseRejection> ready;
      try {
        ready = result->prepare_handoff();
      } catch (...) {
        response = fallback;
      }
      // Callback invocation stays OUTSIDE the exception boundary above. A
      // throwing callback is sticky delivery failure, not a serialization
      // exception to swallow or a reason to attempt a second response.
      if (response != fallback && !ready) {
        fail(ready.error().code);
        return;
      }
    }
    send(response);
  }

 private:
  Callback callback;
  std::shared_ptr<FloatLayoutService> service;
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  bool changes;
  bool is_asset;
  std::size_t cap;
  drogon::HttpResponsePtr fallback;
  std::atomic<bool> delivered{false};
};

auto handle(Route route, const drogon::HttpRequestPtr& request,
            Callback callback,
            const std::shared_ptr<FloatLayoutService>& service,
            std::string_view configured) -> void {
  std::shared_ptr<Reply> reply;
  try {
    const auto accepted = lifecycle::async_tasks().try_acquire();
    // Keep original callback until construction succeeds: allocation failure
    // cannot strand an already-moved callback or trigger a second delivery.
    reply =
        std::make_shared<Reply>(callback, service, accepted, mutation(route),
                                asset(route), maximum(route));
    if (!accepted || !service) {
      reply->fail({});
      return;
    }
    const bool platform = route == Route::PLATFORM;
    const auto caller_auth = auth::get_auth_context(request);
    if (!platform && !caller_auth) {
      reply->fail("not_authenticated");
      return;
    }
    const auto origin = trusted_origin(request, configured);
    if (!origin) {
      reply->fail("invalid_argument");
      return;
    }
    const auto ip = request->peerAddr().toIp();
    if (asset(route)) {
      auto selector = asset_selector(request, platform);
      if (!selector) {
        reply->fail("asset_not_found");
        return;
      }
      service->serve_asset(
          std::move(*selector), request,
          caller_auth.value_or(auth::AuthContext{}), ip,
          [reply](FloatLayoutService::AssetResult result) {
            reply->binary(std::move(result));
          },
          accepted);
      return;
    }
    if (admin(route)) {
      auto completion = [reply](FloatLayoutService::AdminResult result) {
        reply->management(std::move(result));
      };
      if (route == Route::ADMIN_READ) {
        auto query = admin_read_request(request->query(), request->body());
        if (!query) {
          reply->fail(admission_error(query.error()));
          return;
        }
        service->read_admin_defaults(std::move(*query), *caller_auth, ip, 0,
                                     std::move(completion), accepted);
      } else {
        if (route == Route::ADMIN_PUT) {
          auto value = admin_put_request(request->query(), request->body());
          if (!value) {
            reply->fail(admission_error(value.error()));
            return;
          }
          service->put_admin_default(std::move(*value), *caller_auth, ip, 0,
                                     std::move(completion), accepted);
        } else {
          auto value = admin_delete_request(request->query(), request->body());
          if (!value) {
            reply->fail(admission_error(value.error()));
            return;
          }
          service->delete_admin_default(std::move(*value), *caller_auth, ip, 0,
                                        std::move(completion), accepted);
        }
      }
      return;
    }
    if (route == Route::CATALOG) {
      auto query = parse_query(request->query());
      if (!query) {
        reply->fail("invalid_argument");
        return;
      }
      query->trusted_origin = *origin;
      service->catalog(
          std::move(*query), *caller_auth, ip,
          [reply](FloatLayoutService::Result result) {
            reply->json(std::move(result));
          },
          accepted);
      return;
    }
    if (!request->query().empty()) {
      reply->fail("invalid_argument");
      return;
    }
    if (route == Route::ADMISSION) {
      auto targets = parse_surface_admission(request->body());
      if (!targets) {
        reply->fail(admission_error(targets.error()));
        return;
      }
      service->admit(
          std::move(*targets), *origin, *caller_auth, ip,
          [reply](FloatLayoutService::Result result) {
            reply->json(std::move(result));
          },
          accepted);
    } else if (route == Route::COMMIT) {
      auto commit = layout_contract::parse_float_commit(request->body());
      if (!commit) {
        reply->fail(commit_error(commit.error()));
        return;
      }
      service->commit_layout(
          std::move(*commit), *caller_auth, ip,
          [reply](FloatLayoutService::Result result) {
            reply->json(std::move(result));
          },
          accepted);
    } else {
      service->read_layout(
          *origin, *caller_auth, ip,
          [reply](FloatLayoutService::Result result) {
            reply->json(std::move(result));
          },
          accepted);
    }
  } catch (...) {
    if (reply)
      reply->fail({});
    else {
      // No asynchronous handoff happened. If even fixed-response allocation
      // cannot succeed, propagate rather than pretending delivery or retrying
      // a native operation. This callback is attempted once only.
      callback(error_response({}, asset(route), mutation(route)));
    }
  }
}
} // namespace

auto register_float_routes(std::shared_ptr<FloatLayoutService> service,
                           std::string configured_origin) -> void {
  if (!service || configured_origin.size() > 2048 ||
      !valid_browser_origin(configured_origin))
    throw std::invalid_argument("invalid float route configuration");
  for (const auto& definition : DEFINITIONS) {
    auto handler = [service, configured_origin, route = definition.route](
                       const drogon::HttpRequestPtr& request,
                       Callback&& callback) {
      handle(route, request, std::move(callback), service, configured_origin);
    };
    if (definition.regex)
      drogon::app().registerHandlerViaRegex(std::string{definition.path},
                                            std::move(handler),
                                            constraints(definition.route));
    else
      drogon::app().registerHandler(std::string{definition.path},
                                    std::move(handler),
                                    constraints(definition.route));
  }
}

namespace test_seam {
// Actual low-level route helpers only: no service/authority/native work is
// fabricated. Test declarations remain local to the focused test translation.
auto float_admission_error_code(TypedRequestError error) -> std::string_view {
  return admission_error(error);
}
auto float_commit_error_code(layout_contract::Error error) -> std::string_view {
  return commit_error(error);
}
auto parse_float_catalog_query(std::string_view raw)
    -> std::optional<FloatLayoutService::CatalogQuery> {
  return parse_query(raw);
}
auto float_request_origin(const drogon::HttpRequestPtr& request,
                          std::string_view configured)
    -> std::optional<std::string> {
  return trusted_origin(request, configured);
}
auto float_asset_selector(const drogon::HttpRequestPtr& request, bool platform)
    -> std::expected<protocol::AssetSelector, protocol::Error> {
  return asset_selector(request, platform);
}
auto float_admin_read_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::ReadRequest, TypedRequestError> {
  return admin_read_request(query, body);
}
auto float_admin_put_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::PutRequest, TypedRequestError> {
  return admin_put_request(query, body);
}
auto float_admin_delete_request(std::string_view query, std::string_view body)
    -> std::expected<admin_defaults::DeleteRequest, TypedRequestError> {
  return admin_delete_request(query, body);
}
auto float_route_contract() -> Json::Value {
  Json::Value result{Json::arrayValue};
  for (const auto& definition : DEFINITIONS) {
    Json::Value row;
    row["path"] = std::string{definition.path};
    row["regex"] = definition.regex;
    row["methods"] = Json::Value{Json::arrayValue};
    row["filters"] = Json::Value{Json::arrayValue};
    for (const auto& constraint : constraints(definition.route)) {
      if (constraint.type() == drogon::internal::ConstraintType::HttpMethod)
        row["methods"].append(static_cast<int>(constraint.getHttpMethod()));
      else
        row["filters"].append(constraint.getMiddlewareName());
    }
    result.append(std::move(row));
  }
  return result;
}
auto float_json_completion(Callback callback, bool changes)
    -> FloatLayoutService::Completion {
  auto reply = std::make_shared<Reply>(std::move(callback), nullptr, nullptr,
                                       changes, false, 1048576);
  return [reply](FloatLayoutService::Result result) {
    reply->json(std::move(result));
  };
}
auto float_asset_completion(Callback callback)
    -> FloatLayoutService::AssetCompletion {
  auto reply = std::make_shared<Reply>(std::move(callback), nullptr, nullptr,
                                       false, true, 0);
  return [reply](FloatLayoutService::AssetResult result) {
    reply->binary(std::move(result));
  };
}
auto float_admin_completion(Callback callback, bool changes)
    -> FloatLayoutService::AdminCompletion {
  auto reply =
      std::make_shared<Reply>(std::move(callback), nullptr, nullptr, changes,
                              false, admin_defaults::MAX_RESPONSE_BYTES);
  return [reply](FloatLayoutService::AdminResult result) {
    reply->management(std::move(result));
  };
}
} // namespace test_seam
} // namespace plinth::frontend
