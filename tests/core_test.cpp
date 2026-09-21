#include <algorithm>
#include <set>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

BHG_TEST(core, outcome_is_affirmative_only_for_ok) {
  BHG_CHECK(is_affirmative(Outcome::Ok));
  BHG_CHECK(!is_affirmative(Outcome::Unknown));
  BHG_CHECK(!is_affirmative(Outcome::Stale));
  BHG_CHECK(!is_affirmative(Outcome::Conflict));
  BHG_CHECK(!is_affirmative(Outcome::NoEvidence));
  BHG_CHECK(!is_affirmative(Outcome::Invalid));
  BHG_CHECK(!is_affirmative(Outcome::Unsupported));
  BHG_CHECK(!is_affirmative(Outcome::Indeterminate));
  BHG_CHECK(!is_affirmative(Outcome::SearchLimitReached));
  for (std::uint8_t raw = 0; raw < 250; ++raw) {
    const Outcome o = static_cast<Outcome>(raw);
    if (raw <= static_cast<std::uint8_t>(Outcome::Unavailable)) {
      BHG_CHECK(is_valid_outcome(raw));
    } else {
      BHG_CHECK(!is_valid_outcome(raw));
    }
    (void)o;
  }
}

BHG_TEST(core, checked_arithmetic_refuses_wraparound) {
  BHG_CHECK(checked_add<std::uint32_t>(1u, 2u).value() == 3u);
  BHG_CHECK(!checked_add<std::uint32_t>(0xFFFFFFFFu, 1u).has_value());
  BHG_CHECK(!checked_mul<std::uint32_t>(0x10000u, 0x10000u).has_value());
  BHG_CHECK(checked_mul<std::uint64_t>(1024u, 1024u).value() == 1048576u);
  BHG_CHECK(!checked_sub<std::uint64_t>(1u, 2u).has_value());
  BHG_CHECK(checked_sub<std::uint64_t>(5u, 2u).value() == 3u);
  BHG_CHECK(!checked_add<std::int64_t>(INT64_MAX, 1).has_value());
  BHG_CHECK(!checked_sub<std::int64_t>(INT64_MIN, 1).has_value());
  BHG_CHECK(!checked_cast<std::uint8_t>(300).has_value());
  BHG_CHECK(checked_cast<std::uint8_t>(255).value() == 255u);
  BHG_CHECK(!checked_cast<std::uint32_t>(-1).has_value());
  BHG_CHECK(saturating_add<std::uint32_t>(0xFFFFFFFFu, 5u) == 0xFFFFFFFFu);
}

BHG_TEST(core, crc32c_known_vectors) {
  // Standard CRC-32C check value for "123456789".
  const std::uint32_t c = crc32c(std::string_view("123456789"));
  BHG_CHECK_EQ(c, 0xE3069283u);
  BHG_CHECK_EQ(crc32c(std::string_view("")), 0u);
  // Chaining convention: crc32c(B, crc32c(A)) == crc32c(A || B).
  const std::uint32_t chained = crc32c(std::string_view("c"), crc32c(std::string_view("ab")));
  BHG_CHECK_EQ(chained, crc32c(std::string_view("abc")));
  BHG_CHECK_EQ(crc32c(std::string_view(""), crc32c(std::string_view("abc"))),
               crc32c(std::string_view("abc")));
}

BHG_TEST(core, strong_ids_are_not_interchangeable_types) {
  const NodeId n{7};
  const LinkId l{7};
  BHG_CHECK(n.value() == l.value());
  BHG_CHECK(n == NodeId{7});
  BHG_CHECK(n != NodeId{8});
  BHG_CHECK(PathGeneration{3}.next() == PathGeneration{4});
  BHG_CHECK(NodeId{}.is_nil());
  std::set<NodeId> s;
  s.insert(NodeId{3});
  s.insert(NodeId{1});
  s.insert(NodeId{2});
  std::vector<std::uint64_t> order;
  for (const NodeId id : s) order.push_back(id.value());
  BHG_CHECK(order == std::vector<std::uint64_t>({1u, 2u, 3u}));
}

