#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

#include "blackhole/core/canonical.hpp"

namespace bhg {

/// Strongly typed identifier. Distinct tags make NodeId / LinkId / PathId /
/// FenceId ... mutually incompatible at compile time: no interchangeable strings or
/// bare integers cross the public API.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == Rep{0}; }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(StrongId a, StrongId b) noexcept {
    if (a.value_ < b.value_) return std::strong_ordering::less;
    if (a.value_ > b.value_) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

 private:
  Rep value_{0};
};

/// Monotonic generation counter for an authority-bearing dependency. A generation is
/// deliberately a different type from an identifier: matching identity is not
/// matching generation.
template <class Tag, class Rep = std::uint64_t>
class Generation {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr Generation() noexcept = default;
  constexpr explicit Generation(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == Rep{0}; }

  [[nodiscard]] constexpr Generation next() const noexcept { return Generation(value_ + 1); }

  friend constexpr bool operator==(Generation a, Generation b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(Generation a, Generation b) noexcept {
    if (a.value_ < b.value_) return std::strong_ordering::less;
    if (a.value_ > b.value_) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

 private:
  Rep value_{0};
};

/// Monotonic per-authority sequence number (per evidence source, per session, ...).
template <class Tag, class Rep = std::uint64_t>
class Sequence {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr Sequence() noexcept = default;
  constexpr explicit Sequence(Rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == Rep{0}; }

  friend constexpr bool operator==(Sequence a, Sequence b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr std::strong_ordering operator<=>(Sequence a, Sequence b) noexcept {
    if (a.value_ < b.value_) return std::strong_ordering::less;
    if (a.value_ > b.value_) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }

 private:
  Rep value_{0};
};

// ---- identity tags -------------------------------------------------------------

struct NodeIdTag;
struct LinkIdTag;
struct PathIdTag;
struct SegmentIdTag;
struct EvidenceIdTag;
struct EvidenceSourceIdTag;
struct AttemptIdTag;
struct DecisionIdTag;
struct FenceIdTag;
struct SessionIdTag;
struct RequestIdTag;
struct BootIdTag;
struct IncarnationIdTag;
struct PolicyVersionTag;
struct LabelIdTag;

struct PathGenerationTag;
struct TopologyGenerationTag;
struct LinkStateGenerationTag;
struct CoordinatorEpochTag;
struct SourceEpochTag;
struct PolicyGenerationTag;

struct EvidenceSeqTag;
struct SessionSeqTag;
struct AttemptSeqTag;
struct RecordSeqTag;

using NodeId = StrongId<NodeIdTag>;
using LinkId = StrongId<LinkIdTag>;
using PathId = StrongId<PathIdTag>;
using SegmentId = StrongId<SegmentIdTag>;
using EvidenceId = StrongId<EvidenceIdTag>;
using EvidenceSourceId = StrongId<EvidenceSourceIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using DecisionId = StrongId<DecisionIdTag>;
using FenceId = StrongId<FenceIdTag>;
using SessionId = StrongId<SessionIdTag>;
using RequestId = StrongId<RequestIdTag>;
using BootId = StrongId<BootIdTag>;
using IncarnationId = StrongId<IncarnationIdTag>;
using PolicyVersion = StrongId<PolicyVersionTag>;
using LabelId = StrongId<LabelIdTag>;

using PathGeneration = Generation<PathGenerationTag, std::uint64_t>;
using TopologyGeneration = Generation<TopologyGenerationTag, std::uint64_t>;
using LinkStateGeneration = Generation<LinkStateGenerationTag, std::uint64_t>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag, std::uint64_t>;
using SourceEpoch = Generation<SourceEpochTag, std::uint64_t>;
using PolicyGeneration = Generation<PolicyGenerationTag, std::uint64_t>;

using EvidenceSequence = Sequence<EvidenceSeqTag, std::uint64_t>;
using SessionSequence = Sequence<SessionSeqTag, std::uint64_t>;
using AttemptSequence = Sequence<AttemptSeqTag, std::uint64_t>;
using RecordSequence = Sequence<RecordSeqTag, std::uint64_t>;

// ---- canonical codec hooks -----------------------------------------------------

template <class Tag, class Rep>
inline void encode(Writer& w, const StrongId<Tag, Rep>& v) noexcept {
  w.u64(static_cast<std::uint64_t>(v.value()));
}

template <class Tag, class Rep>
inline Outcome decode(Reader& r, StrongId<Tag, Rep>& v) noexcept {
  std::uint64_t raw = 0;
  Outcome o = r.u64(raw);
  if (!is_affirmative(o)) return o;
  v = StrongId<Tag, Rep>(static_cast<Rep>(raw));
  return Outcome::Ok;
}

template <class Tag, class Rep>
inline void encode(Writer& w, const Generation<Tag, Rep>& v) noexcept {
  w.u64(static_cast<std::uint64_t>(v.value()));
}

template <class Tag, class Rep>
inline Outcome decode(Reader& r, Generation<Tag, Rep>& v) noexcept {
  std::uint64_t raw = 0;
  Outcome o = r.u64(raw);
  if (!is_affirmative(o)) return o;
  v = Generation<Tag, Rep>(static_cast<Rep>(raw));
  return Outcome::Ok;
}

template <class Tag, class Rep>
inline void encode(Writer& w, const Sequence<Tag, Rep>& v) noexcept {
  w.u64(static_cast<std::uint64_t>(v.value()));
}

template <class Tag, class Rep>
inline Outcome decode(Reader& r, Sequence<Tag, Rep>& v) noexcept {
  std::uint64_t raw = 0;
  Outcome o = r.u64(raw);
  if (!is_affirmative(o)) return o;
  v = Sequence<Tag, Rep>(static_cast<Rep>(raw));
  return Outcome::Ok;
}

/// Human-readable rendering used by tools/logs. Never parsed back into authority.
std::string to_text(NodeId v);
std::string to_text(LinkId v);
std::string to_text(PathId v);
std::string to_text(EvidenceSourceId v);
std::string to_text(FenceId v);
std::string to_text(DecisionId v);
std::string to_text(SessionId v);
std::string to_text(BootId v);
std::string to_text(IncarnationId v);
std::string to_text(PathGeneration v);
std::string to_text(TopologyGeneration v);
std::string to_text(LinkStateGeneration v);
std::string to_text(CoordinatorEpoch v);
std::string to_text(PolicyVersion v);

}  // namespace bhg

namespace std {

template <class Tag, class Rep>
struct hash<bhg::StrongId<Tag, Rep>> {
  std::size_t operator()(const bhg::StrongId<Tag, Rep>& v) const noexcept {
    return static_cast<std::size_t>(v.value() * 0x9E3779B97F4A7C15ULL);
  }
};

}  // namespace std
