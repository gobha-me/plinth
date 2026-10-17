#include "kernel/packages/protected_restore_policy.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <linux/capability.h>
#include <string>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace plinth::packages::restore {
namespace {

class File {
 public:
  explicit File(int descriptor) : descriptor_(descriptor) {}
  ~File() {
    if (descriptor_ >= 0) ::close(descriptor_);
  }
  File(const File&) = delete;
  auto operator=(const File&) -> File& = delete;
  File(File&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  auto operator=(File&& other) noexcept -> File& {
    if (this != &other) {
      File incoming{std::move(other)};
      std::swap(descriptor_, incoming.descriptor_);
    }
    return *this;
  }
  [[nodiscard]] auto get() const -> int { return descriptor_; }

 private:
  int descriptor_;
};

auto restricted_service() -> bool {
  uid_t real = 0;
  uid_t effective = 0;
  uid_t saved = 0;
  if (::getresuid(&real, &effective, &saved) != 0 || real == 0 ||
      effective == 0 || saved == 0 || ::setfsuid(static_cast<uid_t>(-1)) == 0 ||
      ::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) != 1)
    return false;
  __user_cap_header_struct header{};
  header.version = _LINUX_CAPABILITY_VERSION_3;
  std::array<__user_cap_data_struct, 2> capabilities{};
  if (::syscall(SYS_capget, &header, capabilities.data()) != 0) return false;
  for (const auto& word : capabilities) {
    if (word.effective != 0 || word.permitted != 0 || word.inheritable != 0)
      return false;
  }
  return true;
}

auto protected_metadata(const struct stat& metadata, bool directory) -> bool {
  return metadata.st_uid == 0 && (metadata.st_mode & 0022) == 0 &&
         (directory ? S_ISDIR(metadata.st_mode)
                    : S_ISREG(metadata.st_mode) && metadata.st_nlink == 1) &&
         (metadata.st_mode & (S_ISUID | S_ISGID)) == 0;
}

auto same_file(const struct stat& before, const struct stat& after) -> bool {
  return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
         before.st_mode == after.st_mode && before.st_uid == after.st_uid &&
         before.st_gid == after.st_gid && before.st_nlink == after.st_nlink &&
         before.st_size == after.st_size &&
         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

} // namespace

auto load_protected_policy(std::string_view absolute_path)
    -> std::expected<std::optional<ProtectedPolicy>, PolicyLoadError> {
  if (absolute_path.empty()) return std::optional<ProtectedPolicy>{};
  if (!restricted_service() || absolute_path.front() != '/' ||
      absolute_path.size() > 4096 ||
      absolute_path.find('\0') != absolute_path.npos)
    return std::unexpected(PolicyLoadError::UNPROTECTED);

  File parent{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  struct stat metadata{};
  if (parent.get() < 0 || ::fstat(parent.get(), &metadata) != 0)
    return std::unexpected(PolicyLoadError::UNAVAILABLE);
  if (!protected_metadata(metadata, true))
    return std::unexpected(PolicyLoadError::UNPROTECTED);

  std::size_t start = 1;
  std::size_t components = 0;
  while (start < absolute_path.size()) {
    if (++components > 64) return std::unexpected(PolicyLoadError::UNPROTECTED);
    const auto slash = absolute_path.find('/', start);
    const bool final = slash == absolute_path.npos;
    const auto component = absolute_path.substr(
        start, final ? absolute_path.size() - start : slash - start);
    if (component.empty() || component == "." || component == ".." ||
        component.size() > 255)
      return std::unexpected(PolicyLoadError::UNPROTECTED);
    const std::string name{component};
    const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                      (final ? 0 : O_DIRECTORY);
    File opened{::openat(parent.get(), name.c_str(), flags)};
    if (opened.get() < 0)
      return std::unexpected(errno == ELOOP || errno == ENOTDIR
                                 ? PolicyLoadError::UNPROTECTED
                                 : PolicyLoadError::UNAVAILABLE);
    if (::fstat(opened.get(), &metadata) != 0)
      return std::unexpected(PolicyLoadError::UNAVAILABLE);
    if (!protected_metadata(metadata, !final))
      return std::unexpected(PolicyLoadError::UNPROTECTED);
    if (!final) {
      parent = std::move(opened);
      start = slash + 1;
      continue;
    }
    if (metadata.st_size < 0 ||
        static_cast<unsigned long long>(metadata.st_size) > POLICY_MAX_BYTES)
      return std::unexpected(PolicyLoadError::INVALID);
    std::string bytes(static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t received = 0;
    while (received < bytes.size()) {
      const auto count = ::read(opened.get(), bytes.data() + received,
                                bytes.size() - received);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) return std::unexpected(PolicyLoadError::UNAVAILABLE);
      if (count == 0) return std::unexpected(PolicyLoadError::CHANGED);
      received += static_cast<std::size_t>(count);
    }
    char extra = 0;
    ssize_t count = -1;
    do {
      count = ::read(opened.get(), &extra, 1);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return std::unexpected(PolicyLoadError::UNAVAILABLE);
    struct stat after{};
    struct stat path_after{};
    if (count != 0 || ::fstat(opened.get(), &after) != 0 ||
        ::fstatat(parent.get(), name.c_str(), &path_after,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !same_file(metadata, after) || !same_file(after, path_after))
      return std::unexpected(PolicyLoadError::CHANGED);
    auto parsed = parse_policy(bytes);
    if (!parsed) return std::unexpected(PolicyLoadError::INVALID);
    return std::optional<ProtectedPolicy>{ProtectedPolicy{std::move(*parsed)}};
  }
  return std::unexpected(PolicyLoadError::UNPROTECTED);
}

} // namespace plinth::packages::restore
