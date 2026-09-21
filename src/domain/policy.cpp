#include "blackhole/domain/policy.hpp"

#include "blackhole/core/hash.hpp"
#include "blackhole/domain/evidence.hpp"

namespace bhg {

namespace {

Outcome refuse(const char* what, std::string& why) {
  why = what;
  return Outcome::Invalid;
}

}  // namespace

Outcome validate_policy(const FencePolicy& p, std::string& why) {
  why.clear();
  if (p.version.is_nil()) return refuse("policy version must be non-zero", why);
  if (!p.freshness.is_positive()) return refuse("freshness must be positive", why);
  if (p.max_clock_skew.ns < 0) return refuse("max_clock_skew must be non-negative", why);
  if (p.min_corroborating_sources < 2) {
    return refuse("min_corroborating_sources must be >= 2", why);
  }
  if (p.min_distinct_incarnations < 2) {
    return refuse("min_distinct_incarnations must be >= 2", why);
  }
  if (p.min_distinct_incarnations > p.min_corroborating_sources) {
    return refuse("min_distinct_incarnations cannot exceed min_corroborating_sources", why);
  }
  if (p.min_attempts_per_source == 0) return refuse("min_attempts_per_source must be > 0", why);
  if (p.min_total_attempts < p.min_attempts_per_source) {
    return refuse("min_total_attempts must be >= min_attempts_per_source", why);
  }
  if (p.complete_failure_ppm == 0 || p.complete_failure_ppm > kLossTotalPpm) {
    return refuse("complete_failure_ppm out of range", why);
  }
  if (p.severe_loss_ppm == 0 || p.severe_loss_ppm >= p.complete_failure_ppm) {
    return refuse("severe_loss_ppm must be > 0 and < complete_failure_ppm", why);
  }
  if (p.restore_min_sources < 1) return refuse("restore_min_sources must be >= 1", why);
  if (p.restore_min_incarnations < 1) {
    return refuse("restore_min_incarnations must be >= 1", why);
  }
  if (p.restore_min_successes == 0) return refuse("restore_min_successes must be > 0", why);
  if (p.restore_consecutive_clean == 0) {
    return refuse("restore_consecutive_clean must be > 0", why);
  }
  if (!p.fence_ttl.is_positive()) return refuse("fence_ttl must be positive", why);
  if (p.max_evidence_per_scope == 0) return refuse("max_evidence_per_scope must be > 0", why);
  if (p.max_tracked_scopes == 0) return refuse("max_tracked_scopes must be > 0", why);
  if (p.max_sources_per_scope == 0) return refuse("max_sources_per_scope must be > 0", why);
  if (p.max_fences == 0) return refuse("max_fences must be > 0", why);
  if (p.max_lineage_records == 0) return refuse("max_lineage_records must be > 0", why);
  if (p.max_reasons_per_decision == 0) {
    return refuse("max_reasons_per_decision must be > 0", why);
  }
  if (p.max_journal_records == 0) return refuse("max_journal_records must be > 0", why);
  if (p.max_journal_bytes == 0) return refuse("max_journal_bytes must be > 0", why);
  if (p.max_localization_elements == 0) {
    return refuse("max_localization_elements must be > 0", why);
  }
  if (p.max_localization_search_nodes == 0) {
    return refuse("max_localization_search_nodes must be > 0", why);
  }
  if (p.max_path_hops == 0) return refuse("max_path_hops must be > 0", why);
  if (p.max_path_hops > p.max_localization_elements) {
    return refuse("max_path_hops cannot exceed max_localization_elements", why);
  }
  if (p.max_localization_solutions == 0) {
    return refuse("max_localization_solutions must be > 0", why);
  }
  if (p.max_explanation_elements == 0) {
    return refuse("max_explanation_elements must be > 0", why);
  }
  if (p.max_sessions == 0) return refuse("max_sessions must be > 0", why);
  if (p.max_frame_bytes < 64) return refuse("max_frame_bytes must be >= 64", why);
  if (p.max_concurrent_connections == 0) {
    return refuse("max_concurrent_connections must be > 0", why);
  }
  if (p.log_capacity == 0) return refuse("log_capacity must be > 0", why);
  // Freshness must exceed twice the clock skew, otherwise a legitimately fresh
  // observation could be classified STALE purely from skew.
  const auto need = checked_mul<std::int64_t>(2, p.max_clock_skew.ns);
  if (!need.has_value() || p.freshness.ns <= *need) {
    return refuse("freshness must exceed twice max_clock_skew", why);
  }
  return Outcome::Ok;
}

std::uint64_t policy_fingerprint(const FencePolicy& p) {
  Writer w(4096);
  encode(w, p);
  if (!w.ok()) return 0;
  return fnv1a64(w.span());
}

FencePolicy default_policy() { return FencePolicy{}; }

void encode(Writer& w, const FencePolicy& p) noexcept {
  encode(w, p.version);
  encode(w, p.generation);
  encode(w, p.freshness);
  encode(w, p.max_clock_skew);
  w.u32(p.min_corroborating_sources);
  w.u32(p.min_distinct_incarnations);
  w.u32(p.min_attempts_per_source);
  w.u64(p.min_total_attempts);
  w.boolean(p.require_exact_quality);
  w.boolean(p.require_full_generation_agreement);
  w.u32(p.complete_failure_ppm);
  w.u32(p.severe_loss_ppm);
  w.u32(p.restore_min_sources);
  w.u32(p.restore_min_incarnations);
  w.u64(p.restore_min_successes);
  w.u32(p.restore_consecutive_clean);
  encode(w, p.fence_ttl);
  w.u32(p.max_evidence_per_scope);
  w.u32(p.max_tracked_scopes);
  w.u32(p.max_sources_per_scope);
  w.u32(p.max_fences);
  w.u32(p.max_lineage_records);
  w.u32(p.max_reasons_per_decision);
  w.u32(p.max_journal_records);
  w.u64(p.max_journal_bytes);
  w.u32(p.max_localization_elements);
  w.u32(p.max_localization_search_nodes);
  w.u32(p.max_path_hops);
  w.u32(p.max_localization_solutions);
  w.u32(p.max_explanation_elements);
  w.u32(p.max_sessions);
  w.u32(p.max_inflight_per_session);
  w.u32(p.max_frame_bytes);
  w.u32(p.max_concurrent_connections);
  w.u32(p.log_capacity);
}

Outcome decode(Reader& r, FencePolicy& p) noexcept {
  FencePolicy out{};
  Outcome o = decode(r, out.version);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.generation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.freshness);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.max_clock_skew);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.min_corroborating_sources);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.min_distinct_incarnations);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.min_attempts_per_source);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.min_total_attempts);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.require_exact_quality);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.require_full_generation_agreement);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.complete_failure_ppm);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.severe_loss_ppm);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.restore_min_sources);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.restore_min_incarnations);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.restore_min_successes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.restore_consecutive_clean);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.fence_ttl);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_evidence_per_scope);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_tracked_scopes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_sources_per_scope);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_fences);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_lineage_records);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_reasons_per_decision);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_journal_records);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.max_journal_bytes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_localization_elements);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_localization_search_nodes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_path_hops);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_localization_solutions);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_explanation_elements);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_sessions);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_inflight_per_session);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_frame_bytes);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.max_concurrent_connections);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.log_capacity);
  if (!is_affirmative(o)) return o;

  std::string why;
  const Outcome v = validate_policy(out, why);
  if (!is_affirmative(v)) return v;
  p = out;
  return Outcome::Ok;
}

