#include <algorithm>
#include <set>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// Asserts every product invariant that must hold after *any* operation.
void assert_decision_invariants(const Decision& d, const FencePolicy& policy, const char* where) {
  if (!d.is_valid()) {
    Registry::instance().fail(__FILE__, __LINE__, std::string("invalid decision at ") + where);
  }
  if (d.reasons.size() > policy.max_reasons_per_decision) {
    Registry::instance().fail(__FILE__, __LINE__,
                              std::string("reason list exceeded its bound at ") + where);
  }
  // THE product invariant: positive authority requires a corroborated, affirmative
  // blackhole classification. UNKNOWN / STALE / NO_EVIDENCE / CONFLICT can never
  // reach Authorization or above.
  if (static_cast<std::uint8_t>(d.authority.stage) >=
      static_cast<std::uint8_t>(AuthorityStage::Authorization)) {
    if (d.kind == DecisionKind::Diagnosis) {
      if (d.classification != Classification::Blackhole || d.outcome != Outcome::Ok) {
        Registry::instance().fail(__FILE__, __LINE__,
                                  std::string("authority granted without a confirmed blackhole at ") +
                                      where);
      }
    }
  }
  if (d.has_intent) {
    if (d.authority.stage != AuthorityStage::Intent) {
      Registry::instance().fail(__FILE__, __LINE__, "intent without Intent authority");
    }
    if (!d.intent.is_valid() || d.intent.gens != d.gens || d.intent.boot != d.boot ||
        d.intent.incarnation != d.incarnation) {
      Registry::instance().fail(__FILE__, __LINE__, "intent not bound to decision authority");
    }
  }
  if (d.authority.stage == AuthorityStage::Eligibility && may_authorize_fence(d.classification)) {
    // Eligibility is allowed for an uncorroborated blackhole; nothing stronger.
  }
}

}  // namespace

