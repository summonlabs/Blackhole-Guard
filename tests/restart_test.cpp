#include <algorithm>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// Drives an engine into a corroborated blackhole with an open fence intent.
bool drive_to_fence(Engine& engine, const Scope& scope, const GenerationVector& gens, WallNs at,
                    Decision& decision) {
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t source = 1; source <= 2; ++source) {
    const DeliveryEvidence e = failure_evidence(EvidenceSourceId{source}, IncarnationId{source},
                                                scope, gens, EvidenceSequence{1}, at, 32);
    (void)engine.submit(e, admission, reason);
  }
  (void)engine.evaluate(scope, gens, decision);
  return decision.has_intent;
}

}  // namespace

BHG_TEST(restart, restart_advances_epoch_and_mints_a_new_incarnation) {
  EngineHarness harness("restart-epoch", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const CoordinatorEpoch first = harness.engine().epoch();
  const OpenReport first_report = harness.engine().open_report();
  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  const OpenReport second = harness.engine().open_report();
  BHG_CHECK(second.epoch.value() > first.value());
  BHG_CHECK(second.boot != first_report.boot);
  BHG_CHECK(second.incarnation != first_report.incarnation);
  BHG_CHECK(second.restart_detected);
  BHG_CHECK_EQ(second.boot_count, 2u);
  BHG_CHECK_EQ(second.open_count, 2u);
}

BHG_TEST(restart, prior_fences_are_fenced_and_surfaced_as_interruptions) {
  EngineHarness harness("restart-fence", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();

  Decision decision;
  BHG_REQUIRE(drive_to_fence(harness.engine(), scope, gens, at, decision));
  const FenceId fence = decision.intent.id;
  FenceRecord before;
  BHG_REQUIRE(is_affirmative(harness.engine().fence_state(fence, before)));
  BHG_CHECK(before.is_open());

  harness.reset_engine();
  BHG_CHECK(is_affirmative(harness.open()));
  const OpenReport report = harness.engine().open_report();
  BHG_CHECK_EQ(report.fenced_prior_fences, 1u);

  // The pre-restart intent is not resurrected into the new incarnation.
  FenceRecord after;
  BHG_CHECK(harness.engine().fence_state(fence, after) == Outcome::NotFound);

  // It is visible as an interruption in durable lineage.
  std::vector<LineageEntry> entries;
  std::uint64_t seen = 0;
  std::uint64_t dropped = 0;
  BHG_REQUIRE(is_affirmative(harness.engine().lineage(512, entries, seen, dropped)));
  bool interrupted = false;
  bool fenced_revoke = false;
  for (const LineageEntry& e : entries) {
    if (e.type == RecordType::InterruptionCommit && e.fence == fence) {
      interrupted = true;
      BHG_CHECK(e.outcome == Outcome::Interrupted);
      BHG_CHECK(e.reason == ReasonCode::RestartFencedPriorAuthority);
      BHG_CHECK(e.from_prior_incarnation);
    }
    if (e.type == RecordType::FenceRevokeCommit && e.fence == fence) {
      fenced_revoke = true;
    }
  }
  BHG_CHECK(interrupted);
  BHG_CHECK(fenced_revoke);
}

BHG_TEST(restart, live_evidence_is_not_restored_and_diagnosis_returns_to_no_evidence) {
  EngineHarness harness("restart-liveness", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{2});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();

  Decision first;
  BHG_REQUIRE(drive_to_fence(harness.engine(), scope, gens, at, first));
  BHG_CHECK(first.classification == Classification::Blackhole);
  BHG_CHECK(first.corroborated);

  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));

  Decision after;
  (void)harness.engine().evaluate(scope, gens, after);
  BHG_CHECK(after.outcome == Outcome::NoEvidence);
  BHG_CHECK(after.classification == Classification::NoEvidence);
  BHG_CHECK(after.authority.stage == AuthorityStage::None);
  BHG_CHECK(!after.has_intent);

  // Restoration is refused: absence of evidence is not evidence of restoration.
  Decision restore_decision;
  (void)harness.engine().restore(scope, gens, restore_decision);
  BHG_CHECK(restore_decision.outcome == Outcome::NoEvidence);
  BHG_CHECK(static_cast<std::uint8_t>(restore_decision.authority.stage) <
            static_cast<std::uint8_t>(AuthorityStage::Authorization));
}

