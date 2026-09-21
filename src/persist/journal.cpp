#include "blackhole/persist/journal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "blackhole/core/hash.hpp"
#include "blackhole/core/path.hpp"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bhg {

namespace {

bool flush_and_sync(std::FILE* f) {
  if (std::fflush(f) != 0) return false;
#if defined(_WIN32)
  return _commit(_fileno(f)) == 0;
#else
  return ::fsync(::fileno(f)) == 0;
#endif
}

Outcome read_whole_file(const std::string& path, std::uint64_t max_bytes,
                        std::vector<std::byte>& out) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
  if (ec) return Outcome::NotFound;
  if (size > max_bytes) return Outcome::Oversized;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return Outcome::NotFound;
  out.assign(static_cast<std::size_t>(size), std::byte{0});
  std::size_t read_total = 0;
  while (read_total < out.size()) {
    const std::size_t n =
        std::fread(out.data() + read_total, 1, out.size() - read_total, f);
    if (n == 0) break;
    read_total += n;
  }
  std::fclose(f);
  if (read_total != out.size()) return Outcome::Corrupt;
  return Outcome::Ok;
}

std::uint32_t read_u32(const std::byte* p) noexcept {
  std::uint32_t acc = 0;
  for (int i = 0; i < 4; ++i) {
    acc |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(p[i])) << (8 * i);
  }
  return acc;
}

}  // namespace

RecordVisitor::~RecordVisitor() = default;

JournalWriter::~JournalWriter() { (void)close(); }

Outcome JournalWriter::open_append(const std::string& path, bool truncate) {
  if (f_ != nullptr) (void)close();
  f_ = std::fopen(path.c_str(), truncate ? "wb+" : "ab+");
  if (f_ == nullptr) return Outcome::Unavailable;
  path_ = path;
  bytes_ = truncate ? 0u : file_size(path);
  pending_ = 0;
  return Outcome::Ok;
}

Outcome JournalWriter::append(RecordType type, RecordSequence seq,
                              std::span<const std::byte> payload) {
  if (f_ == nullptr) return Outcome::Invalid;
  Writer w(payload.size() + kJournalHeaderSize + kJournalTrailerSize);
  encode_record(w, type, seq, payload);
  if (!w.ok()) return Outcome::Exhausted;
  const std::size_t written = std::fwrite(w.data().data(), 1, w.size(), f_);
  if (written != w.size()) return Outcome::Rejected;
  bytes_ += w.size();
  pending_ += w.size();
  return Outcome::Ok;
}

Outcome JournalWriter::commit() {
  if (f_ == nullptr) return Outcome::Invalid;
  if (!flush_and_sync(f_)) return Outcome::Rejected;
  pending_ = 0;
  return Outcome::Ok;
}

Outcome JournalWriter::close() {
  if (f_ == nullptr) return Outcome::Ok;
  const bool ok = flush_and_sync(f_);
  const int rc = std::fclose(f_);
  f_ = nullptr;
  pending_ = 0;
  return (ok && rc == 0) ? Outcome::Ok : Outcome::Rejected;
}

