#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "blackhole/core/ids.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/persist/record.hpp"

namespace bhg {

/// ============================================================================
/// Transactional snapshot
/// ============================================================================
///
/// Layout (little endian):
///   offset 0  u32 magic 'B','H','G','S'
///   offset 4  u16 version
///   offset 6  u16 kind (1 = lineage snapshot)
///   offset 8  u64 snapshot_seq
///   offset 16 u32 payload_len
///   offset 20 u32 header_crc  (CRC-32C over bytes [0,20))
///   offset 24 payload
///   offset 24+len u32 payload_crc
///
/// Replacement protocol: write to <path>.tmp, flush, fsync, atomically replace
/// <path>, then fsync the directory. A crash at any point leaves either the old or
/// the new snapshot intact, never a mixture.

inline constexpr std::uint16_t kSnapshotKindLineage = 1;

struct SnapshotReadResult {
  bool present{false};
  Outcome outcome{Outcome::Ok};
  RecordSequence snapshot_seq{};
  std::vector<std::byte> payload;
  std::string detail;
};

Outcome write_snapshot_atomic(const std::string& path, RecordSequence snapshot_seq,
                              std::span<const std::byte> payload);

SnapshotReadResult read_snapshot(const std::string& path, std::uint32_t max_payload);

/// Removes stale staging files left behind by a crash during snapshot replacement.
void clean_staging(const std::string& path);

}  // namespace bhg
