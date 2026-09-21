#pragma once

/// Shared fixtures for the Blackhole Guard suites.
///
/// Every fixture here is SYNTHETIC by construction: it fabricates typed inputs that
/// an adjacent owner (path/topology/link-state/congestion owner or a delivery
/// observer) would supply. No fixture claims physical switch, NIC, RDMA or
/// multi-node behaviour.

#include <memory>
#include <string>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/testkit.hpp"

namespace bhg::test {

/// Deterministic seed derived from a tag, so fixtures are reproducible while
/// distinct fixtures remain distinct.
inline std::uint64_t hash_seed(const char* tag) {
  std::uint64_t h = kFnv1a64Offset;
  for (const char* p = tag; *p != '\0'; ++p) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*p));
    h *= kFnv1a64Prime;
  }
  return h | 1ull;
}

inline Scope path_scope(PathId p) {
  Scope s;
  s.kind = ScopeKind::Path;
  s.path = p;
  return s;
}

inline Scope node_scope(PathId p, NodeId n) {
  Scope s;
  s.kind = ScopeKind::Node;
  s.path = p;
  s.node = n;
  return s;
}

inline Scope link_scope(PathId p, LinkId l) {
  Scope s;
  s.kind = ScopeKind::Link;
  s.path = p;
  s.link = l;
  return s;
}

inline Scope segment_scope(PathId p, std::uint32_t begin_hop, std::uint32_t end_hop) {
  Scope s;
  s.kind = ScopeKind::Segment;
  s.path = p;
  s.begin_hop = begin_hop;
  s.end_hop = end_hop;
  return s;
}

inline GenerationVector make_gens(std::uint64_t path_gen, std::uint64_t topo_gen,
                                  std::uint64_t link_gen, std::uint64_t epoch) {
  GenerationVector g;
  g.path = PathGeneration{path_gen};
  g.topology = TopologyGeneration{topo_gen};
  g.link_state = LinkStateGeneration{link_gen};
  g.epoch = CoordinatorEpoch{epoch};
  return g;
}

struct EvidenceSpec {
  EvidenceSourceId source{EvidenceSourceId{1}};
  BootId boot{BootId{1}};
  IncarnationId incarnation{IncarnationId{1}};
  SourceEpoch epoch{SourceEpoch{1}};
  Scope scope{};
  GenerationVector gens{};
  EvidenceSequence seq{EvidenceSequence{1}};
  AttemptId attempt{AttemptId{1}};
  EvidenceKind kind{EvidenceKind::DeliveryFailure};
  EvidenceQuality quality{EvidenceQuality::Exact};
  Provenance label{Provenance::Synthetic};
  std::uint64_t attempts{32};
  std::uint64_t failures{32};
  std::uint32_t loss_ppm{1000000};
  std::uint32_t rtt_us{100};
  WallNs observed_at{WallNs{1000000000}};
  DurationNs window{seconds(60)};
  EvidenceSourceId external_owner{};
};

inline void fresh_set(DeliveryEvidence& e, DurationNs window) {
  const auto close = checked_add<std::int64_t>(e.observed_at.ns, window.ns);
  e.fresh.open = e.observed_at;
  e.fresh.close = WallNs{close.has_value() ? *close : e.observed_at.ns};
}

inline DeliveryEvidence make_evidence(const EvidenceSpec& spec) {
  DeliveryEvidence e;
  e.source.source = spec.source;
  e.source.boot = spec.boot;
  e.source.incarnation = spec.incarnation;
  e.source.epoch = spec.epoch;
  e.scope = spec.scope;
  e.gens = spec.gens;
  e.seq = spec.seq;
  e.attempt = spec.attempt;
  e.kind = spec.kind;
  e.quality = spec.quality;
  e.provenance = spec.label;
  e.attempts = spec.attempts;
  e.failures = spec.failures;
  e.loss_ppm = spec.loss_ppm;
  e.rtt_us = spec.rtt_us;
  e.observed_at = spec.observed_at;
  fresh_set(e, spec.window);
  e.external_owner = spec.external_owner;
  e.id = derive_evidence_id(e);
  return e;
}

/// A complete-delivery-failure observation from one source instance.
inline DeliveryEvidence failure_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                         const Scope& scope, const GenerationVector& gens,
                                         EvidenceSequence seq, WallNs at,
                                         std::uint64_t attempts = 32) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::DeliveryFailure;
  s.attempts = attempts;
  s.failures = attempts;
  s.loss_ppm = kLossTotalPpm;
  s.observed_at = at;
  return make_evidence(s);
}

