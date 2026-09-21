#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "blackhole/authority/authority.hpp"
#include "blackhole/core/nonce.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/diagnosis/classifier.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/evidence/ledger.hpp"
#include "blackhole/localize/localizer.hpp"
#include "blackhole/persist/store.hpp"

namespace bhg {

struct EngineConfig {
  StoreConfig store{};
  FencePolicy policy{};
  Provenance label{Provenance::Synthetic};
  std::string role{"coordinator"};
};

struct EngineStats {
  std::uint64_t evidence_submitted{0};
  std::uint64_t evidence_admitted{0};
  std::uint64_t evidence_rejected{0};

  /// evaluate()/restore() calls that passed input validation. Each one produces
  /// exactly one decision record attempt.
  std::uint64_t evaluations{0};
  /// Additional decision records produced by localize() when the localization
  /// enriches an already-produced diagnosis.
  std::uint64_t localization_enrichments{0};
  std::uint64_t decisions_produced{0};
  std::uint64_t decisions_committed{0};
  std::uint64_t decision_commit_failures{0};
  /// Failures of *any* durable commit (decision, fence state, interruption).
  std::uint64_t durable_commit_failures{0};

  std::uint64_t fence_intents_issued{0};
  std::uint64_t localizations{0};
  std::uint64_t restorations_authorized{0};
  std::uint64_t restorations_refused{0};
  std::uint64_t refusals_fail_closed{0};
  std::uint64_t fence_revocations{0};

  /// Exact accounting closure:
  ///   evidence_submitted   == evidence_admitted + evidence_rejected
  ///   decisions_produced   == evaluations + localization_enrichments
  ///   decisions_produced   == decisions_committed + decision_commit_failures
  [[nodiscard]] bool accounting_closed() const noexcept {
    return evidence_submitted == evidence_admitted + evidence_rejected &&
           decisions_produced == evaluations + localization_enrichments &&
           decisions_produced == decisions_committed + decision_commit_failures;
  }
};

/// The runtime coordinator. Owns the live evidence ledger, the fence registry, the
/// durable store and the diagnosis/authority pipeline.
///
/// Concurrency: one internal mutex (mu_) protects all mutable engine state. No
/// callback, log sink or foreign code is invoked while it is held; the class
/// performs no lock re-entry and acquires no other lock.
class Engine {
 public:
  Engine(EngineConfig config, Clock& clock, NonceSource& nonces);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  Outcome open();
  Outcome close();
  [[nodiscard]] bool is_open() const;

  Outcome submit(const DeliveryEvidence& e, EvidenceAdmission& admission, ReasonCode& reason);
  Outcome evaluate(const Scope& scope, const GenerationVector& gens, Decision& out);
  Outcome localize(const Scope& scope, const GenerationVector& gens, std::uint32_t hop_count,
                   std::span<const HopProbe> probes, Decision& out);
  Outcome acknowledge_fence(FenceId id, EvidenceSourceId downstream, ReasonCode& reason);
  Outcome report_effect(FenceId id, EvidenceSourceId downstream, bool verified,
                        ReasonCode& reason);
  Outcome restore(const Scope& scope, const GenerationVector& gens, Decision& out);
  Outcome checkpoint(RecordSequence& snapshot_seq);

  [[nodiscard]] EngineStats stats() const;
  Outcome fence_state(FenceId id, FenceRecord& out) const;
  Outcome lineage(std::uint32_t limit, std::vector<LineageEntry>& entries,
                  std::uint64_t& seen, std::uint64_t& dropped) const;
  [[nodiscard]] OpenReport open_report() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] PolicyVersion policy_version() const;
  [[nodiscard]] std::uint64_t policy_fingerprint() const;
  [[nodiscard]] std::string snapshot_path() const;
  [[nodiscard]] std::string journal_path() const;
  [[nodiscard]] Provenance label() const noexcept { return config_.label; }
  [[nodiscard]] WallNs now_wall() const;
  [[nodiscard]] RecordSequence last_record() const;
  [[nodiscard]] const FencePolicy& policy() const noexcept { return policy_; }

 private:
  Decision make_refusal(const Scope& scope, const GenerationVector& gens, Outcome outcome,
                        ReasonCode reason) const;
  Outcome commit_decision(const Decision& d);
  WallNs now() const;
  void revalidate_fences_locked(const GenerationVector& current);

  EngineConfig config_;
  FencePolicy policy_;
  Clock& clock_;
  NonceSource& nonces_;
  mutable std::mutex mu_;
  EvidenceLedger ledger_;
  FenceRegistry fences_;
  Store store_;
  EngineStats stats_{};
  bool opened_{false};
};

}  // namespace bhg
