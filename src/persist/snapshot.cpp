#include "blackhole/persist/snapshot.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "blackhole/core/hash.hpp"
#include "blackhole/core/path.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

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

bool atomic_replace(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  const std::wstring wfrom(from.begin(), from.end());
  const std::wstring wto(to.begin(), to.end());
  return ::MoveFileExW(wfrom.c_str(), wto.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(from), std::filesystem::path(to), ec);
  if (ec) return false;
  sync_directory(parent_dir(to));
  return true;
#endif
}

}  // namespace

void clean_staging(const std::string& path) { (void)remove_file(path + ".tmp"); }

Outcome write_snapshot_atomic(const std::string& path, RecordSequence snapshot_seq,
                              std::span<const std::byte> payload) {
  const std::string tmp = path + ".tmp";
  Writer w(payload.size() + kSnapshotHeaderSize + kSnapshotTrailerSize);
  std::byte header[kSnapshotHeaderSize]{};
  put_u32(header + 0, kSnapshotMagic);
  put_u16(header + 4, kFormatVersion);
  put_u16(header + 6, kSnapshotKindLineage);
  for (int i = 0; i < 8; ++i) {
    header[8 + i] = static_cast<std::byte>((snapshot_seq.value() >> (8 * i)) & 0xFFu);
  }
  put_u32(header + 16, static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t header_crc = crc32c(std::span<const std::byte>(header, 20));
  put_u32(header + 20, header_crc);

  w.raw(std::span<const std::byte>(header, kSnapshotHeaderSize));
  w.raw(payload);
  if (!w.ok()) return Outcome::Exhausted;
  const std::uint32_t payload_crc = crc32c(w.span());
  std::byte trailer[4]{};
  put_u32(trailer, payload_crc);
  w.raw(std::span<const std::byte>(trailer, 4));
  if (!w.ok()) return Outcome::Exhausted;

  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) return Outcome::Unavailable;
  const std::size_t written = std::fwrite(w.data().data(), 1, w.size(), f);
  bool ok = written == w.size();
  if (ok && std::fflush(f) != 0) ok = false;
  if (ok) {
#if defined(_WIN32)
    ok = _commit(_fileno(f)) == 0;
#else
    ok = ::fsync(::fileno(f)) == 0;
#endif
  }
  if (std::fclose(f) != 0) ok = false;
  if (!ok) {
    (void)remove_file(tmp);
    return Outcome::Rejected;
  }
  if (!atomic_replace(tmp, path)) {
    (void)remove_file(tmp);
    return Outcome::Rejected;
  }
  return Outcome::Ok;
}

SnapshotReadResult read_snapshot(const std::string& path, std::uint32_t max_payload) {
  SnapshotReadResult r;
  if (!is_regular_file(path)) {
    r.present = false;
    r.outcome = Outcome::Ok;
    r.detail = "snapshot absent";
    return r;
  }
  r.present = true;
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
  if (ec) {
    r.outcome = Outcome::NotFound;
    r.detail = "snapshot unreadable";
    return r;
  }
  const std::uint64_t min_size = kSnapshotHeaderSize + kSnapshotTrailerSize;
  if (size < min_size) {
    r.outcome = Outcome::Corrupt;
    r.detail = "snapshot shorter than its header";
    return r;
  }
  if (size > static_cast<std::uint64_t>(max_payload) + min_size) {
    r.outcome = Outcome::Oversized;
    r.detail = "snapshot exceeds the configured bound";
    return r;
  }
  std::vector<std::byte> data(static_cast<std::size_t>(size), std::byte{0});
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    r.outcome = Outcome::Unavailable;
    r.detail = "snapshot open failed";
    return r;
  }
  const std::size_t got = std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (got != data.size()) {
    r.outcome = Outcome::Corrupt;
    r.detail = "short read";
    return r;
  }

  if (get_u32(data.data()) != kSnapshotMagic) {
    r.outcome = Outcome::Corrupt;
    r.detail = "snapshot magic mismatch";
    return r;
  }
  const std::uint16_t version = get_u16(data.data() + 4);
  if (version != kFormatVersion) {
    r.outcome = Outcome::Unsupported;
    r.detail = "unsupported snapshot version";
    return r;
  }
  const std::uint16_t kind = get_u16(data.data() + 6);
  if (kind != kSnapshotKindLineage) {
    r.outcome = Outcome::Unsupported;
    r.detail = "unsupported snapshot kind";
    return r;
  }
  const std::uint32_t len = get_u32(data.data() + 16);
  if (len > max_payload) {
    r.outcome = Outcome::Oversized;
    r.detail = "snapshot payload exceeds the configured bound";
    return r;
  }
  if (static_cast<std::uint64_t>(len) + min_size != size) {
    r.outcome = Outcome::Corrupt;
    r.detail = "snapshot length disagrees with its size";
    return r;
  }
  if (crc32c(std::span<const std::byte>(data.data(), 20)) != get_u32(data.data() + 20)) {
    r.outcome = Outcome::Corrupt;
    r.detail = "snapshot header integrity check failed";
    return r;
  }
  const std::size_t payload_end = kSnapshotHeaderSize + len;
  if (crc32c(std::span<const std::byte>(data.data(), payload_end)) !=
      get_u32(data.data() + payload_end)) {
    r.outcome = Outcome::Corrupt;
    r.detail = "snapshot payload integrity check failed";
    return r;
  }

  r.snapshot_seq = RecordSequence{get_u64(data.data() + 8)};
  r.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(kSnapshotHeaderSize),
                   data.begin() + static_cast<std::ptrdiff_t>(payload_end));
  r.outcome = Outcome::Ok;
  r.detail = "snapshot loaded";
  return r;
}

}  // namespace bhg
