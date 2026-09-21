#pragma once

#include <compare>
#include <cstdint>

#include "blackhole/core/canonical.hpp"
#include "blackhole/core/ids.hpp"

namespace bhg {

/// What a decision or fence intent applies to. Every externally visible decision
/// identifies its exact subject; there is no implicit "the path" scope.
enum class ScopeKind : std::uint16_t {
  Node = 1,
  Link = 2,
  Path = 3,
  Segment = 4,  // contiguous hop range [begin_hop, end_hop] within path
};

constexpr bool is_valid_scope_kind(std::uint16_t raw) noexcept {
  return raw >= 1u && raw <= 4u;
}

constexpr const char* to_string(ScopeKind k) noexcept {
  switch (k) {
    case ScopeKind::Node: return "Node";
    case ScopeKind::Link: return "Link";
    case ScopeKind::Path: return "Path";
    case ScopeKind::Segment: return "Segment";
  }
  return "Invalid";
}

/// A path is a sequence of hops; hop i is the element between node i and node i+1.
/// Blackhole Guard never computes this structure -- it is supplied by the topology
/// owner as a typed reference. The runtime only consumes hop indices.
struct Scope {
  ScopeKind kind{ScopeKind::Path};
  PathId path{};
  NodeId node{};
  LinkId link{};
  std::uint32_t begin_hop{0};
  std::uint32_t end_hop{0};

  [[nodiscard]] constexpr bool is_valid() const noexcept {
    if (kind == ScopeKind::Path) return !path.is_nil();
    if (kind == ScopeKind::Node) return !path.is_nil() && !node.is_nil();
    if (kind == ScopeKind::Link) return !path.is_nil() && !link.is_nil();
    if (kind == ScopeKind::Segment) {
      return !path.is_nil() && end_hop > begin_hop;
    }
    return false;
  }

  /// Canonical ordering: kind, then path, then the kind-specific discriminator.
  friend constexpr bool operator==(const Scope& a, const Scope& b) noexcept {
    return a.kind == b.kind && a.path == b.path && a.node == b.node && a.link == b.link &&
           a.begin_hop == b.begin_hop && a.end_hop == b.end_hop;
  }
  friend constexpr std::strong_ordering operator<=>(const Scope& a, const Scope& b) noexcept {
    if (a.kind != b.kind) return a.kind <=> b.kind;
    if (a.path != b.path) return a.path <=> b.path;
    if (a.node != b.node) return a.node <=> b.node;
    if (a.link != b.link) return a.link <=> b.link;
    if (a.begin_hop != b.begin_hop) return a.begin_hop <=> b.begin_hop;
    return a.end_hop <=> b.end_hop;
  }
};

/// True when scopes overlap in subject; used to decide whether a fence intent covers
/// a decision's subject.
constexpr bool scopes_overlap(const Scope& a, const Scope& b) noexcept {
  if (a.path != b.path) return false;
  if (a.kind == ScopeKind::Path || b.kind == ScopeKind::Path) return true;
  if (a.kind == ScopeKind::Node) return b.kind == ScopeKind::Node && a.node == b.node;
  if (a.kind == ScopeKind::Link) return b.kind == ScopeKind::Link && a.link == b.link;
  // Segment/Segment, Segment/Node, Segment/Link: hop ranges intersect.
  if (a.kind == ScopeKind::Segment && b.kind == ScopeKind::Segment) {
    return a.begin_hop < b.end_hop && b.begin_hop < a.end_hop;
  }
  return false;
}

inline void encode(Writer& w, ScopeKind k) noexcept { w.u16(static_cast<std::uint16_t>(k)); }

inline Outcome decode(Reader& r, ScopeKind& k) noexcept {
  std::uint16_t raw = 0;
  Outcome o = r.u16(raw);
  if (!is_affirmative(o)) return o;
  if (!is_valid_scope_kind(raw)) return Outcome::Invalid;
  k = static_cast<ScopeKind>(raw);
  return Outcome::Ok;
}

inline void encode(Writer& w, const Scope& s) noexcept {
  encode(w, s.kind);
  encode(w, s.path);
  encode(w, s.node);
  encode(w, s.link);
  w.u32(s.begin_hop);
  w.u32(s.end_hop);
}

inline Outcome decode(Reader& r, Scope& s) noexcept {
  Outcome o = decode(r, s.kind);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.path);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.node);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.link);
  if (!is_affirmative(o)) return o;
  o = r.u32(s.begin_hop);
  if (!is_affirmative(o)) return o;
  return r.u32(s.end_hop);
}

}  // namespace bhg