inline DeliveryEvidence success_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                         const Scope& scope, const GenerationVector& gens,
                                         EvidenceSequence seq, WallNs at,
                                         std::uint64_t attempts = 32) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::DeliverySuccess;
  s.attempts = attempts;
  s.failures = 0;
  s.loss_ppm = 0;
  s.observed_at = at;
  return make_evidence(s);
}

inline DeliveryEvidence loss_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                      const Scope& scope, const GenerationVector& gens,
                                      EvidenceSequence seq, WallNs at, std::uint32_t loss_ppm,
                                      std::uint64_t attempts = 100) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::LossMeasurement;
  s.attempts = attempts;
  s.failures = static_cast<std::uint64_t>(
      (static_cast<unsigned long long>(attempts) * loss_ppm) / kLossTotalPpm);
  s.loss_ppm = loss_ppm;
  s.observed_at = at;
  return make_evidence(s);
}

inline DeliveryEvidence congestion_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                            const Scope& scope, const GenerationVector& gens,
                                            EvidenceSequence seq, WallNs at,
                                            EvidenceSourceId owner) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::CongestionSignal;
  s.attempts = 1;
  s.failures = 0;
  s.loss_ppm = 0;
  s.observed_at = at;
  s.external_owner = owner;
  return make_evidence(s);
}

inline DeliveryEvidence partition_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                           const Scope& scope, const GenerationVector& gens,
                                           EvidenceSequence seq, WallNs at,
                                           EvidenceSourceId owner) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::PartitionSignal;
  s.attempts = 1;
  s.failures = 0;
  s.loss_ppm = 0;
  s.observed_at = at;
  s.external_owner = owner;
  return make_evidence(s);
}

inline DeliveryEvidence withdrawal_evidence(EvidenceSourceId source, IncarnationId incarnation,
                                            const Scope& scope, const GenerationVector& gens,
                                            EvidenceSequence seq, WallNs at) {
  EvidenceSpec s;
  s.source = source;
  s.boot = BootId{source.value() * 31u + 1u};
  s.incarnation = incarnation;
  s.scope = scope;
  s.gens = gens;
  s.seq = seq;
  s.attempt = AttemptId{seq.value() * 2u + 1u};
  s.kind = EvidenceKind::StructuralWithdrawal;
  s.attempts = 0;
  s.failures = 0;
  s.loss_ppm = 0;
  s.observed_at = at;
  return make_evidence(s);
}

/// Complete in-process runtime fixture: owns clock, nonce source, scratch directory
/// and engine with a stable address for each.
class EngineHarness {
 public:
  explicit EngineHarness(const char* tag, FencePolicy policy = default_policy(),
                         Provenance label = Provenance::Synthetic)
      : dir_(tag), policy_(policy), nonces_(hash_seed(tag)) {
    config_.store.root = dir_.path();
    config_.store.max_journal_bytes = 8u * 1024u * 1024u;
    config_.store.max_lineage_records = 512;
    config_.store.max_snapshot_payload = 2u * 1024u * 1024u;
    config_.policy = policy_;
    config_.label = label;
    engine_ = std::make_unique<Engine>(config_, clock_, nonces_);
  }

  Outcome open() { return engine_->open(); }
  Engine& engine() { return *engine_; }
  ManualClock& clock() { return clock_; }
  SeededNonceSource& nonces() { return nonces_; }
  StoreConfig& store_config() { return config_.store; }
  const std::string& root() const { return dir_.path(); }
  const FencePolicy& policy() const { return policy_; }

  /// Closes the engine and constructs a fresh one over the same durable root. The
  /// clock and nonce source persist so a restart can be proven without relying on
  /// wall-clock or entropy behaviour.
  void reset_engine() {
    (void)engine_->close();
    config_.store.root = dir_.path();
    config_.policy = policy_;
    engine_ = std::make_unique<Engine>(config_, clock_, nonces_);
  }

  /// Constructs an engine over an arbitrary durable root.
  std::unique_ptr<Engine> make_engine(const std::string& durable_root,
                                      const FencePolicy& policy, Provenance label) {
    EngineConfig config;
    config.store = config_.store;
    config.store.root = durable_root;
    config.policy = policy;
    config.label = label;
    return std::make_unique<Engine>(config, clock_, nonces_);
  }

 private:
  TempDir dir_;
  FencePolicy policy_;
  EngineConfig config_{};
  ManualClock clock_;
  // Distinct seed per harness instance so two independent engines never fabricate the
  // same boot/incarnation identity.
  SeededNonceSource nonces_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace bhg::test
