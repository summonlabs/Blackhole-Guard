#include <algorithm>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

struct FenceFixture {
  FenceFixture() : registry(policy) {}
  FencePolicy policy = default_policy();
  FenceRegistry registry;
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const BootId boot{BootId{5}};
  const IncarnationId inc{IncarnationId{6}};
  WallNs now{WallNs{1000000000}};
};

}  // namespace

BHG_TEST(authority, authority_stage_ladder_is_ordered) {
  BHG_CHECK(static_cast<std::uint8_t>(AuthorityStage::Observation) <
            static_cast<std::uint8_t>(AuthorityStage::Eligibility));
  BHG_CHECK(static_cast<std::uint8_t>(AuthorityStage::Eligibility) <
            static_cast<std::uint8_t>(AuthorityStage::Authorization));
  BHG_CHECK(static_cast<std::uint8_t>(AuthorityStage::Authorization) <
            static_cast<std::uint8_t>(AuthorityStage::Intent));
  BHG_CHECK(static_cast<std::uint8_t>(AuthorityStage::Intent) <
            static_cast<std::uint8_t>(AuthorityStage::Acknowledgement));
  BHG_CHECK(static_cast<std::uint8_t>(AuthorityStage::Acknowledgement) <
            static_cast<std::uint8_t>(AuthorityStage::VerifiedEffect));

  AuthorityVector v;
  v.stage = AuthorityStage::Eligibility;
  BHG_CHECK(!v.is_positive());
  v.stage = AuthorityStage::Observation;
  BHG_CHECK(!v.is_positive());
  v.stage = AuthorityStage::Authorization;
  BHG_CHECK(v.is_positive());
  v.revoked = true;
  BHG_CHECK(!v.is_positive());
}

BHG_TEST(authority, binding_requires_matching_generation_boot_and_incarnation) {
  AuthorityVector v;
  v.stage = AuthorityStage::Intent;
  v.gens = make_gens(1, 1, 1, 1);
  v.boot = BootId{1};
  v.incarnation = IncarnationId{1};
  v.expires_at = WallNs{1000};
  BHG_CHECK(v.is_binding_at(make_gens(1, 1, 1, 1), BootId{1}, IncarnationId{1}, WallNs{999}));
  BHG_CHECK(!v.is_binding_at(make_gens(1, 1, 1, 2), BootId{1}, IncarnationId{1}, WallNs{999}));
  BHG_CHECK(!v.is_binding_at(make_gens(1, 1, 1, 1), BootId{2}, IncarnationId{1}, WallNs{999}));
  BHG_CHECK(!v.is_binding_at(make_gens(1, 1, 1, 1), BootId{1}, IncarnationId{2}, WallNs{999}));
  BHG_CHECK(!v.is_binding_at(make_gens(1, 1, 1, 1), BootId{1}, IncarnationId{1}, WallNs{1000}));
}

BHG_TEST(authority, fence_lifecycle_and_exact_accounting) {
  FenceFixture f;
  FenceId id{};
  ReasonCode reason = ReasonCode::None;
  BHG_CHECK(is_affirmative(f.registry.issue(f.scope, f.gens, f.boot, f.inc, Provenance::Synthetic,
                                            ReasonCode::CompleteDeliveryFailure, f.now, id,
                                            reason)));
  BHG_CHECK(!id.is_nil());
  BHG_CHECK(f.registry.stats().accounting_closed());
  BHG_CHECK_EQ(f.registry.stats().open, 1u);

  // A duplicate open intent for the same subject/generation is refused.
  FenceId dup{};
  BHG_CHECK(f.registry.issue(f.scope, f.gens, f.boot, f.inc, Provenance::Synthetic,
                             ReasonCode::CompleteDeliveryFailure, f.now, dup,
                             reason) == Outcome::Duplicate);
  BHG_CHECK(dup == id);

  // A different generation is a different subject.
  FenceId other{};
  BHG_CHECK(is_affirmative(f.registry.issue(f.scope, make_gens(2, 2, 2, 2), f.boot, f.inc,
                                            Provenance::Synthetic,
                                            ReasonCode::CompleteDeliveryFailure, f.now, other,
                                            reason)));
  BHG_CHECK(other != id);

  BHG_CHECK(is_affirmative(f.registry.acknowledge(id, EvidenceSourceId{9}, f.now, reason)));
  BHG_CHECK(reason == ReasonCode::FenceAcknowledged);
  BHG_CHECK(is_affirmative(f.registry.report_effect(id, EvidenceSourceId{9}, true, f.now, reason)));
  BHG_CHECK(reason == ReasonCode::FenceEffectVerified);
  const FenceRecord* rec = f.registry.find(id);
  BHG_REQUIRE(rec != nullptr);
  BHG_CHECK(rec->lifecycle == FenceLifecycle::EffectReported);
  BHG_CHECK(rec->effect_verified);
  BHG_CHECK(rec->is_open());

  BHG_CHECK(is_affirmative(f.registry.revoke(id, ReasonCode::FenceIntentRevoked, f.now)));
  BHG_CHECK(f.registry.stats().accounting_closed());
  BHG_CHECK_EQ(f.registry.stats().revoked, 1u);
  BHG_CHECK_EQ(f.registry.stats().open, 1u);
}

