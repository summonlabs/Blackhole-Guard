#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "blackhole/core/bounded.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/classification.hpp"
#include "blackhole/diagnosis/classifier.hpp"
#include "blackhole/domain/generation.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/localize/localizer.hpp"

namespace bhg {

/// ============================================================================
/// Authority model
/// ============================================================================
///
/// Authority is a strictly ordered ladder. Each rung is a different kind of
/// statement and they are never conflated:
///
///   Observation     -- a raw measurement. Confers nothing.
///   Eligibility     -- policy says this subject *could* be fenced if corroborated.
///   Authorization   -- corroboration requirements are satisfied.
///   Intent          -- this runtime has emitted a bounded fencing *intent*.
///   Acknowledgement -- the downstream route/convergence owner received the intent.
///   VerifiedEffect  -- the downstream owner reported the intent applied.
///
/// Even VerifiedEffect is NOT proof of route removal and is never described as such.
/// Every rung carries the generation vector and the coordinator boot/incarnation it
/// was minted under, and is revocable when any of those dependencies move.

enum class AuthorityStage : std::uint8_t {
  None = 0,
  Observation = 1,
  Eligibility = 2,
  Authorization = 3,
  Intent = 4,
  Acknowledgement = 5,
  VerifiedEffect = 6,
};

constexpr bool is_valid_authority_stage(std::uint8_t raw) noexcept { return raw <= 6u; }

constexpr const char* to_string(AuthorityStage s) noexcept {
  switch (s) {
    case AuthorityStage::None: return "NONE";
    case AuthorityStage::Observation: return "OBSERVATION";
    case AuthorityStage::Eligibility: return "ELIGIBILITY";
    case AuthorityStage::Authorization: return "AUTHORIZATION";
    case AuthorityStage::Intent: return "INTENT";
    case AuthorityStage::Acknowledgement: return "ACKNOWLEDGEMENT";
    case AuthorityStage::VerifiedEffect: return "VERIFIED_EFFECT";
  }
  return "NONE";
}

/// Positive authority is exactly: stage >= Authorization AND not revoked AND not
/// expired AND bound to the current generation vector and coordinator incarnation.
/// Nothing weaker is authority.
struct AuthorityVector {
  AuthorityStage stage{AuthorityStage::None};
  DecisionId decision{};
  FenceId fence{};
  GenerationVector gens{};
  BootId boot{};
  IncarnationId incarnation{};
  PolicyVersion policy{};
  WallNs issued_at{};
  WallNs expires_at{};
  bool revoked{false};
  ReasonCode revoke_reason{ReasonCode::None};
  Provenance provenance{Provenance::Synthetic};

  [[nodiscard]] bool is_positive() const noexcept {
    return static_cast<std::uint8_t>(stage) >= static_cast<std::uint8_t>(AuthorityStage::Authorization) &&
           !revoked;
  }
  /// True only when this vector is still legally current: same generation vector,
  /// same coordinator incarnation, not revoked and not expired.
  [[nodiscard]] bool is_binding_at(const GenerationVector& current, BootId current_boot,
                                   IncarnationId current_incarnation, WallNs now) const noexcept {
    if (!is_positive()) return false;
    if (gens != current) return false;
    if (boot != current_boot) return false;
    if (incarnation != current_incarnation) return false;
    return expires_at.ns == 0 || now.ns < expires_at.ns;
  }
};

struct FenceIntent {
  FenceId id{};
  DecisionId decision{};
  Scope scope{};
  GenerationVector gens{};
  BootId boot{};
  IncarnationId incarnation{};
  PolicyVersion policy{};
  Provenance provenance{Provenance::Synthetic};
  ReasonCode reason{ReasonCode::None};
  WallNs issued_at{};
  WallNs expires_at{};

