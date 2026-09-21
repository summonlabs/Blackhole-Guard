#include <algorithm>
#include <set>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

LocalizationInput make_input(std::uint32_t elements,
                             std::vector<std::vector<std::uint32_t>> sets,
                             std::vector<std::uint32_t> proven_ok = {}) {
  LocalizationInput in;
  in.element_count = elements;
  in.failure_sets = std::move(sets);
  in.proven_ok = std::move(proven_ok);
  std::sort(in.proven_ok.begin(), in.proven_ok.end());
  in.search_node_budget = 200000;
  in.max_solutions = 64;
  for (auto& s : in.failure_sets) {
    std::sort(s.begin(), s.end());
    s.erase(std::unique(s.begin(), s.end()), s.end());
  }
  return in;
}

}  // namespace

BHG_TEST(localizer, single_set_localizes_to_its_smallest_element) {
  // One failed segment over three hops has three minimum localizations; the
  // canonical representative is the smallest element.
  const LocalizationInput in = make_input(6, {{1, 2, 3}});
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Ambiguous);
  BHG_CHECK(r.optimality_proven);
  BHG_CHECK(!r.uniqueness_proven);
  BHG_CHECK_EQ(r.optimal_size, 1u);
  BHG_CHECK_EQ(r.solutions_found, 3u);
  BHG_REQUIRE(r.elements.size() == 1u);
  BHG_CHECK_EQ(r.elements[0], 1u);
  const ValidationReport v = verify_localization(in, r);
  BHG_CHECK(v.valid);
  BHG_CHECK(v.optimality_confirmed);
}

BHG_TEST(localizer, empty_constraint_system_is_resolved_to_the_empty_set) {
  const LocalizationInput in = make_input(4, {});
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Resolved);
  BHG_CHECK_EQ(r.optimal_size, 0u);
  BHG_CHECK(r.elements.empty());
  BHG_CHECK(r.uniqueness_proven);
  BHG_CHECK(verify_localization(in, r).valid);
}

BHG_TEST(localizer, contradictory_probes_are_proven_infeasible_with_a_certificate) {
  // Range [0,2) is proven healthy, yet range [0,2) is also reported failed.
  const LocalizationInput in = make_input(4, {{}, {2, 3}}, {0, 1});
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::ProvenInfeasible);
  BHG_CHECK(r.has_infeasible_witness);
  BHG_CHECK_EQ(r.infeasible_witness, 0u);
  const ValidationReport v = verify_localization(in, r);
  BHG_CHECK(v.valid);
  BHG_CHECK(v.optimality_confirmed);
}

BHG_TEST(localizer, ambiguous_optimum_reports_the_lexicographically_smallest) {
  // Two disjoint sets, each of size two: minimum is one element per set, and the
  // canonical representative is the smallest pair in lexicographic order.
  const LocalizationInput in = make_input(6, {{0, 1}, {2, 3}});
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Ambiguous);
  BHG_CHECK_EQ(r.optimal_size, 2u);
  BHG_CHECK(r.elements == std::vector<std::uint32_t>({0u, 2u}));
  BHG_CHECK(r.optimality_proven);
  BHG_CHECK(r.uniqueness_proven == false);
  const ValidationReport v = verify_localization(in, r);
  BHG_CHECK(v.valid);
  BHG_CHECK(v.optimality_confirmed);
}

BHG_TEST(localizer, greedy_is_not_optimal_on_a_constructed_counterexample) {
  // Constructed so that max-coverage greedy (lowest-index tie-break) returns three
  // elements while the optimum is two.
  const LocalizationInput in = make_input(5, {{1, 4}, {0, 3}, {2, 4}, {1, 3}});
  const std::vector<std::uint32_t> greedy = greedy_localization(in);
  BHG_CHECK_EQ(greedy.size(), 3u);
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.optimality_proven);
  BHG_CHECK_EQ(r.optimal_size, 2u);
  BHG_CHECK(r.elements == std::vector<std::uint32_t>({3u, 4u}));
  BHG_CHECK(r.elements.size() < greedy.size());
  const ValidationReport v = verify_localization(in, r);
  BHG_CHECK(v.valid);
  BHG_CHECK(v.optimality_confirmed);
}

