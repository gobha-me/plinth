#include "kernel/frontend/typed_assets.hpp"

#include "kernel/security/response_headers.hpp"

#include <drogon/utils/Utilities.h>
#include <trantor/net/TcpConnection.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace plinth::frontend {
namespace generation = packages::generation;
namespace {
constexpr std::size_t MAX_STREAMS = 64;
constexpr std::size_t COPY_CHUNK = std::size_t{16} * 1024;

auto valid_member(const packages::restore::ArtifactRecord* member) -> bool {
  return member != nullptr &&
         member->kind == packages::restore::ArtifactKind::REGULAR &&
         member->path.starts_with("client/");
}

auto mime(std::string_view path) -> std::string_view {
  if (path.ends_with(".js") || path.ends_with(".mjs"))
    return "text/javascript; charset=utf-8";
  if (path.ends_with(".css")) return "text/css; charset=utf-8";
  if (path.ends_with(".json")) return "application/json";
  if (path.ends_with(".svg")) return "image/svg+xml";
  if (path.ends_with(".png")) return "image/png";
  if (path.ends_with(".jpg") || path.ends_with(".jpeg")) return "image/jpeg";
  if (path.ends_with(".webp")) return "image/webp";
  if (path.ends_with(".woff2")) return "font/woff2";
  return "application/octet-stream";
}

auto headers(const drogon::HttpResponsePtr& response, std::string_view type,
             const std::string& etag) -> void {
  response->addHeader("Content-Type", std::string{type});
  response->addHeader("ETag", etag);
  // Every request still requires fresh authority, including conditional reads.
  response->addHeader("Cache-Control", "private, no-store");
  response->addHeader("Vary", "Cookie, Authorization");
  response->addHeader("Accept-Ranges", "none");
  security::apply_response_headers(response);
}

auto bodyless_response(std::string_view type, const std::string& etag,
                       std::size_t representation_size, bool not_modified)
    -> drogon::HttpResponsePtr {
  auto response = drogon::HttpResponse::newHttpResponse();
  // Pinned Drogon otherwise synthesizes Content-Length: 0 and duplicates an
  // explicit HEAD length. Forward NO caller headers/status. HttpServer's
  // pre-sending advice still executes independently of passThrough.
  response->setPassThrough(true);
  response->setStatusCode(not_modified ? drogon::k304NotModified
                                       : drogon::k200OK);
  if (!not_modified)
    response->addHeader("Content-Length", std::to_string(representation_size));
  // Pass-through disables the framework's automatic Date, so supply our own
  // server clock value, never a forwarded request header.
  response->addHeader("Date", drogon::utils::getHttpFullDateStr());
  headers(response, type, etag);
  return response;
}
} // namespace

struct TypedAssetStreams::State {
  explicit State(generation::Budget budget_in, generation::BudgetTicket ticket)
      : metadata(std::move(ticket)), budget(std::move(budget_in)) {}
  // Explicit allocations only, not stdlib/allocator/framework overhead.
  generation::BudgetTicket metadata;
  generation::Budget budget;
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::weak_ptr<SendOwner>, MAX_STREAMS> owners;
  std::array<std::weak_ptr<trantor::TcpConnection>, MAX_STREAMS> connections;
  std::array<bool, MAX_STREAMS> occupied{};
  std::size_t active = 0;
  bool accepting = true;
  bool stop_failed = false;
};

struct TypedAssetStreams::SendOwner {
  // Accepted process work is released LAST, after resources and the state.
  std::shared_ptr<lifecycle::AsyncTaskLease> accepted;
  generation::BudgetTicket metadata;
  std::shared_ptr<State> state;
  std::shared_ptr<const generation::CapturedArtifact> artifact;
  const packages::restore::ArtifactRecord* member = nullptr;
  std::unique_ptr<capabilities::drain::DispatchGuard> guard;
  std::atomic<bool> stopped{false};
  std::mutex read_mutex;
  std::size_t offset = 0;
  std::size_t slot = MAX_STREAMS;