BHG_TEST(core, generation_vector_mismatch_classification) {
  const GenerationVector a = make_gens(1, 2, 3, 4);
  const GenerationVector b = make_gens(1, 2, 3, 4);
  BHG_CHECK(a == b);
  BHG_CHECK(classify_mismatch(a, b) == GenMismatch::None);
  BHG_CHECK(classify_mismatch(a, make_gens(9, 2, 3, 4)) == GenMismatch::Path);
  BHG_CHECK(classify_mismatch(a, make_gens(1, 9, 3, 4)) == GenMismatch::Topology);
  BHG_CHECK(classify_mismatch(a, make_gens(1, 2, 9, 4)) == GenMismatch::LinkState);
  BHG_CHECK(classify_mismatch(a, make_gens(1, 2, 3, 9)) == GenMismatch::Epoch);
  BHG_CHECK(classify_mismatch(a, make_gens(9, 9, 3, 4)) == GenMismatch::Multiple);
  BHG_CHECK(!GenerationVector{}.is_complete());
  BHG_CHECK(make_gens(1, 1, 1, 1).is_complete());
}

BHG_TEST(core, canonical_writer_reader_roundtrip_and_totality) {
  Writer w(256);
  w.u8(0xABu);
  w.u16(0x1234u);
  w.u32(0xDEADBEEFu);
  w.u64(0x0123456789ABCDEFull);
  w.i64(-42);
  w.boolean(true);
  w.string("blackhole", 64);
  BHG_REQUIRE(w.ok());

  Reader r(w.span());
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::int64_t e = 0;
  bool f = false;
  std::string s;
  BHG_REQUIRE(is_affirmative(r.u8(a)));
  BHG_REQUIRE(is_affirmative(r.u16(b)));
  BHG_REQUIRE(is_affirmative(r.u32(c)));
  BHG_REQUIRE(is_affirmative(r.u64(d)));
  BHG_REQUIRE(is_affirmative(r.i64(e)));
  BHG_REQUIRE(is_affirmative(r.boolean(f)));
  BHG_REQUIRE(is_affirmative(r.string(s, 64)));
  BHG_CHECK_EQ(static_cast<unsigned>(a), 0xABu);
  BHG_CHECK_EQ(static_cast<unsigned>(b), 0x1234u);
  BHG_CHECK_EQ(c, 0xDEADBEEFu);
  BHG_CHECK_EQ(d, 0x0123456789ABCDEFull);
  BHG_CHECK_EQ(e, -42);
  BHG_CHECK(f);
  BHG_CHECK_EQ(s, std::string("blackhole"));
  BHG_REQUIRE(is_affirmative(r.finish()));

  // Truncation at every prefix must be refused, and the reader must be sticky.
  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    Reader t(w.span().subspan(0, cut));
    std::uint8_t x = 0;
    while (is_affirmative(t.u8(x))) {
    }
    BHG_CHECK(!is_affirmative(t.status()));
    BHG_CHECK(!is_affirmative(t.finish()));
  }

  // Trailing bytes are a hard error.
  Writer w2(64);
  w2.u32(1);
  w2.u32(2);
  Reader t2(w2.span());
  std::uint32_t tmp = 0;
  BHG_CHECK(is_affirmative(t2.u32(tmp)));
  BHG_CHECK(!is_affirmative(t2.finish()));
}

BHG_TEST(core, canonical_writer_is_bounded) {
  Writer w(8);
  w.u64(1);
  BHG_CHECK(w.ok());
  w.u8(1);
  BHG_CHECK(!w.ok());
  BHG_CHECK(w.status() == Outcome::Exhausted);
  Writer w2(4);
  const std::string big(100, 'x');
  w2.string(big, 8);
  BHG_CHECK(!w2.ok());
}

BHG_TEST(core, reader_refuses_oversized_string_and_invalid_bool) {
  Writer w(64);
  w.u32(1000);
  w.raw(std::span<const std::byte>());
  Reader r(w.span());
  std::string s;
  BHG_CHECK(r.string(s, 16) == Outcome::Oversized);
  BHG_CHECK(!is_affirmative(r.status()));

  Writer w2(8);
  w2.u8(2);  // invalid boolean
  Reader r2(w2.span());
  bool b = false;
  BHG_CHECK(r2.boolean(b) == Outcome::Invalid);
}

