#include "blackhole/localize/localizer.hpp"

#include <algorithm>
#include <map>
#include <tuple>

#include "blackhole/core/bitops.hpp"
#include "blackhole/core/checked.hpp"

namespace bhg {

namespace {

using Mask = std::uint64_t;

struct Reduced {
  std::vector<std::uint32_t> candidates;   // ascending element ids
  std::vector<Mask> sets;                  // set si as a mask over candidate indices
  std::vector<std::vector<std::uint32_t>> raw_sets;  // same, as element ids
  std::vector<std::uint32_t> set_origin;   // reduced set -> original failure set index
  std::vector<Mask> cover;                 // cover[ci] = sets containing candidate ci
  Mask full_mask{0};
  std::uint32_t min_set_origin{0};
  bool has_empty_set{false};
  std::uint32_t empty_set_origin{0};
};

struct RawSet {
  std::vector<std::uint32_t> elems;
  std::uint32_t origin{0};
};

Reduced reduce(const LocalizationInput& in) {
  Reduced r;
  std::vector<bool> ok(static_cast<std::size_t>(in.element_count) + 1u, false);
  for (const std::uint32_t e : in.proven_ok) {
    if (e < in.element_count) ok[e] = true;
  }
  for (std::uint32_t e = 0; e < in.element_count; ++e) {
    if (!ok[e]) r.candidates.push_back(e);
  }

  std::vector<RawSet> raw;
  for (std::size_t i = 0; i < in.failure_sets.size(); ++i) {
    std::vector<std::uint32_t> s;
    s.reserve(in.failure_sets[i].size());
    for (const std::uint32_t e : in.failure_sets[i]) {
      if (e < in.element_count && !ok[e]) s.push_back(e);
    }
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
    if (s.empty()) {
      if (!r.has_empty_set) {
        r.has_empty_set = true;
        r.empty_set_origin = static_cast<std::uint32_t>(i);
      }
      continue;
    }
    RawSet rs;
    rs.elems = std::move(s);
    rs.origin = static_cast<std::uint32_t>(i);
    raw.push_back(std::move(rs));
  }

  // Deterministic order: by size, then lexicographically. Duplicate sets and
  // supersets of a kept set carry no additional constraint and are dropped.
  std::vector<std::size_t> order(raw.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (raw[a].elems.size() != raw[b].elems.size()) {
      return raw[a].elems.size() < raw[b].elems.size();
    }
    return raw[a].elems < raw[b].elems;
  });

  std::vector<RawSet> kept;
  for (const std::size_t idx : order) {
    bool dominated = false;
    for (const RawSet& k : kept) {
      if (k.elems.size() <= raw[idx].elems.size() &&
          std::includes(raw[idx].elems.begin(), raw[idx].elems.end(), k.elems.begin(),
                        k.elems.end())) {
        dominated = true;
        break;
      }
    }
    if (!dominated) kept.push_back(raw[idx]);
  }

  if (!kept.empty()) r.min_set_origin = kept.front().origin;

  auto to_cand_mask = [&](const std::vector<std::uint32_t>& elems) -> Mask {
    Mask m = 0;
    for (const std::uint32_t e : elems) {
      const auto it = std::lower_bound(r.candidates.begin(), r.candidates.end(), e);
      if (it != r.candidates.end() && *it == e) {
        m |= (Mask{1} << static_cast<std::size_t>(it - r.candidates.begin()));
      }
    }
    return m;
  };

