#include "blackhole/service/engine.hpp"

#include <algorithm>

namespace bhg {

Engine::Engine(EngineConfig config, Clock& clock, NonceSource& nonces)
    : config_(std::move(config)),
      policy_(config_.policy),
      clock_(clock),
      nonces_(nonces),
      ledger_(policy_),
      fences_(policy_),
      store_(config_.store, policy_) {}

Engine::~Engine() { (void)close(); }

WallNs Engine::now() const { return clock_.wall_now(); }

Outcome Engine::open() {
  std::lock_guard<std::mutex> lock(mu_);
  if (opened_) return Outcome::Invalid;
  const Outcome o = store_.open(nonces_, now(), config_.label);
  if (!is_affirmative(o)) return o;
  // Dynamic liveness is never restored: the ledger starts empty and every prior
  // fence is durable history only. Pre-restart authority does not survive.
  ledger_.reset();
  fences_.clear();
  opened_ = true;
  return Outcome::Ok;
}

Outcome Engine::close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) return Outcome::Ok;
  const Outcome o = store_.close();
  opened_ = false;
  ledger_.reset();
  fences_.clear();
  return o;
}

bool Engine::is_open() const {
  std::lock_guard<std::mutex> lock(mu_);
  return opened_;
}

Outcome Engine::submit(const DeliveryEvidence& e, EvidenceAdmission& admission,
                       ReasonCode& reason) {
  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.evidence_submitted;
  if (!opened_) {
    ++stats_.evidence_rejected;
    admission = EvidenceAdmission::CapacityExhausted;
    reason = ReasonCode::UnsupportedInput;
    return Outcome::Rejected;
  }
  admission = ledger_.admit(e, now(), reason);
  if (admission == EvidenceAdmission::Accepted ||
      admission == EvidenceAdmission::RolloverAccepted) {
    ++stats_.evidence_admitted;
    return Outcome::Ok;
  }
  ++stats_.evidence_rejected;
  return Outcome::Rejected;
}

void Engine::revalidate_fences_locked(const GenerationVector& current) {
  const std::vector<FenceId> revoked = fences_.revalidate(now(), current);
  for (const FenceId id : revoked) {
    const FenceRecord* rec = fences_.find(id);
    if (rec == nullptr) continue;
    FenceStatePayload payload;
    payload.intent = rec->intent;
    payload.lifecycle = static_cast<std::uint8_t>(rec->lifecycle);
    payload.effect_verified = rec->effect_verified;
    payload.ack_owner = rec->ack_owner;
    payload.effect_owner = rec->effect_owner;
    payload.acked_at = rec->acked_at;
    payload.effected_at = rec->effected_at;
    payload.terminal_reason = rec->terminal_reason;
    const Outcome o = store_.commit_fence_state(RecordType::FenceRevokeCommit, payload);
    if (!is_affirmative(o)) ++stats_.durable_commit_failures;
    ++stats_.fence_revocations;
  }
}

Decision Engine::make_refusal(const Scope& scope, const GenerationVector& gens, Outcome outcome,
                              ReasonCode reason) const {
  Decision d;
  d.kind = DecisionKind::Diagnosis;
  d.outcome = outcome;
  d.scope = scope;
  d.gens = gens;
  d.classification = Classification::Unknown;
  d.primary_reason = reason;
  if (d.reasons.size() < policy_.max_reasons_per_decision) d.reasons.push_back(reason);
  d.provenance = config_.label;
  d.policy = policy_.version;
  d.boot = store_.boot();
  d.incarnation = store_.incarnation();
  d.decided_at = now();
  d.authority.stage = AuthorityStage::None;
  d.authority.gens = gens;
  d.authority.boot = d.boot;
  d.authority.incarnation = d.incarnation;
  d.authority.policy = policy_.version;
  d.authority.provenance = config_.label;
  d.id = derive_decision_id(d);
  d.authority.decision = d.id;
  return d;
}

Outcome Engine::commit_decision(const Decision& d) {
  ++stats_.decisions_produced;
  const Outcome o = store_.commit_decision(d);
  if (!is_affirmative(o)) {
    ++stats_.decision_commit_failures;
    ++stats_.durable_commit_failures;
    return o;
  }
  ++stats_.decisions_committed;
  return Outcome::Ok;
}

