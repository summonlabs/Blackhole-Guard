#include "blackhole/persist/record.hpp"

#include <algorithm>

#include "blackhole/core/hash.hpp"

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

std::uint64_t get_u64(const std::byte* p) noexcept {
  std::uint64_t acc = 0;
  for (int i = 0; i < 8; ++i) {
    acc |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
  }
  return acc;
}

}  // namespace

void encode_record(Writer& w, RecordType type, RecordSequence seq,
                   std::span<const std::byte> payload) noexcept {
  std::byte header[kJournalHeaderSize]{};
  put_u32(header + 0, kJournalMagic);
  put_u16(header + 4, kFormatVersion);
  put_u16(header + 6, static_cast<std::uint16_t>(type));
  for (int i = 0; i < 8; ++i) {
    header[8 + i] = static_cast<std::byte>((seq.value() >> (8 * i)) & 0xFFu);
  }
  put_u32(header + 16, static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t header_crc = crc32c(std::span<const std::byte>(header, 20));
  put_u32(header + 20, header_crc);

  Writer body(w.capacity_limit());
  body.raw(std::span<const std::byte>(header, kJournalHeaderSize));
  body.raw(payload);
  if (!body.ok()) {
    return;
  }
  const std::uint32_t record_crc = crc32c(body.span());
  std::byte trailer[4]{};
  put_u32(trailer, record_crc);
  w.raw(std::span<const std::byte>(header, kJournalHeaderSize));
  w.raw(payload);
  w.raw(std::span<const std::byte>(trailer, 4));
}

Outcome decode_record_header(std::span<const std::byte> header, std::uint32_t max_payload,
                             RecordHeader& out) noexcept {
  if (header.size() < kJournalHeaderSize) return Outcome::Malformed;
  if (get_u32(header.data()) != kJournalMagic) return Outcome::Corrupt;
  const std::uint16_t version = get_u16(header.data() + 4);
  if (version != kFormatVersion) return Outcome::Unsupported;
  const std::uint16_t type = get_u16(header.data() + 6);
  if (!is_valid_record_type(type)) return Outcome::Invalid;
  const std::uint32_t payload_len = get_u32(header.data() + 16);
  if (payload_len > max_payload) return Outcome::Oversized;
  const std::uint32_t header_crc = get_u32(header.data() + 20);
  if (crc32c(header.subspan(0, 20)) != header_crc) return Outcome::Corrupt;

  out.version = version;
  out.type = static_cast<RecordType>(type);
  out.seq = RecordSequence{get_u64(header.data() + 8)};
  out.payload_len = payload_len;
  out.header_crc = header_crc;
  return Outcome::Ok;
}

bool is_valid_record_prefix(std::span<const std::byte> bytes, std::uint32_t max_payload) noexcept {
  const std::size_t n = std::min(bytes.size(), kJournalHeaderSize);
  for (std::size_t i = 0; i < n; ++i) {
    std::byte expected{};
    if (i < 4) {
      expected = static_cast<std::byte>((kJournalMagic >> (8 * i)) & 0xFFu);
    } else if (i == 4) {
      expected = static_cast<std::byte>(kFormatVersion & 0xFFu);
    } else if (i == 5) {
      expected = static_cast<std::byte>((kFormatVersion >> 8) & 0xFFu);
    } else if (i >= 6 && i < 8) {
      // Record type: any in-domain value is acceptable in a torn prefix.
      continue;
    } else if (i >= 8 && i < 16) {
      // Sequence: unknowable for a torn record.
      continue;
    } else if (i >= 16 && i < 20) {
      // Payload length: validated once all four bytes are present.
      if (i == 19) {
        const std::uint32_t len = get_u32(bytes.data() + 16);
        if (len > max_payload) return false;
      }
      continue;
    } else {
      // Header CRC: unknowable for a torn record.
      continue;
    }
    if (bytes[i] != expected) return false;
  }
  return true;
}

bool is_all_zero(std::span<const std::byte> bytes) noexcept {
  for (const std::byte b : bytes) {
    if (b != std::byte{0}) return false;
  }
  return true;
}

void encode(Writer& w, const BootPayload& p) noexcept {
  encode(w, p.boot);
  encode(w, p.incarnation);
  encode(w, p.epoch);
  encode(w, p.started_at);
  w.u64(p.boot_count);
  w.u64(p.open_count);
  encode(w, p.policy);
  w.u64(p.policy_fingerprint);
  w.u8(static_cast<std::uint8_t>(p.provenance));
}

Outcome decode(Reader& r, BootPayload& p) noexcept {
  BootPayload out{};
  Outcome o = decode(r, out.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.epoch);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.started_at);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.boot_count);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.open_count);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.policy);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.policy_fingerprint);
  if (!is_affirmative(o)) return o;
  std::uint8_t prov = 0;
  o = r.u8(prov);
  if (!is_affirmative(o)) return o;
  if (!is_valid_provenance(prov)) return Outcome::Invalid;
  out.provenance = static_cast<Provenance>(prov);
  p = out;
  return Outcome::Ok;
}

void encode(Writer& w, const InterruptionPayload& p) noexcept {
  w.u16(static_cast<std::uint16_t>(p.reason));
  encode(w, p.fence);
  encode(w, p.decision);
  encode(w, p.scope);
  encode(w, p.prior_epoch);
  encode(w, p.prior_boot);
  encode(w, p.prior_incarnation);
  encode(w, p.at);
}

Outcome decode(Reader& r, InterruptionPayload& p) noexcept {
  InterruptionPayload out{};
  std::uint16_t reason = 0;
  Outcome o = r.u16(reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(reason)) return Outcome::Invalid;
  out.reason = static_cast<ReasonCode>(reason);
  o = decode(r, out.fence);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.prior_epoch);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.prior_boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.prior_incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.at);
  if (!is_affirmative(o)) return o;
  p = out;
  return Outcome::Ok;
}

void encode(Writer& w, const FenceStatePayload& p) noexcept {
  encode(w, p.intent);
  w.u8(p.lifecycle);
  w.boolean(p.effect_verified);
  encode(w, p.ack_owner);
  encode(w, p.effect_owner);
  encode(w, p.acked_at);
  encode(w, p.effected_at);
  w.u16(static_cast<std::uint16_t>(p.terminal_reason));
}

Outcome decode(Reader& r, FenceStatePayload& p) noexcept {
  FenceStatePayload out{};
  Outcome o = decode(r, out.intent);
  if (!is_affirmative(o)) return o;
  o = r.u8(out.lifecycle);
  if (!is_affirmative(o)) return o;
  if (!is_valid_fence_lifecycle(out.lifecycle)) return Outcome::Invalid;
  o = r.boolean(out.effect_verified);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.ack_owner);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.effect_owner);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.acked_at);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.effected_at);
  if (!is_affirmative(o)) return o;
  std::uint16_t reason = 0;
  o = r.u16(reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(reason)) return Outcome::Invalid;
  out.terminal_reason = static_cast<ReasonCode>(reason);
  p = out;
  return Outcome::Ok;
}

void encode(Writer& w, const CheckpointPayload& p) noexcept {
  encode(w, p.snapshot_seq);
  encode(w, p.epoch);
  w.u64(p.journal_generation);
}

Outcome decode(Reader& r, CheckpointPayload& p) noexcept {
  CheckpointPayload out{};
  Outcome o = decode(r, out.snapshot_seq);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.epoch);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.journal_generation);
  if (!is_affirmative(o)) return o;
  p = out;
  return Outcome::Ok;
}

}  // namespace bhg