  r.cover.assign(r.candidates.size(), 0);
  for (std::size_t si = 0; si < kept.size(); ++si) {
    const Mask m = to_cand_mask(kept[si].elems);
    r.sets.push_back(m);
    r.raw_sets.push_back(kept[si].elems);
    r.set_origin.push_back(kept[si].origin);
    r.full_mask |= (Mask{1} << si);
    for (std::size_t ci = 0; ci < r.candidates.size(); ++ci) {
      if ((m & (Mask{1} << ci)) != 0) r.cover[ci] |= (Mask{1} << si);
    }
  }
  return r;
}

Mask elements_to_mask(const Reduced& r, const std::vector<std::uint32_t>& elems) {
  Mask m = 0;
  for (const std::uint32_t e : elems) {
    const auto it = std::lower_bound(r.candidates.begin(), r.candidates.end(), e);
    if (it != r.candidates.end() && *it == e) {
      m |= (Mask{1} << static_cast<std::size_t>(it - r.candidates.begin()));
    }
  }
  return m;
}

/// Complete feasibility oracle: can the constraint mask be covered using at most
/// budget candidates drawn from candidate indices >= lo?
///
/// Soundness: the lowest-index uncovered set must be hit and every candidate in
/// it is tried. Completeness: induction on budget; the restriction to indices
/// >= lo is the only restriction and it is monotone.
class FeasibilityOracle {
 public:
  FeasibilityOracle(const Reduced& r, std::uint64_t node_budget) : r_(r), budget_(node_budget) {}

  bool covers(Mask mask, std::uint32_t budget, std::uint32_t lo) {
    if (exhausted_) return false;
    if (mask == 0) return true;
    if (budget == 0) return false;
    if (++nodes_ > budget_) {
      exhausted_ = true;
      return false;
    }
    const Key key{mask, budget, lo};
    const auto it = memo_.find(key);
    if (it != memo_.end()) return it->second;

    const std::uint32_t si = lowest_set_bit64(mask);
    bool result = false;
    if (si < 64u) {
      for (std::size_t ci = lo; ci < r_.candidates.size(); ++ci) {
        if ((r_.cover[ci] & (Mask{1} << si)) == 0) continue;
        if (covers(mask & ~r_.cover[ci], budget - 1, lo)) {
          result = true;
          break;
        }
        if (exhausted_) break;
      }
    }
    if (!exhausted_) memo_[key] = result;
    return result;
  }

  [[nodiscard]] std::uint64_t nodes() const noexcept { return nodes_; }
  [[nodiscard]] bool exhausted() const noexcept { return exhausted_; }

 private:
  struct Key {
    Mask mask;
    std::uint32_t budget;
    std::uint32_t lo;
    friend bool operator<(const Key& a, const Key& b) {
      return std::tie(a.mask, a.budget, a.lo) < std::tie(b.mask, b.budget, b.lo);
    }
  };

  const Reduced& r_;
  std::uint64_t budget_;
  std::uint64_t nodes_{0};
  bool exhausted_{false};
  std::map<Key, bool> memo_;
};

/// Enumerates distinct minimum localizations in lexicographic order, pruning with
/// the feasibility oracle. Each solution is produced exactly once.
class SolutionEnumerator {
 public:
  SolutionEnumerator(const Reduced& r, FeasibilityOracle& oracle, std::uint32_t max_solutions,
                     std::uint64_t node_budget)
      : r_(r), oracle_(oracle), max_solutions_(max_solutions), node_budget_(node_budget) {}

  void run(Mask mask, std::uint32_t budget, std::size_t lo) {
    if (truncated_) return;
    if (mask == 0) {
      ++found_;
      if (found_ == 1) first_ = current_;
      if (found_ >= max_solutions_) truncated_ = true;
      return;
    }
    if (budget == 0) return;
    for (std::size_t ci = lo; ci < r_.candidates.size(); ++ci) {
      if (++nodes_ > node_budget_) {
        truncated_ = true;
        return;
      }
      if ((r_.cover[ci] & mask) == 0) continue;
      if (!oracle_.covers(mask & ~r_.cover[ci], budget - 1, static_cast<std::uint32_t>(ci + 1))) {
        if (oracle_.exhausted()) {
          truncated_ = true;
          return;
        }
        continue;
      }
      current_.push_back(r_.candidates[ci]);
      run(mask & ~r_.cover[ci], budget - 1, ci + 1);
      current_.pop_back();
      if (truncated_) return;
    }
  }

