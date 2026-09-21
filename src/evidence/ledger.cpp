#include "blackhole/evidence/ledger.hpp"

#include <algorithm>

namespace bhg {

EvidenceLedger::EvidenceLedger(const FencePolicy& policy) : policy_(policy) {}

void EvidenceLedger::reset() noexcept {
  scopes_.clear();
  retained_ = 0;
  seen_ = 0;
  dropped_ = 0;
  admitted_ = 0;
  rejected_ = 0;
}

EvidenceAdmission EvidenceLedger::admit(const DeliveryEvidence& e, WallNs now,
                                        ReasonCode& reason) {
  reason = ReasonCode::None;
  ++seen_;

  std::string why;
  if (!e.structurally_valid(why)) {
    ++rejected_;
    reason = ReasonCode::InvalidEvidenceRejected;
    return EvidenceAdmission::StructurallyInvalid;
  }

  // Clock sanity: evidence from beyond the tolerated skew into the future cannot be
  // trusted enough to bind authority to.
  if (e.observed_at.ns > now.ns + policy_.max_clock_skew.ns) {
    ++rejected_;
    reason = ReasonCode::ClockSkewExceeded;
    return EvidenceAdmission::ClockSkewExceeded;
  }
  if (!e.fresh.is_fresh_at(now)) {
    ++rejected_;
    reason = ReasonCode::FreshnessWindowExpired;
    return EvidenceAdmission::OutOfFreshnessWindow;
  }

  auto scope_it = scopes_.find(e.scope);
  if (scope_it == scopes_.end()) {
    if (scopes_.size() >= policy_.max_tracked_scopes) {
      ++rejected_;
      reason = ReasonCode::BudgetExhausted;
      return EvidenceAdmission::CapacityExhausted;
    }
    scope_it = scopes_.emplace(e.scope, ScopeState{}).first;
  }
  ScopeState& st = scope_it->second;

  auto cursor_it = st.cursors.find(e.source);
  if (cursor_it == st.cursors.end()) {
    // Never evict an existing cursor: forgetting a source would let it be
    // re-admitted later with a regressed sequence.
    if (st.cursors.size() >= policy_.max_sources_per_scope) {
      ++rejected_;
      reason = ReasonCode::BudgetExhausted;
      return EvidenceAdmission::CapacityExhausted;
    }
    cursor_it = st.cursors.emplace(e.source, SourceCursor{}).first;
    cursor_it->second.source = e.source;
  }
  SourceCursor& cursor = cursor_it->second;
  ++cursor.observed;

  EvidenceAdmission admission = EvidenceAdmission::Accepted;
  if (cursor.accepted > 0) {
    if (e.seq == cursor.last_seq || (!e.attempt.is_nil() && e.attempt == cursor.last_attempt)) {
      ++cursor.duplicates;
      ++cursor.rejected;
      ++rejected_;
      reason = ReasonCode::DuplicateEvidence;
      return EvidenceAdmission::DuplicateAttempt;
    }
    if (e.seq < cursor.last_seq) {
      const bool looks_like_wrap = cursor.last_seq.value() >= kSequenceHalfSpace &&
                                   e.seq.value() < kSequenceHalfSpace;
      if (looks_like_wrap && cursor.rollovers < kMaxRolloversPerSource) {
        ++cursor.rollovers;
        admission = EvidenceAdmission::RolloverAccepted;
        reason = ReasonCode::SequenceRollover;
      } else {
        ++cursor.regressed;
        ++cursor.rejected;
        ++rejected_;
        reason = ReasonCode::RegressedSequence;
        return EvidenceAdmission::Regressed;
      }
    }
  }

  if (st.items.size() >= policy_.max_evidence_per_scope) {
    // Bounded retention: evict the oldest retained observation for this scope and
    // account for the eviction exactly. Eviction is never silent.
    std::size_t victim = 0;
    for (std::size_t i = 1; i < st.items.size(); ++i) {
      const DeliveryEvidence& a = st.items[i];
      const DeliveryEvidence& b = st.items[victim];
      if (a.observed_at < b.observed_at ||
          (a.observed_at == b.observed_at && a.seq < b.seq)) {
        victim = i;
      }
    }
    st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(victim));
    --retained_;
    ++dropped_;
    ++st.evictions;
  }

  st.items.push_back(e);
  ++retained_;
  ++admitted_;

  cursor.last_seq = e.seq;
  if (!e.attempt.is_nil()) cursor.last_attempt = e.attempt;
  cursor.last_observed = e.observed_at;
  ++cursor.accepted;
  return admission;
}

std::vector<DeliveryEvidence> EvidenceLedger::evidence_for(const Scope& scope) const {
  std::vector<DeliveryEvidence> out;
  const auto it = scopes_.find(scope);
  if (it == scopes_.end()) return out;
  out = it->second.items;
  std::sort(out.begin(), out.end(), [](const DeliveryEvidence& a, const DeliveryEvidence& b) {
    if (a.observed_at != b.observed_at) return a.observed_at < b.observed_at;
    if (a.source != b.source) return a.source < b.source;
    return a.seq < b.seq;
  });
  return out;
}

}  // namespace bhg
