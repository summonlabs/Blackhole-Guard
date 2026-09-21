/// Blackhole Guard operator CLI.
///
/// Modes:
///   bhg-cli version
///   bhg-cli demo     --root DIR [--label synthetic|real]
///   bhg-cli report   --root DIR
///   bhg-cli serve    --root DIR [--port N]        (runs until stdin closes)
///
/// Everything the CLI does goes through the same public API a downstream consumer
/// uses; there is no privileged path.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "blackhole/blackhole.hpp"

using namespace bhg;

namespace {

struct Args {
  std::map<std::string, std::string> values;
  bool has(const std::string& k) const { return values.find(k) != values.end(); }
  std::string get(const std::string& k, const std::string& fallback = std::string()) const {
    const auto it = values.find(k);
    return it == values.end() ? fallback : it->second;
  }
  std::uint64_t number(const std::string& k, std::uint64_t fallback) const {
    const std::string v = get(k);
    return v.empty() ? fallback : std::strtoull(v.c_str(), nullptr, 10);
  }
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 2; i + 1 < argc; i += 2) a.values[argv[i]] = argv[i + 1];
  return a;
}

Provenance label_of(const Args& a) {
  return a.get("--label", "synthetic") == "real" ? Provenance::Real : Provenance::Synthetic;
}

Scope path_scope_of(std::uint64_t path) {
  Scope s;
  s.kind = ScopeKind::Path;
  s.path = PathId{path};
  return s;
}

GenerationVector gens_of(std::uint64_t path_gen, std::uint64_t topo, std::uint64_t link,
                         std::uint64_t epoch) {
  GenerationVector g;
  g.path = PathGeneration{path_gen};
  g.topology = TopologyGeneration{topo};
  g.link_state = LinkStateGeneration{link};
  g.epoch = CoordinatorEpoch{epoch};
  return g;
}

DeliveryEvidence failure_from(std::uint64_t source, const Scope& scope, const GenerationVector& g,
                              std::uint64_t seq, WallNs at, std::uint64_t attempts) {
  DeliveryEvidence e;
  e.source.source = EvidenceSourceId{source};
  e.source.boot = BootId{source * 31u + 1u};
  e.source.incarnation = IncarnationId{source};
  e.source.epoch = SourceEpoch{1};
  e.scope = scope;
  e.gens = g;
  e.seq = EvidenceSequence{seq};
  e.attempt = AttemptId{seq};
  e.kind = EvidenceKind::DeliveryFailure;
  e.attempts = attempts;
  e.failures = attempts;
  e.loss_ppm = kLossTotalPpm;
  e.observed_at = at;
  e.fresh.open = at;
  e.fresh.close = WallNs{at.ns + 600 * kNsPerSecond};
  e.id = derive_evidence_id(e);
  return e;
}

void print_decision(const Decision& d, const char* prefix) {
  std::printf("%s kind=%s outcome=%s classification=%s authority=%s corroborated=%d intent=%d\n",
              prefix, to_string(d.kind) , std::string(to_string(d.outcome)).c_str(),
              to_string(d.classification), to_string(d.authority.stage),
              d.corroborated ? 1 : 0, d.has_intent ? 1 : 0);
  std::printf("%s decision=%s reason=%s evidence(fresh=%u complete_failures=%u success=%u)\n",
              prefix, to_text(d.id).c_str(), to_string(d.primary_reason),
              d.census.fresh_current, d.census.complete_failures, d.census.clean_successes);
  if (d.has_localization) {
    std::printf("%s localization=%s size=%u proven=%d ambiguous_truncated=%d elements=[",
                prefix, to_string(d.localization.status), d.localization.optimal_size,
                d.localization.optimality_proven ? 1 : 0,
                d.localization.solutions_truncated ? 1 : 0);
    for (std::size_t i = 0; i < d.localization.elements.size(); ++i) {
      std::printf("%s%u", i == 0 ? "" : ",", d.localization.elements[i]);
    }
    std::printf("]\n");
  }
  if (d.has_intent) {
    std::printf("%s fence=%s scope_kind=%s expires_at=%lld\n", prefix,
                to_text(d.intent.id).c_str(), to_string(d.intent.scope.kind),
                static_cast<long long>(d.intent.expires_at.ns));
  }
}

