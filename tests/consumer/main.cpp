/// Downstream consumer: proves the installed package is usable without the source
/// tree. It performs a complete detect -> localize -> fence -> restore cycle through
/// the installed public headers and the imported CMake target.

#include <cstdio>
#include <string>
#include <vector>

#include <blackhole/blackhole.hpp>

namespace {

bhg::Scope path_scope_of(std::uint64_t path) {
  bhg::Scope s;
  s.kind = bhg::ScopeKind::Path;
  s.path = bhg::PathId{path};
  return s;
}

bhg::GenerationVector gens_of(std::uint64_t epoch) {
  bhg::GenerationVector g;
  g.path = bhg::PathGeneration{1};
  g.topology = bhg::TopologyGeneration{1};
  g.link_state = bhg::LinkStateGeneration{1};
  g.epoch = bhg::CoordinatorEpoch{epoch};
  return g;
}

bhg::DeliveryEvidence evidence_of(std::uint64_t source, const bhg::Scope& scope,
                                  const bhg::GenerationVector& g, std::uint64_t seq,
                                  bhg::WallNs at, bool failure) {
  bhg::DeliveryEvidence e;
  e.source.source = bhg::EvidenceSourceId{source};
  e.source.boot = bhg::BootId{source};
  e.source.incarnation = bhg::IncarnationId{source};
  e.source.epoch = bhg::SourceEpoch{1};
  e.scope = scope;
  e.gens = g;
  e.seq = bhg::EvidenceSequence{seq};
  e.attempt = bhg::AttemptId{seq};
  e.kind = failure ? bhg::EvidenceKind::DeliveryFailure : bhg::EvidenceKind::DeliverySuccess;
  e.provenance = bhg::Provenance::Synthetic;
  e.attempts = 64;
  e.failures = failure ? 64u : 0u;
  e.loss_ppm = failure ? bhg::kLossTotalPpm : 0u;
  e.observed_at = at;
  e.fresh.open = at;
  e.fresh.close = bhg::WallNs{at.ns + 300 * bhg::kNsPerSecond};
  e.id = bhg::derive_evidence_id(e);
  return e;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "./consumer-state";
  bhg::EngineConfig config;
  config.store.root = root;
  config.label = bhg::Provenance::Synthetic;
  // A manual clock keeps the consumer deterministic; production embeds use
  // bhg::SystemClock instead.
  bhg::ManualClock clock(1700000000000000000LL);
  bhg::SystemNonceSource nonces;
  bhg::Engine engine(config, clock, nonces);
  if (!bhg::is_affirmative(engine.open())) {
    std::fprintf(stderr, "consumer: engine open failed\n");
    return 1;
  }

  const bhg::Scope scope = path_scope_of(1);
  const bhg::GenerationVector gens = gens_of(1);
  const bhg::WallNs at = clock.wall_now();

  bhg::EvidenceAdmission admission = bhg::EvidenceAdmission::StructurallyInvalid;
  bhg::ReasonCode reason = bhg::ReasonCode::None;
  for (std::uint64_t source = 1; source <= 2; ++source) {
    (void)engine.submit(evidence_of(source, scope, gens, 1, at, true), admission, reason);
  }

  bhg::Decision decision;
  (void)engine.evaluate(scope, gens, decision);
  if (decision.classification != bhg::Classification::Blackhole || !decision.has_intent ||
      decision.authority.stage != bhg::AuthorityStage::Intent) {
    std::fprintf(stderr, "consumer: expected a corroborated blackhole with a fence intent\n");
    (void)engine.close();
    return 2;
  }

  const std::vector<bhg::HopProbe> probes = {{0, 2, true}, {2, 6, false}, {4, 6, false}};
  bhg::Decision localized;
  (void)engine.localize(scope, gens, 6, probes, localized);
  if (!localized.has_localization || localized.localization.status != bhg::LocalizationStatus::Ambiguous) {
    std::fprintf(stderr, "consumer: expected an ambiguous localization\n");
    (void)engine.close();
    return 3;
  }

  // Fresh positive evidence from two source instances authorizes restoration.
  clock.advance(bhg::seconds(400));
  const bhg::WallNs later = clock.wall_now();
  for (std::uint64_t source = 3; source <= 4; ++source) {
    (void)engine.submit(evidence_of(source, scope, gens, 1, later, false), admission, reason);
  }
  bhg::Decision restored;
  (void)engine.restore(scope, gens, restored);
  if (restored.classification != bhg::Classification::Healthy ||
      restored.authority.stage != bhg::AuthorityStage::Authorization) {
    std::fprintf(stderr, "consumer: expected restoration to be authorized\n");
    (void)engine.close();
    return 4;
  }

  const bhg::EngineStats stats = engine.stats();
  if (!stats.accounting_closed()) {
    std::fprintf(stderr, "consumer: accounting did not close\n");
    (void)engine.close();
    return 5;
  }

  std::printf("consumer ok: version %s decisions=%llu fences=%llu\n", bhg::kVersionString,
              static_cast<unsigned long long>(stats.decisions_committed),
              static_cast<unsigned long long>(stats.fence_intents_issued));
  (void)engine.close();
  return 0;
}
