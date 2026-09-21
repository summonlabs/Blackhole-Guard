#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include "blackhole/core/canonical.hpp"
#include "blackhole/core/outcome.hpp"

namespace bhg {

/// ============================================================================
/// Bounded framed protocol
/// ============================================================================
///
/// Frame layout (little endian):
///   offset 0  u32 magic        'B','H','G','1'
///   offset 4  u16 version      protocol version
///   offset 6  u16 type         MessageType, validated against the enum domain
///   offset 8  u32 flags        reserved, must be zero
///   offset 12 u32 length       payload length, bounded BEFORE allocation
///   offset 16 u32 integrity    CRC-32C over header[0,16) with this field zeroed,
///                              followed by the payload
///   offset 20 payload
///
/// The declared length is checked against the configured maximum before any buffer
/// is sized, so an oversized frame is refused without allocating.
///
/// The integrity field is an error-detecting checksum, NOT a MAC. Transport is not
/// authenticated or encrypted; see the trust-boundary section of the README.

inline constexpr std::uint32_t kFrameMagic = 0x31474842u;  // 'B','H','G','1'
inline constexpr std::size_t kFrameHeaderSize = 20;
inline constexpr std::uint32_t kFrameFlagReservedMask = 0xFFFFFFFFu;

enum class MessageType : std::uint16_t {
  HelloRequest = 1,
  HelloResponse = 2,
  SubmitEvidenceRequest = 3,
  SubmitEvidenceResponse = 4,
  EvaluateRequest = 5,
  EvaluateResponse = 6,
  LocalizeRequest = 7,
  LocalizeResponse = 8,
  AckFenceRequest = 9,
  AckFenceResponse = 10,
  ReportEffectRequest = 11,
  ReportEffectResponse = 12,
  RestoreRequest = 13,
  RestoreResponse = 14,
  LineageRequest = 15,
  LineageResponse = 16,
  CheckpointRequest = 17,
  CheckpointResponse = 18,
  StatsRequest = 19,
  StatsResponse = 20,
  GoodbyeRequest = 21,
  GoodbyeResponse = 22,
  ErrorResponse = 23,
};

constexpr bool is_valid_message_type(std::uint16_t raw) noexcept {
  return raw >= 1u && raw <= 23u;
}

constexpr const char* to_string(MessageType t) noexcept {
  switch (t) {
    case MessageType::HelloRequest: return "HelloRequest";
    case MessageType::HelloResponse: return "HelloResponse";
    case MessageType::SubmitEvidenceRequest: return "SubmitEvidenceRequest";
    case MessageType::SubmitEvidenceResponse: return "SubmitEvidenceResponse";
    case MessageType::EvaluateRequest: return "EvaluateRequest";
    case MessageType::EvaluateResponse: return "EvaluateResponse";
    case MessageType::LocalizeRequest: return "LocalizeRequest";
    case MessageType::LocalizeResponse: return "LocalizeResponse";
    case MessageType::AckFenceRequest: return "AckFenceRequest";
    case MessageType::AckFenceResponse: return "AckFenceResponse";
    case MessageType::ReportEffectRequest: return "ReportEffectRequest";
    case MessageType::ReportEffectResponse: return "ReportEffectResponse";
    case MessageType::RestoreRequest: return "RestoreRequest";
    case MessageType::RestoreResponse: return "RestoreResponse";
    case MessageType::LineageRequest: return "LineageRequest";
    case MessageType::LineageResponse: return "LineageResponse";
    case MessageType::CheckpointRequest: return "CheckpointRequest";
    case MessageType::CheckpointResponse: return "CheckpointResponse";
    case MessageType::StatsRequest: return "StatsRequest";
    case MessageType::StatsResponse: return "StatsResponse";
    case MessageType::GoodbyeRequest: return "GoodbyeRequest";
    case MessageType::GoodbyeResponse: return "GoodbyeResponse";
    case MessageType::ErrorResponse: return "ErrorResponse";
  }
  return "Invalid";
}

struct Frame {
  MessageType type{MessageType::HelloRequest};
  std::uint32_t flags{0};
  std::vector<std::byte> payload;
};

/// Serializes a complete frame into the writer.
void encode_frame(Writer& w, const Frame& frame) noexcept;

/// Streaming decoder with sticky failure.
///
/// Feed arbitrary byte chunks; completed frames are produced in order. The first
/// structural error (bad magic, unsupported version, invalid type, non-zero flags,
/// oversized declared length, integrity mismatch, trailing bytes inside a declared
/// frame) latches permanently: the connection must be torn down, never resynced.
class FrameDecoder {
 public:
  explicit FrameDecoder(std::uint32_t max_frame_bytes = 1u << 20) noexcept
      : max_(max_frame_bytes) {}

  Outcome feed(std::span<const std::byte> bytes);
  /// Produces the next complete frame. Returns NotFound when more bytes are needed.
  Outcome next(Frame& out);
  Outcome finish() noexcept;

  [[nodiscard]] bool failed() const noexcept { return failure_ != Outcome::Ok; }
  [[nodiscard]] Outcome failure() const noexcept { return failure_; }
  [[nodiscard]] std::size_t buffered() const noexcept { return buffered_; }
  [[nodiscard]] std::uint32_t max_frame_bytes() const noexcept { return max_; }

 private:
  Outcome fail(Outcome o) noexcept {
    if (failure_ == Outcome::Ok) failure_ = o;
    return failure_;
  }

  std::uint32_t max_;
  std::vector<std::byte> buffer_;
  std::size_t buffered_{0};
  Outcome failure_{Outcome::Ok};
};

}  // namespace bhg
