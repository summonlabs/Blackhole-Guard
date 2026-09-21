#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/classification.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/generation.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/domain/scope.hpp"

namespace bhg {

/// Deterministic census of the evidence considered for one subject. Every count is
/// reported so an explanation can show exactly what was and was not usable.
struct EvidenceCensus {
  std::uint32_t total{0};
  std::uint32_t fresh_current{0};
  std::uint32_t stale{0};
  std::uint32_t generation_mismatch{0};
  std::uint32_t quality_rejected{0};
  std::uint32_t invalid{0};

  std::uint32_t complete_failures{0};
  std::uint32_t clean_successes{0};
  std::uint32_t congestion_signals{0};
  std::uint32_t partition_signals{0};
  std::uint32_t withdrawals{0};

  std::uint32_t failure_sources{0};
  std::uint32_t failure_incarnations{0};
  std::uint32_t success_sources{0};
  std::uint32_t success_incarnations{0};

  std::uint64_t failure_attempts{0};
  std::uint64_t failure_losses{0};
  std::uint64_t success_attempts{0};

  std::uint32_t min_loss_ppm{0};
  std::uint32_t max_loss_ppm{0};

  GenMismatch mismatch{GenMismatch::None};
};

struct Diagnosis {
  Outcome outcome{Outcome::NoEvidence};
  Classification classification{Classification::NoEvidence};
  ReasonCode primary_reason{ReasonCode::NoEvidenceObserved};
  std::vector<ReasonCode> reasons;

  Scope scope{};
  GenerationVector gens{};
  EvidenceCensus census{};
  WallNs decided_at{};

  /// Policy corroboration satisfied for fencing. Only meaningful when the
  /// classification is Blackhole; false in every other case.
  bool corroborated{false};
  ReasonCode corroboration_reason{ReasonCode::CorroborationInsufficient};

  /// Deterministic confidence in parts per million, derived from the census and
  /// the corroboration result. Never used as authority.
  std::uint32_t confidence_ppm{0};

  /// Sources that contributed usable fresh generation-matched failure evidence.
  std::vector<SourceRef> failure_sources;
  std::vector<SourceRef> success_sources;
};

struct ClassifyRequest {
  Scope scope{};
  GenerationVector current{};
  WallNs now{};
  const FencePolicy* policy{nullptr};
  std::span<const DeliveryEvidence> evidence{};
};

/// Classifies delivery state for a subject.
///
/// Rules, in strict precedence order:
///   1. No observation at all                      -> NO_EVIDENCE / NoEvidence
///   2. Nothing fresh under the current generation -> UNKNOWN / Stale|Unknown
///   3. A fresh structural withdrawal              -> UNKNOWN / Unsupported
///   4. Fresh partition signal                     -> PARTITIONED
///   5. Fresh congestion signal                    -> CONGESTED
///   6. Fresh success and fresh complete failure   -> UNKNOWN / Conflict
///   7. Fresh complete failure only                -> BLACKHOLE
///   8. Fresh partial loss only                    -> LOSSY
///   9. Fresh clean success only                   -> HEALTHY
///
/// A blackhole is therefore never inferred from ambiguity: staleness, generation
/// mismatch, quality refusal, contradiction and structural withdrawal all degrade
/// to UNKNOWN or to the owner-supplied explanation.
Outcome classify(const ClassifyRequest& req, Diagnosis& out);

/// Checks the fencing corroboration requirements of the policy against a
/// diagnosis. Returns the decisive ReasonCode through the out-parameter.
bool evaluate_corroboration(const Diagnosis& d, const FencePolicy& policy, ReasonCode& reason);

/// Canonical, stable fingerprint of the diagnosis content. Two diagnoses that
/// agree on every field used for authority produce the same fingerprint.
std::uint64_t diagnosis_fingerprint(const Diagnosis& d);

}  // namespace bhg
