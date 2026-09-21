#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// Visitor that records what replay produced and can be told to reject a record,
/// modelling a payload that fails to decode.
class CollectingVisitor final : public RecordVisitor {
 public:
  Outcome visit(RecordType type, RecordSequence seq, std::span<const std::byte> payload) override {
    types.push_back(type);
    seqs.push_back(seq);
    sizes.push_back(payload.size());
    if (fail_on_index >= 0 && static_cast<std::size_t>(fail_on_index) + 1u == types.size()) {
      return Outcome::Invalid;
    }
    return Outcome::Ok;
  }
  std::vector<RecordType> types;
  std::vector<RecordSequence> seqs;
  std::vector<std::size_t> sizes;
  int fail_on_index{-1};
};

std::vector<std::byte> payload_of(std::size_t n, std::byte fill) {
  return std::vector<std::byte>(n, fill);
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> read_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<std::byte> out;
  for (std::istreambuf_iterator<char> it(f), end; it != end; ++it) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(*it)));
  }
  return out;
}

/// Builds a journal containing the requested number of freshly framed records.
std::vector<std::byte> build_journal(std::uint32_t count, std::size_t payload_size = 8) {
  Writer w(1u << 20);
  for (std::uint32_t i = 1; i <= count; ++i) {
    const std::vector<std::byte> payload = payload_of(payload_size, static_cast<std::byte>(i));
    encode_record(w, RecordType::EvidenceLineageCommit, RecordSequence{i}, payload);
  }
  return w.take();
}

struct JournalFixture {
  JournalFixture() : path(dir.child("journal.log")) {}
  TempDir dir{"persist"};
  std::string path;
};

}  // namespace

BHG_TEST(persistence, journal_absent_file_is_not_an_error) {
  JournalFixture f;
  CollectingVisitor v;
  JournalScanReport report;
  BHG_CHECK(is_affirmative(
      scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report)));
  BHG_CHECK_EQ(report.records, 0u);
  BHG_CHECK(v.types.empty());
}

BHG_TEST(persistence, journal_roundtrip_preserves_records_and_sequences) {
  JournalFixture f;
  const std::vector<std::byte> bytes = build_journal(7, 12);
  write_bytes(f.path, bytes);
  CollectingVisitor v;
  JournalScanReport report;
  BHG_REQUIRE(is_affirmative(
      scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report)));
  BHG_CHECK_EQ(report.records, 7u);
  BHG_CHECK_EQ(v.types.size(), 7u);
  for (std::uint32_t i = 0; i < 7; ++i) {
    BHG_CHECK_EQ(v.seqs[i].value(), static_cast<std::uint64_t>(i) + 1u);
    BHG_CHECK_EQ(v.sizes[i], 12u);
  }
  BHG_CHECK_EQ(report.valid_bytes, bytes.size());
  BHG_CHECK(!report.torn_tail);
}

BHG_TEST(persistence, torn_tail_is_recovered_at_every_prefix_length) {
  JournalFixture f;
  const std::vector<std::byte> full = build_journal(3, 10);
  const std::size_t one_record = kJournalHeaderSize + 10u + kJournalTrailerSize;
  BHG_CHECK_EQ(one_record * 3u, full.size());

  // Exactly two whole records is not a torn tail; every byte beyond that is.
  for (std::size_t cut = one_record * 2u + 1u; cut < full.size(); ++cut) {
    write_bytes(f.path, std::vector<std::byte>(full.begin(),
                                               full.begin() + static_cast<std::ptrdiff_t>(cut)));
    CollectingVisitor v;
    JournalScanReport report;
    const Outcome o =
        scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report);
    BHG_REQUIRE(is_affirmative(o));
    BHG_CHECK_EQ(report.records, 2u);
    BHG_CHECK(report.torn_tail);
    BHG_CHECK_EQ(report.valid_bytes, one_record * 2u);
    BHG_CHECK_EQ(report.tail_bytes, cut - one_record * 2u);
    BHG_CHECK_EQ(report.records, 2u);
    BHG_REQUIRE(is_affirmative(truncate_file(f.path, report.valid_bytes)));
    CollectingVisitor v2;
    JournalScanReport report2;
    BHG_REQUIRE(is_affirmative(
        scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v2, report2)));
    BHG_CHECK_EQ(report2.records, 2u);
    BHG_CHECK(!report2.torn_tail);
  }
}