BHG_TEST(localizer, differential_against_reference_solver_seeded) {
  Rng rng(0xB16B00B5ull);
  std::uint32_t compared = 0;
  std::uint32_t greedy_worse = 0;
  for (int trial = 0; trial < 4000; ++trial) {
    const std::uint32_t elements = 2u + rng.below(9u);
    const std::uint32_t set_count = 1u + rng.below(6u);
    std::vector<std::vector<std::uint32_t>> sets;
    for (std::uint32_t i = 0; i < set_count; ++i) {
      const std::uint32_t size = 1u + rng.below(4u);
      std::vector<std::uint32_t> s;
      for (std::uint32_t k = 0; k < size; ++k) s.push_back(rng.below(elements));
      sets.push_back(std::move(s));
    }
    std::vector<std::uint32_t> ok;
    for (std::uint32_t e = 0; e < elements; ++e) {
      if (rng.below(6u) == 0u) ok.push_back(e);
    }
    const LocalizationInput in = make_input(elements, sets, ok);
    const LocalizationResult exact = localize(in);
    const LocalizationResult ref = reference_localize(in, 18u);

    BHG_CHECK(ref.status != LocalizationStatus::Indeterminate);
    if (ref.status == LocalizationStatus::ProvenInfeasible) {
      BHG_CHECK(exact.status == LocalizationStatus::ProvenInfeasible);
    } else {
      BHG_CHECK(exact.optimality_proven);
      BHG_CHECK_EQ(exact.optimal_size, ref.optimal_size);
      BHG_CHECK(exact.elements == ref.elements);
      const bool ref_ambiguous = ref.status == LocalizationStatus::Ambiguous;
      const bool exact_ambiguous = exact.status == LocalizationStatus::Ambiguous;
      BHG_CHECK(ref_ambiguous == exact_ambiguous || exact.solutions_truncated);
      const ValidationReport v = verify_localization(in, exact);
      BHG_CHECK(v.valid);
    }
    const std::vector<std::uint32_t> greedy = greedy_localization(in);
    if (ref.status != LocalizationStatus::ProvenInfeasible && !greedy.empty() &&
        greedy.size() > exact.elements.size()) {
      ++greedy_worse;
    }
    ++compared;
  }
  BHG_CHECK_EQ(compared, 4000u);
  // The exact solver must dominate greedy, and greedy must actually be dominated
  // somewhere in the sample, otherwise this differential test proves nothing.
  BHG_CHECK(greedy_worse > 0u);
}

BHG_TEST(localizer, differential_large_instances_against_reference_boundary) {
  Rng rng(0x5EED1234ull);
  for (int trial = 0; trial < 200; ++trial) {
    const std::uint32_t elements = 10u + rng.below(9u);  // <= 18 candidates
    std::vector<std::vector<std::uint32_t>> sets;
    const std::uint32_t set_count = 3u + rng.below(6u);
    for (std::uint32_t i = 0; i < set_count; ++i) {
      const std::uint32_t size = 1u + rng.below(6u);
      std::vector<std::uint32_t> s;
      for (std::uint32_t k = 0; k < size; ++k) s.push_back(rng.below(elements));
      sets.push_back(std::move(s));
    }
    const LocalizationInput in = make_input(elements, sets);
    const LocalizationResult exact = localize(in);
    const LocalizationResult ref = reference_localize(in, 18u);
    if (ref.status == LocalizationStatus::Indeterminate) continue;
    if (ref.status == LocalizationStatus::ProvenInfeasible) {
      BHG_CHECK(exact.status == LocalizationStatus::ProvenInfeasible);
      continue;
    }
    BHG_CHECK_EQ(exact.optimal_size, ref.optimal_size);
    BHG_CHECK(exact.elements == ref.elements);
  }
}

BHG_TEST(localizer, search_limit_is_reported_not_hidden) {
  LocalizationInput in;
  in.element_count = 40;
  in.search_node_budget = 1;  // deliberately tiny
  in.max_solutions = 64;
  for (std::uint32_t i = 0; i < 20; ++i) {
    in.failure_sets.push_back({i, i + 1u, i + 2u});
  }
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Indeterminate);
  BHG_CHECK(!r.optimality_proven);
  BHG_CHECK(r.reason == ReasonCode::LocalizationSearchLimit);
}