  ~SendOwner() {
    // EOF, a null callback, response.clear(), and forceClose() are NOT physical
    // completion. Only destruction of the last shared callback owner gets here.
    artifact.reset();
    guard.reset();
    if (slot != MAX_STREAMS) {
      std::lock_guard lock{state->mutex};
      state->occupied.at(slot) = false;
      state->owners.at(slot).reset();
      state->connections.at(slot).reset();
      --state->active;
      state->changed.notify_all();
    }
  }

  auto pull(char* output, std::size_t length) -> std::size_t {
    if (output == nullptr || length == 0) return 0;
    std::lock_guard lock{read_mutex};
    if (stopped.load()) return 0;
    const auto count =
        std::min({length, COPY_CHUNK, member->content.size() - offset});
    if (count != 0) std::memcpy(output, member->content.data() + offset, count);
    offset += count;
    return count;
  }
};

AdmittedAsset::AdmittedAsset(
    std::shared_ptr<const generation::CapturedArtifact> artifact_in,
    std::string_view member_in,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard_in,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted_in)
    : accepted(std::move(accepted_in)), artifact(std::move(artifact_in)),
      member(artifact ? artifact->find(member_in) : nullptr),
      guard(std::move(guard_in)) {
}

auto AdmittedAsset::operator=(AdmittedAsset&& other) noexcept
    -> AdmittedAsset& {
  if (this != &other) {
    // Retire old resources before releasing their accepted process lease, just
    // as the member declaration order does during ordinary destruction.
    AdmittedAsset retired{std::move(*this)};
    accepted = std::move(other.accepted);
    artifact = std::move(other.artifact);
    member = std::exchange(other.member, nullptr);
    guard = std::move(other.guard);
  }
  return *this;
}

TypedAssetStreams::TypedAssetStreams(generation::Budget budget) {
  auto ticket = budget.try_reserve(sizeof(State) + 128);
  if (!ticket) throw std::length_error("typed asset registry budget");
  state = std::make_shared<State>(std::move(budget), std::move(*ticket));
}

TypedAssetStreams::~TypedAssetStreams() {
  static_cast<void>(request_stop());
}

