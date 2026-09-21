#include "blackhole/authority/authority.hpp"

#include <algorithm>

#include "blackhole/core/hash.hpp"

namespace bhg {

FenceRegistry::FenceRegistry(const FencePolicy& policy) : policy_(policy) {}

void FenceRegistry::clear() noexcept {
  records_.clear();
  // Cumulative counters are preserved: the registry's own stats_ issuance count is
  // also a monotonic input to fence identity derivation.
}

Outcome FenceRegistry::issue(const Scope& scope, const GenerationVector& gens, BootId boot,
                             IncarnationId incarnation, Provenance provenance, ReasonCode reason,
                             WallNs now, FenceId& out_id, ReasonCode& out_reason) {
  ++stats_.issue_attempts;
  out_id = FenceId{};
  out_reason = ReasonCode::None;

  if (!scope.is_valid() || !gens.is_complete() || boot.is_nil() || incarnation.is_nil()) {
    out_reason = ReasonCode::UnsupportedInput;
    return Outcome::Invalid;
  }
  for (const auto& kv : records_) {
    const FenceRecord& r = kv.second;
    if (!r.is_open()) continue;
    if (r.intent.scope == scope && r.intent.gens == gens) {
      ++stats_.duplicates_refused;
      out_id = r.intent.id;
      out_reason = ReasonCode::FenceIntentIssued;
      return Outcome::Duplicate;
    }
  }
  if (records_.size() >= policy_.max_fences) {
    ++stats_.capacity_refused;
    out_reason = ReasonCode::BudgetExhausted;
    return Outcome::Exhausted;
  }

  FenceIntent intent;
  intent.scope = scope;
  intent.gens = gens;
  intent.boot = boot;
  intent.incarnation = incarnation;
  intent.policy = policy_.version;
  intent.provenance = provenance;
  intent.reason = reason;
  intent.issued_at = now;
  const auto expires = checked_add<std::int64_t>(now.ns, policy_.fence_ttl.ns);
  intent.expires_at = WallNs{expires.has_value() ? *expires : now.ns};
  if (!expires.has_value() || intent.expires_at <= intent.issued_at) {
    out_reason = ReasonCode::UnsupportedInput;
    return Outcome::Invalid;
  }

  Writer w(512);
  encode(w, scope);
  encode(w, gens);
  encode(w, boot);
  encode(w, incarnation);
  encode(w, intent.issued_at);
  w.u64(stats_.issue_attempts);
  if (!w.ok()) {
    out_reason = ReasonCode::BudgetExhausted;
    return Outcome::Exhausted;
  }
  intent.id = FenceId{fnv1a64(w.span())};
  intent.decision = DecisionId{mix64(intent.id.value(), 0x5EED1234ULL)};

  FenceRecord rec;
  rec.intent = intent;
  rec.lifecycle = FenceLifecycle::Intent;
  const auto emplaced = records_.emplace(intent.id, rec);
  if (!emplaced.second) {
    ++stats_.duplicates_refused;
    out_reason = ReasonCode::DuplicateEvidence;
    return Outcome::Duplicate;
  }

  ++stats_.issued;
  ++stats_.open;
  out_id = intent.id;
  out_reason = ReasonCode::FenceIntentIssued;
  return Outcome::Ok;
}

Outcome FenceRegistry::stage(const Scope& scope, const GenerationVector& gens, BootId boot,
                            IncarnationId incarnation, Provenance provenance, ReasonCode reason,
                            WallNs now, FenceIntent& out_intent, ReasonCode& out_reason) {
  ++stats_.issue_attempts;
  out_intent = FenceIntent{};
  out_reason = ReasonCode::None;

  if (!scope.is_valid() || !gens.is_complete() || boot.is_nil() || incarnation.is_nil()) {
    out_reason = ReasonCode::UnsupportedInput;
    return Outcome::Invalid;
  }
  for (const auto& kv : records_) {
    const FenceRecord& r = kv.second;
    if (!r.is_open()) continue;
    if (r.intent.scope == scope && r.intent.gens == gens) {
      ++stats_.duplicates_refused;
      out_reason = ReasonCode::FenceIntentIssued;
      return Outcome::Duplicate;
    }
  }
  if (records_.size() >= policy_.max_fences) {
    ++stats_.capacity_refused;
    out_reason = ReasonCode::BudgetExhausted;
    return Outcome::Exhausted;
  }

  FenceIntent intent;
  intent.scope = scope;
  intent.gens = gens;
  intent.boot = boot;
  intent.incarnation = incarnation;
  intent.policy = policy_.version;
  intent.provenance = provenance;
  intent.reason = reason;
  intent.issued_at = now;
  const auto expires = checked_add<std::int64_t>(now.ns, policy_.fence_ttl.ns);
  intent.expires_at = WallNs{expires.has_value() ? *expires : now.ns};
  if (!expires.has_value() || intent.expires_at <= intent.issued_at) {
    out_reason = ReasonCode::UnsupportedInput;
    return Outcome::Invalid;
  }

  Writer w(512);
  encode(w, scope);
  encode(w, gens);
  encode(w, boot);
  encode(w, incarnation);
  encode(w, intent.issued_at);
  w.u64(stats_.issue_attempts);
  if (!w.ok()) {
    out_reason = ReasonCode::BudgetExhausted;
    return Outcome::Exhausted;
  }
  intent.id = FenceId{fnv1a64(w.span())};
  intent.decision = DecisionId{mix64(intent.id.value(), 0x5EED1234ULL)};
  out_intent = intent;
  return Outcome::Ok;
}

Outcome FenceRegistry::commit_staged(const FenceIntent& intent) {
  if (!intent.is_valid()) return Outcome::Invalid;
  if (records_.find(intent.id) != records_.end()) return Outcome::Duplicate;
  for (const auto& kv : records_) {
    const FenceRecord& r = kv.second;
    if (!r.is_open()) continue;
    if (r.intent.scope == intent.scope && r.intent.gens == intent.gens) {
      return Outcome::Duplicate;
    }
  }
  if (records_.size() >= policy_.max_fences) return Outcome::Exhausted;
  FenceRecord rec;
  rec.intent = intent;
  rec.lifecycle = FenceLifecycle::Intent;
  records_.emplace(intent.id, rec);
  ++stats_.issued;
  ++stats_.open;
  return Outcome::Ok;
}

Outcome FenceRegistry::acknowledge(FenceId id, EvidenceSourceId downstream, WallNs now,
                                   ReasonCode& out_reason) {
  out_reason = ReasonCode::None;
  const auto it = records_.find(id);
  if (it == records_.end()) {
    ++stats_.unknown_id_refused;
    out_reason = ReasonCode::PolicyRefused;
    return Outcome::NotFound;
  }
  FenceRecord& r = it->second;
  if (!r.is_open()) {
    out_reason = r.terminal_reason != ReasonCode::None ? r.terminal_reason
                                                       : ReasonCode::FenceIntentRevoked;
    return Outcome::Fenced;
  }
  if (now.ns >= r.intent.expires_at.ns) {
    r.lifecycle = FenceLifecycle::Expired;
    r.terminal_reason = ReasonCode::FenceExpired;
    --stats_.open;
    ++stats_.expired;
    out_reason = ReasonCode::FenceExpired;
    return Outcome::Expired;
  }
  if (r.lifecycle == FenceLifecycle::Intent) {
    r.lifecycle = FenceLifecycle::Acknowledged;
    ++stats_.acknowledged;
  }
  r.ack_owner = downstream;
  r.acked_at = now;
  out_reason = ReasonCode::FenceAcknowledged;
  return Outcome::Ok;
}

Outcome FenceRegistry::report_effect(FenceId id, EvidenceSourceId downstream, bool verified,
                                     WallNs now, ReasonCode& out_reason) {
  out_reason = ReasonCode::None;
  const auto it = records_.find(id);
  if (it == records_.end()) {
    ++stats_.unknown_id_refused;
    out_reason = ReasonCode::PolicyRefused;
    return Outcome::NotFound;
  }
  FenceRecord& r = it->second;
  if (!r.is_open()) {
    out_reason = r.terminal_reason != ReasonCode::None ? r.terminal_reason
                                                       : ReasonCode::FenceIntentRevoked;
    return Outcome::Fenced;
  }
  if (now.ns >= r.intent.expires_at.ns) {
    r.lifecycle = FenceLifecycle::Expired;
    r.terminal_reason = ReasonCode::FenceExpired;
    --stats_.open;
    ++stats_.expired;
    out_reason = ReasonCode::FenceExpired;
    return Outcome::Expired;
  }
  r.lifecycle = FenceLifecycle::EffectReported;
  r.effect_owner = downstream;
  r.effected_at = now;
  r.effect_verified = verified;
  ++stats_.effects_reported;
  if (verified) ++stats_.effects_verified;
  out_reason = verified ? ReasonCode::FenceEffectVerified : ReasonCode::FenceEffectUnverified;
  return Outcome::Ok;
}

Outcome FenceRegistry::revoke(FenceId id, ReasonCode reason, WallNs now) {
  const auto it = records_.find(id);
  if (it == records_.end()) {
    ++stats_.unknown_id_refused;
    return Outcome::NotFound;
  }
  FenceRecord& r = it->second;
  if (!r.is_open()) return Outcome::Fenced;
  r.lifecycle = FenceLifecycle::Revoked;
  r.terminal_reason = reason;
  r.effected_at = now;
  --stats_.open;
  ++stats_.revoked;
  return Outcome::Ok;
}

std::vector<FenceId> FenceRegistry::revalidate(WallNs now, const GenerationVector& current) {
  std::vector<FenceId> out;
  for (auto& kv : records_) {
    FenceRecord& r = kv.second;
    if (!r.is_open()) continue;
    if (now.ns >= r.intent.expires_at.ns) {
      r.lifecycle = FenceLifecycle::Expired;
      r.terminal_reason = ReasonCode::FenceExpired;
      --stats_.open;
      ++stats_.expired;
      out.push_back(kv.first);
      continue;
    }
    if (r.intent.gens != current) {
      r.lifecycle = FenceLifecycle::Revoked;
      r.terminal_reason = ReasonCode::DependencyGenerationChanged;
      --stats_.open;
      ++stats_.revoked;
      out.push_back(kv.first);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<FenceId> FenceRegistry::fence_all(ReasonCode reason, WallNs now) {
  std::vector<FenceId> out;
  for (auto& kv : records_) {
    FenceRecord& r = kv.second;
    if (!r.is_open()) continue;
    r.lifecycle = FenceLifecycle::FencedByRestart;
    r.terminal_reason = reason;
    r.effected_at = now;
    --stats_.open;
    ++stats_.fenced_by_restart;
    out.push_back(kv.first);
  }
  std::sort(out.begin(), out.end());
  return out;
}

const FenceRecord* FenceRegistry::find(FenceId id) const {
  const auto it = records_.find(id);
  return it == records_.end() ? nullptr : &it->second;
}

const FenceRecord* FenceRegistry::open_for(const Scope& scope) const {
  for (const auto& kv : records_) {
    if (kv.second.is_open() && kv.second.intent.scope == scope) return &kv.second;
  }
  return nullptr;
}

std::vector<FenceRecord> FenceRegistry::all() const {
  std::vector<FenceRecord> out;
  out.reserve(records_.size());
  for (const auto& kv : records_) out.push_back(kv.second);
  return out;
}

RestorationDecision evaluate_restoration(const Diagnosis& diagnosis, const FencePolicy& policy,
                                         const FenceRecord* open_fence, WallNs now) {
  RestorationDecision d;
  d.scope = diagnosis.scope;
  d.gens = diagnosis.gens;
  d.decided_at = now;
  d.sources = diagnosis.success_sources;
  d.distinct_incarnations = diagnosis.census.success_incarnations;
  d.success_attempts = diagnosis.census.success_attempts;
  d.had_open_fence = open_fence != nullptr;
  if (open_fence != nullptr) d.revoked_fence = open_fence->intent.id;

  if (diagnosis.outcome != Outcome::Ok || diagnosis.classification != Classification::Healthy) {
    d.authorized = false;
    switch (diagnosis.outcome) {
      case Outcome::NoEvidence:
        d.outcome = Outcome::NoEvidence;
        d.reason = ReasonCode::RestorationRefusedNoEvidence;
        break;
      case Outcome::Stale:
        d.outcome = Outcome::Stale;
        d.reason = ReasonCode::RestorationRefusedStale;
        break;
      case Outcome::Conflict:
        d.outcome = Outcome::Conflict;
        d.reason = ReasonCode::RestorationRefusedConflict;
        break;
      default:
        d.outcome = Outcome::Unknown;
        d.reason = ReasonCode::RestorationRefusedInsufficient;
        break;
    }
    return d;
  }
  if (diagnosis.census.success_sources < policy.restore_min_sources) {
    d.outcome = Outcome::Ok;
    d.reason = ReasonCode::SourcesInsufficient;
    return d;
  }
  if (diagnosis.census.success_incarnations < policy.restore_min_incarnations) {
    d.outcome = Outcome::Ok;
    d.reason = ReasonCode::IncarnationsInsufficient;
    return d;
  }
  if (diagnosis.census.success_attempts < policy.restore_min_successes) {
    d.outcome = Outcome::Ok;
    d.reason = ReasonCode::AttemptsInsufficient;
    return d;
  }
  d.outcome = Outcome::Ok;
  d.authorized = true;
  d.reason = ReasonCode::RestorationCorroborated;
  return d;
}

bool Decision::is_valid() const {
  if (id.is_nil()) return false;
  if (!is_valid_decision_kind(static_cast<std::uint16_t>(kind))) return false;
  if (!scope.is_valid()) return false;
  if (!gens.is_complete()) return false;
  if (!is_valid_classification(static_cast<std::uint16_t>(classification))) return false;
  if (!is_valid_reason_code(static_cast<std::uint16_t>(primary_reason))) return false;
  if (has_intent && !intent.is_valid()) return false;
  if (has_intent && intent.scope != scope) return false;
  if (has_intent && intent.gens != gens) return false;
  if (has_localization && !is_valid_localization_status(
                              static_cast<std::uint8_t>(localization.status))) {
    return false;
  }
  return true;
}

DecisionId derive_decision_id(const Decision& d) {
  Decision copy = d;
  // The identity is derived from content only: fields that hold identity are
  // cleared so the id is a pure function of what the decision asserts.
  copy.id = DecisionId{};
  copy.authority.decision = DecisionId{};
  copy.authority.fence = FenceId{};
  Writer w(kDefaultMaxDocumentBytes);
  encode(w, copy);
  if (!w.ok()) return DecisionId{};
  return DecisionId{fnv1a64(w.span())};
}

void encode(Writer& w, const FenceIntent& f) noexcept {
  encode(w, f.id);
  encode(w, f.decision);
  encode(w, f.scope);
  encode(w, f.gens);
  encode(w, f.boot);
  encode(w, f.incarnation);
  encode(w, f.policy);
  w.u8(static_cast<std::uint8_t>(f.provenance));
  w.u16(static_cast<std::uint16_t>(f.reason));
  encode(w, f.issued_at);
  encode(w, f.expires_at);
}

Outcome decode(Reader& r, FenceIntent& f) noexcept {
  FenceIntent out{};
  Outcome o = decode(r, out.id);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.policy);
  if (!is_affirmative(o)) return o;
  std::uint8_t prov = 0;
  o = r.u8(prov);
  if (!is_affirmative(o)) return o;
  if (!is_valid_provenance(prov)) return Outcome::Invalid;
  out.provenance = static_cast<Provenance>(prov);
  std::uint16_t reason = 0;
  o = r.u16(reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(reason)) return Outcome::Invalid;
  out.reason = static_cast<ReasonCode>(reason);
  o = decode(r, out.issued_at);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.expires_at);
  if (!is_affirmative(o)) return o;
  if (!out.is_valid()) return Outcome::Invalid;
  f = out;
  return Outcome::Ok;
}

void encode(Writer& w, const Decision& d) noexcept {
  encode(w, d.id);
  w.u16(static_cast<std::uint16_t>(d.kind));
  w.u8(static_cast<std::uint8_t>(d.outcome));
  encode(w, d.scope);
  encode(w, d.gens);
  w.u16(static_cast<std::uint16_t>(d.classification));
  w.u16(static_cast<std::uint16_t>(d.primary_reason));
  w.u32(static_cast<std::uint32_t>(d.reasons.size()));
  for (const ReasonCode c : d.reasons) w.u16(static_cast<std::uint16_t>(c));

  w.u8(static_cast<std::uint8_t>(d.authority.stage));
  encode(w, d.authority.decision);
  encode(w, d.authority.fence);
  encode(w, d.authority.gens);
  encode(w, d.authority.boot);
  encode(w, d.authority.incarnation);
  encode(w, d.authority.policy);
  encode(w, d.authority.issued_at);
  encode(w, d.authority.expires_at);
  w.boolean(d.authority.revoked);
  w.u16(static_cast<std::uint16_t>(d.authority.revoke_reason));
  w.u8(static_cast<std::uint8_t>(d.authority.provenance));

  w.u32(d.census.total);
  w.u32(d.census.fresh_current);
  w.u32(d.census.stale);
  w.u32(d.census.generation_mismatch);
  w.u32(d.census.quality_rejected);
  w.u32(d.census.invalid);
  w.u32(d.census.complete_failures);
  w.u32(d.census.clean_successes);
  w.u32(d.census.congestion_signals);
  w.u32(d.census.partition_signals);
  w.u32(d.census.withdrawals);
  w.u32(d.census.failure_sources);
  w.u32(d.census.failure_incarnations);
  w.u32(d.census.success_sources);
  w.u32(d.census.success_incarnations);
  w.u64(d.census.failure_attempts);
  w.u64(d.census.failure_losses);
  w.u64(d.census.success_attempts);
  w.u32(d.census.min_loss_ppm);
  w.u32(d.census.max_loss_ppm);
  w.u16(static_cast<std::uint16_t>(d.census.mismatch));
  w.boolean(d.corroborated);
  w.u16(static_cast<std::uint16_t>(d.corroboration_reason));

  w.boolean(d.has_localization);
  if (d.has_localization) {
    w.u8(static_cast<std::uint8_t>(d.localization.status));
    w.u32(static_cast<std::uint32_t>(d.localization.elements.size()));
    for (const std::uint32_t e : d.localization.elements) w.u32(e);
    w.u32(d.localization.optimal_size);
    w.boolean(d.localization.optimality_proven);
    w.boolean(d.localization.uniqueness_proven);
    w.boolean(d.localization.solutions_truncated);
    w.u32(d.localization.solutions_found);
    w.u64(d.localization.search_nodes);
    w.u32(d.localization.infeasible_witness);
    w.boolean(d.localization.has_infeasible_witness);
    w.u16(static_cast<std::uint16_t>(d.localization.reason));
  }

  w.boolean(d.has_intent);
  if (d.has_intent) encode(w, d.intent);

  w.u8(static_cast<std::uint8_t>(d.provenance));
  encode(w, d.policy);
  encode(w, d.boot);
  encode(w, d.incarnation);
  encode(w, d.decided_at);
}

Outcome decode(Reader& r, Decision& d) noexcept {
  Decision out{};
  Outcome o = decode(r, out.id);
  if (!is_affirmative(o)) return o;
  std::uint16_t kind = 0;
  o = r.u16(kind);
  if (!is_affirmative(o)) return o;
  if (!is_valid_decision_kind(kind)) return Outcome::Invalid;
  out.kind = static_cast<DecisionKind>(kind);
  std::uint8_t outcome_raw = 0;
  o = r.u8(outcome_raw);
  if (!is_affirmative(o)) return o;
  if (!is_valid_outcome(outcome_raw)) return Outcome::Invalid;
  out.outcome = static_cast<Outcome>(outcome_raw);
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  std::uint16_t cls = 0;
  o = r.u16(cls);
  if (!is_affirmative(o)) return o;
  if (!is_valid_classification(cls)) return Outcome::Invalid;
  out.classification = static_cast<Classification>(cls);
  std::uint16_t reason = 0;
  o = r.u16(reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(reason)) return Outcome::Invalid;
  out.primary_reason = static_cast<ReasonCode>(reason);
  std::uint32_t reason_count = 0;
  o = r.u32(reason_count);
  if (!is_affirmative(o)) return o;
  if (reason_count > 256u) return Outcome::Oversized;
  if (r.remaining() < static_cast<std::size_t>(reason_count) * 2u) return Outcome::Malformed;
  for (std::uint32_t i = 0; i < reason_count; ++i) {
    std::uint16_t rc = 0;
    o = r.u16(rc);
    if (!is_affirmative(o)) return o;
    if (!is_valid_reason_code(rc)) return Outcome::Invalid;
    out.reasons.push_back(static_cast<ReasonCode>(rc));
  }

  std::uint8_t stage = 0;
  o = r.u8(stage);
  if (!is_affirmative(o)) return o;
  if (!is_valid_authority_stage(stage)) return Outcome::Invalid;
  out.authority.stage = static_cast<AuthorityStage>(stage);
  o = decode(r, out.authority.decision);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.fence);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.gens);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.policy);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.issued_at);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.authority.expires_at);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.authority.revoked);
  if (!is_affirmative(o)) return o;
  std::uint16_t revoke_reason = 0;
  o = r.u16(revoke_reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(revoke_reason)) return Outcome::Invalid;
  out.authority.revoke_reason = static_cast<ReasonCode>(revoke_reason);
  std::uint8_t auth_prov = 0;
  o = r.u8(auth_prov);
  if (!is_affirmative(o)) return o;
  if (!is_valid_provenance(auth_prov)) return Outcome::Invalid;
  out.authority.provenance = static_cast<Provenance>(auth_prov);

  o = r.u32(out.census.total);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.fresh_current);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.stale);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.generation_mismatch);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.quality_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.invalid);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.complete_failures);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.clean_successes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.congestion_signals);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.partition_signals);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.withdrawals);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.failure_sources);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.failure_incarnations);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.success_sources);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.success_incarnations);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.census.failure_attempts);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.census.failure_losses);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.census.success_attempts);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.min_loss_ppm);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.census.max_loss_ppm);
  if (!is_affirmative(o)) return o;
  std::uint16_t mismatch = 0;
  o = r.u16(mismatch);
  if (!is_affirmative(o)) return o;
  if (!is_valid_gen_mismatch(static_cast<std::uint8_t>(mismatch))) return Outcome::Invalid;
  out.census.mismatch = static_cast<GenMismatch>(mismatch);
  o = r.boolean(out.corroborated);
  if (!is_affirmative(o)) return o;
  std::uint16_t corroboration_reason = 0;
  o = r.u16(corroboration_reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(corroboration_reason)) return Outcome::Invalid;
  out.corroboration_reason = static_cast<ReasonCode>(corroboration_reason);

  o = r.boolean(out.has_localization);
  if (!is_affirmative(o)) return o;
  if (out.has_localization) {
    std::uint8_t ls = 0;
    o = r.u8(ls);
    if (!is_affirmative(o)) return o;
    if (!is_valid_localization_status(ls)) return Outcome::Invalid;
    out.localization.status = static_cast<LocalizationStatus>(ls);
    std::uint32_t n = 0;
    o = r.u32(n);
    if (!is_affirmative(o)) return o;
    if (n > kMaxLocalizationCandidates) return Outcome::Oversized;
    if (r.remaining() < static_cast<std::size_t>(n) * 4u) return Outcome::Malformed;
    out.localization.elements.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      std::uint32_t e = 0;
      o = r.u32(e);
      if (!is_affirmative(o)) return o;
      out.localization.elements.push_back(e);
    }
    o = r.u32(out.localization.optimal_size);
    if (!is_affirmative(o)) return o;
    o = r.boolean(out.localization.optimality_proven);
    if (!is_affirmative(o)) return o;
    o = r.boolean(out.localization.uniqueness_proven);
    if (!is_affirmative(o)) return o;
    o = r.boolean(out.localization.solutions_truncated);
    if (!is_affirmative(o)) return o;
    o = r.u32(out.localization.solutions_found);
    if (!is_affirmative(o)) return o;
    o = r.u64(out.localization.search_nodes);
    if (!is_affirmative(o)) return o;
    o = r.u32(out.localization.infeasible_witness);
    if (!is_affirmative(o)) return o;
    o = r.boolean(out.localization.has_infeasible_witness);
    if (!is_affirmative(o)) return o;
    std::uint16_t lr = 0;
    o = r.u16(lr);
    if (!is_affirmative(o)) return o;
    if (!is_valid_reason_code(lr)) return Outcome::Invalid;
    out.localization.reason = static_cast<ReasonCode>(lr);
  }

  o = r.boolean(out.has_intent);
  if (!is_affirmative(o)) return o;
  if (out.has_intent) {
    o = decode(r, out.intent);
    if (!is_affirmative(o)) return o;
  }

  std::uint8_t prov = 0;
  o = r.u8(prov);
  if (!is_affirmative(o)) return o;
  if (!is_valid_provenance(prov)) return Outcome::Invalid;
  out.provenance = static_cast<Provenance>(prov);
  o = decode(r, out.policy);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.decided_at);
  if (!is_affirmative(o)) return o;

  if (!out.is_valid()) return Outcome::Invalid;
  d = std::move(out);
  return Outcome::Ok;
}

}  // namespace bhg
