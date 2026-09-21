#include <algorithm>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

struct ClassifyFixture {
  ClassifyFixture()
      : scope(path_scope(PathId{1})), gens(make_gens(3, 4, 5, 6)), now(WallNs{5000000000}) {}

  Diagnosis run() {
    ClassifyRequest req;
    req.scope = scope;
    req.current = gens;
    req.now = now;
    req.policy = &policy;
    req.evidence = std::span<const DeliveryEvidence>(items.data(), items.size());
    Diagnosis d;
    (void)classify(req, d);
    return d;
  }

  void add(DeliveryEvidence e) { items.push_back(std::move(e)); }

  FencePolicy policy = default_policy();
  Scope scope;
  GenerationVector gens;
  WallNs now;
  std::vector<DeliveryEvidence> items;
};

}  // namespace

BHG_TEST(diagnosis, no_evidence_is_not_unknown_and_never_blackhole) {
  ClassifyFixture f;
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::NoEvidence);
  BHG_CHECK(d.classification == Classification::NoEvidence);
  BHG_CHECK(d.primary_reason == ReasonCode::NoEvidenceObserved);
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, single_source_complete_failure_classifies_blackhole_but_is_uncorroborated) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Ok);
  BHG_CHECK(d.classification == Classification::Blackhole);
  BHG_CHECK(d.primary_reason == ReasonCode::CompleteDeliveryFailure);
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(d.corroboration_reason == ReasonCode::SourcesInsufficient);
}

BHG_TEST(diagnosis, corroborated_complete_failure_from_two_incarnations) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Blackhole);
  BHG_CHECK(d.corroborated);
  BHG_CHECK(d.corroboration_reason == ReasonCode::CorroborationSatisfied);
  BHG_CHECK_EQ(d.census.failure_sources, 2u);
  BHG_CHECK_EQ(d.census.failure_incarnations, 2u);
  BHG_CHECK_EQ(d.census.failure_attempts, 64u);
}

BHG_TEST(diagnosis, same_incarnation_twice_is_not_corroboration) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(failure_evidence(EvidenceSourceId{2}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Blackhole);
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(d.corroboration_reason == ReasonCode::IncarnationsInsufficient);
}

BHG_TEST(diagnosis, severe_loss_is_lossy_not_blackhole) {
  ClassifyFixture f;
  f.add(loss_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens, EvidenceSequence{1},
                      f.now, 999999u));
  f.add(loss_evidence(EvidenceSourceId{2}, IncarnationId{2}, f.scope, f.gens, EvidenceSequence{1},
                      f.now, 999000u));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Lossy);
  BHG_CHECK(d.primary_reason == ReasonCode::SevereLossObserved);
  BHG_CHECK(!may_authorize_fence(d.classification));
  BHG_CHECK(!d.corroborated);
}

BHG_TEST(diagnosis, partial_loss_is_lossy) {
  ClassifyFixture f;
  f.add(loss_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens, EvidenceSequence{1},
                      f.now, 1000u));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Lossy);
  BHG_CHECK(d.primary_reason == ReasonCode::PartialLossObserved);
}

BHG_TEST(diagnosis, congestion_explains_complete_loss_and_blocks_blackhole) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(congestion_evidence(EvidenceSourceId{9}, IncarnationId{9}, f.scope, f.gens,
                            EvidenceSequence{1}, f.now, EvidenceSourceId{77}));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Congested);
  BHG_CHECK(d.primary_reason == ReasonCode::CongestionExplainsLoss);
  BHG_CHECK(!may_authorize_fence(d.classification));
  BHG_CHECK(!d.corroborated);
  BHG_CHECK_EQ(d.census.congestion_signals, 1u);
}

