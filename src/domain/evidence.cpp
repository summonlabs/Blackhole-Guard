#include "blackhole/domain/evidence.hpp"

#include "blackhole/core/hash.hpp"

namespace bhg {

bool DeliveryEvidence::structurally_valid(std::string& why) const {
  why.clear();
  if (id.is_nil()) { why = "evidence id must be non-zero"; return false; }
  if (!source.is_valid()) { why = "source reference incomplete"; return false; }
  if (!scope.is_valid()) { why = "scope invalid for its kind"; return false; }
  if (!gens.is_complete()) { why = "generation vector incomplete"; return false; }
  if (seq.is_nil()) { why = "sequence must be non-zero"; return false; }
  if (!is_valid_evidence_kind(static_cast<std::uint16_t>(kind))) { why = "evidence kind"; return false; }
  if (!is_valid_quality(static_cast<std::uint8_t>(quality))) { why = "quality"; return false; }
  if (!is_valid_provenance(static_cast<std::uint8_t>(provenance))) { why = "provenance"; return false; }
  if (loss_ppm > kLossTotalPpm) { why = "loss_ppm out of range"; return false; }
  if (failures > attempts) { why = "failures exceed attempts"; return false; }
  if (observed_at.is_zero()) { why = "observed_at is zero"; return false; }
  if (fresh.is_degenerate()) { why = "freshness window degenerate"; return false; }
  if (fresh.close <= observed_at) { why = "freshness window closes at or before observation"; return false; }

  switch (kind) {
    case EvidenceKind::DeliverySuccess:
    case EvidenceKind::DeliveryFailure:
      if (attempts == 0) { why = "delivery evidence must summarize at least one attempt"; return false; }
      break;
    case EvidenceKind::LossMeasurement:
      if (attempts == 0) { why = "loss measurement must summarize at least one sample"; return false; }
      break;
    case EvidenceKind::CongestionSignal:
    case EvidenceKind::PartitionSignal:
      if (external_owner.is_nil()) { why = "external owner reference required"; return false; }
      break;
    case EvidenceKind::StructuralWithdrawal:
      break;
  }
  return true;
}

void encode(Writer& w, const DeliveryEvidence& e) noexcept {
  encode(w, e.id);
  encode(w, e.source);
  encode(w, e.scope);
  encode(w, e.gens);
  encode(w, e.seq);
  encode(w, e.attempt);
  w.u16(static_cast<std::uint16_t>(e.kind));
  w.u8(static_cast<std::uint8_t>(e.quality));
  w.u8(static_cast<std::uint8_t>(e.provenance));
  w.u64(e.attempts);
  w.u64(e.failures);
  w.u32(e.loss_ppm);
  w.u32(e.rtt_us);
  encode(w, e.observed_at);
  encode(w, e.fresh);
  encode(w, e.external_owner);
}

Outcome decode(Reader& r, DeliveryEvidence& e) noexcept {
  DeliveryEvidence out{};
  Outcome o = decode(r, out.id);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.source);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.seq);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.attempt);
  if (!is_affirmative(o)) return o;
  std::uint16_t kind_raw = 0;
  o = r.u16(kind_raw);
  if (!is_affirmative(o)) return o;
  if (!is_valid_evidence_kind(kind_raw)) return Outcome::Invalid;
  out.kind = static_cast<EvidenceKind>(kind_raw);
  std::uint8_t quality_raw = 0;
  o = r.u8(quality_raw);
  if (!is_affirmative(o)) return o;
  if (!is_valid_quality(quality_raw)) return Outcome::Invalid;
  out.quality = static_cast<EvidenceQuality>(quality_raw);
  std::uint8_t prov_raw = 0;
  o = r.u8(prov_raw);
  if (!is_affirmative(o)) return o;
  if (!is_valid_provenance(prov_raw)) return Outcome::Invalid;
  out.provenance = static_cast<Provenance>(prov_raw);
  o = r.u64(out.attempts);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.failures);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.loss_ppm);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.rtt_us);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.observed_at);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.fresh);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.external_owner);
  if (!is_affirmative(o)) return o;

  std::string why;
  if (!out.structurally_valid(why)) return Outcome::Invalid;
  e = out;
  return Outcome::Ok;
}

EvidenceId derive_evidence_id(const DeliveryEvidence& e) {
  DeliveryEvidence copy = e;
  copy.id = EvidenceId{};
  Writer w(1024);
  encode(w, copy);
  if (!w.ok()) return EvidenceId{};
  return EvidenceId{fnv1a64(w.span())};
}