auto TypedAssetStreams::source(
    std::shared_ptr<const generation::CapturedArtifact> artifact,
    const packages::restore::ArtifactRecord* member,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted,
    std::weak_ptr<trantor::TcpConnection> connection)
    -> std::expected<AssetSource, AssetStreamError> {
  if (!artifact || !artifact->belongs_to(state->budget) ||
      !valid_member(member) || !guard || !guard->admitted() || !accepted)
    return std::unexpected(AssetStreamError::INVALID);
  try {
    auto ticket = state->budget.try_reserve(sizeof(SendOwner) + 128);
    if (!ticket) return std::unexpected(AssetStreamError::LIMIT_EXCEEDED);
    auto owner = std::make_shared<SendOwner>();
    owner->accepted = std::move(accepted);
    owner->metadata = std::move(*ticket);
    owner->state = state;
    owner->artifact = std::move(artifact);
    owner->member = member;
    owner->guard = std::move(guard);
    AssetSource callback = [owner](char* output, std::size_t length) {
      return owner->pull(output, length);
    };
    {
      std::lock_guard lock{state->mutex};
      if (!state->accepting) return std::unexpected(AssetStreamError::CLOSED);
      if (state->active == MAX_STREAMS)
        return std::unexpected(AssetStreamError::BUSY);
      const auto free = std::ranges::find(state->occupied, false);
      const auto slot =
          static_cast<std::size_t>(free - state->occupied.begin());
      state->owners.at(slot) = owner;
      state->connections.at(slot) = std::move(connection);
      state->occupied.at(slot) = true;
      owner->slot = slot;
      ++state->active;
    }
    return callback;
  } catch (const std::bad_alloc&) {
    return std::unexpected(AssetStreamError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(AssetStreamError::LIMIT_EXCEEDED);
  }
}

auto TypedAssetStreams::make_response(const drogon::HttpRequestPtr& request,
                                      AdmittedAsset admitted)
    -> std::expected<drogon::HttpResponsePtr, AssetStreamError> {
  if (!request || !admitted.artifact ||
      !admitted.artifact->belongs_to(state->budget) ||
      !valid_member(admitted.member) || !admitted.guard ||
      !admitted.guard->admitted() || !admitted.accepted ||
      (request->method() != drogon::Get && request->method() != drogon::Head))
    return std::unexpected(AssetStreamError::INVALID);
  if (request->headers().contains("range"))
    return std::unexpected(AssetStreamError::RANGE);
  {
    std::lock_guard lock{state->mutex};
    if (!state->accepting) return std::unexpected(AssetStreamError::CLOSED);
  }
  try {
    const auto etag = '"' + admitted.artifact->digest() + '"';
    const auto& conditional = request->getHeader("if-none-match");
    const bool unchanged = conditional == etag || conditional == "*";
    drogon::HttpResponsePtr response;
    if (unchanged || request->method() == drogon::Head) {
      response = bodyless_response(mime(admitted.member->path), etag,
                                   admitted.member->content.size(), unchanged);
    } else {
      auto callback =
          source(admitted.artifact, admitted.member, std::move(admitted.guard),
                 std::move(admitted.accepted), request->getConnectionPtr());
      if (!callback) return std::unexpected(callback.error());
      response = drogon::HttpResponse::newStreamResponse(*callback);
      response->addHeader("Content-Length",
                          std::to_string(admitted.member->content.size()));
      headers(response, mime(admitted.member->path), etag);
    }
    return response;
  } catch (const std::bad_alloc&) {
    return std::unexpected(AssetStreamError::LIMIT_EXCEEDED);
  } catch (const std::length_error&) {
    return std::unexpected(AssetStreamError::LIMIT_EXCEEDED);
  }
}

auto TypedAssetStreams::close_admission() -> void {
  std::lock_guard lock{state->mutex};
  state->accepting = false;
}

auto TypedAssetStreams::request_stop() -> bool {
  std::array<std::weak_ptr<trantor::TcpConnection>, MAX_STREAMS> connections;
  std::array<std::shared_ptr<SendOwner>, MAX_STREAMS> owners;
  {
    std::lock_guard lock{state->mutex};
    state->accepting = false;
    connections = state->connections;
    for (std::size_t index = 0; index < MAX_STREAMS; ++index)
      owners.at(index) = state->owners.at(index).lock();
  }
  // A temporary weak.lock() may become the final owner concurrently. Never
  // destroy that shared reference while holding the owner's teardown mutex.
  for (const auto& owner : owners)
    if (owner) owner->stopped.store(true);
  bool success = true;
  for (const auto& weak : connections) {
    if (const auto connection = weak.lock()) {
      try {
        // forceClose captures its shared connection and posts to the owned
        // loop. Never call it while holding the registry mutex or overwrite
        // hooks.
        connection->forceClose();
      } catch (const std::bad_alloc&) {
        success = false;
      } catch (const std::length_error&) {
        success = false;
      }
    }
  }
  std::lock_guard lock{state->mutex};
  state->stop_failed = state->stop_failed || !success;
  return !state->stop_failed;
}

auto TypedAssetStreams::drain(std::chrono::steady_clock::time_point deadline)
    -> bool {
  const bool stopped = request_stop();
  std::unique_lock lock{state->mutex};
  return state->changed.wait_until(lock, deadline, [this] {
    return state->active == 0;
  }) && stopped;
}

auto TypedAssetStreams::active_count() const -> std::size_t {
  std::lock_guard lock{state->mutex};
  return state->active;
}

auto typed_assets::test_seam::make_source(
    TypedAssetStreams& streams,
    std::shared_ptr<const generation::CapturedArtifact> artifact,
    std::string_view member,
    std::unique_ptr<capabilities::drain::DispatchGuard> guard,
    std::shared_ptr<lifecycle::AsyncTaskLease> accepted,
    std::weak_ptr<trantor::TcpConnection> connection)
    -> std::expected<AssetSource, AssetStreamError> {
  const auto* record = artifact ? artifact->find(member) : nullptr;
  return streams.source(std::move(artifact), record, std::move(guard),
                        std::move(accepted), std::move(connection));
}

auto typed_assets::test_seam::bodyless_response(std::string_view member,
                                                const std::string& quoted_etag,
                                                std::size_t representation_size,
                                                bool not_modified)
    -> drogon::HttpResponsePtr {
  return frontend::bodyless_response(mime(member), quoted_etag,
                                     representation_size, not_modified);
}

} // namespace plinth::frontend
