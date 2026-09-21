#include <algorithm>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

void assert_no_unproven_authority(const Decision& d, const char* what) {
  if (static_cast<std::uint8_t>(d.authority.stage) >=
      static_cast<std::uint8_t>(AuthorityStage::Authorization)) {
    if (d.kind == DecisionKind::Diagnosis) {
      if (d.classification != Classification::Blackhole || d.outcome != Outcome::Ok ||
          !d.corroborated) {
        Registry::instance().fail(__FILE__, __LINE__,
                                  std::string("unproven authority granted at ") + what);
      }
    }
  }
  if (d.has_intent && !d.intent.is_valid()) {
    Registry::instance().fail(__FILE__, __LINE__, "invalid intent published");
  }
}

}  // namespace

BHG_TEST(breakit, evidence_flood_from_one_source_cannot_corroborate) {
  EngineHarness harness("break-flood", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t seq = 1; seq <= 500; ++seq) {
    (void)harness.engine().submit(
        failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                         EvidenceSequence{seq}, at, 32),
        admission, reason);
  }
  Decision d;
  (void)harness.engine().evaluate(scope, gens, d);
  BHG_CHECK(d.classification == Classification::Blackhole);
  // One source is one source no matter how many observations it sends.
  BHG_CHECK(!d.corroborated);
  BHG_CHECK(!d.has_intent);
  assert_no_unproven_authority(d, "flood");
  // Retention stayed bounded.
  BHG_CHECK(harness.engine().stats().accounting_closed());
}

BHG_TEST(breakit, split_brain_sources_never_produce_a_fence) {
  EngineHarness harness("break-split", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  // Two authoritative sources disagree: one reports complete failure, one success.
  (void)harness.engine().submit(
      failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens, EvidenceSequence{1}, at),
      admission, reason);
  (void)harness.engine().submit(
      success_evidence(EvidenceSourceId{2}, IncarnationId{2}, scope, gens, EvidenceSequence{1}, at),
      admission, reason);
  Decision d;
  (void)harness.engine().evaluate(scope, gens, d);
  BHG_CHECK(d.outcome == Outcome::Conflict);
  BHG_CHECK(d.classification == Classification::Unknown);
  BHG_CHECK(d.authority.stage != AuthorityStage::Intent);
  assert_no_unproven_authority(d, "split-brain");
}

BHG_TEST(breakit, generation_flapping_revokes_rather_than_authorizes) {
  EngineHarness harness("break-flap", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  const GenerationVector g1 = make_gens(1, 1, 1, 1);
  for (std::uint64_t s = 1; s <= 2; ++s) {
    (void)harness.engine().submit(failure_evidence(EvidenceSourceId{s}, IncarnationId{s}, scope,
                                                   g1, EvidenceSequence{1}, at, 32),
                                  admission, reason);
  }
  Decision first;
  (void)harness.engine().evaluate(scope, g1, first);
  BHG_REQUIRE(first.has_intent);

  for (std::uint64_t epoch = 2; epoch <= 6; ++epoch) {
    const GenerationVector g = make_gens(1, 1, 1, epoch);
    Decision d;
    (void)harness.engine().evaluate(scope, g, d);
    assert_no_unproven_authority(d, "generation flap");
    // Evidence exists only under the old generation vector, so the answer degrades
    // to STALE/UNKNOWN rather than being reused or silently dropped.
    BHG_CHECK(d.outcome == Outcome::Stale);
    BHG_CHECK(d.classification == Classification::Unknown);
    FenceRecord rec;
    BHG_CHECK(harness.engine().fence_state(first.intent.id, rec) == Outcome::NotFound ||
              !rec.is_open());
  }
}

BHG_TEST(breakit, replayed_and_future_evidence_is_rejected) {
  EngineHarness harness("break-replay", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  const DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                              EvidenceSequence{10}, at, 32);
  BHG_CHECK(is_affirmative(harness.engine().submit(e, admission, reason)));
  BHG_CHECK(!is_affirmative(harness.engine().submit(e, admission, reason)));
  BHG_CHECK(admission == EvidenceAdmission::DuplicateAttempt);

  DeliveryEvidence older = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                            EvidenceSequence{4}, at, 32);
  BHG_CHECK(!is_affirmative(harness.engine().submit(older, admission, reason)));
  BHG_CHECK(admission == EvidenceAdmission::Regressed);

  DeliveryEvidence future = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                             EvidenceSequence{11},
                                             WallNs{at.ns + 600 * kNsPerSecond}, 32);
  BHG_CHECK(!is_affirmative(harness.engine().submit(future, admission, reason)));
  BHG_CHECK(admission == EvidenceAdmission::ClockSkewExceeded);
  BHG_CHECK(harness.engine().stats().accounting_closed());
}