BHG_TEST(authority, fence_expires_at_ttl_boundary) {
  FenceFixture f;
  FenceId id{};
  ReasonCode reason = ReasonCode::None;
  BHG_REQUIRE(is_affirmative(f.registry.issue(f.scope, f.gens, f.boot, f.inc,
                                              Provenance::Synthetic, ReasonCode::CompleteDeliveryFailure,
                                              f.now, id, reason)));
  const WallNs after_ttl{f.now.ns + f.policy.fence_ttl.ns};
  BHG_CHECK(f.registry.acknowledge(id, EvidenceSourceId{1}, after_ttl, reason) == Outcome::Expired);
  BHG_CHECK(reason == ReasonCode::FenceExpired);
  BHG_CHECK(f.registry.stats().accounting_closed());
  BHG_CHECK_EQ(f.registry.stats().expired, 1u);
}

BHG_TEST(authority, revalidate_revokes_on_generation_change) {
  FenceFixture f;
  FenceId id{};
  ReasonCode reason = ReasonCode::None;
  BHG_REQUIRE(is_affirmative(f.registry.issue(f.scope, f.gens, f.boot, f.inc,
                                              Provenance::Synthetic, ReasonCode::CompleteDeliveryFailure,
                                              f.now, id, reason)));
  const auto revoked = f.registry.revalidate(f.now, make_gens(1, 1, 1, 2));
  BHG_REQUIRE(revoked.size() == 1u);
  BHG_CHECK(revoked[0] == id);
  const FenceRecord* rec = f.registry.find(id);
  BHG_REQUIRE(rec != nullptr);
  BHG_CHECK(rec->lifecycle == FenceLifecycle::Revoked);
  BHG_CHECK(rec->terminal_reason == ReasonCode::DependencyGenerationChanged);
  BHG_CHECK(f.registry.stats().accounting_closed());
  BHG_CHECK(f.registry.revalidate(f.now, make_gens(1, 1, 1, 2)).empty());
}

BHG_TEST(authority, fence_all_fences_everything_and_is_idempotent) {
  FenceFixture f;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t g = 1; g <= 3; ++g) {
    FenceId id{};
    BHG_REQUIRE(is_affirmative(f.registry.issue(f.scope, make_gens(g, g, g, g), f.boot, f.inc,
                                                Provenance::Synthetic,
                                                ReasonCode::CompleteDeliveryFailure, f.now, id,
                                                reason)));
  }
  const auto fenced = f.registry.fence_all(ReasonCode::RestartFencedPriorAuthority, f.now);
  BHG_CHECK_EQ(fenced.size(), 3u);
  BHG_CHECK(f.registry.fence_all(ReasonCode::RestartFencedPriorAuthority, f.now).empty());
  BHG_CHECK(f.registry.stats().accounting_closed());
  BHG_CHECK_EQ(f.registry.stats().open, 0u);
}

BHG_TEST(authority, capacity_is_bounded_and_counted) {
  FencePolicy policy = default_policy();
  policy.max_fences = 4;
  FenceRegistry registry(policy);
  const Scope scope = path_scope(PathId{1});
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t g = 1; g <= 4; ++g) {
    FenceId id{};
    BHG_CHECK(is_affirmative(registry.issue(scope, make_gens(g, g, g, g), BootId{1},
                                            IncarnationId{1}, Provenance::Synthetic,
                                            ReasonCode::CompleteDeliveryFailure,
                                            WallNs{1000}, id, reason)));
  }
  FenceId id{};
  BHG_CHECK(registry.issue(scope, make_gens(9, 9, 9, 9), BootId{1}, IncarnationId{1},
                           Provenance::Synthetic, ReasonCode::CompleteDeliveryFailure,
                           WallNs{1000}, id, reason) == Outcome::Exhausted);
  BHG_CHECK_EQ(registry.stats().capacity_refused, 1u);
  BHG_CHECK(registry.stats().accounting_closed());
}