int cmd_demo(const Args& a) {
  EngineConfig config;
  config.store.root = a.get("--root", "./bhg-demo-state");
  config.label = label_of(a);
  SystemClock clock;
  SystemNonceSource nonces;
  Engine engine(config, clock, nonces);
  Outcome o = engine.open();
  if (!is_affirmative(o)) {
    std::fprintf(stderr, "open failed: %s\n", std::string(to_string(o)).c_str());
    return 3;
  }
  const OpenReport report = engine.open_report();
  std::printf("boot=%s incarnation=%s epoch=%llu policy_fingerprint=%llu\n",
              to_text(report.boot).c_str(), to_text(report.incarnation).c_str(),
              static_cast<unsigned long long>(report.epoch.value()),
              static_cast<unsigned long long>(engine.policy_fingerprint()));

  const Scope scope = path_scope_of(1);
  const GenerationVector gens = gens_of(1, 1, 1, 1);
  const WallNs at = clock.wall_now();

  Decision d;
  (void)engine.evaluate(scope, gens, d);
  print_decision(d, "[no-evidence]");

  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  (void)engine.submit(failure_from(1, scope, gens, 1, at, 32), admission, reason);
  (void)engine.evaluate(scope, gens, d);
  print_decision(d, "[one-source]");

  (void)engine.submit(failure_from(2, scope, gens, 1, at, 32), admission, reason);
  (void)engine.evaluate(scope, gens, d);
  print_decision(d, "[corroborated]");

  if (d.has_intent) {
    ReasonCode ack_reason = ReasonCode::None;
    (void)engine.acknowledge_fence(d.intent.id, EvidenceSourceId{99}, ack_reason);
    (void)engine.report_effect(d.intent.id, EvidenceSourceId{99}, true, ack_reason);
    std::printf("[downstream] ack+effect reported by owner 99 (%s)\n", to_string(ack_reason));
  }

  const std::vector<HopProbe> probes = {
      {0, 2, true}, {2, 6, false}, {4, 6, false},
  };
  (void)engine.localize(scope, gens, 6, probes, d);
  print_decision(d, "[localized]");

  Decision restored;
  (void)engine.restore(scope, gens, restored);
  print_decision(restored, "[restore-refused]");

  RecordSequence snapshot{};
  o = engine.checkpoint(snapshot);
  std::printf("[checkpoint] outcome=%s snapshot_seq=%llu\n", std::string(to_string(o)).c_str(),
              static_cast<unsigned long long>(snapshot.value()));
  const EngineStats stats = engine.stats();
  std::printf("[stats] submitted=%llu admitted=%llu rejected=%llu evaluations=%llu committed=%llu "
              "fenced=%llu closed=%d\n",
              static_cast<unsigned long long>(stats.evidence_submitted),
              static_cast<unsigned long long>(stats.evidence_admitted),
              static_cast<unsigned long long>(stats.evidence_rejected),
              static_cast<unsigned long long>(stats.evaluations),
              static_cast<unsigned long long>(stats.decisions_committed),
              static_cast<unsigned long long>(stats.fence_intents_issued),
              stats.accounting_closed() ? 1 : 0);
  (void)engine.close();
  return stats.accounting_closed() ? 0 : 4;
}

