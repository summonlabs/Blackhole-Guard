#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "blackhole/core/ids.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/persist/record.hpp"

namespace bhg {

/// Visitor for journal replay. Returning a non-Ok outcome aborts the scan and is
/// propagated: a payload that cannot be decoded is treated as corruption, not as
/// "skip and continue".
class RecordVisitor {
 public:
  RecordVisitor() = default;
  virtual ~RecordVisitor();
  RecordVisitor(const RecordVisitor&) = delete;
  RecordVisitor& operator=(const RecordVisitor&) = delete;
  virtual Outcome visit(RecordType type, RecordSequence seq,
                        std::span<const std::byte> payload) = 0;
};

struct JournalScanReport {
  Outcome outcome{Outcome::Ok};
  std::uint64_t records{0};
  std::uint64_t valid_bytes{0};
  /// Bytes beyond the last intact record. Only recoverable torn tails or an
  /// all-zero preallocated tail may produce a non-zero value with outcome Ok.
  std::uint64_t tail_bytes{0};
  bool torn_tail{false};
  bool zero_tail{false};
  RecordSequence last_seq{};
  std::string detail;
};

/// Append-side journal handle.
///
/// Durability ordering: append() only buffers; commit() flushes and fsyncs and is
/// the single publish point. Nothing is reported durable before commit() returns.
class JournalWriter {
 public:
  JournalWriter() = default;
  ~JournalWriter();
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;

  Outcome open_append(const std::string& path, bool truncate);
  Outcome append(RecordType type, RecordSequence seq, std::span<const std::byte> payload);
  Outcome commit();
  Outcome close();

  [[nodiscard]] bool is_open() const noexcept { return f_ != nullptr; }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t pending_bytes() const noexcept { return pending_; }

 private:
  std::FILE* f_{nullptr};
  std::string path_;
  std::uint64_t bytes_{0};
  std::uint64_t pending_{0};
};

/// Scans a journal file, replaying every intact record through the visitor.
///
/// Refusal policy:
///   * bad magic / bad header CRC / bad record CRC / out-of-domain type /
///     oversized length / out-of-order sequence -> Corrupt or Invalid: refuse,
///     never truncate;
///   * unsupported format version -> Unsupported: refuse;
///   * a trailing partial record whose bytes are a valid prefix of a record
///     header -> torn tail, reported and recoverable;
///   * an all-zero trailing region -> preallocated tail, reported.
/// The expected sequence of the first record is supplied by the caller so that a
/// journal rotated after a checkpoint (which restarts at snapshot_seq + 1) is
/// still checked for continuity.
Outcome scan_journal(const std::string& path, std::uint32_t max_payload, std::uint64_t max_bytes,
                     std::uint64_t max_records, RecordSequence first_seq, RecordVisitor& visitor,
                     JournalScanReport& report);

/// Truncates a file to the given size (used only for a verified torn tail).
Outcome truncate_file(const std::string& path, std::uint64_t size);

/// Size of a regular file, or 0 when absent.
std::uint64_t file_size(const std::string& path);

}  // namespace bhg
