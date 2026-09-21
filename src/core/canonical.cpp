#include "blackhole/core/canonical.hpp"

namespace bhg {

void Writer::put_byte(std::byte b) noexcept {
  if (!ok_) return;
  if (buf_.size() >= max_) {
    ok_ = false;
    return;
  }
  buf_.push_back(b);
}

void Writer::put(std::span<const std::byte> b) noexcept {
  if (!ok_) return;
  const auto total = checked_add<std::size_t>(buf_.size(), b.size());
  if (!total.has_value() || *total > max_) {
    ok_ = false;
    return;
  }
  buf_.insert(buf_.end(), b.begin(), b.end());
}

void Writer::u8(std::uint8_t v) noexcept { put_byte(static_cast<std::byte>(v)); }

void Writer::u16(std::uint16_t v) noexcept {
  put_byte(static_cast<std::byte>(v & 0xFFu));
  put_byte(static_cast<std::byte>((v >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) {
    put_byte(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
  }
}

void Writer::u64(std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    put_byte(static_cast<std::byte>((v >> (8 * i)) & 0xFFu));
  }
}

void Writer::i64(std::int64_t v) noexcept { u64(static_cast<std::uint64_t>(v)); }

void Writer::boolean(bool v) noexcept { u8(v ? std::uint8_t{1} : std::uint8_t{0}); }

void Writer::bytes(std::span<const std::byte> b) noexcept {
  const auto n = checked_cast<std::uint32_t>(b.size());
  if (!n.has_value()) {
    ok_ = false;
    return;
  }
  u32(*n);
  put(b);
}

void Writer::blob(std::span<const std::byte> b, std::size_t max_len) noexcept {
  if (b.size() > max_len) {
    ok_ = false;
    return;
  }
  bytes(b);
}

void Writer::string(std::string_view s, std::size_t max_len) noexcept {
  if (s.size() > max_len) {
    ok_ = false;
    return;
  }
  bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
}

void Writer::raw(std::span<const std::byte> b) noexcept { put(b); }

Outcome Reader::need(std::size_t n) noexcept {
  if (err_ != Outcome::Ok) return err_;
  if (n > remaining()) return fail(Outcome::Malformed);
  return Outcome::Ok;
}

Outcome Reader::u8(std::uint8_t& v) noexcept {
  Outcome o = need(1);
  if (!is_affirmative(o)) return o;
  v = std::to_integer<std::uint8_t>(d_[p_]);
  ++p_;
  return Outcome::Ok;
}

Outcome Reader::u16(std::uint16_t& v) noexcept {
  Outcome o = need(2);
  if (!is_affirmative(o)) return o;
  v = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(d_[p_]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(d_[p_ + 1])) << 8));
  p_ += 2;
  return Outcome::Ok;
}

Outcome Reader::u32(std::uint32_t& v) noexcept {
  Outcome o = need(4);
  if (!is_affirmative(o)) return o;
  std::uint32_t acc = 0;
  for (int i = 0; i < 4; ++i) {
    acc |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(d_[p_ + static_cast<std::size_t>(i)]))
           << (8 * i);
  }
  p_ += 4;
  v = acc;
  return Outcome::Ok;
}

Outcome Reader::u64(std::uint64_t& v) noexcept {
  Outcome o = need(8);
  if (!is_affirmative(o)) return o;
  std::uint64_t acc = 0;
  for (int i = 0; i < 8; ++i) {
    acc |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(d_[p_ + static_cast<std::size_t>(i)]))
           << (8 * i);
  }
  p_ += 8;
  v = acc;
  return Outcome::Ok;
}

Outcome Reader::i64(std::int64_t& v) noexcept {
  std::uint64_t raw = 0;
  Outcome o = u64(raw);
  if (!is_affirmative(o)) return o;
  v = static_cast<std::int64_t>(raw);
  return Outcome::Ok;
}

Outcome Reader::boolean(bool& v) noexcept {
  std::uint8_t raw = 0;
  Outcome o = u8(raw);
  if (!is_affirmative(o)) return o;
  if (raw > 1u) return fail(Outcome::Invalid);
  v = raw == 1u;
  return Outcome::Ok;
}

Outcome Reader::blob(std::size_t n, std::span<const std::byte>& out) noexcept {
  std::uint32_t len = 0;
  Outcome o = u32(len);
  if (!is_affirmative(o)) return o;
  if (len != n) return fail(Outcome::Malformed);
  return raw(len, out);
}

Outcome Reader::raw(std::size_t n, std::span<const std::byte>& out) noexcept {
  Outcome o = need(n);
  if (!is_affirmative(o)) return o;
  out = d_.subspan(p_, n);
  p_ += n;
  return Outcome::Ok;
}

Outcome Reader::skip(std::size_t n) noexcept {
  Outcome o = need(n);
  if (!is_affirmative(o)) return o;
  p_ += n;
  return Outcome::Ok;
}

Outcome Reader::string(std::string& out, std::size_t max_len) noexcept {
  std::uint32_t len = 0;
  Outcome o = u32(len);
  if (!is_affirmative(o)) return o;
  const std::size_t n = static_cast<std::size_t>(len);
  if (n > max_len) return fail(Outcome::Oversized);
  std::span<const std::byte> s;
  o = raw(n, s);
  if (!is_affirmative(o)) return o;
  out.assign(reinterpret_cast<const char*>(s.data()), s.size());
  return Outcome::Ok;
}

Outcome Reader::finish() noexcept {
  if (err_ != Outcome::Ok) return err_;
  if (p_ != d_.size()) return fail(Outcome::Malformed);
  return Outcome::Ok;
}

}  // namespace bhg
