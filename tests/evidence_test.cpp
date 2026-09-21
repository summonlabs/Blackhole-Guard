#include <algorithm>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

struct LedgerFixture {
  LedgerFixture() : ledger(policy) {}
  FencePolicy policy = default_policy();
  EvidenceLedger ledger;
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const EvidenceSourceId src{EvidenceSourceId{7}};
  const IncarnationId inc{IncarnationId{11}};
  WallNs now{WallNs{1000000000}};
};

}  // namespace

BHG_TEST(evidence, accepts_in_order_and_counts_exactly) {
  LedgerFixture f;
  for (std::uint64_t i = 1; i <= 5; ++i) {
    ReasonCode why = ReasonCode::None;
    const DeliveryEvidence e = failure_evidence(f.src, f.inc, f.scope, f.gens,
                                                EvidenceSequence{i}, f.now);
    const EvidenceAdmission a = f.ledger.admit(e, f.now, why);
    BHG_CHECK(a == EvidenceAdmission::Accepted);
  }
  BHG_CHECK_EQ(f.ledger.retained(), 5u);
  BHG_CHECK_EQ(f.ledger.admitted(), 5u);
  BHG_CHECK_EQ(f.ledger.rejected(), 0u);
  BHG_CHECK_EQ(f.ledger.seen(), 5u);
  BHG_CHECK(f.ledger.accounting_closed());
}

BHG_TEST(evidence, duplicate_sequence_and_attempt_are_refused) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  const DeliveryEvidence e1 = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{5}, f.now);
  BHG_CHECK(f.ledger.admit(e1, f.now, why) == EvidenceAdmission::Accepted);

  const DeliveryEvidence e2 = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{5}, f.now);
  BHG_CHECK(f.ledger.admit(e2, f.now, why) == EvidenceAdmission::DuplicateAttempt);
  BHG_CHECK(why == ReasonCode::DuplicateEvidence);

  // Same attempt id, different sequence: still a duplicate of the same attempt.
  DeliveryEvidence e3 = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{6}, f.now);
  e3.attempt = e1.attempt;
  e3.id = derive_evidence_id(e3);
  BHG_CHECK(f.ledger.admit(e3, f.now, why) == EvidenceAdmission::DuplicateAttempt);
  BHG_CHECK(f.ledger.accounting_closed());
}

BHG_TEST(evidence, replayed_lower_sequence_is_regressed) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  const DeliveryEvidence high = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{100}, f.now);
  BHG_CHECK(f.ledger.admit(high, f.now, why) == EvidenceAdmission::Accepted);
  const DeliveryEvidence low = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{42}, f.now);
  BHG_CHECK(f.ledger.admit(low, f.now, why) == EvidenceAdmission::Regressed);
  BHG_CHECK(why == ReasonCode::RegressedSequence);
  // The refusal must not have moved the cursor: the retry of 101 still works.
  const DeliveryEvidence next = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{101}, f.now);
  BHG_CHECK(f.ledger.admit(next, f.now, why) == EvidenceAdmission::Accepted);
}

BHG_TEST(evidence, counter_rollover_is_accepted_once_and_then_bounded) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  const DeliveryEvidence before = failure_evidence(f.src, f.inc, f.scope, f.gens,
                                                   EvidenceSequence{1ull << 63}, f.now);
  BHG_CHECK(f.ledger.admit(before, f.now, why) == EvidenceAdmission::Accepted);
  const DeliveryEvidence wrapped = failure_evidence(f.src, f.inc, f.scope, f.gens,
                                                    EvidenceSequence{1}, f.now);
  BHG_CHECK(f.ledger.admit(wrapped, f.now, why) == EvidenceAdmission::RolloverAccepted);
  BHG_CHECK(why == ReasonCode::SequenceRollover);
  // After the wrap the space continues forwards.
  BHG_CHECK(f.ledger.admit(failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{2}, f.now),
                           f.now, why) == EvidenceAdmission::Accepted);
  // Further wraps are refused so an adversary cannot loop the counter forever.
  for (int i = 0; i < 8; ++i) {
    (void)f.ledger.admit(failure_evidence(f.src, f.inc, f.scope, f.gens,
                                          EvidenceSequence{1ull << 63}, f.now),
                         f.now, why);
    const EvidenceAdmission a =
        f.ledger.admit(failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{3}, f.now),
                       f.now, why);
    if (a == EvidenceAdmission::Regressed) break;
  }
  BHG_CHECK(why == ReasonCode::RegressedSequence);
  BHG_CHECK(f.ledger.accounting_closed());
}

BHG_TEST(evidence, stale_window_and_future_clock_are_refused) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  DeliveryEvidence old = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{1},
                                          WallNs{f.now.ns - 120 * kNsPerSecond});
  BHG_CHECK(f.ledger.admit(old, f.now, why) == EvidenceAdmission::OutOfFreshnessWindow);
  BHG_CHECK(why == ReasonCode::FreshnessWindowExpired);

  DeliveryEvidence future = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{2},
                                             WallNs{f.now.ns + 60 * kNsPerSecond});
  BHG_CHECK(f.ledger.admit(future, f.now, why) == EvidenceAdmission::ClockSkewExceeded);
  BHG_CHECK(why == ReasonCode::ClockSkewExceeded);
  BHG_CHECK(f.ledger.accounting_closed());
}