Outcome scan_journal(const std::string& path, std::uint32_t max_payload, std::uint64_t max_bytes,
                     std::uint64_t max_records, RecordSequence first_seq, RecordVisitor& visitor,
                     JournalScanReport& report) {
  report = JournalScanReport{};
  if (!is_regular_file(path)) {
    report.outcome = Outcome::Ok;
    report.detail = "journal absent";
    return Outcome::Ok;
  }
  std::vector<std::byte> data;
  Outcome o = read_whole_file(path, max_bytes, data);
  if (!is_affirmative(o)) {
    report.outcome = o;
    report.detail = "journal unreadable or oversized";
    return o;
  }

  std::size_t pos = 0;
  RecordSequence expected{first_seq.value() == 0 ? 1u : first_seq.value()};
  while (true) {
    const std::size_t remaining = data.size() - pos;
    if (remaining == 0) break;

    if (remaining < kJournalHeaderSize) {
      const std::span<const std::byte> tail(data.data() + pos, remaining);
      if (is_all_zero(tail)) {
        report.zero_tail = true;
        report.tail_bytes = remaining;
        report.detail = "all-zero preallocated tail";
        break;
      }
      if (!is_valid_record_prefix(tail, max_payload)) {
        report.outcome = Outcome::Corrupt;
        report.detail = "trailing bytes are not a valid record prefix";
        report.tail_bytes = remaining;
        return report.outcome;
      }
      report.torn_tail = true;
      report.tail_bytes = remaining;
      report.detail = "torn tail recovered";
      break;
    }

    const std::span<const std::byte> header(data.data() + pos, kJournalHeaderSize);
    if (read_u32(header.data()) != kJournalMagic) {
      const std::span<const std::byte> tail(data.data() + pos, remaining);
      if (is_all_zero(tail)) {
        report.zero_tail = true;
        report.tail_bytes = remaining;
        report.detail = "all-zero preallocated tail";
        break;
      }
      report.outcome = Outcome::Corrupt;
      report.detail = "bad record magic";
      return report.outcome;
    }

    RecordHeader rh;
    o = decode_record_header(header, max_payload, rh);
    if (!is_affirmative(o)) {
      report.outcome = o;
      report.detail = "record header rejected";
      return o;
    }
    if (rh.seq != expected) {
      report.outcome = Outcome::Corrupt;
      report.detail = "record sequence regression or gap";
      return report.outcome;
    }
    const std::uint64_t need =
        static_cast<std::uint64_t>(kJournalHeaderSize) + rh.payload_len + kJournalTrailerSize;
    if (need > remaining) {
      const std::span<const std::byte> tail(data.data() + pos, remaining);
      if (!is_valid_record_prefix(tail.subspan(0, std::min<std::size_t>(remaining, kJournalHeaderSize)),
                                  max_payload)) {
        report.outcome = Outcome::Corrupt;
        report.detail = "truncated record with inconsistent header";
        return report.outcome;
      }
      report.torn_tail = true;
      report.tail_bytes = remaining;
      report.detail = "torn tail recovered";
      break;
    }

    const std::span<const std::byte> record(data.data() + pos, static_cast<std::size_t>(need));
    const std::uint32_t stored_crc =
        read_u32(record.data() + kJournalHeaderSize + rh.payload_len);
    if (crc32c(record.subspan(0, kJournalHeaderSize + rh.payload_len)) != stored_crc) {
      report.outcome = Outcome::Corrupt;
      report.detail = "record integrity check failed";
      return report.outcome;
    }

    const std::span<const std::byte> payload = record.subspan(kJournalHeaderSize, rh.payload_len);
    if (report.records >= max_records) {
      report.outcome = Outcome::Exhausted;
      report.detail = "journal record bound exceeded";
      return report.outcome;
    }
    o = visitor.visit(rh.type, rh.seq, payload);
    if (!is_affirmative(o)) {
      report.outcome = o;
      report.detail = "record payload rejected during replay";
      return o;
    }

    ++report.records;
    pos += static_cast<std::size_t>(need);
    report.valid_bytes = pos;
    report.last_seq = rh.seq;
    expected = RecordSequence{rh.seq.value() + 1};
  }

  report.outcome = Outcome::Ok;
  if (report.detail.empty()) report.detail = "journal scanned";
  return Outcome::Ok;
}

Outcome truncate_file(const std::string& path, std::uint64_t size) {
  if (!is_regular_file(path)) return Outcome::NotFound;
#if defined(_WIN32)
  std::FILE* f = std::fopen(path.c_str(), "rb+");
  if (f == nullptr) return Outcome::Unavailable;
  const bool ok = _chsize_s(_fileno(f), static_cast<__int64>(size)) == 0 && flush_and_sync(f);
  std::fclose(f);
  return ok ? Outcome::Ok : Outcome::Rejected;
#else
  if (::truncate(path.c_str(), static_cast<off_t>(size)) != 0) return Outcome::Rejected;
  return Outcome::Ok;
#endif
}

std::uint64_t file_size(const std::string& path) {
  std::error_code ec;
  const auto s = std::filesystem::file_size(std::filesystem::path(path), ec);
  if (ec) return 0;
  return static_cast<std::uint64_t>(s);
}

}  // namespace bhg