BHG_TEST(persistence, torn_tail_deeper_into_the_header_is_still_recovered) {
  JournalFixture f;
  const std::vector<std::byte> full = build_journal(2, 4);
  const std::size_t one_record = kJournalHeaderSize + 4u + kJournalTrailerSize;
  const std::size_t cut = one_record + 3u;
  write_bytes(f.path, std::vector<std::byte>(full.begin(),
                                             full.begin() + static_cast<std::ptrdiff_t>(cut)));
  CollectingVisitor v;
  JournalScanReport report;
  BHG_REQUIRE(is_affirmative(
      scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report)));
  BHG_CHECK_EQ(report.records, 1u);
  BHG_CHECK(report.torn_tail);
  BHG_CHECK_EQ(report.tail_bytes, 3u);
}

BHG_TEST(persistence, trailing_garbage_that_is_not_a_record_prefix_is_refused) {
  JournalFixture f;
  std::vector<std::byte> bytes = build_journal(2, 4);
  bytes.push_back(std::byte{0xFF});
  write_bytes(f.path, bytes);
  CollectingVisitor v;
  JournalScanReport report;
  const Outcome o = scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report);
  BHG_CHECK(o == Outcome::Corrupt);
  BHG_CHECK_EQ(report.records, 2u);
  BHG_CHECK(!report.torn_tail);
}

BHG_TEST(persistence, corrupt_magic_version_type_and_length_are_refused) {
  JournalFixture f;
  const std::vector<std::byte> good = build_journal(2, 8);
  const std::size_t second = kJournalHeaderSize + 8u + kJournalTrailerSize;

  const auto expect = [&](std::size_t offset, std::byte value, Outcome expected) {
    std::vector<std::byte> bytes = good;
    bytes[second + offset] = value;
    write_bytes(f.path, bytes);
    CollectingVisitor v;
    JournalScanReport report;
    const Outcome o = scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report);
    BHG_CHECK_EQ(static_cast<int>(o), static_cast<int>(expected));
    BHG_CHECK_EQ(report.records, 1u);
  };

  expect(0, std::byte{0x00}, Outcome::Corrupt);         // magic
  expect(4, std::byte{0x7F}, Outcome::Unsupported);     // version low byte
  expect(5, std::byte{0x01}, Outcome::Unsupported);     // version high byte
  expect(6, std::byte{0xEE}, Outcome::Invalid);         // record type
  expect(17, std::byte{0xFF}, Outcome::Oversized);      // payload length
  expect(20, std::byte{0xFF}, Outcome::Corrupt);        // header crc
}

BHG_TEST(persistence, payload_integrity_failure_is_never_truncated_away) {
  JournalFixture f;
  std::vector<std::byte> bytes = build_journal(2, 8);
  const std::size_t second = kJournalHeaderSize + 8u + kJournalTrailerSize;
  bytes[second + kJournalHeaderSize + 2u] ^= std::byte{0x01};
  write_bytes(f.path, bytes);
  CollectingVisitor v;
  JournalScanReport report;
  const Outcome o = scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report);
  BHG_CHECK(o == Outcome::Corrupt);
  BHG_CHECK_EQ(report.records, 1u);
  BHG_CHECK(!report.torn_tail);
  BHG_CHECK_EQ(read_bytes(f.path).size(), bytes.size());
}

BHG_TEST(persistence, record_crc_failure_is_refused) {
  JournalFixture f;
  std::vector<std::byte> bytes = build_journal(1, 8);
  bytes[bytes.size() - 1] ^= std::byte{0x80};
  write_bytes(f.path, bytes);
  CollectingVisitor v;
  JournalScanReport report;
  BHG_CHECK(scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report) ==
            Outcome::Corrupt);
  BHG_CHECK_EQ(report.records, 0u);
}

BHG_TEST(persistence, sequence_gap_and_regression_are_refused) {
  JournalFixture f;
  Writer w(4096);
  const std::vector<std::byte> payload = payload_of(4, std::byte{1});
  encode_record(w, RecordType::Boot, RecordSequence{1}, payload);
  encode_record(w, RecordType::Boot, RecordSequence{3}, payload);
  write_bytes(f.path, w.take());
  CollectingVisitor v;
  JournalScanReport report;
  BHG_CHECK(scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report) ==
            Outcome::Corrupt);

  Writer w2(4096);
  encode_record(w2, RecordType::Boot, RecordSequence{2}, payload);
  encode_record(w2, RecordType::Boot, RecordSequence{2}, payload);
  write_bytes(f.path, w2.take());
  CollectingVisitor v2;
  JournalScanReport report2;
  BHG_CHECK(scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{2}, v2, report2) ==
            Outcome::Corrupt);
}

