#pragma once

#include <libpq-fe.h>

#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace plinth::packages::name_operation {

// Compatibility helpers: exact existing installer session advisory-lock
// protocol/error strings. These accept a kernel-generated complete lock seed.
[[nodiscard]] auto lock_key(std::string_view name) -> std::string;
[[nodiscard]] auto try_acquire_seed(PGconn* connection, const std::string& seed)
    -> std::expected<void, std::string>;
auto release_seed(PGconn* connection, const std::string& seed) -> void;
[[nodiscard]] auto release_seed_checked(PGconn* connection,
                                        const std::string& seed)
    -> std::expected<void, std::string>;

enum class NameError { INVALID, NOT_OWNED, BUSY, UNAVAILABLE, LIMIT_EXCEEDED };

// Thread-confined proof tied to the actual PostgreSQL session/name lock.
// Owned form retains its PG connection and releases its own acquisition only.
// Borrowed form must stay inside the caller's connection/name-lock lifetime;
// construction rechecks server ownership WITHOUT recursive acquisition, and its
// destructor NEVER unlocks the caller's lock. Receipt operations recheck again.
// This proves lock ownership, not package provenance/readiness/authorization.
class NameOperation {
 public:
  [[nodiscard]] static auto acquire(std::shared_ptr<PGconn> connection,
                                    std::string name)
      -> std::expected<NameOperation, NameError>;
  [[nodiscard]] static auto borrow_existing(PGconn& connection,
                                            std::string name)
      -> std::expected<NameOperation, NameError>;
  ~NameOperation();
  NameOperation(const NameOperation&) = delete;
  auto operator=(const NameOperation&) -> NameOperation& = delete;
  NameOperation(NameOperation&& other) noexcept;
  auto operator=(NameOperation&& other) noexcept -> NameOperation&;

  [[nodiscard]] auto name() const noexcept -> std::string_view {
    return package_name;
  }
  [[nodiscard]] auto connection() const noexcept -> PGconn* { return session; }
  [[nodiscard]] auto assert_owned() const -> std::expected<void, NameError>;
  [[nodiscard]] auto release_checked() -> std::expected<void, NameError>;

 private:
  NameOperation(PGconn* session, std::shared_ptr<PGconn> retained,
                std::string name, std::string seed, bool release);
  auto release_on_destroy() noexcept -> void;
  std::shared_ptr<PGconn> retained;
  PGconn* session = nullptr;
  std::string package_name;
  std::string seed;
  bool releases = false;
};

} // namespace plinth::packages::name_operation
