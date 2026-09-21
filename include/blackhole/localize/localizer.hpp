#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "blackhole/core/outcome.hpp"
#include "blackhole/domain/evidence.hpp"

namespace bhg {

/// ============================================================================
/// Failure localization
/// ============================================================================
///
/// Supported problem class (exactly stated):
///
///   * The subject is a path decomposed into ordered elements (hop indices
///     0..element_count-1; an element denotes the link authority of that hop).
///   * Segment observations over hop ranges [begin_hop, end_hop) are supplied by
///     adjacent owners. A successful segment proves every element in its range
///     healthy. A failed segment proves at least one element in its range faulty.
///   * Derived constraints:
///       - candidates C = { e < element_count : e proven healthy is false }
///       - for each failed segment: S_i = range_i intersect C
///         (S_i empty means the observations are mutually contradictory)
///   * A localization is a set H subset of C with H intersect S_i non-empty for
///     every i. A localization is
///       valid     -- H subset of C and covers every S_i;
///       feasible  -- at least one valid localization exists;
///       optimal   -- |H| equals the minimum size over all valid localizations.
///
/// Objective: minimise |H|. Tie-break: the lexicographically smallest ascending
/// element list. Output is a deterministic total function of the input and does
/// not depend on container, hash, insertion or discovery order.
///
/// Supported bounds (exceeding them is an explicit refusal, never a wrong answer):
///   at most 64 candidates and at most 64 constraint sets.
///
/// Outcome discipline:
///   * optimality is only claimed when the bounded search proved it;
///   * Indeterminate / SEARCH_LIMIT_REACHED is returned when it did not, and the
///     emitted element list is then explicitly labelled best-known;
///   * ProvenInfeasible is returned only with an explicit certificate.

enum class LocalizationStatus : std::uint8_t {
  /// Optimal size proven; exactly one minimum localization was found.
  Resolved = 0,
  /// Optimal size proven; more than one minimum localization exists.
  Ambiguous = 1,
  /// Optimality NOT proven (search budget exhausted). A best-known candidate may
  /// be present; it must not be treated as proof.
  Indeterminate = 2,
  /// No valid localization exists; certificate present.
  ProvenInfeasible = 3,
  /// The input violates the supported problem class.
  InvalidInput = 4,
};

constexpr bool is_valid_localization_status(std::uint8_t raw) noexcept { return raw <= 4u; }

constexpr const char* to_string(LocalizationStatus s) noexcept {
  switch (s) {
    case LocalizationStatus::Resolved: return "Resolved";
    case LocalizationStatus::Ambiguous: return "Ambiguous";
    case LocalizationStatus::Indeterminate: return "Indeterminate";
    case LocalizationStatus::ProvenInfeasible: return "ProvenInfeasible";
    case LocalizationStatus::InvalidInput: return "InvalidInput";
  }
  return "InvalidInput";
}

inline constexpr std::uint32_t kMaxLocalizationSets = 64;
inline constexpr std::uint32_t kMaxLocalizationCandidates = 64;
inline constexpr std::uint32_t kReferenceSolverMaxCandidates = 18;

struct LocalizationInput {
  /// Number of elements (hops) in the subject; elements are 0..element_count-1.
  std::uint32_t element_count{0};
  /// Elements proven healthy by successful segment observations. Ascending, unique.
  std::vector<std::uint32_t> proven_ok;
  /// Constraint sets; each ascending and unique. An empty set is a contradiction.
  std::vector<std::vector<std::uint32_t>> failure_sets;
  /// Bound on explored search nodes (a deterministic counter, not a clock).
  std::uint64_t search_node_budget{200000};
  /// Bound on enumerated distinct minimum solutions before counting stops.
  std::uint32_t max_solutions{64};
};

/// Structural validation of a LocalizationInput (does not solve it).
Outcome validate_localization_input(const LocalizationInput& in, std::string& why);

struct LocalizationResult {
  LocalizationStatus status{LocalizationStatus::InvalidInput};
  /// Canonical ascending element list: the lexicographically smallest minimum
  /// localization when status is Resolved or Ambiguous; best-known otherwise.
  std::vector<std::uint32_t> elements;
  /// Proven minimum size; 0 when optimality was not proven.
  std::uint32_t optimal_size{0};
  bool optimality_proven{false};
  bool uniqueness_proven{false};
  bool solutions_truncated{false};
  std::uint32_t solutions_found{0};
  std::uint64_t search_nodes{0};
  /// Index of the contradictory/unsatisfiable failure set when ProvenInfeasible.
  std::uint32_t infeasible_witness{0};
  bool has_infeasible_witness{false};
  ReasonCode reason{ReasonCode::None};
};

/// Exact bounded solver. Deterministic; never throws.
LocalizationResult localize(const LocalizationInput& in);

/// Independent validator for a localization result. Re-derives the constraint
/// system and checks validity, size and, where possible, optimality and
/// uniqueness through a separate code path from the solver.
struct ValidationReport {
  enum class OptimalityCheck : std::uint8_t {
    NotRequested = 0,
    Oracle = 1,
    ExhaustiveReference = 2,
    Unavailable = 3,
  };
  bool valid{false};
  bool covers_all{false};
  bool subset_of_candidates{false};
  bool canonical_order{false};
  bool size_matches{false};
  OptimalityCheck optimality{OptimalityCheck::NotRequested};
  bool optimality_confirmed{false};
  std::string detail;
};

ValidationReport verify_localization(const LocalizationInput& in,
                                     const LocalizationResult& result);

/// Slow, obviously-correct reference solver used for differential testing.
/// Enumerates candidate subsets in canonical (lexicographic) order. Intended for
/// small instances; returns Indeterminate when the instance is too large.
LocalizationResult reference_localize(const LocalizationInput& in,
                                      std::uint32_t max_candidates = kReferenceSolverMaxCandidates);

/// Greedy (max-coverage, lowest-index tie-break) heuristic. Deliberately not
/// optimal: it is the differential baseline the exact solver must dominate, and
/// an upper bound during search.
std::vector<std::uint32_t> greedy_localization(const LocalizationInput& in);

/// A segment observation over a hop range, as supplied by an adjacent owner.
struct HopProbe {
  std::uint32_t begin_hop{0};
  std::uint32_t end_hop{0};
  bool success{false};
};

/// Derives the constraint system from segment observations. This is the bridge
/// between delivery evidence and the pure localization problem above.
Outcome build_localization_input(std::uint32_t hop_count, std::span<const HopProbe> probes,
                                 std::uint64_t search_node_budget, std::uint32_t max_solutions,
                                 LocalizationInput& out, std::string& why);

}  // namespace bhg