int cmd_report(const Args& a) {
  EngineConfig config;
  config.store.root = a.get("--root", "./bhg-demo-state");
  config.label = Provenance::Synthetic;
  SystemClock clock;
  SystemNonceSource nonces;
  Engine engine(config, clock, nonces);
  const Outcome o = engine.open();
  if (!is_affirmative(o)) {
    std::fprintf(stderr, "open failed: %s\n", std::string(to_string(o)).c_str());
    return 3;
  }
  const OpenReport r = engine.open_report();
  std::printf("outcome=%s detail=%s\n", std::string(to_string(r.outcome)).c_str(),
              r.detail.c_str());
  std::printf("boot=%s incarnation=%s epoch=%llu boots=%llu opens=%llu\n",
              to_text(r.boot).c_str(), to_text(r.incarnation).c_str(),
              static_cast<unsigned long long>(r.epoch.value()),
              static_cast<unsigned long long>(r.boot_count),
              static_cast<unsigned long long>(r.open_count));
  std::printf("restart_detected=%d snapshot_present=%d torn_tail=%d reclaimed_bytes=%llu "
              "policy_restored=%d fenced_prior_fences=%llu\n",
              r.restart_detected ? 1 : 0, r.snapshot_present ? 1 : 0, r.torn_tail_recovered ? 1 : 0,
              static_cast<unsigned long long>(r.reclaimed_tail_bytes),
              r.policy_restored ? 1 : 0,
              static_cast<unsigned long long>(r.fenced_prior_fences));
  std::vector<LineageEntry> entries;
  std::uint64_t seen = 0;
  std::uint64_t dropped = 0;
  (void)engine.lineage(64, entries, seen, dropped);
  std::printf("lineage seen=%llu dropped=%llu shown=%zu closed=%d\n",
              static_cast<unsigned long long>(seen),
              static_cast<unsigned long long>(dropped), entries.size(),
              seen == static_cast<std::uint64_t>(entries.size()) + dropped ? 1 : 0);
  for (const LineageEntry& e : entries) {
    std::printf("  seq=%llu type=%s outcome=%s classification=%s reason=%s prior=%d decision=%s\n",
                static_cast<unsigned long long>(e.seq.value()), to_string(e.type),
                std::string(to_string(e.outcome)).c_str(), to_string(e.classification),
                to_string(e.reason), e.from_prior_incarnation ? 1 : 0,
                to_text(e.decision).c_str());
  }
  (void)engine.close();
  return 0;
}

int cmd_serve(const Args& a) {
  EngineConfig config;
  config.store.root = a.get("--root", "./bhg-state");
  config.label = label_of(a);
  SystemClock clock;
  SystemNonceSource nonces;
  Engine engine(config, clock, nonces);
  const Outcome o = engine.open();
  if (!is_affirmative(o)) {
    std::fprintf(stderr, "open failed: %s\n", std::string(to_string(o)).c_str());
    return 3;
  }
  ServerConfig sc;
  sc.port = static_cast<std::uint16_t>(a.number("--port", 0));
  Server server(engine, sc);
  std::uint16_t bound = 0;
  if (!is_affirmative(server.start(bound))) {
    std::fprintf(stderr, "listen failed\n");
    return 4;
  }
  std::printf("listening on 127.0.0.1:%u (close stdin to stop)\n", static_cast<unsigned>(bound));
  std::fflush(stdout);
  char buffer[64];
  while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
  }
  (void)server.stop();
  const ServerStats stats = server.stats();
  std::printf("served=%llu rejected=%llu frames=%llu frames_rejected=%llu\n",
              static_cast<unsigned long long>(stats.requests_served),
              static_cast<unsigned long long>(stats.requests_rejected),
              static_cast<unsigned long long>(stats.frames_decoded),
              static_cast<unsigned long long>(stats.frames_rejected));
  (void)engine.close();
  return 0;
}

int usage() {
  std::fprintf(stderr,
               "%s %s\n"
               "usage:\n"
               "  bhg-cli version\n"
               "  bhg-cli demo   --root DIR [--label synthetic|real]\n"
               "  bhg-cli report --root DIR\n"
               "  bhg-cli serve  --root DIR [--port N]\n",
               kProductName, kVersionString);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string mode = argv[1];
  const Args args = parse(argc, argv);
  if (mode == "version") {
    std::printf("%s %s (format %u, protocol %u)\n", kProductName, kVersionString,
                static_cast<unsigned>(kFormatVersion), static_cast<unsigned>(kProtocolVersion));
    return 0;
  }
  if (mode == "demo") return cmd_demo(args);
  if (mode == "report") return cmd_report(args);
  if (mode == "serve") return cmd_serve(args);
  return usage();
}
