#pragma once

#include <compare>
#include <cstdint>

#include "blackhole/core/canonical.hpp"
#include "blackhole/core/ids.hpp"

namespace bhg {

/// The complete set of authority-bearing dependency generations that a delivery
/// claim is bound to. A claim is only ever valid for the exact vector it was
/// produced under.
///
///   path       -- the path definition generation owned by the path/route owner
///   topology   -- the topology generation owned by the topology owner
///   link_state -- the link-state generation owned by the link-state owner
///   epoch      -- this coordinator's term; advances on every process open
///
/// Matching identifiers is NOT matching generations: two claims about the same path
/// id with different vectors are different claims.
struct GenerationVector {
  PathGeneration path{};
  TopologyGeneration topology{};
  LinkStateGeneration link_state{};
  CoordinatorEpoch epoch{};

  friend constexpr bool operator==(const GenerationVector& a, const GenerationVector& b) noexcept {
    return a.path == b.path && a.topology == b.topology && a.link_state == b.link_state &&
           a.epoch == b.epoch;
  }
  friend constexpr std::strong_ordering operator<=>(const GenerationVector& a,
                                                    const GenerationVector& b) noexcept {
    if (a.path != b.path) return a.path <=> b.path;
    if (a.topology != b.topology) return a.topology <=> b.topology;
    if (a.link_state != b.link_state) return a.link_state <=> b.link_state;
    return a.epoch <=> b.epoch;
  }
  [[nodiscard]] constexpr bool is_complete() const noexcept {
    return !path.is_nil() && !topology.is_nil() && !link_state.is_nil() && !epoch.is_nil();
  }
};

/// Which component(s) of the vector disagree. Reported explicitly so diagnostics can
/// say exactly what authority moved.
enum class GenMismatch : std::uint8_t {
  None = 0,
  Path = 1,
  Topology = 2,
  LinkState = 3,
  Epoch = 4,
  Multiple = 5,
};

constexpr bool is_valid_gen_mismatch(std::uint8_t raw) noexcept { return raw <= 5u; }

constexpr const char* to_string(GenMismatch m) noexcept {
  switch (m) {
    case GenMismatch::None: return "None";
    case GenMismatch::Path: return "Path";
    case GenMismatch::Topology: return "Topology";
    case GenMismatch::LinkState: return "LinkState";
    case GenMismatch::Epoch: return "Epoch";
    case GenMismatch::Multiple: return "Multiple";
  }
  return "Multiple";
}

/// Total, deterministic classification of how two vectors differ.
constexpr GenMismatch classify_mismatch(const GenerationVector& a,
                                        const GenerationVector& b) noexcept {
  int n = 0;
  if (a.path != b.path) ++n;
  if (a.topology != b.topology) ++n;
  if (a.link_state != b.link_state) ++n;
  if (a.epoch != b.epoch) ++n;
  if (n == 0) return GenMismatch::None;
  if (n > 1) return GenMismatch::Multiple;
  if (a.path != b.path) return GenMismatch::Path;
  if (a.topology != b.topology) return GenMismatch::Topology;
  if (a.link_state != b.link_state) return GenMismatch::LinkState;
  return GenMismatch::Epoch;
}

/// Identity of an evidence source *instance*. The source id alone is not enough:
/// a restarted source is a different authority even under the same id.
struct SourceRef {
  EvidenceSourceId source{};
  BootId boot{};
  IncarnationId incarnation{};
  SourceEpoch epoch{};

  friend constexpr bool operator==(const SourceRef& a, const SourceRef& b) noexcept {
    return a.source == b.source && a.boot == b.boot && a.incarnation == b.incarnation &&
           a.epoch == b.epoch;
  }
  friend constexpr std::strong_ordering operator<=>(const SourceRef& a,
                                                    const SourceRef& b) noexcept {
    if (a.source != b.source) return a.source <=> b.source;
    if (a.boot != b.boot) return a.boot <=> b.boot;
    if (a.incarnation != b.incarnation) return a.incarnation <=> b.incarnation;
    return a.epoch <=> b.epoch;
  }
  [[nodiscard]] constexpr bool is_valid() const noexcept {
    return !source.is_nil() && !boot.is_nil() && !incarnation.is_nil();
  }
};

inline void encode(Writer& w, const GenerationVector& v) noexcept {
  encode(w, v.path);
  encode(w, v.topology);
  encode(w, v.link_state);
  encode(w, v.epoch);
}

inline Outcome decode(Reader& r, GenerationVector& v) noexcept {
  Outcome o = decode(r, v.path);
  if (!is_affirmative(o)) return o;
  o = decode(r, v.topology);
  if (!is_affirmative(o)) return o;
  o = decode(r, v.link_state);
  if (!is_affirmative(o)) return o;
  return decode(r, v.epoch);
}

inline void encode(Writer& w, const SourceRef& v) noexcept {
  encode(w, v.source);
  encode(w, v.boot);
  encode(w, v.incarnation);
  encode(w, v.epoch);
}

inline Outcome decode(Reader& r, SourceRef& v) noexcept {
  Outcome o = decode(r, v.source);
  if (!is_affirmative(o)) return o;
  o = decode(r, v.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, v.incarnation);
  if (!is_affirmative(o)) return o;
  return decode(r, v.epoch);
}

}  // namespace bhg
