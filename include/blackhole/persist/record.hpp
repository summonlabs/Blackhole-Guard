#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "blackhole/authority/authority.hpp"
#include "blackhole/core/canonical.hpp"
#include "blackhole/version.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/scope.hpp"

namespace bhg {

/// ============================================================================
/// Durable record framing
/// ============================================================================
///
/// Journal record layout (little endian):
///
///   offset 0   u32 magic        'B','H','G','J'
///   offset 4   u16 version      format version, must equal kFormatVersion
///   offset 6   u16 type         RecordType, validated against the enum domain
///   offset 8   u64 seq          record sequence, must be exactly previous + 1
///   offset 16  u32 payload_len  canonical payload length, bounded before use
///   offset 20  u32 header_crc   CRC-32C over bytes [0, 20)
///   offset 24  payload          payload_len bytes
///   offset 24+len u32 record_crc CRC-32C over bytes [0, 24 + len)
///
/// Total record size is 28 + payload_len. The sequence number sits inside the
/// header CRC so a reordered or spliced record cannot keep a valid header.

inline constexpr std::uint32_t kJournalMagic = 0x4A474842u;  // 'B','H','G','J'
inline constexpr std::uint32_t kSnapshotMagic = 0x53474842u;  // 'B','H','G','S'
inline constexpr std::size_t kJournalHeaderSize = 24;
inline constexpr std::size_t kJournalTrailerSize = 4;
inline constexpr std::size_t kSnapshotHeaderSize = 24;
inline constexpr std::size_t kSnapshotTrailerSize = 4;

enum class RecordType : std::uint16_t {
  Boot = 1,
  PolicyCommit = 2,
  DecisionCommit = 3,
  FenceIntentCommit = 4,
  FenceAckCommit = 5,
  FenceEffectCommit = 6,
  FenceRevokeCommit = 7,
  RestorationCommit = 8,
  InterruptionCommit = 9,
  EvidenceLineageCommit = 10,
  EpochAdvance = 11,
  Checkpoint = 12,
};

constexpr bool is_valid_record_type(std::uint16_t raw) noexcept { return raw >= 1u && raw <= 12u; }

constexpr const char* to_string(RecordType t) noexcept {
  switch (t) {
    case RecordType::Boot: return "Boot";
    case RecordType::PolicyCommit: return "PolicyCommit";
    case RecordType::DecisionCommit: return "DecisionCommit";
    case RecordType::FenceIntentCommit: return "FenceIntentCommit";
    case RecordType::FenceAckCommit: return "FenceAckCommit";
    case RecordType::FenceEffectCommit: return "FenceEffectCommit";
    case RecordType::FenceRevokeCommit: return "FenceRevokeCommit";
    case RecordType::RestorationCommit: return "RestorationCommit";
    case RecordType::InterruptionCommit: return "InterruptionCommit";
    case RecordType::EvidenceLineageCommit: return "EvidenceLineageCommit";
    case RecordType::EpochAdvance: return "EpochAdvance";
    case RecordType::Checkpoint: return "Checkpoint";
  }
  return "Unknown";
}

/// Encodes a full framed journal record into the writer.
void encode_record(Writer& w, RecordType type, RecordSequence seq,
                   std::span<const std::byte> payload) noexcept;

/// Decoded header view used by the scanner.
struct RecordHeader {
  std::uint16_t version{0};
  RecordType type{RecordType::Boot};
  RecordSequence seq{};
  std::uint32_t payload_len{0};
  std::uint32_t header_crc{0};
};

/// Decodes and validates a header from exactly kJournalHeaderSize bytes.
/// Returns Corrupt for a bad magic/CRC, Unsupported for an unknown version, and
/// Invalid for an out-of-domain type or an over-large payload.
Outcome decode_record_header(std::span<const std::byte> header, std::uint32_t max_payload,
                             RecordHeader& out) noexcept;

/// True when the supplied bytes are a valid *prefix* of a record header: a torn
/// tail in the journal. Checkable fields (magic, version, type, payload length)
/// must agree; unknowable fields are skipped.
bool is_valid_record_prefix(std::span<const std::byte> bytes, std::uint32_t max_payload) noexcept;

/// True when every byte is zero (a preallocated, never-written tail).
bool is_all_zero(std::span<const std::byte> bytes) noexcept;

// ---- payload codecs ------------------------------------------------------------

struct BootPayload {
  BootId boot{};
  IncarnationId incarnation{};
  CoordinatorEpoch epoch{};
  WallNs started_at{};
  std::uint64_t boot_count{0};
  std::uint64_t open_count{0};
  PolicyVersion policy{};
  std::uint64_t policy_fingerprint{0};
  Provenance provenance{Provenance::Synthetic};
};

struct InterruptionPayload {
  ReasonCode reason{ReasonCode::RestartFencedPriorAuthority};
  FenceId fence{};
  DecisionId decision{};
  Scope scope{};
  CoordinatorEpoch prior_epoch{};
  BootId prior_boot{};
  IncarnationId prior_incarnation{};
  WallNs at{};
};

struct FenceStatePayload {
  FenceIntent intent{};
  std::uint8_t lifecycle{0};
  bool effect_verified{false};
  EvidenceSourceId ack_owner{};
  EvidenceSourceId effect_owner{};
  WallNs acked_at{};
  WallNs effected_at{};
  ReasonCode terminal_reason{ReasonCode::None};
};

struct CheckpointPayload {
  RecordSequence snapshot_seq{};
  CoordinatorEpoch epoch{};
  std::uint64_t journal_generation{0};
};

void encode(Writer& w, const BootPayload& p) noexcept;
Outcome decode(Reader& r, BootPayload& p) noexcept;
void encode(Writer& w, const InterruptionPayload& p) noexcept;
Outcome decode(Reader& r, InterruptionPayload& p) noexcept;
void encode(Writer& w, const FenceStatePayload& p) noexcept;
Outcome decode(Reader& r, FenceStatePayload& p) noexcept;
void encode(Writer& w, const CheckpointPayload& p) noexcept;
Outcome decode(Reader& r, CheckpointPayload& p) noexcept;

}  // namespace bhg