BHG_TEST(core, bounded_history_accounting_is_exact) {
  BoundedHistory<int> h(3);
  for (int i = 0; i < 10; ++i) (void)h.push(i);
  BHG_CHECK_EQ(h.seen(), 10u);
  BHG_CHECK_EQ(h.dropped(), 7u);
  BHG_CHECK_EQ(h.size(), 3u);
  BHG_CHECK(h.accounting_closed());
  BHG_CHECK(h.items() == std::vector<int>({7, 8, 9}));

  BoundedHistory<int> empty(0);
  BHG_CHECK(empty.push(1) == Outcome::Exhausted);
  BHG_CHECK(empty.accounting_closed());
}

BHG_TEST(core, bounded_map_refuses_beyond_capacity) {
  BoundedMap<std::uint32_t, std::string> m(2);
  BHG_CHECK(is_affirmative(m.insert(1, "a")));
  BHG_CHECK(is_affirmative(m.insert(2, "b")));
  BHG_CHECK(m.insert(3, "c") == Outcome::Exhausted);
  BHG_CHECK_EQ(m.rejected(), 1u);
  BHG_CHECK(is_affirmative(m.insert(1, "z")));
  BHG_CHECK(m.find(1) != nullptr && *m.find(1) == "z");
}

BHG_TEST(core, freshness_window_semantics) {
  FreshnessWindow w;
  w.open = WallNs{100};
  w.close = WallNs{200};
  BHG_CHECK(!w.is_fresh_at(WallNs{99}));
  BHG_CHECK(w.is_fresh_at(WallNs{100}));
  BHG_CHECK(w.is_fresh_at(WallNs{199}));
  BHG_CHECK(!w.is_fresh_at(WallNs{200}));
  FreshnessWindow degenerate;
  degenerate.open = WallNs{5};
  degenerate.close = WallNs{5};
  BHG_CHECK(degenerate.is_degenerate());
  BHG_CHECK(!degenerate.is_fresh_at(WallNs{5}));
}

BHG_TEST(core, manual_clock_is_deterministic) {
  ManualClock c(1000);
  BHG_CHECK_EQ(c.wall_now().ns, 1000);
  c.advance(seconds(1));
  BHG_CHECK_EQ(c.wall_now().ns, 1000 + kNsPerSecond);
  c.set_wall(WallNs{7});
  BHG_CHECK_EQ(c.wall_now().ns, 7);
}

BHG_TEST(core, path_component_validation_blocks_traversal) {
  BHG_CHECK(is_safe_path_component("journal.log"));
  BHG_CHECK(!is_safe_path_component(""));
  BHG_CHECK(!is_safe_path_component(".."));
  BHG_CHECK(!is_safe_path_component("."));
  BHG_CHECK(!is_safe_path_component("a/b"));
  BHG_CHECK(!is_safe_path_component("a\\b"));
  BHG_CHECK(!is_safe_path_component("C:evil"));
  BHG_CHECK(!is_safe_path_component(std::string(200, 'a')));
  BHG_CHECK(!is_safe_path_component(std::string("a\0b", 3)));
}

BHG_TEST(core, policy_validation_refuses_weak_policies) {
  std::string why;
  FencePolicy p = default_policy();
  BHG_CHECK(is_affirmative(validate_policy(p, why)));
  p.min_corroborating_sources = 1;
  BHG_CHECK(validate_policy(p, why) == Outcome::Invalid);
  p = default_policy();
  p.min_distinct_incarnations = 1;
  BHG_CHECK(validate_policy(p, why) == Outcome::Invalid);
  p = default_policy();
  p.freshness = millis(1);
  BHG_CHECK(validate_policy(p, why) == Outcome::Invalid);
  p = default_policy();
  p.severe_loss_ppm = p.complete_failure_ppm;
  BHG_CHECK(validate_policy(p, why) == Outcome::Invalid);
  p = default_policy();
  p.max_path_hops = p.max_localization_elements + 1;
  BHG_CHECK(validate_policy(p, why) == Outcome::Invalid);
}

BHG_TEST(core, policy_roundtrip_and_fingerprint_stability) {
  const FencePolicy p = default_policy();
  Writer w(4096);
  encode(w, p);
  BHG_REQUIRE(w.ok());
  Reader r(w.span());
  FencePolicy q;
  BHG_REQUIRE(is_affirmative(decode(r, q)));
  BHG_REQUIRE(is_affirmative(r.finish()));
  BHG_CHECK(p == q);
  BHG_CHECK_EQ(policy_fingerprint(p), policy_fingerprint(q));
  FencePolicy tweaked = p;
  tweaked.min_corroborating_sources = 3;
  BHG_CHECK(policy_fingerprint(tweaked) != policy_fingerprint(p));

  // Invalid policy contents must be refused on decode.
  FencePolicy bad = p;
  bad.min_corroborating_sources = 0;
  Writer w2(4096);
  encode(w2, bad);
  Reader r2(w2.span());
  FencePolicy out;
  BHG_CHECK(decode(r2, out) == Outcome::Invalid);
}

