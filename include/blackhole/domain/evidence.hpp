#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "blackhole/core/canonical.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/generation.hpp"
#include "blackhole/domain/scope.hpp"

namespace bhg {

enum class EvidenceKind : std::uint16_t {
  /// A delivery attempt (or batch of attempts) that succeeded end to end.
  DeliverySuccess = 1,
  /// A delivery attempt (or batch) that failed end to end.
  DeliveryFailure = 2,
  /// Passive loss measurement reported by the observing owner.
  LossMeasurement = 3,
  /// Congestion state reported by the congestion owner. Never computed here.
  CongestionSignal = 4,
  /// Partition/severance reported by the topology or link-state owner.
  PartitionSignal = 5,
  /// The path no longer exists structurally (withdrawn by its owner).
  StructuralWithdrawal = 6,
};

constexpr bool is_valid_evidence_kind(std::uint16_t raw) noexcept {
  return raw >= 1u && raw <= 6u;
}

constexpr const char* to_string(EvidenceKind k) noexcept {
  switch (k) {
    case EvidenceKind::DeliverySuccess: return "DeliverySuccess";
    case EvidenceKind::DeliveryFailure: return "DeliveryFailure";
    case EvidenceKind::LossMeasurement: return "LossMeasurement";
    case EvidenceKind::CongestionSignal: return "CongestionSignal";
    case EvidenceKind::PartitionSignal: return "PartitionSignal";
    case EvidenceKind::StructuralWithdrawal: return "StructuralWithdrawal";
  }
  return "Invalid";
}

/// How the observation was produced. Policy may refuse weak quality.
enum class EvidenceQuality : std::uint8_t {
  Exact = 0,
  Sampled = 1,
  Approximate = 2,
};

constexpr bool is_valid_quality(std::uint8_t raw) noexcept { return raw <= 2u; }

constexpr const char* to_string(EvidenceQuality q) noexcept {
  switch (q) {
    case EvidenceQuality::Exact: return "Exact";
    case EvidenceQuality::Sampled: return "Sampled";
    case EvidenceQuality::Approximate: return "Approximate";
  }
  return "Invalid";
}

/// Explicit reality label. Nothing labelled Synthetic may be presented as physical
/// hardware evidence, and nothing labelled Real was produced by a fixture.
enum class Provenance : std::uint8_t {
  Real = 0,
  Synthetic = 1,
  Unsupported = 2,
};

constexpr bool is_valid_provenance(std::uint8_t raw) noexcept { return raw <= 2u; }

constexpr const char* to_string(Provenance p) noexcept {
  switch (p) {
    case Provenance::Real: return "REAL";
    case Provenance::Synthetic: return "SYNTHETIC";
    case Provenance::Unsupported: return "UNSUPPORTED";
  }
  return "Invalid";
}

inline constexpr std::uint32_t kLossTotalPpm = 1000000u;

/// A single delivery-evidence record. This is an *observation*: it carries no
/// authority of its own and can never, by itself, authorize a fence.
struct DeliveryEvidence {
  EvidenceId id{};
  SourceRef source{};
  Scope scope{};
  GenerationVector gens{};
  EvidenceSequence seq{};
  AttemptId attempt{};
  EvidenceKind kind{EvidenceKind::DeliveryFailure};
  EvidenceQuality quality{EvidenceQuality::Exact};
  Provenance provenance{Provenance::Synthetic};

  std::uint64_t attempts{0};
  std::uint64_t failures{0};
  std::uint32_t loss_ppm{0};
  std::uint32_t rtt_us{0};

  WallNs observed_at{};
  FreshnessWindow fresh{};

  /// Optional reference to the adjacent owner that reported congestion/partition
  /// state, so the explanation can point at the authority rather than paraphrase it.
  EvidenceSourceId external_owner{};

  [[nodiscard]] bool structurally_valid(std::string& why) const;
  [[nodiscard]] bool is_complete_delivery_failure() const noexcept {
    return failures > 0 && failures == attempts && loss_ppm >= kLossTotalPpm;
  }
  [[nodiscard]] bool is_clean_delivery_success() const noexcept {
    return kind == EvidenceKind::DeliverySuccess && failures == 0 && attempts > 0 &&
           loss_ppm == 0;
  }
  [[nodiscard]] bool is_fresh_at(WallNs t) const noexcept { return fresh.is_fresh_at(t); }
};

void encode(Writer& w, const DeliveryEvidence& e) noexcept;
Outcome decode(Reader& r, DeliveryEvidence& e) noexcept;

/// Deterministic identity for an evidence record: derived from its canonical bytes so
/// that two structurally identical records have the same id on any host.
EvidenceId derive_evidence_id(const DeliveryEvidence& e);

/// Reason codes. Deterministic, bounded, and part of the public explanation surface.
enum class ReasonCode : std::uint16_t {
  None = 0,

  NoEvidenceObserved = 1,
  OnlyStaleEvidence = 2,
  GenerationMismatch = 3,
  SourceIncarnationChanged = 4,
  FreshnessWindowExpired = 5,
  ConflictingEvidence = 6,
  InvalidEvidenceRejected = 7,
  ReplayedSequence = 8,
  RegressedSequence = 9,
  SequenceRollover = 10,
  DuplicateEvidence = 11,
  WindowViolation = 12,
  ClockSkewExceeded = 13,

  CompleteDeliveryFailure = 20,
  PartialLossObserved = 21,
  SevereLossObserved = 22,
  CongestionExplainsLoss = 23,
  PartitionExplainsLoss = 24,
  StructurallyWithdrawn = 25,
  QualityBelowPolicy = 26,

  CorroborationInsufficient = 40,
  CorroborationSatisfied = 41,
  SourcesInsufficient = 42,
  IncarnationsInsufficient = 43,
  AttemptsInsufficient = 44,
  PolicyRefused = 45,

  FenceIntentIssued = 60,
  FenceIntentRevoked = 61,
  FenceExpired = 62,
  FenceAcknowledged = 63,
  FenceEffectVerified = 64,
  FenceEffectUnverified = 65,
  RestartFencedPriorAuthority = 66,
  DependencyGenerationChanged = 67,

  RestorationEvidenceFresh = 80,
  RestorationCorroborated = 81,
  RestorationRefusedNoEvidence = 82,
  RestorationRefusedStale = 83,
  RestorationRefusedConflict = 84,
  RestorationRefusedInsufficient = 85,

  LocalizationResolved = 100,
  LocalizationAmbiguous = 101,
  LocalizationInfeasible = 102,
  LocalizationSearchLimit = 103,
  LocalizationInvalid = 104,

  BudgetExhausted = 120,
  UnsupportedInput = 121,
  InternalInvariantViolation = 122,
};

constexpr bool is_valid_reason_code(std::uint16_t raw) noexcept {
  switch (raw) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 9:
    case 10: case 11: case 12: case 13:
    case 20: case 21: case 22: case 23: case 24: case 25: case 26:
    case 40: case 41: case 42: case 43: case 44: case 45:
    case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67:
    case 80: case 81: case 82: case 83: case 84: case 85:
    case 100: case 101: case 102: case 103: case 104:
    case 120: case 121: case 122:
      return true;
    default:
      return false;
  }
}

const char* to_string(ReasonCode c) noexcept;

}  // namespace bhg