  [[nodiscard]] bool is_valid() const noexcept {
    return !id.is_nil() && !decision.is_nil() && scope.is_valid() && gens.is_complete() &&
           !boot.is_nil() && !incarnation.is_nil() && expires_at.ns > issued_at.ns;
  }
};

/// Lifecycle of a fencing intent. Note that Applied is a downstream-reported fact,
/// not something this runtime can observe.
enum class FenceLifecycle : std::uint8_t {
  Intent = 0,
  Acknowledged = 1,
  EffectReported = 2,
  Revoked = 3,
  Expired = 4,
  FencedByRestart = 5,
};

constexpr bool is_valid_fence_lifecycle(std::uint8_t raw) noexcept { return raw <= 5u; }

constexpr const char* to_string(FenceLifecycle s) noexcept {
  switch (s) {
    case FenceLifecycle::Intent: return "INTENT";
    case FenceLifecycle::Acknowledged: return "ACKNOWLEDGED";
    case FenceLifecycle::EffectReported: return "EFFECT_REPORTED";
    case FenceLifecycle::Revoked: return "REVOKED";
    case FenceLifecycle::Expired: return "EXPIRED";
    case FenceLifecycle::FencedByRestart: return "FENCED_BY_RESTART";
  }
  return "REVOKED";
}

struct FenceRecord {
  FenceIntent intent{};
  FenceLifecycle lifecycle{FenceLifecycle::Intent};
  bool effect_verified{false};
  EvidenceSourceId ack_owner{};
  WallNs acked_at{};
  EvidenceSourceId effect_owner{};
  WallNs effected_at{};
  ReasonCode terminal_reason{ReasonCode::None};

  [[nodiscard]] bool is_open() const noexcept {
    return lifecycle == FenceLifecycle::Intent || lifecycle == FenceLifecycle::Acknowledged ||
           lifecycle == FenceLifecycle::EffectReported;
  }
};

struct FenceStats {
  std::uint64_t issue_attempts{0};
  std::uint64_t issued{0};
  std::uint64_t open{0};
  std::uint64_t acknowledged{0};
  std::uint64_t effects_reported{0};
  std::uint64_t effects_verified{0};
  std::uint64_t revoked{0};
  std::uint64_t expired{0};
  std::uint64_t fenced_by_restart{0};
  std::uint64_t duplicates_refused{0};
  std::uint64_t capacity_refused{0};
  std::uint64_t unknown_id_refused{0};

  /// Exact accounting closure:
  ///   issue_attempts == issued + duplicates_refused + capacity_refused
  ///   issued         == open + revoked + expired + fenced_by_restart
  [[nodiscard]] bool accounting_closed() const noexcept {
    return issue_attempts == issued + duplicates_refused + capacity_refused &&
           issued == open + revoked + expired + fenced_by_restart;
  }
};

/// Bounded registry of fencing intents. Synchronised by the owning engine.
class FenceRegistry {
 public:
  explicit FenceRegistry(const FencePolicy& policy);

  /// Mints and stores a fencing intent. Refuses with Duplicate when an open intent
  /// already exists for the same subject under the same generation vector.
  Outcome issue(const Scope& scope, const GenerationVector& gens, BootId boot,
                IncarnationId incarnation, Provenance provenance, ReasonCode reason, WallNs now,
                FenceId& out_id, ReasonCode& out_reason);

  /// Two-phase issuance, used where the intent must be made durable BEFORE it is
  /// allowed to exist in memory. stage() validates and mints an id without
  /// mutating the registry; commit_staged() publishes it once the durable record
  /// is confirmed.
  Outcome stage(const Scope& scope, const GenerationVector& gens, BootId boot,
                IncarnationId incarnation, Provenance provenance, ReasonCode reason, WallNs now,
                FenceIntent& out_intent, ReasonCode& out_reason);
  Outcome commit_staged(const FenceIntent& intent);

  Outcome acknowledge(FenceId id, EvidenceSourceId downstream, WallNs now, ReasonCode& out_reason);
  Outcome report_effect(FenceId id, EvidenceSourceId downstream, bool verified, WallNs now,
                        ReasonCode& out_reason);
  Outcome revoke(FenceId id, ReasonCode reason, WallNs now);
  /// Revokes every open intent that is expired or whose generation vector no longer
  /// matches. Returns the revoked ids in deterministic order.
  std::vector<FenceId> revalidate(WallNs now, const GenerationVector& current);
  /// Revokes every open intent. Used on restart: pre-restart authority never
  /// survives into a new incarnation.
  std::vector<FenceId> fence_all(ReasonCode reason, WallNs now);

  [[nodiscard]] const FenceRecord* find(FenceId id) const;
  [[nodiscard]] const FenceRecord* open_for(const Scope& scope) const;
  [[nodiscard]] std::vector<FenceRecord> all() const;
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return policy_.max_fences; }
  [[nodiscard]] const FenceStats& stats() const noexcept { return stats_; }
  /// Drops all records but keeps cumulative lifetime accounting. Used on restart:
  /// the in-memory registry never inherits pre-restart authority.
  void clear() noexcept;

 private:
  FencePolicy policy_;
  std::map<FenceId, FenceRecord> records_;
  FenceStats stats_{};
};