BHG_TEST(core, nonce_sources_are_deterministic_and_distinct) {
  SeededNonceSource a(1234);
  SeededNonceSource b(1234);
  BHG_CHECK_EQ(a.next(), b.next());
  SeededNonceSource c(1);
  SeededNonceSource d(2);
  BHG_CHECK(c.next() != d.next());
  SystemNonceSource s;
  const std::uint64_t x = s.next();
  const std::uint64_t y = s.next();
  BHG_CHECK(x != y);
  BHG_CHECK(current_process_id() != 0u);
}

BHG_TEST(core, popcount_and_lowest_bit) {
  BHG_CHECK_EQ(popcount64(0), 0u);
  BHG_CHECK_EQ(popcount64(0xF0F0u), 8u);
  BHG_CHECK_EQ(lowest_set_bit64(0), 64u);
  BHG_CHECK_EQ(lowest_set_bit64(1), 0u);
  BHG_CHECK_EQ(lowest_set_bit64(0x8000000000000000ull), 63u);
}

BHG_TEST(core, scope_overlap_semantics) {
  const PathId p{1};
  const PathId q{2};
  BHG_CHECK(scopes_overlap(path_scope(p), link_scope(p, LinkId{9})));
  BHG_CHECK(!scopes_overlap(path_scope(p), path_scope(q)));
  BHG_CHECK(scopes_overlap(segment_scope(p, 0, 3), segment_scope(p, 2, 5)));
  BHG_CHECK(!scopes_overlap(segment_scope(p, 0, 2), segment_scope(p, 2, 5)));
  BHG_CHECK(!scopes_overlap(node_scope(p, NodeId{1}), node_scope(p, NodeId{2})));
  BHG_CHECK(node_scope(p, NodeId{1}) != node_scope(p, NodeId{2}));
}

BHG_TEST(core, evidence_structural_validation) {
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  DeliveryEvidence e = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                        EvidenceSequence{1}, WallNs{1000});
  std::string why;
  BHG_CHECK(e.structurally_valid(why));

  DeliveryEvidence bad = e;
  bad.failures = bad.attempts + 1;
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.loss_ppm = kLossTotalPpm + 1;
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.fresh.close = bad.fresh.open;
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.gens.epoch = CoordinatorEpoch{0};
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.seq = EvidenceSequence{0};
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.fresh.close = WallNs{bad.observed_at.ns - 1};
  BHG_CHECK(!bad.structurally_valid(why));
  bad = e;
  bad.attempts = 0;
  BHG_CHECK(!bad.structurally_valid(why));
}

BHG_TEST(core, evidence_identity_is_deterministic) {
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const DeliveryEvidence a = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                               EvidenceSequence{1}, WallNs{1000});
  const DeliveryEvidence b = failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens,
                                              EvidenceSequence{1}, WallNs{1000});
  BHG_CHECK(a.id == b.id);
  DeliveryEvidence c = b;
  c.seq = EvidenceSequence{2};
  c.attempt = AttemptId{99};
  BHG_CHECK(derive_evidence_id(c) != a.id);
}

BHG_TEST(core, classification_authority_gate) {
  BHG_CHECK(may_authorize_fence(Classification::Blackhole));
  BHG_CHECK(!may_authorize_fence(Classification::Unknown));
  BHG_CHECK(!may_authorize_fence(Classification::NoEvidence));
  BHG_CHECK(!may_authorize_fence(Classification::Lossy));
  BHG_CHECK(!may_authorize_fence(Classification::Congested));
  BHG_CHECK(!may_authorize_fence(Classification::Partitioned));
  BHG_CHECK(!may_authorize_fence(Classification::Healthy));
  for (std::uint16_t raw = 0; raw < 20; ++raw) {
    BHG_CHECK(is_valid_classification(raw) == (raw <= 6u));
  }
}