BHG_TEST(property, unknown_and_stale_never_authorize_fencing) {
  Rng rng(0xC0FFEEull);
  EngineHarness harness("prop-unknown", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector current = make_gens(1, 1, 1, 1);
  const WallNs now = harness.clock().wall_now();

  for (int step = 0; step < 400; ++step) {
    const std::uint32_t mode = rng.below(6u);
    DeliveryEvidence e;
    switch (mode) {
      case 0:
        e = failure_evidence(EvidenceSourceId{rng.below(3u) + 1u},
                             IncarnationId{rng.below(2u) + 1u}, scope, current,
                             EvidenceSequence{rng.below(1000u) + 1u}, now, 32);
        break;
      case 1:
        // Stale: window already closed.
        e = failure_evidence(EvidenceSourceId{rng.below(3u) + 1u},
                             IncarnationId{rng.below(2u) + 1u}, scope, current,
                             EvidenceSequence{rng.below(1000u) + 1u},
                             WallNs{now.ns - (70 + static_cast<std::int64_t>(rng.below(30u))) * kNsPerSecond},
                             32);
        break;
      case 2:
        // Generation-mismatched.
        e = failure_evidence(EvidenceSourceId{rng.below(3u) + 1u},
                             IncarnationId{rng.below(2u) + 1u}, scope,
                             make_gens(1, 1, 1, 1 + rng.below(3u)),
                             EvidenceSequence{rng.below(1000u) + 1u}, now, 32);
        break;
      case 3:
        e = success_evidence(EvidenceSourceId{rng.below(3u) + 1u},
                             IncarnationId{rng.below(2u) + 1u}, scope, current,
                             EvidenceSequence{rng.below(1000u) + 1u}, now, 32);
        break;
      case 4:
        e = congestion_evidence(EvidenceSourceId{rng.below(3u) + 8u},
                                IncarnationId{rng.below(2u) + 8u}, scope, current,
                                EvidenceSequence{rng.below(1000u) + 1u}, now,
                                EvidenceSourceId{99});
        break;
      default:
        e = loss_evidence(EvidenceSourceId{rng.below(3u) + 1u},
                          IncarnationId{rng.below(2u) + 1u}, scope, current,
                          EvidenceSequence{rng.below(1000u) + 1u}, now, rng.below(999999u), 100);
        break;
    }
    EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
    ReasonCode reason = ReasonCode::None;
    (void)harness.engine().submit(e, admission, reason);

    Decision d;
    const Outcome o = harness.engine().evaluate(scope, current, d);
    (void)o;
    assert_decision_invariants(d, harness.policy(), "randomized loop");

    // If the decision did not reach a confirmed blackhole, it must not carry intent.
    if (d.classification != Classification::Blackhole) {
      if (d.has_intent) {
        Registry::instance().fail(__FILE__, __LINE__, "non-blackhole decision carried a fence intent");
      }
    }
  }
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
}

BHG_TEST(property, engine_accounting_and_retention_stay_closed) {
  Rng rng(0xDEADBEEFull);
  FencePolicy policy = default_policy();
  policy.max_evidence_per_scope = 16;
  policy.max_lineage_records = 64;
  EngineHarness harness("prop-accounting", policy, Provenance::Synthetic);
  harness.store_config().max_lineage_records = 64;
  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));

  const Scope scope = path_scope(PathId{5});
  const GenerationVector current = make_gens(2, 2, 2, 2);
  const WallNs now = harness.clock().wall_now();

  for (int step = 0; step < 300; ++step) {
    const std::uint64_t seq = static_cast<std::uint64_t>(step) + 1u;
    DeliveryEvidence e =
        (rng.coin() ? failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, current,
                                       EvidenceSequence{seq}, now, 32)
                    : success_evidence(EvidenceSourceId{2}, IncarnationId{2}, scope, current,
                                       EvidenceSequence{seq}, now, 32));
    EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
    ReasonCode reason = ReasonCode::None;
    (void)harness.engine().submit(e, admission, reason);
    Decision d;
    (void)harness.engine().evaluate(scope, current, d);
    assert_decision_invariants(d, harness.policy(), "accounting loop");

    const EngineStats s = harness.engine().stats();
    if (!s.accounting_closed()) {
      Registry::instance().fail(__FILE__, __LINE__, "engine accounting did not close");
      break;
    }
    std::vector<LineageEntry> entries;
    std::uint64_t seen = 0;
    std::uint64_t dropped = 0;
    (void)harness.engine().lineage(64, entries, seen, dropped);
    if (seen != static_cast<std::uint64_t>(entries.size()) + dropped) {
      Registry::instance().fail(__FILE__, __LINE__, "lineage accounting did not close");
      break;
    }
  }
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
  BHG_CHECK(s.evaluations == s.decisions_committed + s.durable_commit_failures);
}

BHG_TEST(property, evidence_insertion_order_does_not_change_the_decision) {
  Rng rng(0x1234ABCDull);
  const std::uint32_t count = 24;
  std::vector<DeliveryEvidence> events;
  const Scope scope = path_scope(PathId{9});
  const GenerationVector gens = make_gens(4, 4, 4, 4);
  const WallNs base{WallNs{9000000000}};
  for (std::uint32_t i = 0; i < count; ++i) {
    const WallNs at{base.ns + static_cast<std::int64_t>(i) * 1000};
    events.push_back(failure_evidence(EvidenceSourceId{i % 3u + 1u}, IncarnationId{i % 2u + 1u},
                                      scope, gens, EvidenceSequence{i + 1u}, at, 32));
  }

  auto run = [&](const std::vector<DeliveryEvidence>& order) {
    EngineHarness harness("prop-order", default_policy(), Provenance::Synthetic);
    if (!is_affirmative(harness.open())) return DecisionId{};
    for (const DeliveryEvidence& e : order) {
      EvidenceAdmission a = EvidenceAdmission::StructurallyInvalid;
      ReasonCode r = ReasonCode::None;
      (void)harness.engine().submit(e, a, r);
    }
    Decision d;
    (void)harness.engine().evaluate(scope, gens, d);
    return d.id;
  };

  std::vector<DeliveryEvidence> shuffled = events;
  for (std::size_t i = shuffled.size(); i > 1; --i) {
    const std::size_t j = rng.below(static_cast<std::uint32_t>(i));
    std::swap(shuffled[i - 1], shuffled[j]);
  }

  const DecisionId a = run(events);
  const DecisionId b = run(shuffled);
  BHG_CHECK(!a.is_nil());
  BHG_CHECK_EQ(a.value(), b.value());
}