BHG_TEST(localizer, solution_enumeration_is_bounded_and_declared_truncated) {
  // Six disjoint pairs: the minimum is six elements and there are 2^6 minimum
  // solutions, so counting stops at the configured bound and says so.
  LocalizationInput in;
  in.element_count = 12;
  in.max_solutions = 2;
  in.search_node_budget = 200000;
  for (std::uint32_t i = 0; i < 12; i += 2) {
    in.failure_sets.push_back({i, i + 1u});
  }
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Ambiguous);
  BHG_CHECK_EQ(r.optimal_size, 6u);
  BHG_CHECK(r.solutions_truncated);
  BHG_CHECK(!r.uniqueness_proven);
  BHG_CHECK(r.elements == std::vector<std::uint32_t>({0u, 2u, 4u, 6u, 8u, 10u}));
  const ValidationReport v = verify_localization(in, r);
  BHG_CHECK(v.valid);
}

BHG_TEST(localizer, input_validation_refuses_out_of_class_inputs) {
  std::string why;
  LocalizationInput in;
  in.element_count = 0;
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Invalid);

  in = make_input(4, {{0}});
  in.failure_sets[0] = {4};
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Invalid);

  in = make_input(4, {});
  in.failure_sets.push_back({2, 1});  // deliberately not ascending
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Invalid);

  in = make_input(4, {});
  in.proven_ok = {1, 1};
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Invalid);

  in = make_input(200, {});
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Unsupported);

  in = make_input(4, {});
  in.failure_sets.assign(kMaxLocalizationSets + 1u, std::vector<std::uint32_t>{0});
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Oversized);

  in = make_input(4, {{0}});
  in.search_node_budget = 0;
  BHG_CHECK(validate_localization_input(in, why) == Outcome::Invalid);

  const LocalizationInput bad = make_input(0, {{0}});
  const LocalizationResult r = localize(bad);
  BHG_CHECK(r.status == LocalizationStatus::InvalidInput);
  BHG_CHECK(verify_localization(bad, r).valid == false);
}

BHG_TEST(localizer, result_is_independent_of_container_and_insertion_order) {
  std::vector<std::vector<std::uint32_t>> forward = {{3, 4}, {0, 1, 2}, {5}, {2, 5}};
  std::vector<std::vector<std::uint32_t>> reversed(forward.rbegin(), forward.rend());
  const LocalizationResult a = localize(make_input(8, forward));
  const LocalizationResult b = localize(make_input(8, reversed));
  BHG_CHECK(a.elements == b.elements);
  BHG_CHECK_EQ(a.optimal_size, b.optimal_size);
  BHG_CHECK(a.status == b.status);

  // Duplicate sets and supersets must not change the answer.
  std::vector<std::vector<std::uint32_t>> with_dupes = forward;
  with_dupes.push_back({0, 1, 2});
  with_dupes.push_back({0, 1, 2, 3, 4, 5});
  const LocalizationResult c = localize(make_input(8, with_dupes));
  BHG_CHECK(c.elements == a.elements);
  BHG_CHECK_EQ(c.optimal_size, a.optimal_size);
}

