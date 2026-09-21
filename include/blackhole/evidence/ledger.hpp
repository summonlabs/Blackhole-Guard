#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "blackhole/core/bounded.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/policy.hpp"

namespace bhg {

/// Outcome of admitting one observation into the live ledger. Every rejection is
/// explicit; nothing is silently dropped or silently accepted.
enum class EvidenceAdmission : std::uint8_t {
  Accepted = 0,
  DuplicateAttempt = 1,
  Replayed = 2,
  Regressed = 3,
  RolloverAccepted = 4,
  StructurallyInvalid = 5,
  OutOfFreshnessWindow = 6,
  ClockSkewExceeded = 7,
  CapacityExhausted = 8,
};

constexpr bool is_valid_admission(std::uint8_t raw) noexcept { return raw <= 8u; }

constexpr const char* to_string(EvidenceAdmission a) noexcept {
  switch (a) {
    case EvidenceAdmission::Accepted: return "Accepted";
    case EvidenceAdmission::DuplicateAttempt: return "DuplicateAttempt";
    case EvidenceAdmission::Replayed: return "Replayed";
    case EvidenceAdmission::Regressed: return "Regressed";
    case EvidenceAdmission::RolloverAccepted: return "RolloverAccepted";
    case EvidenceAdmission::StructurallyInvalid: return "StructurallyInvalid";
    case EvidenceAdmission::OutOfFreshnessWindow: return "OutOfFreshnessWindow";
    case EvidenceAdmission::ClockSkewExceeded: return "ClockSkewExceeded";
    case EvidenceAdmission::CapacityExhausted: return "CapacityExhausted";
  }
  return "StructurallyInvalid";
}

/// A counter wrap is only believed when the previous value sat in the high half of
/// the counter space and the new value sits in the low half. A backward jump that
/// does not look like a wrap is a regression (replay) and is refused.
inline constexpr std::uint64_t kSequenceHalfSpace = 1ull << 63;
inline constexpr std::uint32_t kMaxRolloversPerSource = 4;

/// Per-source sequencing cursor. Sequences are monotonic per (source instance,
/// subject); the cursor is dynamic liveness and is never persisted.
struct SourceCursor {
  SourceRef source{};
  EvidenceSequence last_seq{};
  AttemptId last_attempt{};
  std::uint32_t rollovers{0};
  WallNs last_observed{};
  std::uint64_t observed{0};
  std::uint64_t accepted{0};
  std::uint64_t rejected{0};
  std::uint64_t duplicates{0};
  std::uint64_t regressed{0};

  [[nodiscard]] bool accounting_closed() const noexcept {
    return observed == accepted + rejected;
  }
};

/// Bounded live evidence state.
///
/// CRITICAL: this object is dynamic liveness. It is never persisted and never
/// reconstructed from durable records. After a restart the ledger starts empty and
/// prior evidence must be re-observed before it can influence authority again.
class EvidenceLedger {
 public:
  explicit EvidenceLedger(const FencePolicy& policy);

  /// Admits an observation. Returns the admission decision; the out-parameter
  /// carries a deterministic explanation code for every rejection.
  EvidenceAdmission admit(const DeliveryEvidence& e, WallNs now, ReasonCode& reason);

  /// Observations currently retained for a scope, in deterministic order.
  [[nodiscard]] std::vector<DeliveryEvidence> evidence_for(const Scope& scope) const;

  [[nodiscard]] std::size_t tracked_scopes() const noexcept { return scopes_.size(); }
  [[nodiscard]] std::size_t retained() const noexcept { return retained_; }
  [[nodiscard]] std::uint64_t seen() const noexcept { return seen_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] std::uint64_t admitted() const noexcept { return admitted_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

  /// Exact accounting closure:
  ///   seen == admitted + rejected   and   admitted == retained + dropped.
  [[nodiscard]] bool accounting_closed() const noexcept {
    return seen_ == admitted_ + rejected_ && admitted_ == retained_ + dropped_;
  }

  /// Drops every observation. Called on restart; durability never repopulates it.
  void reset() noexcept;

 private:
  struct ScopeState {
    std::vector<DeliveryEvidence> items;
    std::map<SourceRef, SourceCursor> cursors;
    std::uint64_t evictions{0};
  };

  // The policy is owned by value. Holding a reference here was a lifetime footgun:
  // a caller could bind it to a temporary and leave the ledger reading freed state.
  FencePolicy policy_;
  std::map<Scope, ScopeState> scopes_;
  std::size_t retained_{0};
  std::uint64_t seen_{0};
  std::uint64_t dropped_{0};
  std::uint64_t admitted_{0};
  std::uint64_t rejected_{0};
};

}  // namespace bhg