BHG_TEST(property, fence_authority_never_outlives_its_dependencies) {
  Rng rng(0xABCDEF01ull);
  FencePolicy policy = default_policy();
  policy.fence_ttl = seconds(10);
  EngineHarness harness("prop-fence", policy, Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));

  const Scope scope = path_scope(PathId{3});
  GenerationVector current = make_gens(1, 1, 1, 1);
  const WallNs start = harness.clock().wall_now();

  for (std::uint64_t i = 1; i <= 20; ++i) {
    const WallNs at{start.ns + static_cast<std::int64_t>(i) * kNsPerSecond};
    harness.clock().set_wall(at);
    DeliveryEvidence e1 = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, current,
                                           EvidenceSequence{i * 2u}, at, 32);
    DeliveryEvidence e2 = failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, scope, current,
                                           EvidenceSequence{i * 2u}, at, 32);
    EvidenceAdmission a = EvidenceAdmission::StructurallyInvalid;
    ReasonCode r = ReasonCode::None;
    (void)harness.engine().submit(e1, a, r);
    (void)harness.engine().submit(e2, a, r);

    Decision d;
    (void)harness.engine().evaluate(scope, current, d);
    assert_decision_invariants(d, harness.policy(), "fence loop");

    if (d.has_intent) {
      FenceRecord rec;
      BHG_REQUIRE(is_affirmative(harness.engine().fence_state(d.intent.id, rec)));
      BHG_CHECK(rec.intent.gens == current);
      BHG_CHECK(rec.intent.boot == d.boot);
      BHG_CHECK(rec.intent.expires_at.ns > rec.intent.issued_at.ns);

      // Move the generation: the fence must be revoked on the next evaluation.
      current = make_gens(1, 1, 1, current.epoch.value() + 1u);
      Decision next;
      (void)harness.engine().evaluate(scope, current, next);
      FenceRecord after;
      const Outcome fo = harness.engine().fence_state(d.intent.id, after);
      BHG_CHECK(is_affirmative(fo));
      BHG_CHECK(!after.is_open());
      BHG_CHECK(after.terminal_reason == ReasonCode::DependencyGenerationChanged);
    }

    if (rng.coin()) {
      harness.clock().advance(seconds(3));
    }
  }
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
}

BHG_TEST(property, restoration_never_authorized_from_non_healthy_evidence) {
  Rng rng(0x00FACADEull);
  EngineHarness harness("prop-restore", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{4});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now = harness.clock().wall_now();

  for (int step = 0; step < 200; ++step) {
    DeliveryEvidence e;
    switch (rng.below(4u)) {
      case 0: e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                   EvidenceSequence{rng.below(500u) + 1u}, now, 32); break;
      case 1: e = loss_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                EvidenceSequence{rng.below(500u) + 1u}, now, 500000u, 100); break;
      case 2: e = congestion_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                      EvidenceSequence{rng.below(500u) + 1u}, now,
                                      EvidenceSourceId{9}); break;
      default: e = success_evidence(EvidenceSourceId{rng.below(2u) + 1u},
                                     IncarnationId{rng.below(2u) + 1u}, scope, gens,
                                     EvidenceSequence{rng.below(500u) + 1u}, now, 32); break;
    }
    EvidenceAdmission a = EvidenceAdmission::StructurallyInvalid;
    ReasonCode r = ReasonCode::None;
    (void)harness.engine().submit(e, a, r);

    Decision d;
    (void)harness.engine().restore(scope, gens, d);
    assert_decision_invariants(d, harness.policy(), "restore loop");
    if (static_cast<std::uint8_t>(d.authority.stage) >=
        static_cast<std::uint8_t>(AuthorityStage::Authorization)) {
      if (d.classification != Classification::Healthy) {
        Registry::instance().fail(__FILE__, __LINE__,
                                  "restoration authorized without a healthy classification");
      }
    }
  }
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
}

