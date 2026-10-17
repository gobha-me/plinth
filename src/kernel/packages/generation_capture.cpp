#include "kernel/packages/generation_capture.hpp"

#include "kernel/packages/detail/launcher_validation.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

namespace plinth::packages::generation {
namespace {

constexpr std::size_t HEADER_BYTES =
    std::string_view{"plinth.restore.artifact"}.size() + 1 + 4 + 4;

class File {
 public:
  explicit File(int descriptor = -1) : descriptor(descriptor) {}
  ~File() {
    if (descriptor >= 0) ::close(descriptor);
  }
  File(const File&) = delete;
  auto operator=(const File&) -> File& = delete;
  File(File&& other) noexcept
      : descriptor(std::exchange(other.descriptor, -1)) {}
  auto operator=(File&& other) noexcept -> File& {
    if (this != &other) {
      if (descriptor >= 0) ::close(descriptor);
      descriptor = std::exchange(other.descriptor, -1);
    }
    return *this;
  }
  [[nodiscard]] auto get() const -> int { return descriptor; }

 private:
  int descriptor;
};

auto same_identity(const struct stat& left, const struct stat& right) -> bool {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_mode == right.st_mode && left.st_size == right.st_size &&
         left.st_nlink == right.st_nlink &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
         left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
         left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

auto checkpoint(const CaptureRequest& request, std::stop_token owner_stop)
    -> std::expected<void, CaptureError> {
  if (owner_stop.stop_requested() || request.cancellation.stop_requested())
    return std::unexpected(CaptureError::CANCELLED);
  if (std::chrono::steady_clock::now() >= request.deadline)
    return std::unexpected(CaptureError::DEADLINE);
  return {};
}

auto open_root(const CaptureRequest& request, std::stop_token owner_stop)
    -> std::expected<File, CaptureError> {
  const auto& root = request.root;
  if (root.empty()) return std::unexpected(CaptureError::INVALID);
  File directory{::open(root.is_absolute() ? "/" : ".",
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  if (directory.get() < 0) return std::unexpected(CaptureError::UNAVAILABLE);
  for (const auto& component : root) {
    if (auto ready = checkpoint(request, owner_stop); !ready)
      return std::unexpected(ready.error());
    if (component.empty() || component == "/" || component == ".") continue;
    if (component == ".." || component.native().find('\0') != std::string::npos)
      return std::unexpected(CaptureError::INVALID);
    File next{
        ::openat(directory.get(), component.c_str(),
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (next.get() < 0) return std::unexpected(CaptureError::INVALID);
    directory = std::move(next);
  }
  return directory;
}

auto valid_path(std::string_view path) -> bool {
  if (!detail::unicode_scalar_count(path)) return false;
  return std::ranges::none_of(path, [](char byte) {
    const auto value = static_cast<unsigned char>(byte);
    return value < 0x20U || value == 0x7fU || byte == '\\';
  });
}

struct OwnedRecord {
  std::unique_ptr<char[]> path;
  std::unique_ptr<char[]> content;
  struct stat identity{};
};

struct CloseDirectory {
  auto operator()(DIR* directory) const noexcept -> void {
    ::closedir(directory);
  }
};

} // namespace

struct CapturedStorage {
  BudgetTicket ticket;
  GenerationKey key;
  std::string digest;
  std::unique_ptr<OwnedRecord[]> owned;
  std::unique_ptr<restore::ArtifactRecord[]> views;
  std::size_t count = 0;
  std::size_t content_bytes = 0;
  std::size_t metadata_bytes = HEADER_BYTES;
  std::size_t allocated_bytes = 0;
};

class CaptureBuilder {
 public:
  CaptureBuilder(const CaptureRequest& request, BudgetTicket ticket,
                 std::stop_token owner_stop)
      : request(request), owner_stop(owner_stop),
        storage(std::make_unique<CapturedStorage>()) {
    storage->ticket = std::move(ticket);
    storage->key = request.key;
    storage->owned =
        std::make_unique<OwnedRecord[]>(restore::INVENTORY_MAX_RECORDS);
    storage->views = std::make_unique<restore::ArtifactRecord[]>(
        restore::INVENTORY_MAX_RECORDS);
    storage->allocated_bytes =
        sizeof(CapturedStorage) + sizeof(CapturedArtifact) +
        restore::INVENTORY_MAX_RECORDS *
            (sizeof(OwnedRecord) + sizeof(restore::ArtifactRecord)) +
        storage->key.package_id.capacity() + storage->key.name.capacity() +
        storage->key.version.capacity() + 3 + 65 + 256;
  }

  auto run() -> CaptureResult {
    auto root = open_root(request, owner_stop);
    if (!root) return std::unexpected(root.error());
    struct stat initial{};
    if (::fstat(root->get(), &initial) != 0)
      return std::unexpected(CaptureError::UNAVAILABLE);
    std::array<char, restore::INVENTORY_PATH_MAX_BYTES + 1> path{};
    if (auto walked = walk(root->get(), path, 0, 0); !walked)
      return std::unexpected(walked.error());
    // Reopen every component without following links. This also detects a
    // directory replaced after its own enumeration completed.
    for (std::size_t index = 0; index < storage->count; ++index) {
      if (auto ready = checkpoint(request, owner_stop); !ready)
        return std::unexpected(ready.error());
      const auto& view = storage->views[index];
      auto reopened = reopen(root->get(), view.path);
      if (!reopened) return std::unexpected(reopened.error());
      struct stat current{};
      if (::fstat(reopened->get(), &current) != 0 ||
          !same_identity(storage->owned[index].identity, current))
        return std::unexpected(CaptureError::CHANGED);
    }
    struct stat final_metadata{};
    if (::fstat(root->get(), &final_metadata) != 0 ||
        !same_identity(initial, final_metadata))
      return std::unexpected(CaptureError::CHANGED);
    if (auto ready = checkpoint(request, owner_stop); !ready)
      return std::unexpected(ready.error());
    auto digest =
        restore::artifact_digest({storage->views.get(), storage->count});
    if (!digest)
      return std::unexpected(digest.error() ==
                                     restore::CodecError::HASHING_FAILED
                                 ? CaptureError::HASHING_FAILED
                                 : CaptureError::INVALID);
    if (auto ready = checkpoint(request, owner_stop); !ready)
      return std::unexpected(ready.error());
    storage->digest = std::move(*digest);
    storage->allocated_bytes += storage->digest.capacity() + 1;
    if (storage->allocated_bytes > storage->ticket.bytes())
      return std::unexpected(CaptureError::TOO_LARGE);
    storage->ticket.shrink(storage->allocated_bytes);
    return std::shared_ptr<const CapturedArtifact>(
        new CapturedArtifact(std::move(storage)));
  }

 private:
  auto reopen(int root, std::string_view path)
      -> std::expected<File, CaptureError> {
    File current{::fcntl(root, F_DUPFD_CLOEXEC, 0)};
    if (current.get() < 0) return std::unexpected(CaptureError::UNAVAILABLE);
    std::size_t begin = 0;
    while (begin < path.size()) {
      if (auto ready = checkpoint(request, owner_stop); !ready)
        return std::unexpected(ready.error());
      const auto slash = path.find('/', begin);
      const bool last = slash == std::string_view::npos;
      const auto end = last ? path.size() : slash;
      std::array<char, restore::INVENTORY_PATH_MAX_BYTES + 1> component{};
      const auto length = end - begin;
      std::memcpy(component.data(), path.data() + begin, length);
      File next{::openat(current.get(), component.data(),
                         O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                             (last ? 0 : O_DIRECTORY))};
      if (next.get() < 0) return std::unexpected(CaptureError::CHANGED);
      current = std::move(next);
      if (last) return current;
      begin = end + 1;
    }
    return std::unexpected(CaptureError::INVALID);
  }

  auto walk(int directory,
            std::array<char, restore::INVENTORY_PATH_MAX_BYTES + 1>& path,
            std::size_t prefix, std::size_t depth)
      -> std::expected<void, CaptureError> {
    if (auto ready = checkpoint(request, owner_stop); !ready) return ready;
    struct stat before{};
    if (::fstat(directory, &before) != 0)
      return std::unexpected(CaptureError::UNAVAILABLE);
    const int duplicate = ::fcntl(directory, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) return std::unexpected(CaptureError::UNAVAILABLE);
    DIR* raw = ::fdopendir(duplicate);
    if (raw == nullptr) {
      ::close(duplicate);
      return std::unexpected(CaptureError::UNAVAILABLE);
    }
    const std::unique_ptr<DIR, CloseDirectory> entries(raw);
    for (;;) {
      if (auto ready = checkpoint(request, owner_stop); !ready) return ready;
      errno = 0;
      const auto* entry = ::readdir(raw);
      if (entry == nullptr) {
        if (errno != 0) return std::unexpected(CaptureError::UNAVAILABLE);
        break;
      }
      const std::string_view name{entry->d_name};
      if (name == "." || name == "..") continue;
      if (storage->count == restore::INVENTORY_MAX_RECORDS ||
          depth == restore::INVENTORY_MAX_COMPONENTS)
        return std::unexpected(CaptureError::TOO_LARGE);
      const auto length = prefix + (prefix == 0 ? 0 : 1) + name.size();
      if (length > restore::INVENTORY_PATH_MAX_BYTES)
        return std::unexpected(CaptureError::TOO_LARGE);
      auto offset = prefix;
      if (prefix != 0) path[offset++] = '/';
      std::memcpy(path.data() + offset, name.data(), name.size());
      path[length] = '\0';
      const std::string_view relative{path.data(), length};
      if (!valid_path(relative)) return std::unexpected(CaptureError::INVALID);
      struct stat observed{};
      if (::fstatat(directory, entry->d_name, &observed, AT_SYMLINK_NOFOLLOW) !=
          0)
        return std::unexpected(CaptureError::CHANGED);
      const bool is_directory = S_ISDIR(observed.st_mode);
      if ((!is_directory && !S_ISREG(observed.st_mode)) ||
          (!is_directory && observed.st_nlink != 1) ||
          (observed.st_mode & 07000U) != 0)
        return std::unexpected(CaptureError::INVALID);
      if (observed.st_size < 0) return std::unexpected(CaptureError::INVALID);
      const auto content_size =
          is_directory ? 0 : static_cast<std::uintmax_t>(observed.st_size);
      if (content_size >
          restore::INVENTORY_CONTENT_MAX_BYTES - storage->content_bytes)
        return std::unexpected(CaptureError::TOO_LARGE);
      const auto metadata = length + 1 + 4 + 4 + (is_directory ? 0 : 8);
      if (metadata >
          restore::INVENTORY_METADATA_MAX_BYTES - storage->metadata_bytes)
        return std::unexpected(CaptureError::TOO_LARGE);
      File opened{::openat(directory, entry->d_name,
                           O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                               (is_directory ? O_DIRECTORY : 0))};
      struct stat actual{};
      if (opened.get() < 0 || ::fstat(opened.get(), &actual) != 0 ||
          !same_identity(observed, actual))
        return std::unexpected(CaptureError::CHANGED);
      const auto index = storage->count++;
      auto& owned = storage->owned[index];
      owned.path = std::make_unique<char[]>(length + 1);
      std::memcpy(owned.path.get(), path.data(), length + 1);
      if (content_size != 0)
        owned.content =
            std::make_unique<char[]>(static_cast<std::size_t>(content_size));
      auto& view = storage->views[index];
      view = {.kind = is_directory ? restore::ArtifactKind::DIRECTORY
                                   : restore::ArtifactKind::REGULAR,
              .path = {owned.path.get(), length},
              .mode = static_cast<std::uint32_t>(observed.st_mode & 0777U),
              .content = content_size == 0
                             ? std::string_view{}
                             : std::string_view{
                                   owned.content.get(),
                                   static_cast<std::size_t>(content_size)}};
      storage->metadata_bytes += metadata;
      storage->content_bytes += static_cast<std::size_t>(content_size);
      storage->allocated_bytes +=
          length + 1 + static_cast<std::size_t>(content_size);
      if (is_directory) {
        if (auto nested = walk(opened.get(), path, length, depth + 1); !nested)
          return nested;
      } else {
        std::size_t read = 0;
        while (read < view.content.size()) {
          if (auto ready = checkpoint(request, owner_stop); !ready)
            return ready;
          const auto amount = ::read(
              opened.get(), owned.content.get() + read,
              std::min(view.content.size() - read, std::size_t{64} * 1024));
          if (amount < 0 && errno == EINTR) continue;
          if (amount <= 0) return std::unexpected(CaptureError::CHANGED);
          read += static_cast<std::size_t>(amount);
        }
        char extra = 0;
        if (::read(opened.get(), &extra, 1) != 0)
          return std::unexpected(CaptureError::CHANGED);
      }
      if (::fstat(opened.get(), &owned.identity) != 0 ||
          !same_identity(observed, owned.identity))
        return std::unexpected(CaptureError::CHANGED);
    }
    struct stat after{};
    if (::fstat(directory, &after) != 0 || !same_identity(before, after))
      return std::unexpected(CaptureError::CHANGED);
    return {};
  }

  const CaptureRequest& request;
  std::stop_token owner_stop;
  std::unique_ptr<CapturedStorage> storage;
};

CapturedArtifact::CapturedArtifact(std::unique_ptr<CapturedStorage> owner)
    : storage(std::move(owner)) {
}
CapturedArtifact::~CapturedArtifact() = default;
auto CapturedArtifact::key() const noexcept -> const GenerationKey& {
  return storage->key;
}
auto CapturedArtifact::digest() const noexcept -> const std::string& {
  return storage->digest;
}
auto CapturedArtifact::records() const noexcept
    -> std::span<const restore::ArtifactRecord> {
  return {storage->views.get(), storage->count};
}
auto CapturedArtifact::find(std::string_view path) const noexcept
    -> const restore::ArtifactRecord* {
  const auto all = records();
  const auto found =
      std::ranges::find(all, path, &restore::ArtifactRecord::path);
  return found == all.end() ? nullptr : &*found;
}
auto CapturedArtifact::charged_bytes() const noexcept -> std::size_t {
  return storage->ticket.bytes();
}
auto CapturedArtifact::belongs_to(const Budget& budget) const noexcept -> bool {
  return budget.owns(storage->ticket);
}

auto capture_artifact(const CaptureRequest& request, Budget& budget,
                      std::stop_token owner_stop) -> CaptureResult {
  if (auto ready = checkpoint(request, owner_stop); !ready)
    return std::unexpected(ready.error());
  auto ticket = budget.try_reserve(CAPTURE_ATTEMPT_BYTES);
  if (!ticket) return std::unexpected(CaptureError::BUSY);
  return capture_artifact(request, std::move(*ticket), owner_stop);
}
auto capture_artifact(const CaptureRequest& request, BudgetTicket ticket,
                      std::stop_token owner_stop) -> CaptureResult {
  if (auto ready = checkpoint(request, owner_stop); !ready)
    return std::unexpected(ready.error());
  if (request.key.package_id.empty() || request.key.package_id.size() > 256 ||
      request.key.name.empty() || request.key.name.size() > 64 ||
      request.key.version.empty() || request.key.version.size() > 256 ||
      request.key.profile != 1 || request.root.native().size() > 4096 ||
      request.key.package_id.find('\0') != std::string::npos ||
      request.key.name.find('\0') != std::string::npos ||
      request.key.version.find('\0') != std::string::npos)
    return std::unexpected(CaptureError::INVALID);
  if (ticket.bytes() < CAPTURE_ATTEMPT_BYTES)
    return std::unexpected(CaptureError::BUSY);
  try {
    return CaptureBuilder(request, std::move(ticket), owner_stop).run();
  } catch (const std::bad_alloc&) {
    return std::unexpected(CaptureError::TOO_LARGE);
  } catch (const std::length_error&) {
    return std::unexpected(CaptureError::TOO_LARGE);
  }
}

CaptureWorker::CaptureWorker(Budget source_budget)
    : budget(std::move(source_budget)), worker([this] { run(); }) {
}
CaptureWorker::~CaptureWorker() {
  if (!drain(std::chrono::steady_clock::now() + std::chrono::seconds{5}))
    std::terminate();
}
auto CaptureWorker::try_submit(CaptureRequest request)
    -> std::expected<std::future<CaptureResult>, CaptureError> {
  {
    std::lock_guard lock(mutex);
    if (!accepting || stop.stop_requested())
      return std::unexpected(CaptureError::CANCELLED);
    if (active) return std::unexpected(CaptureError::BUSY);
  }
  auto ticket = budget.try_reserve(CAPTURE_ATTEMPT_BYTES);
  if (!ticket) return std::unexpected(CaptureError::BUSY);
  return try_submit(std::move(request), std::move(*ticket));
}
auto CaptureWorker::try_submit(CaptureRequest request, BudgetTicket ticket)
    -> std::expected<std::future<CaptureResult>, CaptureError> {
  std::lock_guard lock(mutex);
  if (!accepting || stop.stop_requested())
    return std::unexpected(CaptureError::CANCELLED);
  if (active) return std::unexpected(CaptureError::BUSY);
  if (!budget.owns(ticket)) return std::unexpected(CaptureError::INVALID);
  if (ticket.bytes() < CAPTURE_ATTEMPT_BYTES)
    return std::unexpected(CaptureError::BUSY);
  if (request.key.package_id.size() > 256 || request.key.name.size() > 64 ||
      request.key.version.size() > 256 || request.root.native().size() > 4096)
    return std::unexpected(CaptureError::INVALID);
  // Moved request buffers and the future's bounded per-job bookkeeping remain
  // separately charged after capture shrinks its envelope to retained bytes.
  // This is explicit source accounting, not a stdlib allocator-overhead claim.
  auto metadata_bytes = sizeof(Job) + std::size_t{256} + 4;
  const std::array capacities{
      request.key.package_id.capacity(), request.key.name.capacity(),
      request.key.version.capacity(), request.root.native().capacity()};
  for (const auto capacity : capacities) {
    if (capacity > CAPTURE_ATTEMPT_BYTES - metadata_bytes)
      return std::unexpected(CaptureError::BUSY);
    metadata_bytes += capacity;
  }
  auto metadata = budget.try_reserve(metadata_bytes);
  if (!metadata) return std::unexpected(CaptureError::BUSY);
  try {
    Job next{.metadata_ticket = std::move(*metadata),
             .request = std::move(request),
             .ticket = std::move(ticket),
             .result = {}};
    auto result = next.result.get_future();
    job.emplace(std::move(next));
    active = true;
    changed.notify_all();
    return result;
  } catch (const std::bad_alloc&) {
    return std::unexpected(CaptureError::TOO_LARGE);
  } catch (const std::length_error&) {
    return std::unexpected(CaptureError::TOO_LARGE);
  }
}
auto CaptureWorker::close_admission() -> void {
  std::lock_guard lock(mutex);
  accepting = false;
}
auto CaptureWorker::request_stop() -> void {
  close_admission();
  stop.request_stop();
  changed.notify_all();
}
auto CaptureWorker::drain(CaptureDeadline deadline) -> bool {
  request_stop();
  std::unique_lock lock(mutex);
  if (!changed.wait_until(lock, deadline, [this] { return !joining; }))
    return false;
  joining = true;
  bool ready = false;
  try {
    ready = changed.wait_until(lock, deadline, [this] { return finished; });
    if (ready) {
      lock.unlock();
      // std::thread has no timed join. Completion is cooperative; the
      // production watchdog remains the absolute OS/thread-library bound.
      if (worker.joinable()) worker.join();
    }
  } catch (...) {
    if (!lock.owns_lock()) lock.lock();
    joining = false;
    lock.unlock();
    changed.notify_all();
    throw;
  }
  if (!lock.owns_lock()) lock.lock();
  joining = false;
  lock.unlock();
  changed.notify_all();
  return ready;
}
auto CaptureWorker::run() -> void {
  for (;;) {
    std::unique_lock lock(mutex);
    changed.wait(lock,
                 [this] { return job.has_value() || stop.stop_requested(); });
    if (!job) {
      finished = true;
      changed.notify_all();
      return;
    }
    Job current = std::move(*job);
    job.reset();
    lock.unlock();
    auto result = capture_artifact(current.request, std::move(current.ticket),
                                   stop.get_token());
    current.result.set_value(std::move(result));
    lock.lock();
    active = false;
    changed.notify_all();
  }
}

} // namespace plinth::packages::generation
