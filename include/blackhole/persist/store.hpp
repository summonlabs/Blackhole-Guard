#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "blackhole/authority/authority.hpp"
#include "blackhole/core/nonce.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/persist/journal.hpp"
#include "blackhole/persist/snapshot.hpp"

namespace bhg {

struct StoreConfig {
  std::string root{"./bhg-state"};
  std::uint32_t max_record_payload{1u << 16};
  std::uint64_t max_journal_bytes{64ull * 1024ull * 1024ull};
  std::uint64_t max_journal_records{65536};
  std::uint32_t max_snapshot_payload{4u << 20};
  std::uint32_t max_lineage_records{4096};
  std::uint32_t max_prior_fences{1024};
};

/// One durable lineage entry. Lineage is history: it records what was decided and
/// under which authority, and it never confers authority after a restart.
struct LineageEntry {
  RecordType type{RecordType::DecisionCommit};
  RecordSequence seq{};
  WallNs at{};
  DecisionId decision{};
  FenceId fence{};
  Scope scope{};
  GenerationVector gens{};
  Outcome outcome{Outcome::Ok};
  Classification classification{Classification::NoEvidence};
  ReasonCode reason{ReasonCode::None};
  BootId boot{};
  IncarnationId incarnation{};
  bool from_prior_incarnation{false};
};

struct OpenReport {
  Outcome outcome{Outcome::Ok};
  BootId boot{};
  IncarnationId incarnation{};
  CoordinatorEpoch epoch{};
  std::uint64_t boot_count{0};
  std::uint64_t open_count{0};
  std::uint64_t journal_records_replayed{0};
  std::uint64_t reclaimed_tail_bytes{0};
  std::uint64_t fenced_prior_fences{0};
  std::uint64_t prior_lineage_entries{0};
  bool snapshot_present{false};
  bool torn_tail_recovered{false};
  bool zero_tail_observed{false};
  bool restart_detected{false};
  bool policy_restored{false};
  std::string detail;
};

/// Durable state: definitions, policy, committed lineage, completed outcomes and
/// fences -- only state whose semantics survive a restart.
///
/// It deliberately does NOT contain: observed evidence freshness, source sequencing
/// cursors, leases, in-flight attempts, open session authority, or any backend
/// effect. Those are dynamic liveness and must be re-established.
struct DurableState {
  std::uint16_t format_version{kFormatVersion};
  RecordSequence snapshot_seq{};
  CoordinatorEpoch epoch{};
  std::uint64_t boot_count{0};
  std::uint64_t open_count{0};
  PolicyVersion policy{};
  std::uint64_t policy_fingerprint{0};
  BootId last_boot{};
  IncarnationId last_incarnation{};
  std::uint64_t total_decisions{0};
  std::uint64_t total_fences{0};
  std::uint64_t total_interruptions{0};
  std::uint64_t lineage_seen{0};
  std::uint64_t lineage_dropped{0};
  std::vector<LineageEntry> lineage;
  std::vector<FenceStatePayload> fences;
};

void encode(Writer& w, const DurableState& s) noexcept;
Outcome decode(Reader& r, DurableState& s) noexcept;

/// Versioned, integrity-checked, transactional durable store.
///
/// Ordering contract for every durable mutation: append -> flush -> fsync ->
/// publish (return). Snapshot replacement is staged and atomically renamed.
class Store {
 public:
  Store(StoreConfig config, const FencePolicy& policy);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  /// Opens the store, replays durable state, advances the epoch and appends a new
  /// boot record. On any integrity doubt the store refuses to open (fail closed).
  Outcome open(NonceSource& nonces, WallNs now, Provenance label);

  Outcome append_commit(RecordType type, std::span<const std::byte> payload,
                        RecordSequence& out_seq);
  Outcome commit_policy(const FencePolicy& policy);
  Outcome commit_decision(const Decision& d);
  Outcome commit_fence_state(RecordType type, const FenceStatePayload& f);
  Outcome commit_interruption(const InterruptionPayload& p);
  Outcome checkpoint(WallNs now);
  Outcome close();

  [[nodiscard]] const OpenReport& report() const noexcept { return report_; }
  [[nodiscard]] const DurableState& state() const noexcept { return state_; }
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept { return state_.epoch; }
  [[nodiscard]] BootId boot() const noexcept { return boot_; }
  [[nodiscard]] IncarnationId incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] RecordSequence last_seq() const noexcept { return last_seq_; }
  [[nodiscard]] bool is_open() const noexcept { return opened_; }
  [[nodiscard]] bool lineage_accounting_closed() const noexcept {
    return state_.lineage_seen ==
           static_cast<std::uint64_t>(state_.lineage.size()) + state_.lineage_dropped;
  }
  [[nodiscard]] const std::string& journal_path() const noexcept { return journal_path_; }
  [[nodiscard]] const std::string& snapshot_path() const noexcept { return snapshot_path_; }
  [[nodiscard]] std::uint64_t journal_bytes() const noexcept { return journal_bytes_; }

 private:
  Outcome apply_record(RecordType type, RecordSequence seq, std::span<const std::byte> payload);
  void push_lineage(const LineageEntry& e);
  DurableState build_snapshot_state() const;

  class ReplayVisitor;

  StoreConfig config_;
  FencePolicy policy_;
  std::string journal_path_;
  std::string snapshot_path_;
  JournalWriter journal_;
  DurableState state_{};
  OpenReport report_{};
  BootId boot_{};
  IncarnationId incarnation_{};
  RecordSequence last_seq_{};
  bool opened_{false};
  std::uint64_t journal_bytes_{0};
};

}  // namespace bhg