BHG_TEST(property, localize_never_publishes_a_failed_validation) {
  Rng rng(0x5A5A5A5Aull);
  EngineHarness harness("prop-localize", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{6});
  const GenerationVector gens = make_gens(1, 1, 1, 1);

  for (int step = 0; step < 300; ++step) {
    const std::uint32_t hops = 2u + rng.below(8u);
    std::vector<HopProbe> probes;
    const std::uint32_t count = 1u + rng.below(5u);
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t a = rng.below(hops);
      const std::uint32_t b = a + 1u + rng.below(hops - a);
      probes.push_back(HopProbe{a, b, rng.coin()});
    }
    Decision d;
    (void)harness.engine().localize(scope, gens, hops, probes, d);
    assert_decision_invariants(d, harness.policy(), "localize loop");
    if (!d.has_localization) continue;
    if (d.localization.status == LocalizationStatus::InvalidInput) continue;
    if (d.localization.status == LocalizationStatus::ProvenInfeasible) {
      // A proven-infeasible answer must carry a witness index that is in range.
      BHG_CHECK(d.localization.has_infeasible_witness);
      continue;
    }
    // Published elements must be within the hop range and strictly ascending.
    for (std::size_t i = 0; i < d.localization.elements.size(); ++i) {
      BHG_CHECK(d.localization.elements[i] < hops);
      if (i > 0) BHG_CHECK(d.localization.elements[i - 1] < d.localization.elements[i]);
    }
    if (d.localization.optimality_proven) {
      BHG_CHECK_EQ(d.localization.optimal_size,
                   static_cast<std::uint32_t>(d.localization.elements.size()));
    }
  }
}

BHG_TEST(property, report_and_decision_encoding_survive_mutation_fuzzing) {
  Rng rng(0xFEEDFACEull);
  EngineHarness harness("prop-fuzz", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{7});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now = harness.clock().wall_now();
  for (std::uint64_t i = 1; i <= 6; ++i) {
    EvidenceAdmission a = EvidenceAdmission::StructurallyInvalid;
    ReasonCode r = ReasonCode::None;
    (void)harness.engine().submit(failure_evidence(EvidenceSourceId{i % 2u + 1u},
                                                   IncarnationId{i % 2u + 1u}, scope, gens,
                                                   EvidenceSequence{i}, now, 32),
                                  a, r);
  }
  Decision d;
  (void)harness.engine().evaluate(scope, gens, d);
  BHG_REQUIRE(d.has_intent);

  Writer w(kDefaultMaxDocumentBytes);
  encode(w, d);
  BHG_REQUIRE(w.ok());
  const std::vector<std::byte> original = w.take();

  for (int trial = 0; trial < 3000; ++trial) {
    std::vector<std::byte> bytes = original;
    const std::uint32_t flips = 1u + rng.below(4u);
    for (std::uint32_t f = 0; f < flips; ++f) {
      bytes[rng.below(static_cast<std::uint32_t>(bytes.size()))] ^=
          static_cast<std::byte>(1u << rng.below(8u));
    }
    Reader r(std::span<const std::byte>(bytes.data(), bytes.size()));
    Decision out;
    const Outcome o = decode(r, out);
    if (is_affirmative(o)) {
      // A mutated decision must still be structurally valid; it must never decode
      // into an out-of-domain state.
      BHG_CHECK(out.is_valid());
      BHG_CHECK(is_valid_classification(static_cast<std::uint16_t>(out.classification)));
      BHG_CHECK(is_valid_outcome(static_cast<std::uint8_t>(out.outcome)));
      for (const ReasonCode rc : out.reasons) {
        BHG_CHECK(is_valid_reason_code(static_cast<std::uint16_t>(rc)));
      }
    }
  }
}
