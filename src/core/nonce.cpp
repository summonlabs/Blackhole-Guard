#include "blackhole/core/nonce.hpp"

#include "blackhole/core/hash.hpp"
#include "blackhole/core/time.hpp"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bhg {

NonceSource::~NonceSource() = default;

std::uint64_t current_process_id() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

SystemNonceSource::SystemNonceSource() noexcept {
  const SystemClock clock;
  std::uint64_t entropy = static_cast<std::uint64_t>(clock.wall_now().ns);
  entropy = mix64(entropy, current_process_id());
  const volatile std::uintptr_t stack_marker = reinterpret_cast<std::uintptr_t>(&entropy);
  seed_ = mix64(entropy, static_cast<std::uint64_t>(stack_marker));
}

std::uint64_t SystemNonceSource::next() noexcept {
  ++counter_;
  seed_ = mix64(seed_, counter_);
  return seed_;
}

std::uint64_t SeededNonceSource::next() noexcept {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

}  // namespace bhg
