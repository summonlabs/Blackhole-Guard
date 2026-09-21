#include "blackhole/core/hash.hpp"

namespace bhg {

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFu;
  for (const std::byte b : data) {
    const std::uint8_t idx = static_cast<std::uint8_t>(
        (crc ^ static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b))) & 0xFFu);
    crc = detail::kCrc32cTable[idx] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::string_view text, std::uint32_t seed) noexcept {
  return crc32c(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(text.data()), text.size()),
                seed);
}

std::uint64_t fnv1a64(std::span<const std::byte> data, std::uint64_t seed) noexcept {
  std::uint64_t h = seed;
  for (const std::byte b : data) {
    h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
    h *= kFnv1a64Prime;
  }
  return h;
}

std::uint64_t fnv1a64(std::string_view text) noexcept {
  return fnv1a64(std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::uint64_t mix64(std::uint64_t a, std::uint64_t b) noexcept {
  std::uint64_t z = a + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= b;
  z = (z ^ (z >> 31)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 29)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 32);
}

}  // namespace bhg