BHG_TEST(localizer, hop_probe_derivation) {
  const std::vector<HopProbe> probes = {
      {0, 4, true},   // hops 0..3 healthy
      {2, 6, false},  // failure within hops 2..5, restricted to {4,5}
      {4, 6, false},  // failure within hops 4..5
  };
  LocalizationInput in;
  std::string why;
  BHG_REQUIRE(is_affirmative(build_localization_input(6, probes, 200000, 64, in, why)));
  BHG_CHECK(in.element_count == 6u);
  BHG_CHECK(in.proven_ok == std::vector<std::uint32_t>({0u, 1u, 2u, 3u}));
  BHG_REQUIRE(in.failure_sets.size() == 2u);
  BHG_CHECK(in.failure_sets[0] == std::vector<std::uint32_t>({4u, 5u}));
  BHG_CHECK(in.failure_sets[1] == std::vector<std::uint32_t>({4u, 5u}));
  const LocalizationResult r = localize(in);
  BHG_CHECK(r.status == LocalizationStatus::Ambiguous);
  BHG_CHECK_EQ(r.optimal_size, 1u);

  // A probe range that contradicts a successful probe yields a proven-infeasible
  // instance rather than an invented localization.
  const std::vector<HopProbe> contradictory = {{0, 3, true}, {1, 2, false}};
  BHG_REQUIRE(is_affirmative(build_localization_input(4, contradictory, 200000, 64, in, why)));
  const LocalizationResult r2 = localize(in);
  BHG_CHECK(r2.status == LocalizationStatus::ProvenInfeasible);
  BHG_CHECK(verify_localization(in, r2).valid);

  // Structural refusals.
  const std::vector<HopProbe> no_probes;
  const std::vector<HopProbe> backwards = {{2, 1, false}};
  const std::vector<HopProbe> out_of_range = {{0, 9, false}};
  BHG_CHECK(build_localization_input(0, no_probes, 200000, 64, in, why) == Outcome::Invalid);
  BHG_CHECK(build_localization_input(4, backwards, 200000, 64, in, why) == Outcome::Invalid);
  BHG_CHECK(build_localization_input(4, out_of_range, 200000, 64, in, why) == Outcome::Invalid);
  std::vector<HopProbe> too_many(kMaxLocalizationSets + 1u, HopProbe{0, 1, false});
  BHG_CHECK(build_localization_input(4, too_many, 200000, 64, in, why) == Outcome::Oversized);
}

BHG_TEST(localizer, adversarial_chain_and_star_topologies) {
  // Chain: each failed segment covers a distinct pair; optimum is a hitting set.
  const LocalizationInput chain = make_input(8, {{0, 1}, {2, 3}, {4, 5}, {6, 7}});
  const LocalizationResult cr = localize(chain);
  BHG_CHECK_EQ(cr.optimal_size, 4u);
  BHG_CHECK(cr.elements == std::vector<std::uint32_t>({0u, 2u, 4u, 6u}));
  BHG_CHECK(verify_localization(chain, cr).optimality_confirmed);

  // Star: one shared element hits every set.
  std::vector<std::vector<std::uint32_t>> star;
  for (std::uint32_t i = 1; i < 16; ++i) star.push_back({0, i});
  const LocalizationResult sr = localize(make_input(16, star));
  BHG_CHECK_EQ(sr.optimal_size, 1u);
  BHG_REQUIRE(sr.elements.size() == 1u);
  BHG_CHECK_EQ(sr.elements[0], 0u);
  // Element 0 is in every set and no other single element hits more than one, so
  // the minimum localization is unique.
  BHG_CHECK(sr.status == LocalizationStatus::Resolved);
  BHG_CHECK(sr.uniqueness_proven);
  BHG_CHECK(verify_localization(make_input(16, star), sr).optimality_confirmed);
}

BHG_TEST(localizer, verify_rejects_tampered_results) {
  const LocalizationInput in = make_input(6, {{0, 1}, {3, 4}});
  const LocalizationResult good = localize(in);
  BHG_CHECK(verify_localization(in, good).valid);

  LocalizationResult tampered = good;
  tampered.elements = {5};
  BHG_CHECK(!verify_localization(in, tampered).valid);

  tampered = good;
  tampered.elements = {0};
  BHG_CHECK(!verify_localization(in, tampered).valid);

  tampered = good;
  tampered.elements = {4, 1};
  BHG_CHECK(!verify_localization(in, tampered).valid);

  tampered = good;
  tampered.optimal_size = 1;
  tampered.elements = {0, 3};
  tampered.optimality_proven = true;
  BHG_CHECK(!verify_localization(in, tampered).valid);

  LocalizationResult fake_infeasible;
  fake_infeasible.status = LocalizationStatus::ProvenInfeasible;
  fake_infeasible.has_infeasible_witness = false;
  BHG_CHECK(!verify_localization(in, fake_infeasible).valid);
}