BHG_TEST(persistence, all_zero_tail_is_treated_as_preallocation) {
  JournalFixture f;
  std::vector<std::byte> bytes = build_journal(1, 4);
  const std::size_t before = bytes.size();
  bytes.resize(before + 4096, std::byte{0});
  write_bytes(f.path, bytes);
  CollectingVisitor v;
  JournalScanReport report;
  BHG_REQUIRE(is_affirmative(
      scan_journal(f.path, 4096, 1u << 20, 10000, RecordSequence{1}, v, report)));
  BHG_CHECK_EQ(report.records, 1u);
  BHG_CHECK(report.zero_tail);
  BHG_CHECK(!report.torn_tail);
  BHG_CHECK_EQ(report.valid_bytes, before);
}

BHG_TEST(persistence, record_and_byte_bounds_are_enforced) {
  JournalFixture f;
  write_bytes(f.path, build_journal(10, 4));
  CollectingVisitor v;
  JournalScanReport report;
  BHG_CHECK(scan_journal(f.path, 4096, 1u << 20, 5, RecordSequence{1}, v, report) ==
            Outcome::Exhausted);
  CollectingVisitor v2;
  JournalScanReport report2;
  BHG_CHECK(scan_journal(f.path, 4, 16, 1000, RecordSequence{1}, v2, report2) ==
            Outcome::Oversized);
  CollectingVisitor v3;
  JournalScanReport report3;
  BHG_CHECK(scan_journal(f.path, 2, 1u << 20, 1000, RecordSequence{1}, v3, report3) ==
            Outcome::Oversized);
}

BHG_TEST(persistence, payload_rejection_aborts_replay) {
  JournalFixture f;
  write_bytes(f.path, build_journal(3, 4));
  CollectingVisitor v;
  v.fail_on_index = 1;
  JournalScanReport report;
  BHG_CHECK(scan_journal(f.path, 4096, 1u << 20, 1000, RecordSequence{1}, v, report) ==
            Outcome::Invalid);
  BHG_CHECK_EQ(report.records, 1u);
}

BHG_TEST(persistence, snapshot_roundtrip) {
  TempDir dir("snapshot");
  const std::string path = dir.child("lineage.snapshot");
  const std::vector<std::byte> payload = payload_of(64, std::byte{0x42});
  BHG_REQUIRE(is_affirmative(write_snapshot_atomic(path, RecordSequence{99}, payload)));
  const SnapshotReadResult r = read_snapshot(path, 1u << 20);
  BHG_CHECK(r.present);
  BHG_REQUIRE(is_affirmative(r.outcome));
  BHG_CHECK_EQ(r.snapshot_seq.value(), 99ull);
  BHG_CHECK(r.payload == payload);
  BHG_CHECK(!is_regular_file(path + ".tmp"));
}

BHG_TEST(persistence, snapshot_replacement_is_atomic_and_reclaims_staging) {
  TempDir dir("snapshot-replace");
  const std::string path = dir.child("lineage.snapshot");
  BHG_REQUIRE(is_affirmative(
      write_snapshot_atomic(path, RecordSequence{1}, payload_of(8, std::byte{1}))));
  BHG_REQUIRE(is_affirmative(
      write_snapshot_atomic(path, RecordSequence{2}, payload_of(16, std::byte{2}))));
  const SnapshotReadResult r = read_snapshot(path, 1u << 20);
  BHG_REQUIRE(is_affirmative(r.outcome));
  BHG_CHECK_EQ(r.snapshot_seq.value(), 2ull);
  BHG_CHECK_EQ(r.payload.size(), 16u);
  write_bytes(path + ".tmp", payload_of(32, std::byte{9}));
  clean_staging(path);
  BHG_CHECK(!is_regular_file(path + ".tmp"));
  BHG_CHECK(is_affirmative(read_snapshot(path, 1u << 20).outcome));
}

