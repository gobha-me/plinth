// SPDX-License-Identifier: MIT
#pragma once
#include "kernel/extension_http/codec.hpp"
#include "kernel/extension_http/ownership.hpp"

#include <array>
#include <memory>
#include <span>
#include <variant>

namespace plinth::extension_http {
// Pure protocol VALUES. Neither receipt/peer/clock provenance nor permission.
struct HandshakeIdentity {
  std::string owner_epoch, adapter_epoch, clock_domain_id;
  Extension extension;
  auto operator==(const HandshakeIdentity&) const -> bool = default;
};
struct FrameIdentity {
  std::string owner_epoch, adapter_epoch, dispatch_id, generation_receipt_id;
  auto operator==(const FrameIdentity&) const -> bool = default;
};
struct Hello {
  HandshakeIdentity identity;
  auto operator==(const Hello&) const -> bool = default;
};
struct HelloAck {
  HandshakeIdentity identity;
  auto operator==(const HelloAck&) const -> bool = default;
};
struct Call {
  FrameIdentity identity;
  NativeRequest request;
  ContextData context;
  auto operator==(const Call&) const -> bool = default;
};
struct Accepted {
  FrameIdentity identity;
  auto operator==(const Accepted&) const -> bool = default;
};
struct Entered {
  FrameIdentity identity;
  std::uint64_t entered_at_ns = 0;
  auto operator==(const Entered&) const -> bool = default;
};
using FrameOutcome = std::variant<NativeResponse, AdapterFailure>;
struct Result {
  FrameIdentity identity;
  FrameOutcome outcome;
  auto operator==(const Result&) const -> bool = default;
};
struct Cancel {
  FrameIdentity identity;
  CancelReason reason = CancelReason::DEADLINE;
  auto operator==(const Cancel&) const -> bool = default;
};
enum class SettlementReason { COMPLETED, CANCELLED, FAILED };
enum class PeerEntryState { NOT_ENTERED, ENTERED };
struct Settlement {
  SettlementReason reason = SettlementReason::COMPLETED;
  PeerEntryState entry_state = PeerEntryState::NOT_ENTERED;
  std::optional<std::uint64_t> entered_at_ns;
  auto operator==(const Settlement&) const -> bool = default;
};
struct Settled {
  FrameIdentity identity;
  Settlement settlement;
  auto operator==(const Settled&) const -> bool = default;
};
using Frame = std::variant<Hello, HelloAck, Call, Accepted, Entered, Result,
                           Cancel, Settled>;
enum class FrameError { INVALID, TOO_LARGE, RESOURCE_UNAVAILABLE };
template <typename T> using FrameResult = std::expected<T, FrameError>;

// No allocation. Header length excludes its four bytes; zero/>12MiB refuse.
[[nodiscard]] auto decode_frame_length(std::array<std::uint8_t, 4> header)
    -> FrameResult<std::uint32_t>;

class FramePayload {
 public:
  FramePayload(FramePayload&&) noexcept = default;
  auto operator=(FramePayload&&) noexcept -> FramePayload&;
  FramePayload(const FramePayload&) = delete;
  auto operator=(const FramePayload&) -> FramePayload& = delete;
  // Capacity is not proof bytes were received. The future owned socket loop
  // must fill exactly this span before passing its read view to the parser.
  [[nodiscard]] auto writable() noexcept -> std::span<std::uint8_t>;
  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::uint8_t>;

 private:
  friend auto allocate_frame_payload(std::array<std::uint8_t, 4>,
                                     std::shared_ptr<AdmissionLease>)
      -> FrameResult<FramePayload>;
  FramePayload(Binary, std::shared_ptr<AdmissionLease>, ExtraMemoryCharge);
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge charge;
  Binary buffer;
};
class OwnedFrame {
 public:
  OwnedFrame(OwnedFrame&&) noexcept = default;
  auto operator=(OwnedFrame&&) noexcept -> OwnedFrame&;
  OwnedFrame(const OwnedFrame&) = delete;
  auto operator=(const OwnedFrame&) -> OwnedFrame& = delete;
  [[nodiscard]] auto value() const noexcept -> const Frame&;

 private:
  friend auto parse_frame_payload(std::string_view,
                                  std::shared_ptr<AdmissionLease>)
      -> FrameResult<OwnedFrame>;
  OwnedFrame(Frame, std::shared_ptr<AdmissionLease>, ExtraMemoryCharge);
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge charge;
  Frame frame;
};
class EncodedFrame {
 public:
  EncodedFrame(EncodedFrame&&) noexcept = default;
  auto operator=(EncodedFrame&&) noexcept -> EncodedFrame&;
  EncodedFrame(const EncodedFrame&) = delete;
  auto operator=(const EncodedFrame&) -> EncodedFrame& = delete;
  [[nodiscard]] auto bytes() const noexcept -> std::span<const std::uint8_t>;

 private:
  friend auto encode_frame(const Frame&, std::shared_ptr<AdmissionLease>)
      -> FrameResult<EncodedFrame>;
  EncodedFrame(Binary, std::shared_ptr<AdmissionLease>, ExtraMemoryCharge);
  std::shared_ptr<AdmissionLease> admission;
  ExtraMemoryCharge charge;
  Binary buffer;
};
[[nodiscard]] auto allocate_frame_payload(std::array<std::uint8_t, 4>,
                                          std::shared_ptr<AdmissionLease>)
    -> FrameResult<FramePayload>;
// Borrowed input must remain caller-owned AND charged for the synchronous call.
// Every allocating API, including handshake/control frames, requires admission.
[[nodiscard]] auto parse_frame_payload(std::string_view,
                                       std::shared_ptr<AdmissionLease>)
    -> FrameResult<OwnedFrame>;
[[nodiscard]] auto parse_framed_buffer(std::span<const std::uint8_t>,
                                       std::shared_ptr<AdmissionLease>)
    -> FrameResult<OwnedFrame>;
[[nodiscard]] auto encode_frame(const Frame&, std::shared_ptr<AdmissionLease>)
    -> FrameResult<EncodedFrame>;
// Reservation envelopes: parser 3*payload+12MiB workspace +8MiB+256KiB retained
// DTO, serializer28MiB workspace +12MiB+4 output, caller's existing inputs
// extra. They cover bounded bulk copies, not measured allocator/RSS or a proof
// that a full adapter's concurrent buffers fit. All concurrent copies reserve
// on the SAME admitted owner. No I/O, queues, endpoint, retry, temp files or
// timers.
} // namespace plinth::extension_http
