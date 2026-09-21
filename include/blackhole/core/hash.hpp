#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace bhg {

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256u; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = ((c & 1u) != 0u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace detail

/// CRC-32C (Castagnoli), reflected, init/xorout 0xFFFFFFFF.
/// Used as an integrity check for durable records and wire frames. This is NOT a
/// cryptographic authenticator and is not claimed to be one.
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0u) noexcept;
std::uint32_t crc32c(std::string_view text, std::uint32_t seed = 0u) noexcept;

inline constexpr std::uint64_t kFnv1a64Offset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;

/// FNV-1a 64-bit. Deterministic fingerprint used to derive identity from canonical
/// bytes. Explicitly NOT a cryptographic hash and never used for authentication.
std::uint64_t fnv1a64(std::span<const std::byte> data, std::uint64_t seed = kFnv1a64Offset) noexcept;
std::uint64_t fnv1a64(std::string_view text) noexcept;

/// Deterministic mixing of two 64-bit values (SplitMix64 finalizer composition).
std::uint64_t mix64(std::uint64_t a, std::uint64_t b) noexcept;

}  // namespace bhg