  [[nodiscard]] const std::vector<std::uint32_t>& first() const noexcept { return first_; }
  [[nodiscard]] std::uint32_t found() const noexcept { return found_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  [[nodiscard]] std::uint64_t nodes() const noexcept { return nodes_; }
  [[nodiscard]] bool has_first() const noexcept { return found_ > 0; }

 private:
  const Reduced& r_;
  FeasibilityOracle& oracle_;
  std::uint32_t max_solutions_;
  std::uint64_t node_budget_;
  std::vector<std::uint32_t> current_;
  std::vector<std::uint32_t> first_;
  std::uint32_t found_{0};
  bool truncated_{false};
  std::uint64_t nodes_{0};
};

/// Greedy maximal packing of pairwise element-disjoint constraint sets. The count
/// is a valid lower bound on any hitting set because disjoint sets need distinct
/// elements.
std::uint32_t disjoint_lower_bound(const Reduced& r) {
  std::vector<std::size_t> order(r.sets.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    const std::uint32_t pa = popcount64(r.sets[a]);
    const std::uint32_t pb = popcount64(r.sets[b]);
    if (pa != pb) return pa < pb;
    return r.sets[a] < r.sets[b];
  });
  Mask used = 0;
  std::uint32_t count = 0;
  for (const std::size_t i : order) {
    if ((r.sets[i] & used) != 0) continue;
    used |= r.sets[i];
    ++count;
  }
  return count;
}

}  // namespace

Outcome validate_localization_input(const LocalizationInput& in, std::string& why) {
  why.clear();
  if (in.element_count == 0) {
    why = "element_count must be positive";
    return Outcome::Invalid;
  }
  if (in.failure_sets.size() > kMaxLocalizationSets) {
    why = "failure_sets exceeds the supported mask width";
    return Outcome::Oversized;
  }
  if (in.proven_ok.size() > in.element_count) {
    why = "proven_ok larger than element_count";
    return Outcome::Invalid;
  }
  if (static_cast<std::size_t>(in.element_count) - in.proven_ok.size() >
      kMaxLocalizationCandidates) {
    why = "candidate count exceeds the supported bound";
    return Outcome::Unsupported;
  }
  if (in.search_node_budget == 0) {
    why = "search_node_budget must be positive";
    return Outcome::Invalid;
  }
  if (in.max_solutions == 0) {
    why = "max_solutions must be positive";
    return Outcome::Invalid;
  }
  std::uint32_t prev = 0;
  bool first = true;
  for (const std::uint32_t e : in.proven_ok) {
    if (e >= in.element_count) {
      why = "proven_ok element out of range";
      return Outcome::Invalid;
    }
    if (!first && e <= prev) {
      why = "proven_ok must be ascending and unique";
      return Outcome::Invalid;
    }
    prev = e;
    first = false;
  }
  for (const auto& s : in.failure_sets) {
    prev = 0;
    first = true;
    for (const std::uint32_t e : s) {
      if (e >= in.element_count) {
        why = "failure set element out of range";
        return Outcome::Invalid;
      }
      if (!first && e <= prev) {
        why = "failure set must be ascending and unique";
        return Outcome::Invalid;
      }
      prev = e;
      first = false;
    }
  }
  return Outcome::Ok;
}

