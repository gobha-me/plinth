// SPDX-License-Identifier: MIT
#pragma once

#include "kernel/extension_http/codec.hpp"
#include "kernel/extension_http/ownership.hpp"

#include <quickjs.h>

#include <expected>
#include <functional>
#include <memory>
#include <variant>

namespace plinth::extension_http {

enum class QuickJsCodecError {
  INVALID,
  TOO_LARGE,
  RESOURCE_EXHAUSTED,
  ENGINE_FAILURE,
  RETIRED
};
template <typename T>
using QuickJsCodecResult = std::expected<T, QuickJsCodecError>;

// Accounting only. The actual realm owner reserves BEFORE allocating/retaining
// its finite configured engine heap, configures that same limit, and retains
// this token until actual context/runtime destruction. Neither this token nor
// a scalar budget establishes the runtime limit, ownership or physical join.
// One reservation accounts one realm, not several concurrent engine heaps.
class EngineHeapReservation final {
 public:
  [[nodiscard]] static auto create(std::shared_ptr<AdmissionLease> admission,
                                   std::size_t configured_heap_bytes)
      -> QuickJsCodecResult<std::shared_ptr<EngineHeapReservation>>;
  EngineHeapReservation(const EngineHeapReservation&) = delete;
  auto operator=(const EngineHeapReservation&)
      -> EngineHeapReservation& = delete;
  [[nodiscard]] auto bytes() const noexcept -> std::size_t;

 private:
  friend class QuickJsCodecTurn;
  EngineHeapReservation(std::shared_ptr<AdmissionLease>, ExtraMemoryCharge,
                        std::size_t);
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge allocation;
  std::size_t configured_bytes;
};

class ChargedNativeResponse final {
 public:
  ChargedNativeResponse(ChargedNativeResponse&&) noexcept = default;
  auto operator=(ChargedNativeResponse&&) noexcept
      -> ChargedNativeResponse& = delete;
  ChargedNativeResponse(const ChargedNativeResponse&) = delete;
  auto operator=(const ChargedNativeResponse&)
      -> ChargedNativeResponse& = delete;
  [[nodiscard]] auto value() const noexcept -> const NativeResponse&;

 private:
  friend class QuickJsCodecTurn;
  ChargedNativeResponse(ExtraMemoryCharge, NativeResponse);
  // Destruction order: bytes first, charge afterwards. No uncharged take().
  ExtraMemoryCharge allocation;
  NativeResponse response;
};
struct PendingNativePromise {};
struct RejectedNativePromise {};
using InspectedReturn =
    std::variant<PendingNativePromise, RejectedNativePromise,
                 ChargedNativeResponse>;

// Borrowed only for with_encoded_call. The actual driver supplies its owned
// host controls. Shape is not authenticity: the codec neither mints controls
// nor establishes their cancellation/queue/settlement protocol.
struct BorrowedContextControls {
  JSValueConst cancel;
  JSValueConst remaining_execution_ms;
};
using EncodedCallConsumer = std::function<QuickJsCodecResult<void>(
    JSValueConst request, JSValueConst context)>;

// Synchronous, exclusive-context lexical boundary. The actual caller retains
// context/runtime/generation/driver ownership and heap reservation across ALL
// jobs and internally retained JS inputs, even after these lexical values free.
// It must not release a request-containing realm into an idle RuntimePool:
// release preserves module/heap state; destroy can refuse/defer physical join.
// No method here creates a checkout, pumps jobs, authorizes entry or joins
// work.
class QuickJsCodecTurn final {
 public:
  using OwnedLiveCheck = std::function<bool()>;
  [[nodiscard]] static auto create(
      JSContext& context, std::shared_ptr<AdmissionLease> admission,
      std::shared_ptr<const EngineHeapReservation> engine_heap,
      OwnedLiveCheck live)
      -> QuickJsCodecResult<std::unique_ptr<QuickJsCodecTurn>>;
  ~QuickJsCodecTurn();
  QuickJsCodecTurn(const QuickJsCodecTurn&) = delete;
  auto operator=(const QuickJsCodecTurn&) -> QuickJsCodecTurn& = delete;

  // Consumer may synchronously call its owned handler. It owns any actual
  // handler result/error before returning. It must not stash/duplicate/export
  // these borrowed INPUT handles in C++, queue them, or release the realm.
  // JS itself may retain the arguments: the actual realm/heap owner persists.
  [[nodiscard]] auto with_encoded_call(const NativeRequest&, const ContextData&,
                                       BorrowedContextControls,
                                       const EncodedCallConsumer&)
      -> QuickJsCodecResult<void>;
  // Borrowed input stays owned by caller for this call. No JS pointer/value
  // escapes. Pending is data, not ownership/settlement. Native response copies
  // retain their charge and are independent of subsequent JS mutation/teardown.
  [[nodiscard]] auto inspect_return(JSValueConst)
      -> QuickJsCodecResult<InspectedReturn>;

 private:
  struct State;
  explicit QuickJsCodecTurn(std::unique_ptr<State>);
  std::unique_ptr<State> state;
};
} // namespace plinth::extension_http
