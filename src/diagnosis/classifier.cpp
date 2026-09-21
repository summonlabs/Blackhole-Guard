#include "blackhole/diagnosis/classifier.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "blackhole/core/hash.hpp"

namespace bhg {

namespace {

void add_reason(Diagnosis& d, ReasonCode c, std::uint32_t max_reasons) {
  if (d.reasons.size() >= max_reasons) return;
  if (std::find(d.reasons.begin(), d.reasons.end(), c) != d.reasons.end()) return;
  d.reasons.push_back(c);
}

bool is_loss_kind(EvidenceKind k) noexcept {
  return k == EvidenceKind::DeliverySuccess || k == EvidenceKind::DeliveryFailure ||
         k == EvidenceKind::LossMeasurement;
}

}  // namespace

Outcome classify(const ClassifyRequest& req, Diagnosis& out) {
  out = Diagnosis{};
  out.scope = req.scope;
  out.gens = req.current;
  out.decided_at = req.now;

  if (req.policy == nullptr) {
    out.outcome = Outcome::Invalid;
    out.classification = Classification::Unknown;
    out.primary_reason = ReasonCode::UnsupportedInput;
    add_reason(out, ReasonCode::UnsupportedInput, 8);
    return Outcome::Invalid;
  }
  const FencePolicy& policy = *req.policy;
  const std::uint32_t max_reasons = policy.max_reasons_per_decision;
  if (!req.scope.is_valid() || !req.current.is_complete()) {
    out.outcome = Outcome::Invalid;
    out.classification = Classification::Unknown;
    out.primary_reason = ReasonCode::UnsupportedInput;
    add_reason(out, ReasonCode::UnsupportedInput, max_reasons);
    return Outcome::Invalid;
  }

  std::set<SourceRef> failure_sources;
  std::set<SourceRef> success_sources;
  std::map<SourceRef, std::uint64_t> attempts_by_source;

  bool any_quality_rejected = false;
  bool any_invalid = false;
  GenMismatch worst_mismatch = GenMismatch::None;

  out.census.min_loss_ppm = kLossTotalPpm;
  out.census.max_loss_ppm = 0;

  for (const DeliveryEvidence& e : req.evidence) {
    ++out.census.total;
    std::string why;
    if (!e.structurally_valid(why)) {
      ++out.census.invalid;
      any_invalid = true;
      continue;
    }
    if (e.gens != req.current) {
      ++out.census.generation_mismatch;
      const GenMismatch m = classify_mismatch(e.gens, req.current);
      if (worst_mismatch == GenMismatch::None) {
        worst_mismatch = m;
      } else if (worst_mismatch != m) {
        worst_mismatch = GenMismatch::Multiple;
      }
      continue;
    }
    if (!e.fresh.is_fresh_at(req.now)) {
      ++out.census.stale;
      continue;
    }
    if (policy.require_exact_quality && e.quality != EvidenceQuality::Exact) {
      ++out.census.quality_rejected;
      any_quality_rejected = true;
      continue;
    }
    ++out.census.fresh_current;

    switch (e.kind) {
      case EvidenceKind::StructuralWithdrawal:
        ++out.census.withdrawals;
        break;
      case EvidenceKind::CongestionSignal:
        ++out.census.congestion_signals;
        break;
      case EvidenceKind::PartitionSignal:
        ++out.census.partition_signals;
        break;
      case EvidenceKind::DeliverySuccess:
      case EvidenceKind::DeliveryFailure:
      case EvidenceKind::LossMeasurement: {
        out.census.min_loss_ppm = std::min(out.census.min_loss_ppm, e.loss_ppm);
        out.census.max_loss_ppm = std::max(out.census.max_loss_ppm, e.loss_ppm);
        const bool complete = e.loss_ppm >= policy.complete_failure_ppm && e.failures > 0 &&
                              e.failures == e.attempts;
        const bool clean = e.failures == 0 && e.attempts > 0 && e.loss_ppm == 0;
        if (complete) {
          ++out.census.complete_failures;
          out.census.failure_attempts += e.attempts;
          out.census.failure_losses += e.failures;
          failure_sources.insert(e.source);
          attempts_by_source[e.source] += e.attempts;
        } else if (clean) {
          ++out.census.clean_successes;
          out.census.success_attempts += e.attempts;
          success_sources.insert(e.source);
        }
        break;
      }
    }
  }

  out.census.mismatch = worst_mismatch;
  out.census.failure_sources = static_cast<std::uint32_t>(failure_sources.size());
  out.census.success_sources = static_cast<std::uint32_t>(success_sources.size());
  {
    std::set<IncarnationId> incs;
    for (const SourceRef& s : failure_sources) incs.insert(s.incarnation);
    out.census.failure_incarnations = static_cast<std::uint32_t>(incs.size());
  }
  {
    std::set<IncarnationId> incs;
    for (const SourceRef& s : success_sources) incs.insert(s.incarnation);
    out.census.success_incarnations = static_cast<std::uint32_t>(incs.size());
  }
  out.failure_sources.assign(failure_sources.begin(), failure_sources.end());
  out.success_sources.assign(success_sources.begin(), success_sources.end());

  // 1. No observation at all.
  if (out.census.total == 0) {
    out.outcome = Outcome::NoEvidence;
    out.classification = Classification::NoEvidence;
    out.primary_reason = ReasonCode::NoEvidenceObserved;
    add_reason(out, ReasonCode::NoEvidenceObserved, max_reasons);
    return out.outcome;
  }

  // 2. Nothing usable under the current generation.
  if (out.census.fresh_current == 0) {
    out.classification = Classification::Unknown;
    if (out.census.stale > 0 && out.census.generation_mismatch == 0) {
      out.outcome = Outcome::Stale;
      out.primary_reason = ReasonCode::OnlyStaleEvidence;
      add_reason(out, ReasonCode::OnlyStaleEvidence, max_reasons);
      add_reason(out, ReasonCode::FreshnessWindowExpired, max_reasons);
    } else if (out.census.generation_mismatch > 0) {
      out.outcome = Outcome::Stale;
      out.primary_reason = ReasonCode::GenerationMismatch;
      add_reason(out, ReasonCode::GenerationMismatch, max_reasons);
    } else if (out.census.quality_rejected > 0) {
      out.outcome = Outcome::Unknown;
      out.primary_reason = ReasonCode::QualityBelowPolicy;
      add_reason(out, ReasonCode::QualityBelowPolicy, max_reasons);
    } else {
      out.outcome = Outcome::Unknown;
      out.primary_reason = ReasonCode::InvalidEvidenceRejected;
      add_reason(out, any_invalid ? ReasonCode::InvalidEvidenceRejected
                                  : ReasonCode::OnlyStaleEvidence,
                 max_reasons);
    }
    if (any_quality_rejected) add_reason(out, ReasonCode::QualityBelowPolicy, max_reasons);
    return out.outcome;
  }

  // 3. Structural withdrawal: the subject no longer exists as a path. This is not a
  //    delivery blackhole and must never be fenced as one.
  if (out.census.withdrawals > 0 && out.census.clean_successes == 0) {
    out.outcome = Outcome::Unsupported;
    out.classification = Classification::Unknown;
    out.primary_reason = ReasonCode::StructurallyWithdrawn;
    add_reason(out, ReasonCode::StructurallyWithdrawn, max_reasons);
    return out.outcome;
  }

  // 4. Partition: the topology/link-state owner has severed the subject.
  if (out.census.partition_signals > 0) {
    out.outcome = Outcome::Ok;
    out.classification = Classification::Partitioned;
    out.primary_reason = ReasonCode::PartitionExplainsLoss;
    add_reason(out, ReasonCode::PartitionExplainsLoss, max_reasons);
    return out.outcome;
  }

  // 5. Congestion: severe or complete loss that the congestion owner accounts for.
  if (out.census.congestion_signals > 0) {
    out.outcome = Outcome::Ok;
    out.classification = Classification::Congested;
    out.primary_reason = ReasonCode::CongestionExplainsLoss;
    add_reason(out, ReasonCode::CongestionExplainsLoss, max_reasons);
    return out.outcome;
  }

  // 6. Contradiction: fresh clean success and fresh complete failure coexist under
  //    the same generation vector. Ambiguity is never promoted to blackhole.
  if (out.census.complete_failures > 0 && out.census.clean_successes > 0) {
    out.outcome = Outcome::Conflict;
    out.classification = Classification::Unknown;
    out.primary_reason = ReasonCode::ConflictingEvidence;
    add_reason(out, ReasonCode::ConflictingEvidence, max_reasons);
    return out.outcome;
  }

  // 7. Complete delivery failure with no competing explanation.
  if (out.census.complete_failures > 0) {
    out.outcome = Outcome::Ok;
    out.classification = Classification::Blackhole;
    out.primary_reason = ReasonCode::CompleteDeliveryFailure;
    add_reason(out, ReasonCode::CompleteDeliveryFailure, max_reasons);
    out.corroborated = evaluate_corroboration(out, policy, out.corroboration_reason);
    add_reason(out, out.corroboration_reason, max_reasons);
    const std::uint32_t src = out.census.failure_sources;
    const std::uint32_t need = policy.min_corroborating_sources;
    out.confidence_ppm =
        src >= need ? kLossTotalPpm
                    : static_cast<std::uint32_t>(
                          (static_cast<std::uint64_t>(src) * kLossTotalPpm) / need);
    return out.outcome;
  }

  // 8. Partial loss only.
  if (out.census.max_loss_ppm > 0) {
    out.outcome = Outcome::Ok;
    out.classification = Classification::Lossy;
    out.primary_reason = out.census.max_loss_ppm >= policy.severe_loss_ppm
                             ? ReasonCode::SevereLossObserved
                             : ReasonCode::PartialLossObserved;
    add_reason(out, out.primary_reason, max_reasons);
    out.confidence_ppm = out.census.max_loss_ppm;
    return out.outcome;
  }

  // 9. Clean success.
  out.outcome = Outcome::Ok;
  out.classification = Classification::Healthy;
  out.primary_reason = ReasonCode::RestorationEvidenceFresh;
  add_reason(out, ReasonCode::RestorationEvidenceFresh, max_reasons);
  out.confidence_ppm = kLossTotalPpm;
  return out.outcome;
}

bool evaluate_corroboration(const Diagnosis& d, const FencePolicy& policy, ReasonCode& reason) {
  reason = ReasonCode::CorroborationInsufficient;
  if (d.classification != Classification::Blackhole) {
    reason = ReasonCode::PolicyRefused;
    return false;
  }
  if (d.outcome != Outcome::Ok) {
    reason = ReasonCode::PolicyRefused;
    return false;
  }
  if (d.census.failure_sources < policy.min_corroborating_sources) {
    reason = ReasonCode::SourcesInsufficient;
    return false;
  }
  if (d.census.failure_incarnations < policy.min_distinct_incarnations) {
    reason = ReasonCode::IncarnationsInsufficient;
    return false;
  }
  if (d.census.failure_attempts < policy.min_total_attempts) {
    reason = ReasonCode::AttemptsInsufficient;
    return false;
  }
  if (policy.require_full_generation_agreement && d.census.generation_mismatch > 0) {
    reason = ReasonCode::GenerationMismatch;
    return false;
  }
  reason = ReasonCode::CorroborationSatisfied;
  return true;
}

std::uint64_t diagnosis_fingerprint(const Diagnosis& d) {
  Writer w(1024);
  w.u8(static_cast<std::uint8_t>(d.outcome));
  w.u16(static_cast<std::uint16_t>(d.classification));
  w.u16(static_cast<std::uint16_t>(d.primary_reason));
  encode(w, d.scope);
  encode(w, d.gens);
  w.u32(d.census.total);
  w.u32(d.census.fresh_current);
  w.u32(d.census.complete_failures);
  w.u32(d.census.clean_successes);
  w.u32(d.census.congestion_signals);
  w.u32(d.census.partition_signals);
  w.u32(d.census.withdrawals);
  w.u32(d.census.failure_sources);
  w.u32(d.census.failure_incarnations);
  w.u64(d.census.failure_attempts);
  for (const SourceRef& s : d.failure_sources) encode(w, s);
  if (!w.ok()) return 0;
  return fnv1a64(w.span());
}

}  // namespace bhg