LocalizationResult localize(const LocalizationInput& in) {
  LocalizationResult out;
  std::string why;
  if (!is_affirmative(validate_localization_input(in, why))) {
    out.status = LocalizationStatus::InvalidInput;
    out.reason = ReasonCode::LocalizationInvalid;
    return out;
  }

  const Reduced r = reduce(in);
  if (r.has_empty_set) {
    out.status = LocalizationStatus::ProvenInfeasible;
    out.has_infeasible_witness = true;
    out.infeasible_witness = r.empty_set_origin;
    out.reason = ReasonCode::LocalizationInfeasible;
    return out;
  }
  if (r.sets.empty()) {
    // No failed segment: the empty set is the unique proven-optimal localization.
    out.status = LocalizationStatus::Resolved;
    out.optimality_proven = true;
    out.uniqueness_proven = true;
    out.solutions_found = 1;
    out.reason = ReasonCode::LocalizationResolved;
    return out;
  }

  FeasibilityOracle oracle(r, in.search_node_budget);
  const std::uint32_t lb = disjoint_lower_bound(r);
  const std::uint32_t start = lb == 0 ? 1u : lb;

  std::uint32_t best = 0;
  bool found_size = false;
  for (std::uint32_t k = start; k <= kMaxLocalizationCandidates; ++k) {
    if (oracle.covers(r.full_mask, k, 0)) {
      best = k;
      found_size = true;
      break;
    }
    if (oracle.exhausted()) break;
  }
  out.search_nodes = oracle.nodes();

  if (!found_size) {
    // With non-empty sets over at most 64 candidates a cover always exists, so
    // this is only reachable when the node budget was exhausted.
    out.status = LocalizationStatus::Indeterminate;
    out.reason = ReasonCode::LocalizationSearchLimit;
    out.elements = greedy_localization(in);
    return out;
  }

  out.optimality_proven = true;
  out.optimal_size = best;

  const std::uint64_t remaining_budget =
      in.search_node_budget > out.search_nodes ? in.search_node_budget - out.search_nodes : 1u;
  SolutionEnumerator en(r, oracle, in.max_solutions, remaining_budget);
  en.run(r.full_mask, best, 0);
  out.search_nodes += en.nodes();
  out.solutions_found = en.found();
  out.solutions_truncated = en.truncated();
  // Uniqueness is claimed only when the enumeration completed AND produced exactly
  // one minimum localization. An exhaustive enumeration that found several proves
  // ambiguity, not uniqueness.
  out.uniqueness_proven = !en.truncated() && en.found() == 1;

  if (!en.has_first()) {
    // Minimum size proven, but witness enumeration was cut short: report the
    // greedy cover and revoke the optimality claim rather than overstate it.
    out.elements = greedy_localization(in);
    out.optimality_proven = false;
    out.status = LocalizationStatus::Indeterminate;
    out.reason = ReasonCode::LocalizationSearchLimit;
    return out;
  }

  out.elements = en.first();
  out.status = en.found() > 1 ? LocalizationStatus::Ambiguous : LocalizationStatus::Resolved;
  out.reason =
      en.found() > 1 ? ReasonCode::LocalizationAmbiguous : ReasonCode::LocalizationResolved;
  return out;
}

std::vector<std::uint32_t> greedy_localization(const LocalizationInput& in) {
  const Reduced r = reduce(in);
  std::vector<std::uint32_t> chosen;
  Mask remaining = r.full_mask;
  while (remaining != 0) {
    std::size_t best_idx = r.candidates.size();
    std::uint32_t best_count = 0;
    for (std::size_t ci = 0; ci < r.candidates.size(); ++ci) {
      const std::uint32_t cnt = popcount64(r.cover[ci] & remaining);
      if (cnt > best_count) {
        best_count = cnt;
        best_idx = ci;
      }
    }
    if (best_idx >= r.candidates.size() || best_count == 0) break;
    chosen.push_back(r.candidates[best_idx]);
    remaining &= ~r.cover[best_idx];
  }
  std::sort(chosen.begin(), chosen.end());
  return chosen;
}