/// Restoration gate. Restoration requires fresh positive delivery evidence under
/// the *current* generation vector; absence of blackhole evidence is never enough.
struct RestorationDecision {
  Outcome outcome{Outcome::NoEvidence};
  DecisionId id{};
  bool authorized{false};
  ReasonCode reason{ReasonCode::RestorationRefusedNoEvidence};
  Scope scope{};
  GenerationVector gens{};
  std::vector<SourceRef> sources;
  std::uint32_t distinct_incarnations{0};
  std::uint64_t success_attempts{0};
  bool had_open_fence{false};
  FenceId revoked_fence{};
  WallNs decided_at{};
};

RestorationDecision evaluate_restoration(const Diagnosis& diagnosis, const FencePolicy& policy,
                                         const FenceRecord* open_fence, WallNs now);

/// Every externally visible decision produced by this runtime.
enum class DecisionKind : std::uint16_t {
  Diagnosis = 1,
  FenceIntent = 2,
  FenceAcknowledgement = 3,
  FenceEffect = 4,
  FenceRevocation = 5,
  Restoration = 6,
  Interruption = 7,
};

constexpr bool is_valid_decision_kind(std::uint16_t raw) noexcept {
  return raw >= 1u && raw <= 7u;
}

constexpr const char* to_string(DecisionKind k) noexcept {
  switch (k) {
    case DecisionKind::Diagnosis: return "Diagnosis";
    case DecisionKind::FenceIntent: return "FenceIntent";
    case DecisionKind::FenceAcknowledgement: return "FenceAcknowledgement";
    case DecisionKind::FenceEffect: return "FenceEffect";
    case DecisionKind::FenceRevocation: return "FenceRevocation";
    case DecisionKind::Restoration: return "Restoration";
    case DecisionKind::Interruption: return "Interruption";
  }
  return "Diagnosis";
}

struct Decision {
  DecisionId id{};
  DecisionKind kind{DecisionKind::Diagnosis};
  Outcome outcome{Outcome::NoEvidence};
  Scope scope{};
  GenerationVector gens{};
  Classification classification{Classification::NoEvidence};
  ReasonCode primary_reason{ReasonCode::None};
  std::vector<ReasonCode> reasons{};
  AuthorityVector authority{};
  EvidenceCensus census{};
  /// Whether the policy corroboration requirements were satisfied for this
  /// subject. Reported separately from authority so an uncorroborated blackhole is
  /// never confused with an authorized one.
  bool corroborated{false};
  ReasonCode corroboration_reason{ReasonCode::None};
  LocalizationResult localization{};
  bool has_localization{false};
  bool has_intent{false};
  FenceIntent intent{};
  Provenance provenance{Provenance::Synthetic};
  PolicyVersion policy{};
  BootId boot{};
  IncarnationId incarnation{};
  WallNs decided_at{};

  [[nodiscard]] bool is_valid() const;
};

/// Deterministic decision identity derived from the canonical content.
DecisionId derive_decision_id(const Decision& d);

void encode(Writer& w, const Decision& d) noexcept;
Outcome decode(Reader& r, Decision& d) noexcept;

void encode(Writer& w, const FenceIntent& f) noexcept;
Outcome decode(Reader& r, FenceIntent& f) noexcept;

/// Lifecycle state of a tracked subject. Exposed so callers can see that a subject
/// is revalidating rather than silently assume it is healthy.
enum class PathLifecycle : std::uint8_t {
  Unobserved = 0,
  Observed = 1,
  Suspected = 2,
  Diagnosed = 3,
  FenceIntended = 4,
  FenceAcknowledged = 5,
  Restored = 6,
  Interrupted = 7,
  Fenced = 8,
};

constexpr const char* to_string(PathLifecycle s) noexcept {
  switch (s) {
    case PathLifecycle::Unobserved: return "UNOBSERVED";
    case PathLifecycle::Observed: return "OBSERVED";
    case PathLifecycle::Suspected: return "SUSPECTED";
    case PathLifecycle::Diagnosed: return "DIAGNOSED";
    case PathLifecycle::FenceIntended: return "FENCE_INTENDED";
    case PathLifecycle::FenceAcknowledged: return "FENCE_ACKNOWLEDGED";
    case PathLifecycle::Restored: return "RESTORED";
    case PathLifecycle::Interrupted: return "INTERRUPTED";
    case PathLifecycle::Fenced: return "FENCED";
  }
  return "UNOBSERVED";
}

}  // namespace bhg
