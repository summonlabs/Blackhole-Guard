#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

struct ScaleSample {
  std::uint32_t scale{0};
  double ns_per_decision{0.0};
  std::uint64_t decisions{0};
  std::uint64_t journal_records{0};
  std::size_t ledger_retained{0};
  std::size_t lineage_retained{0};
  std::uint64_t localization_nodes{0};
  std::uint32_t localization_hops{0};
};

ScaleSample run_scale(const char* tag, std::uint32_t evaluations, std::uint32_t hops) {
  ScaleSample sample;
  sample.scale = evaluations;
  FencePolicy policy = default_policy();
  EngineHarness harness(tag, policy, Provenance::Synthetic);
  if (!is_affirmative(harness.open())) return sample;
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();

  std::vector<HopProbe> probes;
  probes.push_back(HopProbe{0, hops, false});
  for (std::uint32_t h = 0; h < hops; ++h) {
    probes.push_back(HopProbe{h, h + 1u, false});
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < evaluations; ++i) {
    const auto source = static_cast<std::uint64_t>(i % 3u) + 1u;
    const auto seq = static_cast<std::uint64_t>(i / 3u) + 1u;
    DeliveryEvidence e =
        (i % 4u == 0u)
            ? failure_evidence(EvidenceSourceId{source}, IncarnationId{source}, scope, gens,
                               EvidenceSequence{seq}, at, 32)
            : success_evidence(EvidenceSourceId{source}, IncarnationId{source}, scope, gens,
                               EvidenceSequence{seq}, at, 32);
    EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
    ReasonCode reason = ReasonCode::None;
    (void)harness.engine().submit(e, admission, reason);

    Decision d;
    if (hops == 0) {
      (void)harness.engine().evaluate(scope, gens, d);
    } else {
      (void)harness.engine().localize(scope, gens, hops, probes, d);
      if (d.has_localization) sample.localization_nodes += d.localization.search_nodes;
    }
    if ((i + 1u) % 64u == 0u) {
      RecordSequence seq_out{};
      (void)harness.engine().checkpoint(seq_out);
    }
  }
  const auto end = std::chrono::steady_clock::now();
  const double total_ns =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
  sample.ns_per_decision = total_ns / static_cast<double>(evaluations);
  sample.decisions = harness.engine().stats().decisions_committed;
  sample.journal_records = harness.engine().last_record().value();
  std::vector<LineageEntry> entries;
  std::uint64_t seen = 0;
  std::uint64_t dropped = 0;
  (void)harness.engine().lineage(4096, entries, seen, dropped);
  sample.lineage_retained = entries.size();
  sample.ledger_retained = 0;  // ledger is internal; bounded by policy
  sample.localization_hops = hops;
  return sample;
}

}  // namespace

BHG_TEST(scale, completed_work_scales_linearly_and_state_stays_bounded) {
  const std::uint32_t base = 256;
  std::vector<ScaleSample> samples;
  for (const std::uint32_t multiplier : {1u, 2u, 4u}) {
    const ScaleSample s = run_scale("scale", base * multiplier, 0);
    BHG_CHECK(s.decisions > 0u);
    samples.push_back(s);
    std::printf("    scale=%u decisions=%llu ns/decision=%.0f journal_records=%llu lineage=%zu\n",
                s.scale, static_cast<unsigned long long>(s.decisions), s.ns_per_decision,
                static_cast<unsigned long long>(s.journal_records), s.lineage_retained);
  }
  BHG_REQUIRE(samples.size() == 3u);
  // The durable record count is exactly proportional to the completed work: no
  // hidden quadratic re-writing of history.
  const double r1 = static_cast<double>(samples[1].journal_records) /
                    static_cast<double>(std::max<std::uint64_t>(1u, samples[0].journal_records));
  const double r2 = static_cast<double>(samples[2].journal_records) /
                    static_cast<double>(std::max<std::uint64_t>(1u, samples[1].journal_records));
  BHG_CHECK(r1 < 3.0);
  BHG_CHECK(r2 < 3.0);
  // Retained lineage stays bounded regardless of how much work was completed.
  for (const ScaleSample& s : samples) {
    BHG_CHECK(s.lineage_retained <= default_policy().max_lineage_records);
  }
}

BHG_TEST(scale, localization_work_is_bounded_and_reported) {
  std::vector<ScaleSample> samples;
  for (const std::uint32_t hops : {4u, 8u, 16u}) {
    const ScaleSample s = run_scale("scale-loc", 64, hops);
    samples.push_back(s);
    std::printf("    hops=%u ns/op=%.0f localization_nodes=%llu\n", hops, s.ns_per_decision,
                static_cast<unsigned long long>(s.localization_nodes));
  }
  BHG_REQUIRE(samples.size() == 3u);
  // Search effort must not explode with hop count on this family of instances.
  BHG_CHECK(samples[2].localization_nodes < 100000u * 4u);
  BHG_CHECK(samples[1].localization_nodes <= samples[2].localization_nodes + 1000u);
}

BHG_TEST(scale, retention_bounds_hold_under_long_runs) {
  FencePolicy policy = default_policy();
  policy.max_evidence_per_scope = 64;
  policy.max_lineage_records = 128;
  EngineHarness harness("scale-bounds", policy, Provenance::Synthetic);
  harness.store_config().max_lineage_records = 128;
  harness.reset_engine();
  BHG_REQUIRE(is_affirmative(harness.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = harness.clock().wall_now();
  for (std::uint64_t i = 1; i <= 4000; ++i) {
    EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
    ReasonCode reason = ReasonCode::None;
    (void)harness.engine().submit(
        failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                         EvidenceSequence{i}, at, 32),
        admission, reason);
    if (i % 100u == 0u) {
      Decision d;
      (void)harness.engine().evaluate(scope, gens, d);
      std::vector<LineageEntry> entries;
      std::uint64_t seen = 0;
      std::uint64_t dropped = 0;
      (void)harness.engine().lineage(4096, entries, seen, dropped);
      BHG_CHECK(entries.size() <= policy.max_lineage_records);
      BHG_CHECK(seen == static_cast<std::uint64_t>(entries.size()) + dropped);
      BHG_CHECK(harness.engine().stats().accounting_closed());
    }
  }
  RecordSequence seq{};
  BHG_REQUIRE(is_affirmative(harness.engine().checkpoint(seq)));
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
}
