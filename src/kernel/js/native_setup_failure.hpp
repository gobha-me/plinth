// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <new>

namespace plinth::js {

// Internal trusted-C++ testing only. No binding, SDK method, environment or
// configuration surface may arm these checkpoints. Each failure is consumed
// once, at an actual production setup boundary with normal owners intact.
enum class NativeSetupFailurePoint : std::uint8_t {
  none,
  dispatcher_commit_task,
  launcher_commit_task,
  batch_commit_task,
  wrapper_commit_task,
  commit_completion_allocation,
  commit_callback_install,
  batch_begin_metadata,
  batch_begin_queue,
  batch_commit_metadata,
  execution_lifetime_setup,
  inherited_cancel_registration,
  inherited_cancel_notification,
};

namespace native_setup_failure_detail {
inline std::atomic<NativeSetupFailurePoint> armed{
    NativeSetupFailurePoint::none};
inline std::atomic<NativeSetupFailurePoint> consumed{
    NativeSetupFailurePoint::none};
} // namespace native_setup_failure_detail

inline auto set_native_setup_failure_for_test(
    NativeSetupFailurePoint point) noexcept -> void {
  native_setup_failure_detail::consumed.store(NativeSetupFailurePoint::none,
                                              std::memory_order_release);
  native_setup_failure_detail::armed.store(point, std::memory_order_release);
}

inline auto clear_native_setup_failure_for_test() noexcept -> void {
  native_setup_failure_detail::armed.store(NativeSetupFailurePoint::none,
                                           std::memory_order_release);
}

[[nodiscard]] inline auto native_setup_failure_observed_for_test() noexcept
    -> NativeSetupFailurePoint {
  return native_setup_failure_detail::consumed.load(std::memory_order_acquire);
}

inline auto maybe_fail_native_setup(NativeSetupFailurePoint point) -> void {
  if (point == NativeSetupFailurePoint::none) {
    return;
  }
  auto expected = point;
  if (native_setup_failure_detail::armed.compare_exchange_strong(
          expected, NativeSetupFailurePoint::none, std::memory_order_acq_rel)) {
    native_setup_failure_detail::consumed.store(point,
                                                std::memory_order_release);
    throw std::bad_alloc{};
  }
}

} // namespace plinth::js