LocalizationResult reference_localize(const LocalizationInput& in, std::uint32_t max_candidates) {
  LocalizationResult out;
  std::string why;
  if (!is_affirmative(validate_localization_input(in, why))) {
    out.status = LocalizationStatus::InvalidInput;
    out.reason = ReasonCode::LocalizationInvalid;
    return out;
  }
  const Reduced r = reduce(in);
  if (r.has_empty_set) {
    out.status = LocalizationStatus::ProvenInfeasible;
    out.has_infeasible_witness = true;
    out.infeasible_witness = r.empty_set_origin;
    out.reason = ReasonCode::LocalizationInfeasible;
    return out;
  }
  if (r.sets.empty()) {
    out.status = LocalizationStatus::Resolved;
    out.optimality_proven = true;
    out.uniqueness_proven = true;
    out.solutions_found = 1;
    out.reason = ReasonCode::LocalizationResolved;
    return out;
  }
  const std::size_t n = r.candidates.size();
  if (n > max_candidates) {
    out.status = LocalizationStatus::Indeterminate;
    out.reason = ReasonCode::LocalizationSearchLimit;
    return out;
  }

  std::vector<std::vector<std::uint32_t>> mins;
  for (std::size_t k = 1; k <= n; ++k) {
    std::vector<std::size_t> idx(k);
    for (std::size_t i = 0; i < k; ++i) idx[i] = i;
    bool done = false;
    while (!done) {
      Mask m = 0;
      for (const std::size_t i : idx) m |= (Mask{1} << i);
      Mask covered = 0;
      for (std::size_t ci = 0; ci < n; ++ci) {
        if ((m & (Mask{1} << ci)) != 0) covered |= r.cover[ci];
      }
      if (covered == r.full_mask) {
        std::vector<std::uint32_t> sol;
        sol.reserve(k);
        for (const std::size_t i : idx) sol.push_back(r.candidates[i]);
        mins.push_back(std::move(sol));
        if (mins.size() >= in.max_solutions) break;
      }
      std::size_t pos = k;
      done = true;
      while (pos > 0) {
        --pos;
        if (idx[pos] < n - k + pos) {
          ++idx[pos];
          for (std::size_t j = pos + 1; j < k; ++j) idx[j] = idx[j - 1] + 1;
          done = false;
          break;
        }
      }
    }
    if (!mins.empty()) {
      out.optimal_size = static_cast<std::uint32_t>(k);
      out.optimality_proven = true;
      out.solutions_found = static_cast<std::uint32_t>(mins.size());
      out.uniqueness_proven = mins.size() == 1 && !out.solutions_truncated;
      out.solutions_truncated = mins.size() >= in.max_solutions && in.max_solutions > 1;
      out.elements = mins.front();
      out.status = mins.size() > 1 ? LocalizationStatus::Ambiguous
                                   : LocalizationStatus::Resolved;
      out.reason = mins.size() > 1 ? ReasonCode::LocalizationAmbiguous
                                   : ReasonCode::LocalizationResolved;
      return out;
    }
  }
  out.status = LocalizationStatus::ProvenInfeasible;
  out.has_infeasible_witness = true;
  out.infeasible_witness = r.min_set_origin;
  out.reason = ReasonCode::LocalizationInfeasible;
  return out;
}

