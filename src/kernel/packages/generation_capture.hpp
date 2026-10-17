#pragma once

#include "kernel/packages/generation_cache.hpp"
#include "kernel/packages/restore_digest.hpp"

#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>

namespace plinth::packages::generation {

enum class CaptureError {
  BUSY,
  CANCELLED,
  DEADLINE,
  CHANGED,
  INVALID,
  TOO_LARGE,
  UNAVAILABLE,
  HASHING_FAILED,
};

using CaptureDeadline = std::chrono::steady_clock::time_point;

struct CaptureRequest {
  GenerationKey key;
  std::filesystem::path root;
  CaptureDeadline deadline;
  std::stop_token cancellation;
};

struct CapturedStorage;
class CaptureBuilder;

// Physical bytes only: this does not establish database generation/readiness,
// protected policy trust, reviewer eligibility or installation provenance.
// Views remain valid and immutable for the entire shared artifact lifetime.
class CapturedArtifact {
 public:
  ~CapturedArtifact();
  CapturedArtifact(const CapturedArtifact&) = delete;
  auto operator=(const CapturedArtifact&) -> CapturedArtifact& = delete;
  [[nodiscard]] auto key() const noexcept -> const GenerationKey&;
  [[nodiscard]] auto digest() const noexcept -> const std::string&;
  [[nodiscard]] auto records() const noexcept
      -> std::span<const restore::ArtifactRecord>;
  [[nodiscard]] auto find(std::string_view path) const noexcept
      -> const restore::ArtifactRecord*;
  [[nodiscard]] auto charged_bytes() const noexcept -> std::size_t;
  [[nodiscard]] auto belongs_to(const Budget& budget) const noexcept -> bool;

 private:
  friend class CaptureBuilder;
  explicit CapturedArtifact(std::unique_ptr<CapturedStorage> storage);
  std::unique_ptr<CapturedStorage> storage;
};

using CaptureResult =
    std::expected<std::shared_ptr<const CapturedArtifact>, CaptureError>;

// Reserves the finite attempt envelope before opening/allocating. Cancellation
// and deadlines are cooperative between syscalls, NOT hard I/O interruption.
[[nodiscard]] auto capture_artifact(const CaptureRequest& request,
                                    Budget& budget,
                                    std::stop_token owner_stop = {})
    -> CaptureResult;
// Consume a reservation obtained from GenerationCache::reserve_capture(),
// without a second reservation or a reclaim/capture lock across filesystem I/O.
[[nodiscard]] auto capture_artifact(const CaptureRequest& request,
                                    BudgetTicket ticket,
                                    std::stop_token owner_stop = {})
    -> CaptureResult;

// One physically active job, no waiting-job queue or detached callbacks.
// drain seals, cancels and joins; a timed-out owner MUST remain alive.
// Destructor attempts a bounded cooperative drain and terminates on failure,
// never detaches or destroys state reachable by the worker. Production must
// drain explicitly under its shutdown watchdog before destruction.
class CaptureWorker {
 public:
  explicit CaptureWorker(Budget budget);
  ~CaptureWorker();
  CaptureWorker(const CaptureWorker&) = delete;
  auto operator=(const CaptureWorker&) -> CaptureWorker& = delete;
  [[nodiscard]] auto try_submit(CaptureRequest request)
      -> std::expected<std::future<CaptureResult>, CaptureError>;
  [[nodiscard]] auto try_submit(CaptureRequest request, BudgetTicket ticket)
      -> std::expected<std::future<CaptureResult>, CaptureError>;
  auto close_admission() -> void;
  auto request_stop() -> void;
  [[nodiscard]] auto drain(CaptureDeadline deadline) -> bool;

 private:
  struct Job {
    BudgetTicket metadata_ticket;
    CaptureRequest request;
    BudgetTicket ticket;
    std::promise<CaptureResult> result;
  };
  auto run() -> void;
  Budget budget;
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<Job> job;
  bool accepting = true;
  bool active = false;
  bool finished = false;
  bool joining = false;
  std::stop_source stop;
  std::thread worker;
};

} // namespace plinth::packages::generation
