#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "blackhole/core/bounded.hpp"
#include "blackhole/core/canonical.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/scope.hpp"

namespace bhg {

/// Policy is durable *definition* state: it survives restart by design because its
/// semantics do not depend on liveness. Everything downstream that depends on policy
/// records the exact PolicyVersion it was decided under.
struct FencePolicy {
  PolicyVersion version{PolicyVersion{1}};
  PolicyGeneration generation{PolicyGeneration{1}};

  // Evidence acceptance window.
  DurationNs freshness = seconds(30);
  DurationNs max_clock_skew = seconds(2);

  // Corroboration required before fencing a structurally valid path.
  std::uint32_t min_corroborating_sources = 2;    // distinct EvidenceSourceId
  std::uint32_t min_distinct_incarnations = 2;    // distinct (boot, incarnation)
  std::uint32_t min_attempts_per_source = 16;     // delivery attempts summarized
  std::uint64_t min_total_attempts = 32;
  bool require_exact_quality = true;              // reject Sampled/Approximate
  bool require_full_generation_agreement = true;

  // Loss semantics (parts per million, 0..1000000).
  std::uint32_t complete_failure_ppm = 1000000;   // exactly total delivery failure
  std::uint32_t severe_loss_ppm = 500000;         // >= this but < total => LOSSY

  // Restoration gate.
  std::uint32_t restore_min_sources = 2;
  std::uint32_t restore_min_incarnations = 2;
  std::uint64_t restore_min_successes = 16;
  std::uint32_t restore_consecutive_clean = 2;    // consecutive clean observations

  // Fence intent lifetime.
  DurationNs fence_ttl = seconds(60);

  // Resource bounds (all tables/histories/queues derived from these).
  std::uint32_t max_evidence_per_scope = 256;
  std::uint32_t max_tracked_scopes = 4096;
  std::uint32_t max_sources_per_scope = 64;
  std::uint32_t max_fences = 1024;
  std::uint32_t max_lineage_records = 4096;
  std::uint32_t max_reasons_per_decision = 16;
  std::uint32_t max_journal_records = 65536;
  std::uint64_t max_journal_bytes = 64ull * 1024ull * 1024ull;
  std::uint32_t max_localization_elements = 256;
  std::uint32_t max_localization_search_nodes = 200000;
  std::uint32_t max_path_hops = 64;
  std::uint32_t max_localization_solutions = 64;
  std::uint32_t max_explanation_elements = 32;
  std::uint32_t max_sessions = 64;
  std::uint32_t max_inflight_per_session = 1;
  std::uint32_t max_frame_bytes = 1u << 20;
  std::uint32_t max_concurrent_connections = 64;
  std::uint32_t log_capacity = 4096;

  friend bool operator==(const FencePolicy&, const FencePolicy&) = default;
};

/// Validates structural policy invariants. Returns Invalid with a reason code when a
/// policy could silently weaken the product invariants.
Outcome validate_policy(const FencePolicy& p, std::string& why);

/// Deterministic canonical fingerprint of the policy definition. Changing any field
/// changes the fingerprint; the fingerprint is what downstream decisions bind to.
std::uint64_t policy_fingerprint(const FencePolicy& p);

FencePolicy default_policy();

void encode(Writer& w, const FencePolicy& p) noexcept;
Outcome decode(Reader& r, FencePolicy& p) noexcept;

/// Reference to the topology/path structure supplied by the adjacent owner. Blackhole
/// Guard consumes it; it never computes it.
struct PathStructure {
  PathId path{};
  std::vector<NodeId> nodes;   // size == hop_count + 1
  std::vector<LinkId> links;   // size == hop_count

  [[nodiscard]] std::uint32_t hop_count() const noexcept {
    return static_cast<std::uint32_t>(links.size());
  }
  [[nodiscard]] bool is_valid() const noexcept {
    return !path.is_nil() && !nodes.empty() && nodes.size() == links.size() + 1;
  }
};

void encode(Writer& w, const PathStructure& p, std::uint32_t max_hops) noexcept;
Outcome decode(Reader& r, PathStructure& p, std::uint32_t max_hops) noexcept;

}  // namespace bhg