Outcome Engine::evaluate(const Scope& scope, const GenerationVector& gens, Decision& out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    ++stats_.refusals_fail_closed;
    out = make_refusal(scope, gens, Outcome::Unavailable, ReasonCode::UnsupportedInput);
    return Outcome::Unavailable;
  }
  if (!scope.is_valid() || !gens.is_complete()) {
    ++stats_.refusals_fail_closed;
    out = make_refusal(scope, gens, Outcome::Invalid, ReasonCode::UnsupportedInput);
    return Outcome::Invalid;
  }
  // Counted only once the call is a real evaluation: every counted call produces
  // exactly one decision record attempt, which is what makes the identity close.
  ++stats_.evaluations;

  revalidate_fences_locked(gens);

  const std::vector<DeliveryEvidence> evidence = ledger_.evidence_for(scope);
  ClassifyRequest req;
  req.scope = scope;
  req.current = gens;
  req.now = now();
  req.policy = &policy_;
  req.evidence = std::span<const DeliveryEvidence>(evidence.data(), evidence.size());

  Diagnosis diag;
  (void)classify(req, diag);

  Decision d;
  d.kind = DecisionKind::Diagnosis;
  d.outcome = diag.outcome;
  d.scope = scope;
  d.gens = gens;
  d.classification = diag.classification;
  d.primary_reason = diag.primary_reason;
  d.reasons = diag.reasons;
  d.census = diag.census;
  d.corroborated = diag.corroborated;
  d.corroboration_reason = diag.corroboration_reason;
  d.provenance = config_.label;
  d.policy = policy_.version;
  d.boot = store_.boot();
  d.incarnation = store_.incarnation();
  d.decided_at = req.now;

  AuthorityStage stage = AuthorityStage::None;
  if (diag.census.total > 0) stage = AuthorityStage::Observation;
  if (diag.classification == Classification::Blackhole) {
    stage = diag.corroborated ? AuthorityStage::Authorization : AuthorityStage::Eligibility;
  }

  d.authority.stage = stage;
  d.authority.gens = gens;
  d.authority.boot = d.boot;
  d.authority.incarnation = d.incarnation;
  d.authority.policy = policy_.version;
  d.authority.issued_at = d.decided_at;
  d.authority.provenance = config_.label;
  d.has_intent = false;

  if (stage == AuthorityStage::Authorization) {
    // Fail closed: the intent is only allowed to exist once its durable record is
    // confirmed. A failed commit leaves authority at Authorization, never Intent.
    FenceIntent intent;
    ReasonCode fence_reason = ReasonCode::None;
    const Outcome so = fences_.stage(scope, gens, d.boot, d.incarnation, config_.label,
                                     diag.primary_reason, d.decided_at, intent, fence_reason);
    if (is_affirmative(so)) {
      FenceStatePayload payload;
      payload.intent = intent;
      payload.lifecycle = static_cast<std::uint8_t>(FenceLifecycle::Intent);
      payload.terminal_reason = ReasonCode::None;
      const Outcome co2 = store_.commit_fence_state(RecordType::FenceIntentCommit, payload);
      if (is_affirmative(co2)) {
        const Outcome po = fences_.commit_staged(intent);
        if (is_affirmative(po)) {
          d.has_intent = true;
          d.intent = intent;
          d.authority.stage = AuthorityStage::Intent;
          d.authority.fence = intent.id;
          d.authority.expires_at = intent.expires_at;
          ++stats_.fence_intents_issued;
          if (d.reasons.size() < policy_.max_reasons_per_decision) {
            d.reasons.push_back(ReasonCode::FenceIntentIssued);
          }
        } else {
          ++stats_.durable_commit_failures;
        }
      } else {
        ++stats_.durable_commit_failures;
        ++stats_.refusals_fail_closed;
        if (d.reasons.size() < policy_.max_reasons_per_decision) {
          d.reasons.push_back(ReasonCode::BudgetExhausted);
        }
      }
    } else if (so == Outcome::Duplicate) {
      // An identical open intent already exists for this subject and generation.
      const FenceRecord* existing = fences_.open_for(scope);
      if (existing != nullptr) {
        d.has_intent = true;
        d.intent = existing->intent;
        d.authority.stage = AuthorityStage::Intent;
        d.authority.fence = existing->intent.id;
        d.authority.expires_at = existing->intent.expires_at;
      }
    } else {
      ++stats_.refusals_fail_closed;
      if (d.reasons.size() < policy_.max_reasons_per_decision) {
        d.reasons.push_back(fence_reason);
      }
    }
  }

  d.id = derive_decision_id(d);
  d.authority.decision = d.id;
  if (d.has_intent) d.authority.fence = d.intent.id;

  const Outcome commit = commit_decision(d);
  if (!is_affirmative(commit) && d.has_intent) {
    // The decision record did not become durable. Revoke the in-memory intent so
    // that no authority outlives its evidence of record.
    (void)fences_.revoke(d.intent.id, ReasonCode::FenceIntentRevoked, now());
    d.has_intent = false;
    d.authority.stage = AuthorityStage::Authorization;
    d.authority.fence = FenceId{};
    if (d.reasons.size() < policy_.max_reasons_per_decision) {
      d.reasons.push_back(ReasonCode::FenceIntentRevoked);
    }
  }
  out = d;
  return d.outcome;
}

