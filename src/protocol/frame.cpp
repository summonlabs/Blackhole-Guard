#include "blackhole/protocol/frame.hpp"

#include "blackhole/core/checked.hpp"
#include "blackhole/core/hash.hpp"
#include "blackhole/version.hpp"

namespace bhg {

namespace {

void put_u16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::byte>(v & 0xFFu);
  p[1] = static_cast<std::byte>((v >> 8) & 0xFFu);
}

void put_u32(std::byte* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
}

std::uint16_t get_u16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]) |
                                    (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]))
                                     << 8));
}

std::uint32_t get_u32(const std::byte* p) noexcept {
  std::uint32_t acc = 0;
  for (int i = 0; i < 4; ++i) {
    acc |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
  }
  return acc;
}

std::uint32_t frame_integrity(std::span<const std::byte> header16,
                              std::span<const std::byte> payload) noexcept {
  // crc32c(data, seed) treats seed as a previously finalised CRC, which is exactly
  // the chaining convention required here.
  return crc32c(payload, crc32c(header16));
}

}  // namespace

void encode_frame(Writer& w, const Frame& frame) noexcept {
  std::byte header[kFrameHeaderSize]{};
  put_u32(header + 0, kFrameMagic);
  put_u16(header + 4, kProtocolVersion);
  put_u16(header + 6, static_cast<std::uint16_t>(frame.type));
  put_u32(header + 8, frame.flags);
  put_u32(header + 12, static_cast<std::uint32_t>(frame.payload.size()));
  put_u32(header + 16, 0u);
  const std::uint32_t integrity =
      frame_integrity(std::span<const std::byte>(header, 16),
                      std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  put_u32(header + 16, integrity);
  w.raw(std::span<const std::byte>(header, kFrameHeaderSize));
  w.raw(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
}

Outcome FrameDecoder::feed(std::span<const std::byte> bytes) {
  if (failure_ != Outcome::Ok) return failure_;
  const auto total = checked_add<std::size_t>(buffered_, bytes.size());
  if (!total.has_value()) return fail(Outcome::Oversized);
  // The buffer only ever holds at most one incomplete frame.
  if (*total > static_cast<std::size_t>(max_) + kFrameHeaderSize) {
    return fail(Outcome::Oversized);
  }
  if (buffer_.size() < *total) buffer_.resize(*total);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    buffer_[buffered_ + i] = bytes[i];
  }
  buffered_ = *total;
  return Outcome::Ok;
}

Outcome FrameDecoder::next(Frame& out) {
  if (failure_ != Outcome::Ok) return failure_;
  if (buffered_ < kFrameHeaderSize) return Outcome::NotFound;

  const std::byte* h = buffer_.data();
  if (get_u32(h) != kFrameMagic) return fail(Outcome::Corrupt);
  if (get_u16(h + 4) != kProtocolVersion) return fail(Outcome::Unsupported);
  const std::uint16_t type = get_u16(h + 6);
  if (!is_valid_message_type(type)) return fail(Outcome::Invalid);
  const std::uint32_t flags = get_u32(h + 8);
  if (flags != 0u) return fail(Outcome::Invalid);
  const std::uint32_t length = get_u32(h + 12);
  if (length > max_) return fail(Outcome::Oversized);

  const std::size_t total = kFrameHeaderSize + static_cast<std::size_t>(length);
  if (buffered_ < total) return Outcome::NotFound;

  const std::uint32_t stored = get_u32(h + 16);
  // Recompute over header[0,16) exactly as sent (integrity field excluded).
  const std::uint32_t computed =
      frame_integrity(std::span<const std::byte>(h, 16),
                      std::span<const std::byte>(h + kFrameHeaderSize, length));
  if (computed != stored) return fail(Outcome::Corrupt);

  out.type = static_cast<MessageType>(type);
  out.flags = flags;
  out.payload.assign(h + kFrameHeaderSize, h + total);

  for (std::size_t i = total; i < buffered_; ++i) {
    buffer_[i - total] = buffer_[i];
  }
  buffered_ -= total;
  return Outcome::Ok;
}

Outcome FrameDecoder::finish() noexcept {
  if (failure_ != Outcome::Ok) return failure_;
  if (buffered_ != 0) return fail(Outcome::Malformed);
  return Outcome::Ok;
}

}  // namespace bhg
