#pragma once

#include <cstdint>

namespace bhg {

/// Delivery classification. This is the *evidence-derived* statement about a path;
/// it is deliberately separate from authority (what may be fenced).
///
/// The distinction that defines the product: path legality is not delivery proof,
/// and ambiguity is never promoted to Blackhole.
enum class Classification : std::uint16_t {
  /// Fresh, generation-matched evidence shows successful delivery.
  Healthy = 0,
  /// No observation of any kind exists for this subject under this generation.
  NoEvidence = 1,
  /// Observations exist but cannot support a definitive classification:
  /// stale-only, generation-mismatched, contradictory, below quality, or
  /// otherwise indeterminate.
  Unknown = 2,
  /// Delivery succeeds but with measurable loss below complete failure.
  Lossy = 3,
  /// Complete or severe loss that is explained by congestion state reported by the
  /// congestion owner. Traffic may be dropped, but this is not an unexplained
  /// blackhole and must not be fenced as one.
  Congested = 4,
  /// Loss explained by a partition/severance reported by the topology or
  /// link-state owner.
  Partitioned = 5,
  /// Complete delivery failure, generation-matched and fresh, with no congestion or
  /// partition explanation and no contradicting success.
  Blackhole = 6,
};

constexpr bool is_valid_classification(std::uint16_t raw) noexcept { return raw <= 6u; }

constexpr const char* to_string(Classification c) noexcept {
  switch (c) {
    case Classification::Healthy: return "HEALTHY";
    case Classification::NoEvidence: return "NO_EVIDENCE";
    case Classification::Unknown: return "UNKNOWN";
    case Classification::Lossy: return "LOSSY";
    case Classification::Congested: return "CONGESTED";
    case Classification::Partitioned: return "PARTITIONED";
    case Classification::Blackhole: return "BLACKHOLE";
  }
  return "UNKNOWN";
}

/// Only an unexplained complete delivery failure may ever be fenced.
constexpr bool may_authorize_fence(Classification c) noexcept {
  return c == Classification::Blackhole;
}

}  // namespace bhg