BHG_TEST(diagnosis, partition_explains_complete_loss_and_beats_congestion) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(congestion_evidence(EvidenceSourceId{9}, IncarnationId{9}, f.scope, f.gens,
                            EvidenceSequence{1}, f.now, EvidenceSourceId{77}));
  f.add(partition_evidence(EvidenceSourceId{8}, IncarnationId{8}, f.scope, f.gens,
                           EvidenceSequence{1}, f.now, EvidenceSourceId{66}));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Partitioned);
  BHG_CHECK(d.primary_reason == ReasonCode::PartitionExplainsLoss);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, structural_withdrawal_is_unsupported_not_blackhole) {
  ClassifyFixture f;
  f.add(withdrawal_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                            EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Unsupported);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.primary_reason == ReasonCode::StructurallyWithdrawn);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, contradictory_success_and_failure_is_unknown_conflict) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  f.add(success_evidence(EvidenceSourceId{2}, IncarnationId{2}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Conflict);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.primary_reason == ReasonCode::ConflictingEvidence);
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, generation_mismatch_degrades_to_stale_unknown) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, make_gens(3, 4, 5, 5),
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Stale);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.primary_reason == ReasonCode::GenerationMismatch);
  BHG_CHECK(d.census.mismatch == GenMismatch::Epoch);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, stale_freshness_window_degrades_to_stale) {
  ClassifyFixture f;
  // Observed 70s ago with a 60s freshness window: the window closed 10s ago, so the
  // observation is structurally valid but no longer fresh.
  DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                                        EvidenceSequence{1}, WallNs{f.now.ns - 70 * kNsPerSecond});
  f.add(e);
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Stale);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.primary_reason == ReasonCode::OnlyStaleEvidence);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, weak_quality_is_refused_when_policy_demands_exact) {
  ClassifyFixture f;
  DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                                        EvidenceSequence{1}, f.now);
  e.quality = EvidenceQuality::Approximate;
  e.id = derive_evidence_id(e);
  f.add(e);
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.primary_reason == ReasonCode::QualityBelowPolicy);
  BHG_CHECK_EQ(d.census.quality_rejected, 1u);
}

BHG_TEST(diagnosis, sampled_quality_is_accepted_when_policy_allows) {
  ClassifyFixture f;
  f.policy.require_exact_quality = false;
  DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                                        EvidenceSequence{1}, f.now);
  e.quality = EvidenceQuality::Sampled;
  e.id = derive_evidence_id(e);
  f.add(e);
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Blackhole);
}

BHG_TEST(diagnosis, healthy_delivery_classifies_healthy) {
  ClassifyFixture f;
  f.add(success_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.outcome == Outcome::Ok);
  BHG_CHECK(d.classification == Classification::Healthy);
  BHG_CHECK(!may_authorize_fence(d.classification));
}

BHG_TEST(diagnosis, invalid_evidence_is_counted_not_silently_dropped) {
  ClassifyFixture f;
  DeliveryEvidence bad = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                                          EvidenceSequence{1}, f.now);
  bad.failures = bad.attempts + 5;
  f.add(bad);
  const Diagnosis d = f.run();
  BHG_CHECK_EQ(d.census.total, 1u);
  BHG_CHECK_EQ(d.census.invalid, 1u);
  BHG_CHECK(d.classification == Classification::Unknown);
}

BHG_TEST(diagnosis, attempts_below_policy_block_corroboration_only) {
  ClassifyFixture f;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now, 2));
  f.add(failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now, 2));
  const Diagnosis d = f.run();
  BHG_CHECK(d.classification == Classification::Blackhole);
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(d.corroboration_reason == ReasonCode::AttemptsInsufficient);
}

BHG_TEST(diagnosis, reason_list_is_bounded) {
  ClassifyFixture f;
  f.policy.max_reasons_per_decision = 2;
  f.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, f.scope, f.gens,
                         EvidenceSequence{1}, f.now));
  const Diagnosis d = f.run();
  BHG_CHECK(d.reasons.size() <= 2u);
}

BHG_TEST(diagnosis, fingerprint_is_content_addressed) {
  ClassifyFixture a;
  a.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, a.scope, a.gens,
                         EvidenceSequence{1}, a.now));
  ClassifyFixture b;
  b.add(failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, b.scope, b.gens,
                         EvidenceSequence{1}, b.now));
  const Diagnosis da = a.run();
  const Diagnosis db = b.run();
  BHG_CHECK_EQ(diagnosis_fingerprint(da), diagnosis_fingerprint(db));
  ClassifyFixture c;
  c.add(failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, c.scope, c.gens,
                         EvidenceSequence{1}, c.now));
  BHG_CHECK(diagnosis_fingerprint(c.run()) != diagnosis_fingerprint(da));
}

BHG_TEST(diagnosis, null_policy_and_invalid_scope_are_refused) {
  ClassifyRequest req;
  req.scope = path_scope(PathId{1});
  req.current = make_gens(1, 1, 1, 1);
  req.policy = nullptr;
  Diagnosis d;
  BHG_CHECK(classify(req, d) == Outcome::Invalid);

  const FencePolicy policy = default_policy();
  req.policy = &policy;
  req.scope = Scope{};
  BHG_CHECK(classify(req, d) == Outcome::Invalid);
  req.scope = path_scope(PathId{1});
  req.current = GenerationVector{};
  BHG_CHECK(classify(req, d) == Outcome::Invalid);
}