BHG_TEST(persistence, corrupt_snapshots_are_refused) {
  TempDir dir("snapshot-corrupt");
  const std::string path = dir.child("lineage.snapshot");
  const std::vector<std::byte> payload = payload_of(32, std::byte{0x11});
  BHG_REQUIRE(is_affirmative(write_snapshot_atomic(path, RecordSequence{5}, payload)));
  const std::vector<std::byte> good = read_bytes(path);

  const auto expect = [&](std::size_t offset, std::byte value, Outcome expected) {
    std::vector<std::byte> bytes = good;
    bytes[offset] = value;
    write_bytes(path, bytes);
    const SnapshotReadResult r = read_snapshot(path, 1u << 20);
    BHG_CHECK(r.present);
    BHG_CHECK_EQ(static_cast<int>(r.outcome), static_cast<int>(expected));
  };

  expect(0, std::byte{0x00}, Outcome::Corrupt);
  expect(4, std::byte{0x09}, Outcome::Unsupported);
  expect(6, std::byte{0x09}, Outcome::Unsupported);
  expect(17, std::byte{0xFF}, Outcome::Corrupt);
  expect(20, std::byte{0xFF}, Outcome::Corrupt);
  expect(good.size() - 1u, std::byte{0xFF}, Outcome::Corrupt);

  for (std::size_t cut = 0; cut < good.size(); cut += 5) {
    write_bytes(path, std::vector<std::byte>(good.begin(),
                                             good.begin() + static_cast<std::ptrdiff_t>(cut)));
    const SnapshotReadResult r = read_snapshot(path, 1u << 20);
    // The file exists, so it is present on disk, but no truncated snapshot may ever
    // be accepted as usable state.
    BHG_CHECK(r.present);
    BHG_CHECK(!is_affirmative(r.outcome));
  }

  std::vector<std::byte> extra = good;
  extra.push_back(std::byte{0});
  write_bytes(path, extra);
  BHG_CHECK(read_snapshot(path, 1u << 20).outcome == Outcome::Corrupt);

  (void)remove_file(path);
  const SnapshotReadResult absent = read_snapshot(path, 1u << 20);
  BHG_CHECK(!absent.present);
  BHG_CHECK(is_affirmative(absent.outcome));
}

BHG_TEST(persistence, store_open_reopen_advances_epoch_and_preserves_lineage) {
  const FencePolicy policy = default_policy();
  StoreConfig config;
  TempDir dir("store");
  config.root = dir.path();
  config.max_journal_bytes = 1u << 20;
  SeededNonceSource nonces(7);
  const SystemClock clock;

  {
    Store store(config, policy);
    BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
    BHG_CHECK_EQ(store.epoch().value(), 1ull);
    BHG_CHECK(!store.report().restart_detected);
    BHG_CHECK(store.report().policy_restored == false);
    Decision d;
    d.kind = DecisionKind::Diagnosis;
    d.outcome = Outcome::Ok;
    d.scope = path_scope(PathId{1});
    d.gens = make_gens(1, 1, 1, 1);
    d.classification = Classification::Blackhole;
    d.primary_reason = ReasonCode::CompleteDeliveryFailure;
    d.boot = store.boot();
    d.incarnation = store.incarnation();
    d.decided_at = clock.wall_now();
    d.policy = policy.version;
    d.id = derive_decision_id(d);
    BHG_REQUIRE(is_affirmative(store.commit_decision(d)));
    BHG_CHECK(store.lineage_accounting_closed());
    BHG_REQUIRE(is_affirmative(store.close()));
  }

  {
    Store store(config, policy);
    BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
    BHG_CHECK_EQ(store.epoch().value(), 2ull);
    BHG_CHECK(store.report().restart_detected);
    BHG_CHECK(store.report().policy_restored);
    BHG_CHECK_EQ(store.report().boot_count, 2u);
    BHG_CHECK_EQ(store.report().open_count, 2u);
    BHG_CHECK(store.state().total_decisions >= 1u);
    bool found = false;
    for (const LineageEntry& e : store.state().lineage) {
      if (e.type == RecordType::DecisionCommit) {
        found = true;
        BHG_CHECK(e.from_prior_incarnation);
        BHG_CHECK(e.classification == Classification::Blackhole);
      }
    }
    BHG_CHECK(found);
    BHG_REQUIRE(is_affirmative(store.close()));
  }
}

BHG_TEST(persistence, store_refuses_to_open_on_corrupt_journal) {
  const FencePolicy policy = default_policy();
  StoreConfig config;
  TempDir dir("store-corrupt");
  config.root = dir.path();
  config.max_journal_bytes = 1u << 20;
  SeededNonceSource nonces(11);
  const SystemClock clock;

  {
    Store store(config, policy);
    BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
    BHG_REQUIRE(is_affirmative(store.close()));
  }
  const std::string journal = dir.child("journal.log");
  std::vector<std::byte> bytes = read_bytes(journal);
  BHG_REQUIRE(bytes.size() > kJournalHeaderSize + 4u);
  bytes[kJournalHeaderSize + 1u] ^= std::byte{0x20};
  write_bytes(journal, bytes);

  Store store(config, policy);
  const Outcome o = store.open(nonces, clock.wall_now(), Provenance::Synthetic);
  BHG_CHECK(o == Outcome::Corrupt);
  BHG_CHECK(store.report().outcome == Outcome::Corrupt);
  BHG_CHECK(!store.is_open());
  BHG_CHECK(!store.report().detail.empty());
}

