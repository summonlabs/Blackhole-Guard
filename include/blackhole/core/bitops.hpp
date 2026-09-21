#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace bhg {

/// Portable population count. Used by the localization solver and by accounting
/// checks; kept dependency-free.
inline std::uint32_t popcount64(std::uint64_t v) noexcept {
#if defined(_MSC_VER)
  return static_cast<std::uint32_t>(__popcnt64(v));
#else
  return static_cast<std::uint32_t>(__builtin_popcountll(v));
#endif
}

/// Index of the lowest set bit; 64 when v == 0.
inline std::uint32_t lowest_set_bit64(std::uint64_t v) noexcept {
  if (v == 0) return 64u;
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward64(&index, v);
  return static_cast<std::uint32_t>(index);
#else
  return static_cast<std::uint32_t>(__builtin_ctzll(v));
#endif
}

}  // namespace bhg