BHG_TEST(breakit, journal_exhaustion_fails_closed_without_authority) {
  FencePolicy policy = default_policy();
  EngineHarness harness("break-exhaust", policy, Provenance::Synthetic);
  harness.store_config().max_journal_bytes = 2048;
  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t s = 1; s <= 2; ++s) {
    (void)harness.engine().submit(failure_evidence(EvidenceSourceId{s}, IncarnationId{s}, scope,
                                                   gens, EvidenceSequence{1}, at, 32),
                                  admission, reason);
  }
  bool exhausted_seen = false;
  bool intent_after_exhaustion = false;
  for (int i = 0; i < 400; ++i) {
    Decision d;
    (void)harness.engine().evaluate(scope, gens, d);
    assert_no_unproven_authority(d, "journal exhaustion");
    if (harness.engine().stats().durable_commit_failures > 0u) exhausted_seen = true;
    // Once durable writes are failing, no authority may be claimed at all.
    if (exhausted_seen && d.has_intent) intent_after_exhaustion = true;
    if (d.classification != Classification::Blackhole && d.has_intent) {
      Registry::instance().fail(__FILE__, __LINE__, "intent published on a failed commit");
    }
  }
  const EngineStats exhausted = harness.engine().stats();
  BHG_CHECK(exhausted_seen);
  BHG_CHECK(exhausted.durable_commit_failures > 0u);
  BHG_CHECK(exhausted.decision_commit_failures > 0u);
  BHG_CHECK(!intent_after_exhaustion);
  BHG_CHECK(exhausted.accounting_closed());
  const EngineStats s = exhausted;
  // Recovery is possible and explicit.
  RecordSequence seq{};
  BHG_REQUIRE(is_affirmative(harness.engine().checkpoint(seq)));
  Decision after;
  BHG_REQUIRE(is_affirmative(harness.engine().evaluate(scope, gens, after)));
  BHG_CHECK(after.classification == Classification::Blackhole);
}

BHG_TEST(breakit, boundary_hop_counts_and_localization_limits) {
  EngineHarness harness("break-localize", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);

  // Hop count of zero is refused.
  Decision d;
  BHG_CHECK(!is_affirmative(harness.engine().localize(scope, gens, 0, {}, d)));
  BHG_CHECK(d.has_localization);
  BHG_CHECK(d.localization.status == LocalizationStatus::InvalidInput);

  // Exactly at the supported bound.
  std::vector<HopProbe> probes;
  probes.push_back(HopProbe{0, 64, false});
  BHG_REQUIRE(is_affirmative(harness.engine().localize(scope, gens, 64, probes, d)));
  (void)0;
  BHG_CHECK(d.has_localization);
  BHG_CHECK(d.localization.optimality_proven);

  // Out-of-range probe.
  std::vector<HopProbe> bad;
  bad.push_back(HopProbe{0, 65, false});
  BHG_CHECK(!is_affirmative(harness.engine().localize(scope, gens, 64, bad, d)));

  // Too many probes.
  std::vector<HopProbe> too_many(kMaxLocalizationSets + 1u, HopProbe{0, 1, false});
  BHG_CHECK(!is_affirmative(harness.engine().localize(scope, gens, 64, too_many, d)));
}