Outcome Engine::localize(const Scope& scope, const GenerationVector& gens,
                         std::uint32_t hop_count, std::span<const HopProbe> probes,
                         Decision& out) {
  // The diagnosis is produced under the engine lock; the localization search runs
  // outside it because it is bounded but potentially long.
  const Outcome eo = evaluate(scope, gens, out);

  LocalizationInput input;
  std::string why;
  const Outcome bo = build_localization_input(
      hop_count, probes, policy_.max_localization_search_nodes,
      policy_.max_localization_solutions, input, why);

  LocalizationResult result;
  bool validation_failed = false;
  if (!is_affirmative(bo)) {
    result.status = LocalizationStatus::InvalidInput;
    result.reason = ReasonCode::LocalizationInvalid;
  } else {
    result = bhg::localize(input);
    const ValidationReport vr = verify_localization(input, result);
    validation_failed = !vr.valid;
    if (validation_failed) {
      // A solver output that fails independent validation is never published as a
      // localization; the decision degrades to an explicit indeterminate state.
      result.status = LocalizationStatus::Indeterminate;
      result.optimality_proven = false;
      result.uniqueness_proven = false;
      result.elements.clear();
      result.reason = ReasonCode::LocalizationInvalid;
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  ++stats_.localizations;
  if (!is_affirmative(bo)) {
    out.localization = std::move(result);
    out.has_localization = true;
    out.primary_reason = ReasonCode::UnsupportedInput;
    if (out.reasons.size() < policy_.max_reasons_per_decision) {
      out.reasons.push_back(ReasonCode::UnsupportedInput);
    }
    return bo;
  }
  if (validation_failed) {
    if (out.reasons.size() < policy_.max_reasons_per_decision) {
      out.reasons.push_back(ReasonCode::InternalInvariantViolation);
    }
  } else if (result.status == LocalizationStatus::Ambiguous) {
    if (out.reasons.size() < policy_.max_reasons_per_decision) {
      out.reasons.push_back(ReasonCode::LocalizationAmbiguous);
    }
  } else if (result.status == LocalizationStatus::Indeterminate) {
    if (out.reasons.size() < policy_.max_reasons_per_decision) {
      out.reasons.push_back(ReasonCode::LocalizationSearchLimit);
    }
  } else if (result.status == LocalizationStatus::ProvenInfeasible) {
    if (out.reasons.size() < policy_.max_reasons_per_decision) {
      out.reasons.push_back(ReasonCode::LocalizationInfeasible);
    }
  }
  out.localization = std::move(result);
  out.has_localization = true;
  const DecisionId previous = out.id;
  out.id = derive_decision_id(out);
  out.authority.decision = out.id;
  if (out.id != previous) {
    // Re-commit the enriched decision; the earlier record remains as lineage.
    ++stats_.localization_enrichments;
    (void)commit_decision(out);
  }
  // The return value describes the *localization*, not the diagnosis: a successful
  // localization of a NO_EVIDENCE subject is still a successful localization, and the
  // decision carries the diagnosis outcome alongside it.
  (void)eo;
  return Outcome::Ok;
}

Outcome Engine::acknowledge_fence(FenceId id, EvidenceSourceId downstream, ReasonCode& reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    reason = ReasonCode::UnsupportedInput;
    return Outcome::Rejected;
  }
  const Outcome o = fences_.acknowledge(id, downstream, now(), reason);
  if (!is_affirmative(o)) return o;
  const FenceRecord* rec = fences_.find(id);
  if (rec == nullptr) {
    reason = ReasonCode::PolicyRefused;
    return Outcome::NotFound;
  }
  FenceStatePayload payload;
  payload.intent = rec->intent;
  payload.lifecycle = static_cast<std::uint8_t>(rec->lifecycle);
  payload.effect_verified = rec->effect_verified;
  payload.ack_owner = rec->ack_owner;
  payload.effect_owner = rec->effect_owner;
  payload.acked_at = rec->acked_at;
  payload.effected_at = rec->effected_at;
  payload.terminal_reason = rec->terminal_reason;
  const Outcome co = store_.commit_fence_state(RecordType::FenceAckCommit, payload);
  if (!is_affirmative(co)) ++stats_.durable_commit_failures;
  return o;
}

Outcome Engine::report_effect(FenceId id, EvidenceSourceId downstream, bool verified,
                              ReasonCode& reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    reason = ReasonCode::UnsupportedInput;
    return Outcome::Rejected;
  }
  const Outcome o = fences_.report_effect(id, downstream, verified, now(), reason);
  if (!is_affirmative(o)) return o;
  const FenceRecord* rec = fences_.find(id);
  if (rec == nullptr) {
    reason = ReasonCode::PolicyRefused;
    return Outcome::NotFound;
  }
  FenceStatePayload payload;
  payload.intent = rec->intent;
  payload.lifecycle = static_cast<std::uint8_t>(rec->lifecycle);
  payload.effect_verified = rec->effect_verified;
  payload.ack_owner = rec->ack_owner;
  payload.effect_owner = rec->effect_owner;
  payload.acked_at = rec->acked_at;
  payload.effected_at = rec->effected_at;
  payload.terminal_reason = rec->terminal_reason;
  const Outcome co = store_.commit_fence_state(RecordType::FenceEffectCommit, payload);
  if (!is_affirmative(co)) ++stats_.durable_commit_failures;
  return o;
}