BHG_TEST(persistence, store_checkpoint_rotates_the_journal_and_bounds_growth) {
  const FencePolicy policy = default_policy();
  StoreConfig config;
  TempDir dir("store-rotate");
  config.root = dir.path();
  config.max_journal_bytes = 4096;
  SeededNonceSource nonces(13);
  const SystemClock clock;

  Store store(config, policy);
  BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  std::uint32_t committed = 0;
  for (std::uint32_t i = 0; i < 200; ++i) {
    Decision d;
    d.kind = DecisionKind::Diagnosis;
    d.outcome = Outcome::Ok;
    d.scope = path_scope(PathId{1});
    d.gens = make_gens(1, 1, 1, 1);
    d.classification = Classification::Lossy;
    d.primary_reason = ReasonCode::PartialLossObserved;
    d.boot = store.boot();
    d.incarnation = store.incarnation();
    d.decided_at = WallNs{1000 + i};
    d.policy = policy.version;
    d.id = derive_decision_id(d);
    const Outcome o = store.commit_decision(d);
    if (!is_affirmative(o)) {
      BHG_CHECK(o == Outcome::Exhausted);
      BHG_REQUIRE(is_affirmative(store.checkpoint(clock.wall_now())));
      BHG_CHECK(store.journal_bytes() < config.max_journal_bytes);
      BHG_REQUIRE(is_affirmative(store.commit_decision(d)));
    }
    ++committed;
  }
  BHG_CHECK_EQ(committed, 200u);
  BHG_CHECK(store.lineage_accounting_closed());
  BHG_REQUIRE(is_affirmative(store.checkpoint(clock.wall_now())));
  BHG_REQUIRE(is_affirmative(store.close()));

  Store reopened(config, policy);
  BHG_REQUIRE(is_affirmative(reopened.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  BHG_CHECK(reopened.report().snapshot_present);
  BHG_CHECK(reopened.state().total_decisions >= 200u);
  BHG_CHECK(reopened.lineage_accounting_closed());
  BHG_REQUIRE(is_affirmative(reopened.close()));
}

BHG_TEST(persistence, store_reclaims_a_torn_tail_on_open) {
  const FencePolicy policy = default_policy();
  StoreConfig config;
  TempDir dir("store-torn");
  config.root = dir.path();
  config.max_journal_bytes = 1u << 20;
  SeededNonceSource nonces(17);
  const SystemClock clock;
  {
    Store store(config, policy);
    BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
    BHG_REQUIRE(is_affirmative(store.close()));
  }
  const std::string journal = dir.child("journal.log");
  std::vector<std::byte> bytes = read_bytes(journal);
  const std::size_t before = bytes.size();
  Writer partial(64);
  const std::vector<std::byte> payload = payload_of(6, std::byte{1});
  encode_record(partial, RecordType::Boot, RecordSequence{999}, payload);
  const std::vector<std::byte> full = partial.take();
  bytes.insert(bytes.end(), full.begin(), full.begin() + 12);
  write_bytes(journal, bytes);

  Store store(config, policy);
  BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  BHG_CHECK(store.report().torn_tail_recovered);
  BHG_CHECK_EQ(store.report().reclaimed_tail_bytes, 12u);
  BHG_CHECK(file_size(journal) > before);
  BHG_REQUIRE(is_affirmative(store.close()));
}

BHG_TEST(persistence, store_refuses_corrupt_snapshot) {
  const FencePolicy policy = default_policy();
  StoreConfig config;
  TempDir dir("store-badsnap");
  config.root = dir.path();
  SeededNonceSource nonces(19);
  const SystemClock clock;
  {
    Store store(config, policy);
    BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
    BHG_REQUIRE(is_affirmative(store.checkpoint(clock.wall_now())));
    BHG_REQUIRE(is_affirmative(store.close()));
  }
  const std::string snapshot = dir.child("lineage.snapshot");
  std::vector<std::byte> bytes = read_bytes(snapshot);
  BHG_REQUIRE(bytes.size() > 8u);
  bytes[6] = std::byte{0x7F};
  write_bytes(snapshot, bytes);

  Store store(config, policy);
  const Outcome o = store.open(nonces, clock.wall_now(), Provenance::Synthetic);
  BHG_CHECK(o == Outcome::Unsupported);
  BHG_CHECK(!store.is_open());
}

BHG_TEST(persistence, store_rejects_an_unusable_root) {
  const FencePolicy policy = default_policy();
  StoreConfig bad;
  bad.root = std::string("\0invalid", 8);
  Store store(bad, policy);
  SeededNonceSource nonces(3);
  const SystemClock clock;
  BHG_CHECK(!is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  BHG_CHECK(!store.is_open());
}
