#pragma once

#include <compare>
#include <cstdint>
#include <memory>

#include "blackhole/core/canonical.hpp"

namespace bhg {

/// Wall-clock instant in nanoseconds since the Unix epoch. Used only where
/// cross-process comparability is required (evidence freshness windows). It is
/// explicitly not monotonic and not trusted to be precise across hosts.
struct WallNs {
  std::int64_t ns{0};
  friend constexpr bool operator==(WallNs a, WallNs b) noexcept { return a.ns == b.ns; }
  friend constexpr std::strong_ordering operator<=>(WallNs a, WallNs b) noexcept {
    if (a.ns < b.ns) return std::strong_ordering::less;
    if (a.ns > b.ns) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return ns == 0; }
};

/// Monotonic instant local to this process boot. Never compared across processes.
struct MonoNs {
  std::int64_t ns{0};
  friend constexpr bool operator==(MonoNs a, MonoNs b) noexcept { return a.ns == b.ns; }
  friend constexpr std::strong_ordering operator<=>(MonoNs a, MonoNs b) noexcept {
    if (a.ns < b.ns) return std::strong_ordering::less;
    if (a.ns > b.ns) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }
};

struct DurationNs {
  std::int64_t ns{0};
  friend constexpr bool operator==(DurationNs a, DurationNs b) noexcept { return a.ns == b.ns; }
  friend constexpr std::strong_ordering operator<=>(DurationNs a, DurationNs b) noexcept {
    if (a.ns < b.ns) return std::strong_ordering::less;
    if (a.ns > b.ns) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
  }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return ns > 0; }
};

inline constexpr std::int64_t kNsPerSecond = 1000000000LL;
inline constexpr std::int64_t kNsPerMilli = 1000000LL;

constexpr DurationNs seconds(std::int64_t n) noexcept { return DurationNs{n * kNsPerSecond}; }
constexpr DurationNs millis(std::int64_t n) noexcept { return DurationNs{n * kNsPerMilli}; }

/// Half-open freshness window [open, close). Evidence is fresh at instant t iff
/// open <= t < close. A zero-length window is never fresh.
struct FreshnessWindow {
  WallNs open{};
  WallNs close{};

  [[nodiscard]] constexpr bool is_fresh_at(WallNs t) const noexcept {
    return open.ns < close.ns && open.ns <= t.ns && t.ns < close.ns;
  }
  [[nodiscard]] constexpr bool is_degenerate() const noexcept { return open.ns >= close.ns; }
  friend constexpr bool operator==(const FreshnessWindow&, const FreshnessWindow&) = default;
  friend constexpr std::strong_ordering operator<=>(const FreshnessWindow& a,
                                                    const FreshnessWindow& b) noexcept {
    if (a.open != b.open) return a.open <=> b.open;
    return a.close <=> b.close;
  }
};

inline void encode(Writer& w, WallNs v) noexcept { w.i64(v.ns); }
inline Outcome decode(Reader& r, WallNs& v) noexcept { return r.i64(v.ns); }
inline void encode(Writer& w, MonoNs v) noexcept { w.i64(v.ns); }
inline Outcome decode(Reader& r, MonoNs& v) noexcept { return r.i64(v.ns); }
inline void encode(Writer& w, DurationNs v) noexcept { w.i64(v.ns); }
inline Outcome decode(Reader& r, DurationNs& v) noexcept { return r.i64(v.ns); }
inline void encode(Writer& w, const FreshnessWindow& v) noexcept {
  encode(w, v.open);
  encode(w, v.close);
}
inline Outcome decode(Reader& r, FreshnessWindow& v) noexcept {
  Outcome o = decode(r, v.open);
  if (!is_affirmative(o)) return o;
  return decode(r, v.close);
}

/// Clock abstraction. Deterministic replay and tests inject ManualClock; production
/// uses SystemClock.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  [[nodiscard]] virtual WallNs wall_now() const noexcept = 0;
  [[nodiscard]] virtual MonoNs mono_now() const noexcept = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] WallNs wall_now() const noexcept override;
  [[nodiscard]] MonoNs mono_now() const noexcept override;
};

/// Deterministic manual clock used by tests, replay and benchmarks.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t wall_start_ns = 1700000000000000000LL) noexcept;

  [[nodiscard]] WallNs wall_now() const noexcept override;
  [[nodiscard]] MonoNs mono_now() const noexcept override;

  /// Advance both wall and monotonic time.
  void advance(DurationNs d) noexcept;
  /// Set wall time independently (models clock skew between hosts).
  void set_wall(WallNs w) noexcept;

 private:
  std::int64_t wall_{0};
  std::int64_t mono_{0};
};

}  // namespace bhg