BHG_TEST(restart, prior_diagnoses_are_demoted_to_lineage_not_authority) {
  EngineHarness harness("restart-demote", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{3});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  Decision first;
  BHG_REQUIRE(drive_to_fence(harness.engine(), scope, gens, at, first));
  const DecisionId first_id = first.id;

  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  std::vector<LineageEntry> entries;
  std::uint64_t seen = 0;
  std::uint64_t dropped = 0;
  BHG_REQUIRE(is_affirmative(harness.engine().lineage(512, entries, seen, dropped)));
  bool found = false;
  for (const LineageEntry& e : entries) {
    if (e.decision == first_id && e.type == RecordType::DecisionCommit) {
      found = true;
      BHG_CHECK(e.from_prior_incarnation);
      BHG_CHECK(e.classification == Classification::Blackhole);
    }
  }
  BHG_CHECK(found);
  // The claim survives as history; it does not produce current authority.
  Decision now_decision;
  (void)harness.engine().evaluate(scope, gens, now_decision);
  BHG_CHECK(now_decision.id != first_id);
  BHG_CHECK(now_decision.classification == Classification::NoEvidence);
}

BHG_TEST(restart, repeated_restarts_are_monotonic_and_consistent) {
  EngineHarness harness("restart-many", default_policy(), Provenance::Synthetic);
  const Scope scope = path_scope(PathId{4});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  std::uint64_t last_epoch = 0;
  std::uint64_t last_boot_count = 0;
  for (int cycle = 0; cycle < 5; ++cycle) {
    BHG_REQUIRE(is_affirmative(harness.open()));
    const OpenReport r = harness.engine().open_report();
    BHG_CHECK(r.epoch.value() > last_epoch);
    BHG_CHECK(r.boot_count > last_boot_count);
    last_epoch = r.epoch.value();
    last_boot_count = r.boot_count;
    const WallNs at = harness.clock().wall_now();
    Decision d;
    (void)drive_to_fence(harness.engine(), scope, gens, at, d);
    harness.clock().advance(seconds(1));
    harness.reset_engine();
  }
  BHG_REQUIRE(is_affirmative(harness.open()));
  BHG_CHECK_EQ(harness.engine().open_report().boot_count, 6u);
  BHG_CHECK_EQ(harness.engine().open_report().fenced_prior_fences, 1u);
}

BHG_TEST(restart, restart_after_checkpoint_replays_from_the_snapshot) {
  EngineHarness harness("restart-snapshot", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{5});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  Decision d;
  BHG_REQUIRE(drive_to_fence(harness.engine(), scope, gens, at, d));

  RecordSequence snapshot_seq{};
  BHG_REQUIRE(is_affirmative(harness.engine().checkpoint(snapshot_seq)));
  BHG_CHECK(snapshot_seq.value() > 0ull);
  const std::uint64_t lineage_before = harness.engine().open_report().prior_lineage_entries;

  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  const OpenReport r = harness.engine().open_report();
  BHG_CHECK(r.snapshot_present);
  BHG_CHECK_EQ(r.fenced_prior_fences, 1u);
  BHG_CHECK(r.restart_detected);
  BHG_CHECK_EQ(r.fenced_prior_fences, 1u);
  std::vector<LineageEntry> entries;
  std::uint64_t seen = 0;
  std::uint64_t dropped = 0;
  BHG_REQUIRE(is_affirmative(harness.engine().lineage(512, entries, seen, dropped)));
  BHG_CHECK(!entries.empty());
  BHG_CHECK(seen >= lineage_before);
}

BHG_TEST(restart, engine_refuses_operations_before_open_and_after_close) {
  EngineHarness harness("restart-closed", default_policy(), Provenance::Synthetic);
  const Scope scope = path_scope(PathId{6});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  Decision d;
  BHG_CHECK(!is_affirmative(harness.engine().evaluate(scope, gens, d)));
  BHG_CHECK(d.outcome == Outcome::Unavailable);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(static_cast<std::uint8_t>(d.authority.stage) <
            static_cast<std::uint8_t>(AuthorityStage::Authorization));

  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  const DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                              EvidenceSequence{1}, harness.clock().wall_now());
  BHG_CHECK(!is_affirmative(harness.engine().submit(e, admission, reason)));

  BHG_REQUIRE(is_affirmative(harness.open()));
  BHG_REQUIRE(is_affirmative(harness.engine().close()));
  BHG_CHECK(!is_affirmative(harness.engine().evaluate(scope, gens, d)));
  RecordSequence seq{};
  BHG_CHECK(harness.engine().checkpoint(seq) == Outcome::Unavailable);
}

BHG_TEST(restart, restart_does_not_restore_expired_fence_authority) {
  FencePolicy policy = default_policy();
  policy.fence_ttl = seconds(5);
  EngineHarness harness("restart-ttl", policy, Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{7});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  Decision d;
  BHG_REQUIRE(drive_to_fence(harness.engine(), scope, gens, at, d));
  const FenceId fence = d.intent.id;

  // Let the intent expire, then restart: it must be fenced, not revived.
  harness.clock().advance(seconds(60));
  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  FenceRecord rec;
  BHG_CHECK(harness.engine().fence_state(fence, rec) == Outcome::NotFound);
  BHG_CHECK_EQ(harness.engine().open_report().fenced_prior_fences, 1u);
}
