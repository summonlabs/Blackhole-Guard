#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace bhg {

/// Checked arithmetic. Every size/length/offset computation in this runtime that is
/// influenced by external input goes through these helpers; wraparound is a defect.

template <class T>
constexpr std::optional<T> checked_add(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) return std::nullopt;
    return static_cast<T>(a + b);
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) return std::nullopt;
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) return std::nullopt;
    return static_cast<T>(a + b);
  }
}

template <class T>
constexpr std::optional<T> checked_sub(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if constexpr (std::is_unsigned_v<T>) {
    if (a < b) return std::nullopt;
    return static_cast<T>(a - b);
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) return std::nullopt;
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) return std::nullopt;
    return static_cast<T>(a - b);
  }
}

template <class T>
constexpr std::optional<T> checked_mul(T a, T b) noexcept {
  static_assert(std::is_integral_v<T>, "integral required");
  if (a == 0 || b == 0) return static_cast<T>(0);
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() / b)) return std::nullopt;
    return static_cast<T>(a * b);
  } else {
    constexpr T lo = std::numeric_limits<T>::min();
    constexpr T hi = std::numeric_limits<T>::max();
    if (a > 0) {
      if (b > 0) {
        if (a > hi / b) return std::nullopt;
      } else {
        if (b < lo / a) return std::nullopt;
      }
    } else {
      if (b > 0) {
        if (a < lo / b) return std::nullopt;
      } else {
        if (a != 0 && b < hi / a) return std::nullopt;
      }
    }
    return static_cast<T>(a * b);
  }
}

template <class To, class From>
constexpr std::optional<To> checked_cast(From v) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "integral required");
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To> &&
                sizeof(To) >= sizeof(From)) {
    return static_cast<To>(v);
  } else if constexpr (std::is_signed_v<From> && std::is_unsigned_v<To>) {
    if (v < 0) return std::nullopt;
    using UFrom = std::make_unsigned_t<From>;
    if (static_cast<UFrom>(v) > static_cast<UFrom>(std::numeric_limits<To>::max())) {
      return std::nullopt;
    }
    return static_cast<To>(v);
  } else if constexpr (std::is_unsigned_v<From> && std::is_signed_v<To>) {
    if (v > static_cast<From>(std::numeric_limits<To>::max())) return std::nullopt;
    return static_cast<To>(v);
  } else {
    if (v > static_cast<From>(std::numeric_limits<To>::max())) return std::nullopt;
    if (v < static_cast<From>(std::numeric_limits<To>::min())) return std::nullopt;
    return static_cast<To>(v);
  }
}

/// Saturating helper used only where a saturated counter is explicitly documented.
template <class T>
constexpr T saturating_add(T a, T b) noexcept {
  const auto r = checked_add(a, b);
  return r.has_value() ? *r : std::numeric_limits<T>::max();
}

}  // namespace bhg