const char* to_string(ReasonCode c) noexcept {
  switch (c) {
    case ReasonCode::None: return "NONE";
    case ReasonCode::NoEvidenceObserved: return "NO_EVIDENCE_OBSERVED";
    case ReasonCode::OnlyStaleEvidence: return "ONLY_STALE_EVIDENCE";
    case ReasonCode::GenerationMismatch: return "GENERATION_MISMATCH";
    case ReasonCode::SourceIncarnationChanged: return "SOURCE_INCARNATION_CHANGED";
    case ReasonCode::FreshnessWindowExpired: return "FRESHNESS_WINDOW_EXPIRED";
    case ReasonCode::ConflictingEvidence: return "CONFLICTING_EVIDENCE";
    case ReasonCode::InvalidEvidenceRejected: return "INVALID_EVIDENCE_REJECTED";
    case ReasonCode::ReplayedSequence: return "REPLAYED_SEQUENCE";
    case ReasonCode::RegressedSequence: return "REGRESSED_SEQUENCE";
    case ReasonCode::SequenceRollover: return "SEQUENCE_ROLLOVER";
    case ReasonCode::DuplicateEvidence: return "DUPLICATE_EVIDENCE";
    case ReasonCode::WindowViolation: return "WINDOW_VIOLATION";
    case ReasonCode::ClockSkewExceeded: return "CLOCK_SKEW_EXCEEDED";
    case ReasonCode::CompleteDeliveryFailure: return "COMPLETE_DELIVERY_FAILURE";
    case ReasonCode::PartialLossObserved: return "PARTIAL_LOSS_OBSERVED";
    case ReasonCode::SevereLossObserved: return "SEVERE_LOSS_OBSERVED";
    case ReasonCode::CongestionExplainsLoss: return "CONGESTION_EXPLAINS_LOSS";
    case ReasonCode::PartitionExplainsLoss: return "PARTITION_EXPLAINS_LOSS";
    case ReasonCode::StructurallyWithdrawn: return "STRUCTURALLY_WITHDRAWN";
    case ReasonCode::QualityBelowPolicy: return "QUALITY_BELOW_POLICY";
    case ReasonCode::CorroborationInsufficient: return "CORROBORATION_INSUFFICIENT";
    case ReasonCode::CorroborationSatisfied: return "CORROBORATION_SATISFIED";
    case ReasonCode::SourcesInsufficient: return "SOURCES_INSUFFICIENT";
    case ReasonCode::IncarnationsInsufficient: return "INCARNATIONS_INSUFFICIENT";
    case ReasonCode::AttemptsInsufficient: return "ATTEMPTS_INSUFFICIENT";
    case ReasonCode::PolicyRefused: return "POLICY_REFUSED";
    case ReasonCode::FenceIntentIssued: return "FENCE_INTENT_ISSUED";
    case ReasonCode::FenceIntentRevoked: return "FENCE_INTENT_REVOKED";
    case ReasonCode::FenceExpired: return "FENCE_EXPIRED";
    case ReasonCode::FenceAcknowledged: return "FENCE_ACKNOWLEDGED";
    case ReasonCode::FenceEffectVerified: return "FENCE_EFFECT_VERIFIED";
    case ReasonCode::FenceEffectUnverified: return "FENCE_EFFECT_UNVERIFIED";
    case ReasonCode::RestartFencedPriorAuthority: return "RESTART_FENCED_PRIOR_AUTHORITY";
    case ReasonCode::DependencyGenerationChanged: return "DEPENDENCY_GENERATION_CHANGED";
    case ReasonCode::RestorationEvidenceFresh: return "RESTORATION_EVIDENCE_FRESH";
    case ReasonCode::RestorationCorroborated: return "RESTORATION_CORROBORATED";
    case ReasonCode::RestorationRefusedNoEvidence: return "RESTORATION_REFUSED_NO_EVIDENCE";
    case ReasonCode::RestorationRefusedStale: return "RESTORATION_REFUSED_STALE";
    case ReasonCode::RestorationRefusedConflict: return "RESTORATION_REFUSED_CONFLICT";
    case ReasonCode::RestorationRefusedInsufficient: return "RESTORATION_REFUSED_INSUFFICIENT";
    case ReasonCode::LocalizationResolved: return "LOCALIZATION_RESOLVED";
    case ReasonCode::LocalizationAmbiguous: return "LOCALIZATION_AMBIGUOUS";
    case ReasonCode::LocalizationInfeasible: return "LOCALIZATION_INFEASIBLE";
    case ReasonCode::LocalizationSearchLimit: return "LOCALIZATION_SEARCH_LIMIT";
    case ReasonCode::LocalizationInvalid: return "LOCALIZATION_INVALID";
    case ReasonCode::BudgetExhausted: return "BUDGET_EXHAUSTED";
    case ReasonCode::UnsupportedInput: return "UNSUPPORTED_INPUT";
    case ReasonCode::InternalInvariantViolation: return "INTERNAL_INVARIANT_VIOLATION";
  }
  return "UNKNOWN_REASON";
}

}  // namespace bhg
