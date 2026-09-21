#include "blackhole/core/time.hpp"

#include <chrono>

namespace bhg {

Clock::~Clock() = default;

WallNs SystemClock::wall_now() const noexcept {
  using namespace std::chrono;
  const auto now = system_clock::now().time_since_epoch();
  return WallNs{duration_cast<nanoseconds>(now).count()};
}

MonoNs SystemClock::mono_now() const noexcept {
  using namespace std::chrono;
  // steady_clock is monotonic per process boot on every supported platform; the
  // value is meaningless across processes and is never persisted as authority.
  const auto now = steady_clock::now().time_since_epoch();
  return MonoNs{duration_cast<nanoseconds>(now).count()};
}

ManualClock::ManualClock(std::int64_t wall_start_ns) noexcept
    : wall_(wall_start_ns), mono_(0) {}

WallNs ManualClock::wall_now() const noexcept { return WallNs{wall_}; }

MonoNs ManualClock::mono_now() const noexcept { return MonoNs{mono_}; }

void ManualClock::advance(DurationNs d) noexcept {
  wall_ += d.ns;
  mono_ += d.ns;
}

void ManualClock::set_wall(WallNs w) noexcept { wall_ = w.ns; }

}  // namespace bhg