Outcome Engine::restore(const Scope& scope, const GenerationVector& gens, Decision& out) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) {
    ++stats_.refusals_fail_closed;
    ++stats_.restorations_refused;
    out = make_refusal(scope, gens, Outcome::Unavailable, ReasonCode::UnsupportedInput);
    return Outcome::Unavailable;
  }
  if (!scope.is_valid() || !gens.is_complete()) {
    ++stats_.refusals_fail_closed;
    ++stats_.restorations_refused;
    out = make_refusal(scope, gens, Outcome::Invalid, ReasonCode::UnsupportedInput);
    return Outcome::Invalid;
  }
  ++stats_.evaluations;

  revalidate_fences_locked(gens);

  const std::vector<DeliveryEvidence> evidence = ledger_.evidence_for(scope);
  ClassifyRequest req;
  req.scope = scope;
  req.current = gens;
  req.now = now();
  req.policy = &policy_;
  req.evidence = std::span<const DeliveryEvidence>(evidence.data(), evidence.size());

  Diagnosis diag;
  (void)classify(req, diag);
  const FenceRecord* open_fence = fences_.open_for(scope);
  RestorationDecision rd = evaluate_restoration(diag, policy_, open_fence, req.now);

  Decision d;
  d.kind = DecisionKind::Restoration;
  d.outcome = rd.outcome;
  d.scope = scope;
  d.gens = gens;
  d.classification = diag.classification;
  d.primary_reason = rd.reason;
  d.reasons = diag.reasons;
  if (d.reasons.size() < policy_.max_reasons_per_decision) d.reasons.push_back(rd.reason);
  d.census = diag.census;
  d.provenance = config_.label;
  d.policy = policy_.version;
  d.boot = store_.boot();
  d.incarnation = store_.incarnation();
  d.decided_at = req.now;

  AuthorityStage stage = AuthorityStage::None;
  if (diag.census.total > 0) stage = AuthorityStage::Observation;
  if (rd.authorized) {
    stage = AuthorityStage::Authorization;
    ++stats_.restorations_authorized;
    if (open_fence != nullptr) {
      FenceStatePayload payload;
      payload.intent = open_fence->intent;
      payload.lifecycle = static_cast<std::uint8_t>(FenceLifecycle::Revoked);
      payload.effect_verified = open_fence->effect_verified;
      payload.ack_owner = open_fence->ack_owner;
      payload.effect_owner = open_fence->effect_owner;
      payload.acked_at = open_fence->acked_at;
      payload.effected_at = req.now;
      payload.terminal_reason = ReasonCode::FenceIntentRevoked;
      const Outcome co = store_.commit_fence_state(RecordType::FenceRevokeCommit, payload);
      if (is_affirmative(co)) {
        (void)fences_.revoke(open_fence->intent.id, ReasonCode::FenceIntentRevoked, req.now);
      } else {
        // Fail closed: without a durable revocation record, authority is not
        // withdrawn in memory either.
        ++stats_.durable_commit_failures;
        ++stats_.refusals_fail_closed;
        stage = AuthorityStage::Eligibility;
        d.primary_reason = ReasonCode::PolicyRefused;
        if (d.reasons.size() < policy_.max_reasons_per_decision) {
          d.reasons.push_back(ReasonCode::PolicyRefused);
        }
      }
    }
  } else {
    ++stats_.restorations_refused;
    if (diag.census.total > 0) stage = AuthorityStage::Observation;
  }

  d.authority.stage = stage;
  d.authority.gens = gens;
  d.authority.boot = d.boot;
  d.authority.incarnation = d.incarnation;
  d.authority.policy = policy_.version;
  d.authority.issued_at = d.decided_at;
  d.authority.expires_at = WallNs{};
  d.authority.provenance = config_.label;
  d.id = derive_decision_id(d);
  d.authority.decision = d.id;

  const Outcome commit = commit_decision(d);
  if (!is_affirmative(commit)) {
    d.authority.stage = AuthorityStage::Observation;
    ++stats_.refusals_fail_closed;
  }
  out = d;
  return d.outcome;
}