BHG_TEST(authority, unknown_fence_ids_are_refused) {
  FenceFixture f;
  ReasonCode reason = ReasonCode::None;
  BHG_CHECK(f.registry.acknowledge(FenceId{123}, EvidenceSourceId{1}, f.now, reason) ==
            Outcome::NotFound);
  BHG_CHECK(f.registry.report_effect(FenceId{123}, EvidenceSourceId{1}, true, f.now, reason) ==
            Outcome::NotFound);
  BHG_CHECK(f.registry.revoke(FenceId{123}, ReasonCode::FenceIntentRevoked, f.now) ==
            Outcome::NotFound);
  BHG_CHECK_EQ(f.registry.stats().unknown_id_refused, 3u);
}

BHG_TEST(authority, intent_encoding_roundtrip_and_validation) {
  FenceFixture f;
  FenceId id{};
  ReasonCode reason = ReasonCode::None;
  BHG_REQUIRE(is_affirmative(f.registry.issue(f.scope, f.gens, f.boot, f.inc,
                                              Provenance::Synthetic, ReasonCode::CompleteDeliveryFailure,
                                              f.now, id, reason)));
  const FenceRecord* rec = f.registry.find(id);
  BHG_REQUIRE(rec != nullptr);
  Writer w(512);
  encode(w, rec->intent);
  BHG_REQUIRE(w.ok());
  Reader r(w.span());
  FenceIntent out;
  BHG_REQUIRE(is_affirmative(decode(r, out)));
  BHG_REQUIRE(is_affirmative(r.finish()));
  BHG_CHECK(out.id == rec->intent.id);
  BHG_CHECK(out.scope == rec->intent.scope);
  BHG_CHECK(out.gens == rec->intent.gens);
  BHG_CHECK(out.expires_at == rec->intent.expires_at);

  for (std::size_t cut = 0; cut < w.size(); ++cut) {
    Reader t(w.span().subspan(0, cut));
    FenceIntent partial;
    const Outcome o = decode(t, partial);
    BHG_CHECK(!is_affirmative(o));
  }
}

BHG_TEST(authority, restoration_refused_without_fresh_positive_evidence) {
  FencePolicy policy = default_policy();
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now{WallNs{1000000}};

  Diagnosis none;
  none.scope = scope;
  none.gens = gens;
  none.outcome = Outcome::NoEvidence;
  none.classification = Classification::NoEvidence;
  RestorationDecision rd = evaluate_restoration(none, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::RestorationRefusedNoEvidence);

  Diagnosis stale;
  stale.scope = scope;
  stale.gens = gens;
  stale.outcome = Outcome::Stale;
  stale.classification = Classification::Unknown;
  rd = evaluate_restoration(stale, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::RestorationRefusedStale);

  Diagnosis conflict;
  conflict.scope = scope;
  conflict.gens = gens;
  conflict.outcome = Outcome::Conflict;
  conflict.classification = Classification::Unknown;
  rd = evaluate_restoration(conflict, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::RestorationRefusedConflict);

  Diagnosis blackhole;
  blackhole.scope = scope;
  blackhole.gens = gens;
  blackhole.outcome = Outcome::Ok;
  blackhole.classification = Classification::Blackhole;
  rd = evaluate_restoration(blackhole, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::RestorationRefusedInsufficient);
}

BHG_TEST(authority, restoration_requires_corroborated_success) {
  FencePolicy policy = default_policy();
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs now{WallNs{1000000}};

  Diagnosis one;
  one.scope = scope;
  one.gens = gens;
  one.outcome = Outcome::Ok;
  one.classification = Classification::Healthy;
  one.census.clean_successes = 1;
  one.census.success_sources = 1;
  one.census.success_incarnations = 1;
  one.census.success_attempts = 32;
  RestorationDecision rd = evaluate_restoration(one, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::SourcesInsufficient);

  Diagnosis two = one;
  two.census.success_sources = 2;
  two.census.success_incarnations = 2;
  two.success_sources.push_back(SourceRef{});
  rd = evaluate_restoration(two, policy, nullptr, now);
  BHG_CHECK(rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::RestorationCorroborated);

  Diagnosis few_attempts = two;
  few_attempts.census.success_attempts = 4;
  rd = evaluate_restoration(few_attempts, policy, nullptr, now);
  BHG_CHECK(!rd.authorized);
  BHG_CHECK(rd.reason == ReasonCode::AttemptsInsufficient);
}