ValidationReport verify_localization(const LocalizationInput& in,
                                     const LocalizationResult& result) {
  ValidationReport rep;
  const Reduced r = reduce(in);

  if (result.status == LocalizationStatus::ProvenInfeasible) {
    if (!result.has_infeasible_witness) {
      rep.detail = "ProvenInfeasible reported without a certificate";
      return rep;
    }
    if (result.infeasible_witness >= in.failure_sets.size()) {
      rep.detail = "certificate index out of range";
      return rep;
    }
    for (const std::uint32_t e : in.failure_sets[result.infeasible_witness]) {
      if (e < in.element_count &&
          !std::binary_search(in.proven_ok.begin(), in.proven_ok.end(), e)) {
        rep.detail = "certificate set still contains a live candidate";
        return rep;
      }
    }
    rep.valid = true;
    rep.covers_all = true;
    rep.subset_of_candidates = true;
    rep.canonical_order = true;
    rep.size_matches = true;
    rep.optimality = ValidationReport::OptimalityCheck::Oracle;
    rep.optimality_confirmed = true;
    rep.detail = "infeasibility certificate verified";
    return rep;
  }
  if (result.status == LocalizationStatus::InvalidInput) {
    rep.detail = "result is InvalidInput";
    return rep;
  }

  for (std::size_t i = 0; i < result.elements.size(); ++i) {
    const std::uint32_t e = result.elements[i];
    if (e >= in.element_count) {
      rep.detail = "element out of range";
      return rep;
    }
    if (std::binary_search(in.proven_ok.begin(), in.proven_ok.end(), e)) {
      rep.detail = "element is proven healthy";
      return rep;
    }
    if (i > 0 && e <= result.elements[i - 1]) {
      rep.detail = "elements are not strictly ascending";
      return rep;
    }
  }
  rep.subset_of_candidates = true;
  rep.canonical_order = true;

  bool covers = true;
  for (const auto& s : in.failure_sets) {
    bool hit = false;
    for (const std::uint32_t e : s) {
      if (std::binary_search(result.elements.begin(), result.elements.end(), e)) {
        hit = true;
        break;
      }
    }
    if (!hit) {
      covers = false;
      break;
    }
  }
  rep.covers_all = covers;
  rep.size_matches =
      !result.optimality_proven || result.optimal_size == result.elements.size();
  rep.valid = covers && rep.subset_of_candidates && rep.canonical_order;

  if (result.optimality_proven) {
    if (r.candidates.size() <= kReferenceSolverMaxCandidates) {
      const LocalizationResult ref = reference_localize(in, kReferenceSolverMaxCandidates);
      rep.optimality = ValidationReport::OptimalityCheck::ExhaustiveReference;
      if (ref.status == LocalizationStatus::Indeterminate) {
        rep.optimality = ValidationReport::OptimalityCheck::Unavailable;
        rep.detail = "reference solver unavailable for this instance";
        return rep;
      }
      const bool same_size = ref.optimal_size == result.optimal_size;
      const bool same_set = ref.elements == result.elements;
      const bool same_ambiguity =
          (ref.status == LocalizationStatus::Ambiguous) ==
              (result.status == LocalizationStatus::Ambiguous) ||
          result.solutions_truncated;
      rep.optimality_confirmed = same_size && same_set && same_ambiguity;
      if (!rep.optimality_confirmed) {
        rep.valid = false;
        rep.detail = "independent reference disagrees with the solver output";
      } else {
        rep.detail = "optimality confirmed by exhaustive reference";
      }
    } else {
      FeasibilityOracle oracle(r, 4000000u);
      const Mask m = elements_to_mask(r, result.elements);
      const bool covers_mask = (m & r.full_mask) == r.full_mask;
      const bool no_smaller =
          result.optimal_size == 0
              ? true
              : (!oracle.covers(r.full_mask, result.optimal_size - 1, 0) && !oracle.exhausted());
      rep.optimality = ValidationReport::OptimalityCheck::Oracle;
      rep.optimality_confirmed = covers_mask && no_smaller && !oracle.exhausted();
      if (!rep.optimality_confirmed) {
        rep.valid = false;
        rep.detail = "optimality oracle could not confirm the claimed minimum";
      } else {
        rep.detail = "optimality confirmed by feasibility oracle";
      }
    }
  }
  if (rep.detail.empty()) rep.detail = "validity verified";
  return rep;
}

Outcome build_localization_input(std::uint32_t hop_count, std::span<const HopProbe> probes,
                                 std::uint64_t search_node_budget, std::uint32_t max_solutions,
                                 LocalizationInput& out, std::string& why) {
  why.clear();
  if (hop_count == 0) {
    why = "hop_count must be positive";
    return Outcome::Invalid;
  }
  if (probes.size() > kMaxLocalizationSets) {
    why = "probe count exceeds the supported mask width";
    return Outcome::Oversized;
  }
  LocalizationInput in;
  in.element_count = hop_count;
  in.search_node_budget = search_node_budget;
  in.max_solutions = max_solutions;

  std::vector<bool> ok(hop_count, false);
  for (const HopProbe& p : probes) {
    if (p.end_hop <= p.begin_hop || p.end_hop > hop_count) {
      why = "probe range invalid";
      return Outcome::Invalid;
    }
    if (p.success) {
      for (std::uint32_t h = p.begin_hop; h < p.end_hop; ++h) ok[h] = true;
    }
  }
  for (std::uint32_t h = 0; h < hop_count; ++h) {
    if (ok[h]) in.proven_ok.push_back(h);
  }
  for (const HopProbe& p : probes) {
    if (p.success) continue;
    std::vector<std::uint32_t> s;
    for (std::uint32_t h = p.begin_hop; h < p.end_hop; ++h) {
      if (!ok[h]) s.push_back(h);
    }
    in.failure_sets.push_back(std::move(s));
  }
  const Outcome v = validate_localization_input(in, why);
  if (!is_affirmative(v)) return v;
  out = std::move(in);
  return Outcome::Ok;
}

}  // namespace bhg