Outcome Engine::checkpoint(RecordSequence& snapshot_seq) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!opened_) return Outcome::Unavailable;
  snapshot_seq = store_.last_seq();
  return store_.checkpoint(now());
}

EngineStats Engine::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

Outcome Engine::fence_state(FenceId id, FenceRecord& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  const FenceRecord* rec = fences_.find(id);
  if (rec == nullptr) return Outcome::NotFound;
  out = *rec;
  return Outcome::Ok;
}

Outcome Engine::lineage(std::uint32_t limit, std::vector<LineageEntry>& entries,
                        std::uint64_t& seen, std::uint64_t& dropped) const {
  std::lock_guard<std::mutex> lock(mu_);
  const DurableState& s = store_.state();
  entries.clear();
  seen = s.lineage_seen;
  dropped = s.lineage_dropped;
  const std::size_t take = std::min<std::size_t>(limit, s.lineage.size());
  entries.reserve(take);
  const std::size_t start = s.lineage.size() - take;
  for (std::size_t i = start; i < s.lineage.size(); ++i) entries.push_back(s.lineage[i]);
  return Outcome::Ok;
}

OpenReport Engine::open_report() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_.report();
}

CoordinatorEpoch Engine::epoch() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_.epoch();
}

PolicyVersion Engine::policy_version() const { return policy_.version; }

std::uint64_t Engine::policy_fingerprint() const { return bhg::policy_fingerprint(policy_); }

WallNs Engine::now_wall() const { return now(); }

RecordSequence Engine::last_record() const {
  std::lock_guard<std::mutex> lock(mu_);
  return store_.last_seq();
}

std::string Engine::snapshot_path() const { return store_.snapshot_path(); }

std::string Engine::journal_path() const { return store_.journal_path(); }

}  // namespace bhg