BHG_TEST(authority, decision_identity_and_validation) {
  Decision d;
  d.kind = DecisionKind::Diagnosis;
  d.outcome = Outcome::Ok;
  d.scope = path_scope(PathId{1});
  d.gens = make_gens(1, 1, 1, 1);
  d.classification = Classification::Blackhole;
  d.primary_reason = ReasonCode::CompleteDeliveryFailure;
  d.boot = BootId{1};
  d.incarnation = IncarnationId{1};
  d.decided_at = WallNs{1000};
  d.policy = PolicyVersion{1};
  d.authority.stage = AuthorityStage::Authorization;
  d.authority.gens = d.gens;
  d.authority.boot = d.boot;
  d.authority.incarnation = d.incarnation;
  d.authority.policy = d.policy;
  d.id = derive_decision_id(d);
  BHG_CHECK(d.is_valid());
  Decision same = d;
  BHG_CHECK(derive_decision_id(same) == d.id);
  same.classification = Classification::Lossy;
  BHG_CHECK(derive_decision_id(same) != d.id);
  same = d;
  same.scope = path_scope(PathId{2});
  BHG_CHECK(derive_decision_id(same) != d.id);
  same = d;
  same.gens = make_gens(1, 1, 1, 2);
  BHG_CHECK(derive_decision_id(same) != d.id);

  Decision invalid = d;
  invalid.has_intent = true;
  BHG_CHECK(!invalid.is_valid());
}

BHG_TEST(authority, decision_encoding_roundtrip_and_total_decode) {
  Decision d;
  d.kind = DecisionKind::FenceIntent;
  d.outcome = Outcome::Ok;
  d.scope = path_scope(PathId{1});
  d.gens = make_gens(1, 2, 3, 4);
  d.classification = Classification::Blackhole;
  d.primary_reason = ReasonCode::CompleteDeliveryFailure;
  d.reasons.push_back(ReasonCode::CorroborationSatisfied);
  d.census.total = 3;
  d.census.fresh_current = 3;
  d.census.complete_failures = 3;
  d.census.failure_sources = 2;
  d.census.failure_incarnations = 2;
  d.census.failure_attempts = 96;
  d.boot = BootId{1};
  d.incarnation = IncarnationId{2};
  d.decided_at = WallNs{1000};
  d.policy = PolicyVersion{1};
  d.provenance = Provenance::Synthetic;

  FenceIntent intent;
  intent.id = FenceId{9};
  intent.decision = DecisionId{10};
  intent.scope = d.scope;
  intent.gens = d.gens;
  intent.boot = d.boot;
  intent.incarnation = d.incarnation;
  intent.policy = d.policy;
  intent.issued_at = WallNs{1000};
  intent.expires_at = WallNs{2000};
  intent.reason = ReasonCode::CompleteDeliveryFailure;
  d.intent = intent;
  d.has_intent = true;
  d.authority.stage = AuthorityStage::Intent;
  d.authority.fence = intent.id;
  d.authority.gens = d.gens;
  d.authority.boot = d.boot;
  d.authority.incarnation = d.incarnation;
  d.authority.policy = d.policy;
  d.authority.expires_at = intent.expires_at;
  d.id = derive_decision_id(d);
  d.authority.decision = d.id;

  Writer w(kDefaultMaxDocumentBytes);
  encode(w, d);
  BHG_REQUIRE(w.ok());
  Reader r(w.span());
  Decision out;
  BHG_REQUIRE(is_affirmative(decode(r, out)));
  BHG_REQUIRE(is_affirmative(r.finish()));
  BHG_CHECK(out.id == d.id);
  BHG_CHECK(out.classification == d.classification);
  BHG_CHECK(out.has_intent);
  BHG_CHECK(out.intent.id == intent.id);
  BHG_CHECK(out.census.failure_attempts == 96u);
  BHG_CHECK(out.reasons == d.reasons);

  // A single-bit flip anywhere in the decision encoding must be refused or change
  // the decoded content; it must never decode into an invalid decision.
  for (std::size_t i = 0; i < w.size(); i += 7) {
    Writer flipped(kDefaultMaxDocumentBytes);
    flipped.raw(w.span());
    std::vector<std::byte> bytes = flipped.take();
    bytes[i] ^= std::byte{0x01};
    Reader fr(std::span<const std::byte>(bytes.data(), bytes.size()));
    Decision survivor;
    const Outcome o = decode(fr, survivor);
    if (is_affirmative(o)) {
      BHG_CHECK(survivor.is_valid());
    }
  }
}