void encode(Writer& w, const PathStructure& p, std::uint32_t max_hops) noexcept {
  encode(w, p.path);
  const std::size_t n = p.links.size();
  if (n > max_hops) {
    w.u32(0xFFFFFFFFu);
    return;
  }
  w.u32(static_cast<std::uint32_t>(n));
  for (const NodeId nid : p.nodes) encode(w, nid);
  for (const LinkId lid : p.links) encode(w, lid);
}

Outcome decode(Reader& r, PathStructure& p, std::uint32_t max_hops) noexcept {
  PathStructure out{};
  Outcome o = decode(r, out.path);
  if (!is_affirmative(o)) return o;
  std::uint32_t hops = 0;
  o = r.u32(hops);
  if (!is_affirmative(o)) return o;
  if (hops > max_hops) return Outcome::Oversized;
  const std::size_t node_count = static_cast<std::size_t>(hops) + 1u;
  // Refuse before materializing: node_count + hops must fit in what remains.
  const std::size_t needed = (node_count + static_cast<std::size_t>(hops)) * 8u;
  if (r.remaining() < needed) return Outcome::Malformed;
  out.nodes.reserve(node_count);
  out.links.reserve(hops);
  for (std::size_t i = 0; i < node_count; ++i) {
    NodeId n;
    o = decode(r, n);
    if (!is_affirmative(o)) return o;
    out.nodes.push_back(n);
  }
  for (std::size_t i = 0; i < hops; ++i) {
    LinkId l;
    o = decode(r, l);
    if (!is_affirmative(o)) return o;
    out.links.push_back(l);
  }
  if (!out.is_valid()) return Outcome::Invalid;
  p = std::move(out);
  return Outcome::Ok;
}

}  // namespace bhg