BHG_TEST(evidence, structurally_invalid_is_refused) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  DeliveryEvidence bad = failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{1}, f.now);
  bad.failures = bad.attempts + 1;
  BHG_CHECK(f.ledger.admit(bad, f.now, why) == EvidenceAdmission::StructurallyInvalid);
  BHG_CHECK(why == ReasonCode::InvalidEvidenceRejected);
  BHG_CHECK(f.ledger.accounting_closed());
}

BHG_TEST(evidence, retention_is_bounded_and_evictions_are_counted) {
  FencePolicy policy = default_policy();
  policy.max_evidence_per_scope = 8;
  EvidenceLedger ledger(policy);
  const Scope scope = path_scope(PathId{2});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now{WallNs{2000000000}};
  ReasonCode why = ReasonCode::None;
  for (std::uint64_t i = 1; i <= 40; ++i) {
    const DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                                EvidenceSequence{i}, now);
    BHG_CHECK(ledger.admit(e, now, why) == EvidenceAdmission::Accepted);
  }
  BHG_CHECK_EQ(ledger.retained(), 8u);
  BHG_CHECK_EQ(ledger.admitted(), 40u);
  BHG_CHECK_EQ(ledger.dropped(), 32u);
  BHG_CHECK(ledger.accounting_closed());
  const auto items = ledger.evidence_for(scope);
  BHG_CHECK_EQ(items.size(), 8u);
  // The newest eight survive.
  BHG_CHECK_EQ(items.front().seq.value(), 33ull);
  BHG_CHECK_EQ(items.back().seq.value(), 40ull);
}

BHG_TEST(evidence, per_scope_source_table_is_bounded_without_forgetting) {
  FencePolicy policy = default_policy();
  policy.max_sources_per_scope = 2;
  EvidenceLedger ledger(policy);
  const Scope scope = path_scope(PathId{3});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now{WallNs{3000000000}};
  ReasonCode why = ReasonCode::None;
  BHG_CHECK(ledger.admit(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                          EvidenceSequence{1}, now),
                         now, why) == EvidenceAdmission::Accepted);
  BHG_CHECK(ledger.admit(failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, scope, gens,
                                          EvidenceSequence{1}, now),
                         now, why) == EvidenceAdmission::Accepted);
  BHG_CHECK(ledger.admit(failure_evidence(EvidenceSourceId{3}, IncarnationId{3}, scope, gens,
                                          EvidenceSequence{1}, now),
                         now, why) == EvidenceAdmission::CapacityExhausted);
  // Source 1 keeps working and its cursor was never forgotten.
  BHG_CHECK(ledger.admit(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                          EvidenceSequence{2}, now),
                         now, why) == EvidenceAdmission::Accepted);
  BHG_CHECK(ledger.admit(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                          EvidenceSequence{1}, now),
                         now, why) == EvidenceAdmission::Regressed ||
             why == ReasonCode::DuplicateEvidence);
  BHG_CHECK(ledger.accounting_closed());
}

BHG_TEST(evidence, tracked_scope_table_is_bounded) {
  FencePolicy policy = default_policy();
  policy.max_tracked_scopes = 3;
  EvidenceLedger ledger(policy);
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now{WallNs{4000000000}};
  ReasonCode why = ReasonCode::None;
  for (std::uint64_t i = 1; i <= 6; ++i) {
    const Scope s = path_scope(PathId{i});
    const EvidenceAdmission a = ledger.admit(failure_evidence(EvidenceSourceId{1}, IncarnationId{1},
                                                              s, gens, EvidenceSequence{1}, now),
                                             now, why);
    if (i <= 3) {
      BHG_CHECK(a == EvidenceAdmission::Accepted);
    } else {
      BHG_CHECK(a == EvidenceAdmission::CapacityExhausted);
      BHG_CHECK(why == ReasonCode::BudgetExhausted);
    }
  }
  BHG_CHECK_EQ(ledger.tracked_scopes(), 3u);
  BHG_CHECK(ledger.accounting_closed());
}

BHG_TEST(evidence, reset_drops_everything) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  (void)f.ledger.admit(failure_evidence(f.src, f.inc, f.scope, f.gens, EvidenceSequence{1}, f.now),
                       f.now, why);
  f.ledger.reset();
  BHG_CHECK_EQ(f.ledger.retained(), 0u);
  BHG_CHECK_EQ(f.ledger.seen(), 0u);
  BHG_CHECK(f.ledger.accounting_closed());
  BHG_CHECK(f.ledger.evidence_for(f.scope).empty());
}

BHG_TEST(evidence, evidence_for_is_deterministic_and_sorted) {
  LedgerFixture f;
  ReasonCode why = ReasonCode::None;
  // Insert out of order but observe later timestamps for lower sequences.
  for (std::uint64_t i = 1; i <= 6; ++i) {
    const WallNs at{f.now.ns - static_cast<std::int64_t>(i) * 1000};
    const DeliveryEvidence e = failure_evidence(EvidenceSourceId{i}, IncarnationId{i}, f.scope,
                                                f.gens, EvidenceSequence{1}, at);
    (void)f.ledger.admit(e, f.now, why);
  }
  const auto a = f.ledger.evidence_for(f.scope);
  const auto b = f.ledger.evidence_for(f.scope);
  BHG_CHECK_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    BHG_CHECK(a[i].id == b[i].id);
    if (i > 0) BHG_CHECK(a[i - 1].observed_at <= a[i].observed_at);
  }
}