BHG_TEST(breakit, extreme_generation_and_sequence_values) {
  EngineHarness harness("break-extremes", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector huge = make_gens(UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  const DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, huge,
                                              EvidenceSequence{UINT64_MAX}, at, UINT64_MAX);
  BHG_CHECK(is_affirmative(harness.engine().submit(e, admission, reason)));
  Decision d;
  (void)harness.engine().evaluate(scope, huge, d);
  BHG_CHECK(d.classification == Classification::Blackhole);
  BHG_CHECK(!d.has_intent);
  assert_no_unproven_authority(d, "extremes");
  BHG_CHECK(harness.engine().stats().accounting_closed());
}

BHG_TEST(breakit, zero_and_nil_identities_are_refused) {
  EngineHarness harness("break-nil", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;

  DeliveryEvidence nil_source = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                                 EvidenceSequence{1}, at, 32);
  nil_source.source.source = EvidenceSourceId{};
  nil_source.id = derive_evidence_id(nil_source);
  BHG_CHECK(!is_affirmative(harness.engine().submit(nil_source, admission, reason)));

  Decision d;
  BHG_CHECK(!is_affirmative(harness.engine().evaluate(Scope{}, gens, d)));
  BHG_CHECK(!is_affirmative(harness.engine().evaluate(scope, GenerationVector{}, d)));
  BHG_CHECK(d.classification == Classification::Unknown);
  assert_no_unproven_authority(d, "nil identities");
}

BHG_TEST(breakit, fence_acknowledgement_after_revocation_is_refused) {
  EngineHarness harness("break-ack", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector g1 = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t s = 1; s <= 2; ++s) {
    (void)harness.engine().submit(failure_evidence(EvidenceSourceId{s}, IncarnationId{s}, scope,
                                                   g1, EvidenceSequence{1}, at, 32),
                                  admission, reason);
  }
  Decision d;
  (void)harness.engine().evaluate(scope, g1, d);
  BHG_REQUIRE(d.has_intent);
  // Move the generation: the intent is revoked on the next evaluation.
  Decision bumped;
  (void)harness.engine().evaluate(scope, make_gens(1, 1, 1, 2), bumped);
  const Outcome ack = harness.engine().acknowledge_fence(d.intent.id, EvidenceSourceId{5}, reason);
  BHG_CHECK(!is_affirmative(ack));
  const Outcome eff =
      harness.engine().report_effect(d.intent.id, EvidenceSourceId{5}, true, reason);
  BHG_CHECK(!is_affirmative(eff));
  // An unknown fence id is refused outright.
  BHG_CHECK(harness.engine().acknowledge_fence(FenceId{4242}, EvidenceSourceId{5}, reason) ==
            Outcome::NotFound);
}

BHG_TEST(breakit, oversized_and_hostile_wire_input_is_refused_before_allocation) {
  // A frame declaring a payload far beyond the configured bound is refused without
  // allocating it.
  Writer w(64);
  w.u32(0x31474842u);
  w.u16(kProtocolVersion);
  w.u16(static_cast<std::uint16_t>(MessageType::LineageRequest));
  w.u32(0);
  w.u32(0x7FFFFFFFu);
  w.u32(0);
  const std::vector<std::byte> bytes = w.take();
  FrameDecoder decoder(1u << 16);
  const Outcome feed = decoder.feed(std::span<const std::byte>(bytes.data(), bytes.size()));
  if (is_affirmative(feed)) {
    Frame f;
    BHG_CHECK(decoder.next(f) == Outcome::Oversized);
  } else {
    BHG_CHECK_EQ(static_cast<int>(feed), static_cast<int>(Outcome::Oversized));
  }
  BHG_CHECK(decoder.failed());
  BHG_CHECK_EQ(decoder.buffered() <= (1u << 16) + kFrameHeaderSize, true);
}
