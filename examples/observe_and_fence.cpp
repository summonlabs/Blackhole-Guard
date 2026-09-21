/// Example: embed Blackhole Guard in a process and answer the product question.
///
///   Given a structurally legal path, is traffic being blackholed now, where is the
///   failure localized, what authority must be fenced, and when may it be restored?
///
/// Run: bhg_example_fence <state-root>

#include <cstdio>
#include <string>
#include <vector>

#include "blackhole/blackhole.hpp"

using namespace bhg;

namespace {

Scope path_scope_of(std::uint64_t path) {
  Scope s;
  s.kind = ScopeKind::Path;
  s.path = PathId{path};
  return s;
}

GenerationVector gens_of(std::uint64_t epoch) {
  GenerationVector g;
  g.path = PathGeneration{1};
  g.topology = TopologyGeneration{1};
  g.link_state = LinkStateGeneration{1};
  g.epoch = CoordinatorEpoch{epoch};
  return g;
}

DeliveryEvidence evidence_of(std::uint64_t source, const Scope& scope, const GenerationVector& g,
                             std::uint64_t seq, WallNs at, bool failure) {
  DeliveryEvidence e;
  e.source.source = EvidenceSourceId{source};
  e.source.boot = BootId{source};
  e.source.incarnation = IncarnationId{source};
  e.source.epoch = SourceEpoch{1};
  e.scope = scope;
  e.gens = g;
  e.seq = EvidenceSequence{seq};
  e.attempt = AttemptId{seq};
  e.kind = failure ? EvidenceKind::DeliveryFailure : EvidenceKind::DeliverySuccess;
  e.provenance = Provenance::Synthetic;  // this example fabricates its inputs
  e.attempts = 64;
  e.failures = failure ? 64u : 0u;
  e.loss_ppm = failure ? kLossTotalPpm : 0u;
  e.observed_at = at;
  e.fresh.open = at;
  e.fresh.close = WallNs{at.ns + 300 * kNsPerSecond};
  e.id = derive_evidence_id(e);
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "./bhg-example-state";
  EngineConfig config;
  config.store.root = root;
  config.label = Provenance::Synthetic;
  SystemClock clock;
  SystemNonceSource nonces;
  Engine engine(config, clock, nonces);

  const Outcome opened = engine.open();
  if (!is_affirmative(opened)) {
    std::fprintf(stderr, "engine open failed: %s\n", std::string(to_string(opened)).c_str());
    return 1;
  }

  const Scope scope = path_scope_of(1);
  const GenerationVector gens = gens_of(1);
  const WallNs at = clock.wall_now();

  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t source = 1; source <= 2; ++source) {
    (void)engine.submit(evidence_of(source, scope, gens, 1, at, true), admission, reason);
  }

  Decision decision;
  (void)engine.evaluate(scope, gens, decision);
  std::printf("classification=%s authority=%s corroborated=%d\n", to_string(decision.classification),
              to_string(decision.authority.stage), decision.corroborated ? 1 : 0);

  if (decision.has_intent) {
    std::printf("fence intent %s emitted for epoch %llu; the route owner decides whether to apply "
                "it\n",
                to_text(decision.intent.id).c_str(),
                static_cast<unsigned long long>(decision.intent.gens.epoch.value()));
    (void)engine.acknowledge_fence(decision.intent.id, EvidenceSourceId{7}, reason);
    (void)engine.report_effect(decision.intent.id, EvidenceSourceId{7}, true, reason);
  }

  const std::vector<HopProbe> probes = {{0, 3, true}, {3, 8, false}, {5, 8, false}};
  Decision localized;
  (void)engine.localize(scope, gens, 8, probes, localized);
  if (localized.has_localization) {
    std::printf("localization=%s size=%u proven=%d elements=[", to_string(localized.localization.status),
                localized.localization.optimal_size, localized.localization.optimality_proven ? 1 : 0);
    for (std::size_t i = 0; i < localized.localization.elements.size(); ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", localized.localization.elements[i]);
    }
    std::printf("]\n");
  }

  const EngineStats stats = engine.stats();
  std::printf("accounting_closed=%d evaluations=%llu committed=%llu\n",
              stats.accounting_closed() ? 1 : 0,
              static_cast<unsigned long long>(stats.evaluations),
              static_cast<unsigned long long>(stats.decisions_committed));
  (void)engine.close();
  return stats.accounting_closed() ? 0 : 2;
}
