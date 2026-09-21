#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "blackhole/core/checked.hpp"
#include "blackhole/core/outcome.hpp"

namespace bhg {

/// Canonical little-endian byte encoding.
///
/// Determinism contract:
///   * fixed-width little-endian primitives only (no native layout, no padding);
///   * length-prefixed byte strings and text with an explicit maximum;
///   * identical logical values always encode to identical bytes;
///   * decoding is total: every input either decodes or returns a non-Ok Outcome,
///     never throws, never reads out of bounds, never over-allocates.
///
/// The encoding is a canonical form, NOT a cryptographic envelope: it provides no
/// authenticity or confidentiality.

inline constexpr std::size_t kDefaultMaxDocumentBytes = 1u << 20;   // 1 MiB
inline constexpr std::size_t kDefaultMaxStringBytes = 256;
inline constexpr std::size_t kDefaultMaxBlobBytes = 1u << 16;       // 64 KiB

class Writer {
 public:
  explicit Writer(std::size_t max_bytes = kDefaultMaxDocumentBytes) noexcept
      : max_(max_bytes) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] Outcome status() const noexcept {
    return ok_ ? Outcome::Ok : Outcome::Exhausted;
  }
  [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
  [[nodiscard]] std::size_t capacity_limit() const noexcept { return max_; }

  void u8(std::uint8_t v) noexcept;
  void u16(std::uint16_t v) noexcept;
  void u32(std::uint32_t v) noexcept;
  void u64(std::uint64_t v) noexcept;
  void i64(std::int64_t v) noexcept;
  void boolean(bool v) noexcept;
  void bytes(std::span<const std::byte> b) noexcept;
  void blob(std::span<const std::byte> b, std::size_t max_len) noexcept;
  void string(std::string_view s, std::size_t max_len) noexcept;
  /// Unprefixed bytes; caller is responsible for length framing.
  void raw(std::span<const std::byte> b) noexcept;

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buf_; }
  [[nodiscard]] std::span<const std::byte> span() const noexcept {
    return std::span<const std::byte>(buf_.data(), buf_.size());
  }
  [[nodiscard]] std::string_view text() const noexcept {
    return std::string_view(reinterpret_cast<const char*>(buf_.data()), buf_.size());
  }
  std::vector<std::byte> take() noexcept { return std::move(buf_); }

 private:
  void put(std::span<const std::byte> b) noexcept;
  void put_byte(std::byte b) noexcept;

  std::vector<std::byte> buf_;
  std::size_t max_{kDefaultMaxDocumentBytes};
  bool ok_{true};
};

/// Sticky-failure reader. The first failure latches and is returned by every
/// subsequent accessor, so partially-consumed input cannot be mistaken for a
/// successful decode.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) noexcept : d_(data) {}

  [[nodiscard]] Outcome status() const noexcept { return err_; }
  [[nodiscard]] bool ok() const noexcept { return err_ == Outcome::Ok; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return d_.size() > p_ ? d_.size() - p_ : 0u;
  }
  [[nodiscard]] std::size_t position() const noexcept { return p_; }

  Outcome u8(std::uint8_t& v) noexcept;
  Outcome u16(std::uint16_t& v) noexcept;
  Outcome u32(std::uint32_t& v) noexcept;
  Outcome u64(std::uint64_t& v) noexcept;
  Outcome i64(std::int64_t& v) noexcept;
  Outcome boolean(bool& v) noexcept;
  Outcome blob(std::size_t n, std::span<const std::byte>& out) noexcept;
  Outcome string(std::string& out, std::size_t max_len) noexcept;
  Outcome raw(std::size_t n, std::span<const std::byte>& out) noexcept;
  Outcome skip(std::size_t n) noexcept;

  /// Succeeds only when every byte was consumed. Trailing bytes are a hard error
  /// because they usually mean a version or layout mismatch.
  Outcome finish() noexcept;

 private:
  Outcome need(std::size_t n) noexcept;
  Outcome fail(Outcome o) noexcept {
    if (err_ == Outcome::Ok) err_ = o;
    return err_;
  }

  std::span<const std::byte> d_;
  std::size_t p_{0};
  Outcome err_{Outcome::Ok};
};

// ---- primitive helpers ---------------------------------------------------------

inline void encode(Writer& w, std::uint8_t v) noexcept { w.u8(v); }
inline void encode(Writer& w, std::uint16_t v) noexcept { w.u16(v); }
inline void encode(Writer& w, std::uint32_t v) noexcept { w.u32(v); }
inline void encode(Writer& w, std::uint64_t v) noexcept { w.u64(v); }
inline void encode(Writer& w, std::int64_t v) noexcept { w.i64(v); }
inline void encode(Writer& w, bool v) noexcept { w.boolean(v); }

inline Outcome decode(Reader& r, std::uint8_t& v) noexcept { return r.u8(v); }
inline Outcome decode(Reader& r, std::uint16_t& v) noexcept { return r.u16(v); }
inline Outcome decode(Reader& r, std::uint32_t& v) noexcept { return r.u32(v); }
inline Outcome decode(Reader& r, std::uint64_t& v) noexcept { return r.u64(v); }
inline Outcome decode(Reader& r, std::int64_t& v) noexcept { return r.i64(v); }
inline Outcome decode(Reader& r, bool& v) noexcept { return r.boolean(v); }

/// Enum codec that validates the raw value against a caller-supplied predicate so
/// that invalid enums are rejected rather than silently reinterpreted.
template <class E, class Valid>
inline void encode_enum(Writer& w, E v, Valid&& /*valid*/) noexcept {
  w.u16(static_cast<std::uint16_t>(v));
}

template <class E, class Valid>
inline Outcome decode_enum(Reader& r, E& v, Valid&& valid) noexcept {
  std::uint16_t raw = 0;
  Outcome o = r.u16(raw);
  if (!is_affirmative(o)) return o;
  if (!valid(raw)) {
    v = static_cast<E>(0);
    return Outcome::Invalid;
  }
  v = static_cast<E>(raw);
  return Outcome::Ok;
}

// ---- std::string / std::vector<std::byte> --------------------------------------

inline void encode_text(Writer& w, std::string_view s, std::size_t max_len) noexcept {
  w.string(s, max_len);
}

inline Outcome decode_text(Reader& r, std::string& s, std::size_t max_len) noexcept {
  return r.string(s, max_len);
}

}  // namespace bhg
