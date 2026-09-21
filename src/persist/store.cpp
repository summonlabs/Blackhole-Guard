#include "blackhole/persist/store.hpp"

#include <algorithm>

#include "blackhole/core/hash.hpp"
#include "blackhole/core/path.hpp"

namespace bhg {

namespace {

constexpr const char* kJournalName = "journal.log";
constexpr const char* kSnapshotName = "lineage.snapshot";

void encode_lineage_entry(Writer& w, const LineageEntry& e) noexcept {
  w.u16(static_cast<std::uint16_t>(e.type));
  encode(w, e.seq);
  encode(w, e.at);
  encode(w, e.decision);
  encode(w, e.fence);
  encode(w, e.scope);
  encode(w, e.gens);
  w.u8(static_cast<std::uint8_t>(e.outcome));
  w.u16(static_cast<std::uint16_t>(e.classification));
  w.u16(static_cast<std::uint16_t>(e.reason));
  encode(w, e.boot);
  encode(w, e.incarnation);
  w.boolean(e.from_prior_incarnation);
}

Outcome decode_lineage_entry(Reader& r, LineageEntry& e) noexcept {
  LineageEntry out{};
  std::uint16_t type = 0;
  Outcome o = r.u16(type);
  if (!is_affirmative(o)) return o;
  if (!is_valid_record_type(type)) return Outcome::Invalid;
  out.type = static_cast<RecordType>(type);
  o = decode(r, out.seq);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.at);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.fence);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  std::uint8_t outcome = 0;
  o = r.u8(outcome);
  if (!is_affirmative(o)) return o;
  if (!is_valid_outcome(outcome)) return Outcome::Invalid;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint16_t cls = 0;
  o = r.u16(cls);
  if (!is_affirmative(o)) return o;
  if (!is_valid_classification(cls)) return Outcome::Invalid;
  out.classification = static_cast<Classification>(cls);
  std::uint16_t reason = 0;
  o = r.u16(reason);
  if (!is_affirmative(o)) return o;
  if (!is_valid_reason_code(reason)) return Outcome::Invalid;
  out.reason = static_cast<ReasonCode>(reason);
  o = decode(r, out.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.incarnation);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.from_prior_incarnation);
  if (!is_affirmative(o)) return o;
  e = out;
  return Outcome::Ok;
}

}  // namespace

void encode(Writer& w, const DurableState& s) noexcept {
  w.u16(s.format_version);
  encode(w, s.snapshot_seq);
  encode(w, s.epoch);
  w.u64(s.boot_count);
  w.u64(s.open_count);
  encode(w, s.policy);
  w.u64(s.policy_fingerprint);
  encode(w, s.last_boot);
  encode(w, s.last_incarnation);
  w.u64(s.total_decisions);
  w.u64(s.total_fences);
  w.u64(s.total_interruptions);
  w.u64(s.lineage_seen);
  w.u64(s.lineage_dropped);
  w.u32(static_cast<std::uint32_t>(s.lineage.size()));
  for (const LineageEntry& e : s.lineage) encode_lineage_entry(w, e);
  w.u32(static_cast<std::uint32_t>(s.fences.size()));
  for (const FenceStatePayload& f : s.fences) encode(w, f);
}

Outcome decode(Reader& r, DurableState& s) noexcept {
  DurableState out{};
  Outcome o = r.u16(out.format_version);
  if (!is_affirmative(o)) return o;
  if (out.format_version != kFormatVersion) return Outcome::Unsupported;
  o = decode(r, out.snapshot_seq);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.epoch);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.boot_count);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.open_count);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.policy);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.policy_fingerprint);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.last_boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.last_incarnation);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.total_decisions);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.total_fences);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.total_interruptions);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.lineage_seen);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.lineage_dropped);
  if (!is_affirmative(o)) return o;
  std::uint32_t lineage_count = 0;
  o = r.u32(lineage_count);
  if (!is_affirmative(o)) return o;
  if (lineage_count > 1u << 20) return Outcome::Oversized;
  if (r.remaining() < static_cast<std::size_t>(lineage_count) * 32u) return Outcome::Malformed;
  out.lineage.reserve(lineage_count);
  for (std::uint32_t i = 0; i < lineage_count; ++i) {
    LineageEntry e;
    o = decode_lineage_entry(r, e);
    if (!is_affirmative(o)) return o;
    out.lineage.push_back(e);
  }
  std::uint32_t fence_count = 0;
  o = r.u32(fence_count);
  if (!is_affirmative(o)) return o;
  if (fence_count > 1u << 20) return Outcome::Oversized;
  if (r.remaining() < static_cast<std::size_t>(fence_count) * 8u) return Outcome::Malformed;
  out.fences.reserve(fence_count);
  for (std::uint32_t i = 0; i < fence_count; ++i) {
    FenceStatePayload f;
    o = decode(r, f);
    if (!is_affirmative(o)) return o;
    out.fences.push_back(f);
  }
  s = std::move(out);
  return Outcome::Ok;
}

class Store::ReplayVisitor final : public RecordVisitor {
 public:
  ReplayVisitor(Store& store, RecordSequence snapshot_seq)
      : store_(store), snapshot_seq_(snapshot_seq) {}

  Outcome visit(RecordType type, RecordSequence seq, std::span<const std::byte> payload) override {
    if (seq <= snapshot_seq_) {
      // Already folded into the snapshot; still framed-validated by the scanner.
      ++skipped_;
      return Outcome::Ok;
    }
    return store_.apply_record(type, seq, payload);
  }

  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

 private:
  Store& store_;
  RecordSequence snapshot_seq_;
  std::uint64_t skipped_{0};
};

Store::Store(StoreConfig config, const FencePolicy& policy)
    : config_(std::move(config)), policy_(policy) {
  journal_path_ = join_path(config_.root, kJournalName);
  snapshot_path_ = join_path(config_.root, kSnapshotName);
}

Store::~Store() { (void)close(); }

Outcome Store::open(NonceSource& nonces, WallNs now, Provenance label) {
  report_ = OpenReport{};
  if (opened_) return Outcome::Invalid;
  if (!is_safe_path_component(kJournalName) || !is_safe_path_component(kSnapshotName)) {
    report_.outcome = Outcome::Invalid;
    report_.detail = "internal path constant rejected";
    return report_.outcome;
  }
  if (!ensure_directory(config_.root)) {
    report_.outcome = Outcome::Rejected;
    report_.detail = "state root not creatable";
    return report_.outcome;
  }
  clean_staging(snapshot_path_);

  const SnapshotReadResult snap = read_snapshot(snapshot_path_, config_.max_snapshot_payload);
  if (snap.present) {
    report_.snapshot_present = true;
    if (!is_affirmative(snap.outcome)) {
      report_.outcome = snap.outcome;
      report_.detail = "snapshot rejected: " + snap.detail;
      return report_.outcome;
    }
    Reader r(std::span<const std::byte>(snap.payload.data(), snap.payload.size()));
    Outcome o = decode(r, state_);
    if (!is_affirmative(o)) {
      report_.outcome = o;
      report_.detail = "snapshot payload rejected";
      return o;
    }
    o = r.finish();
    if (!is_affirmative(o)) {
      report_.outcome = o;
      report_.detail = "trailing bytes after snapshot payload";
      return o;
    }
    if (state_.snapshot_seq != snap.snapshot_seq) {
      report_.outcome = Outcome::Corrupt;
      report_.detail = "snapshot sequence disagrees with its payload";
      return report_.outcome;
    }
    if (state_.lineage.size() > config_.max_lineage_records) {
      report_.outcome = Outcome::Oversized;
      report_.detail = "snapshot lineage exceeds the configured bound";
      return report_.outcome;
    }
  }

  last_seq_ = state_.snapshot_seq;
  ReplayVisitor visitor(*this, state_.snapshot_seq);
  JournalScanReport scan;
  const Outcome so = scan_journal(
      journal_path_, config_.max_record_payload, config_.max_journal_bytes,
      config_.max_journal_records,
      RecordSequence{state_.snapshot_seq.value() + 1}, visitor, scan);
  if (!is_affirmative(so)) {
    report_.outcome = so;
    report_.detail = "journal rejected: " + scan.detail;
    return so;
  }
  report_.journal_records_replayed = scan.records;
  report_.torn_tail_recovered = scan.torn_tail;
  report_.zero_tail_observed = scan.zero_tail;
  if (scan.torn_tail) {
    const Outcome to = truncate_file(journal_path_, scan.valid_bytes);
    if (!is_affirmative(to)) {
      report_.outcome = to;
      report_.detail = "torn tail could not be reclaimed";
      return to;
    }
    report_.reclaimed_tail_bytes = scan.tail_bytes;
  }
  if (scan.records > 0 || report_.snapshot_present) report_.restart_detected = true;
  // Policy is durable definition state: it is restored from either the snapshot or
  // the replayed journal, and re-committed only when the effective policy differs.
  report_.policy_restored = state_.policy_fingerprint == policy_fingerprint(policy_);

  // Advance the incarnation boundary: every restart is a new authority instance.
  state_.boot_count += 1;
  state_.open_count += 1;
  state_.epoch = state_.epoch.next();
  boot_ = BootId{nonces.next()};
  incarnation_ = IncarnationId{nonces.next()};
  if (boot_.is_nil() || incarnation_.is_nil()) {
    report_.outcome = Outcome::Unavailable;
    report_.detail = "nonce source produced a nil identity";
    return report_.outcome;
  }
  report_.boot = boot_;
  report_.incarnation = incarnation_;
  report_.epoch = state_.epoch;
  report_.boot_count = state_.boot_count;
  report_.open_count = state_.open_count;
  report_.prior_lineage_entries = static_cast<std::uint64_t>(state_.lineage.size());
  for (LineageEntry& e : state_.lineage) e.from_prior_incarnation = true;

  const Outcome jo = journal_.open_append(journal_path_, false);
  if (!is_affirmative(jo)) {
    report_.outcome = jo;
    report_.detail = "journal not openable for append";
    return jo;
  }
  journal_bytes_ = file_size(journal_path_);
  opened_ = true;

  BootPayload boot;
  boot.boot = boot_;
  boot.incarnation = incarnation_;
  boot.epoch = state_.epoch;
  boot.started_at = now;
  boot.boot_count = state_.boot_count;
  boot.open_count = state_.open_count;
  boot.policy = policy_.version;
  boot.policy_fingerprint = policy_fingerprint(policy_);
  boot.provenance = label;
  Writer bw(256);
  encode(bw, boot);
  if (!bw.ok()) {
    report_.outcome = Outcome::Exhausted;
    report_.detail = "boot record not encodable";
    return report_.outcome;
  }
  RecordSequence seq{};
  Outcome o = append_commit(RecordType::Boot, bw.span(), seq);
  if (!is_affirmative(o)) {
    report_.outcome = o;
    report_.detail = "boot record not durable";
    return o;
  }

  // Policy is durable definition state: commit it whenever the effective policy
  // differs from what was persisted.
  if (!report_.policy_restored) {
    o = commit_policy(policy_);
    if (!is_affirmative(o)) {
      report_.outcome = o;
      report_.detail = "policy commit failed";
      return o;
    }
    state_.policy = policy_.version;
    state_.policy_fingerprint = policy_fingerprint(policy_);
  }

  // Fence every open pre-restart intent and surface each as an interruption.
  std::uint64_t fenced = 0;
  for (FenceStatePayload& f : state_.fences) {
    if (f.lifecycle != static_cast<std::uint8_t>(FenceLifecycle::Intent) &&
        f.lifecycle != static_cast<std::uint8_t>(FenceLifecycle::Acknowledged) &&
        f.lifecycle != static_cast<std::uint8_t>(FenceLifecycle::EffectReported)) {
      continue;
    }
    f.lifecycle = static_cast<std::uint8_t>(FenceLifecycle::FencedByRestart);
    f.terminal_reason = ReasonCode::RestartFencedPriorAuthority;
    InterruptionPayload ip;
    ip.reason = ReasonCode::RestartFencedPriorAuthority;
    ip.fence = f.intent.id;
    ip.decision = f.intent.decision;
    ip.scope = f.intent.scope;
    ip.prior_epoch = f.intent.gens.epoch;
    ip.prior_boot = f.intent.boot;
    ip.prior_incarnation = f.intent.incarnation;
    ip.at = now;
    o = commit_interruption(ip);
    if (!is_affirmative(o)) {
      report_.outcome = o;
      report_.detail = "interruption record not durable";
      return o;
    }
    o = commit_fence_state(RecordType::FenceRevokeCommit, f);
    if (!is_affirmative(o)) {
      report_.outcome = o;
      report_.detail = "fence revoke record not durable";
      return o;
    }
    ++fenced;
  }
  report_.fenced_prior_fences = fenced;
  report_.outcome = Outcome::Ok;
  report_.detail = "store open";
  return Outcome::Ok;
}

Outcome Store::apply_record(RecordType type, RecordSequence seq, std::span<const std::byte> payload) {
  Reader r(payload);
  switch (type) {
    case RecordType::Boot: {
      BootPayload p;
      Outcome o = decode(r, p);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      if (p.epoch <= state_.epoch && state_.epoch.value() != 0) {
        return Outcome::Corrupt;  // epoch regression
      }
      state_.epoch = p.epoch;
      state_.boot_count = p.boot_count;
      state_.open_count = p.open_count;
      state_.policy = p.policy;
      state_.last_boot = p.boot;
      state_.last_incarnation = p.incarnation;
      break;
    }
    case RecordType::PolicyCommit: {
      FencePolicy p;
      Outcome o = decode(r, p);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      state_.policy = p.version;
      state_.policy_fingerprint = policy_fingerprint(p);
      break;
    }
    case RecordType::Checkpoint: {
      CheckpointPayload p;
      Outcome o = decode(r, p);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      if (p.snapshot_seq > seq) return Outcome::Corrupt;
      break;
    }
    case RecordType::DecisionCommit:
    case RecordType::RestorationCommit:
    case RecordType::EvidenceLineageCommit: {
      Decision d;
      Outcome o = decode(r, d);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      LineageEntry e;
      e.type = type;
      e.seq = seq;
      e.at = d.decided_at;
      e.decision = d.id;
      e.scope = d.scope;
      e.gens = d.gens;
      e.outcome = d.outcome;
      e.classification = d.classification;
      e.reason = d.primary_reason;
      e.boot = d.boot;
      e.incarnation = d.incarnation;
      e.fence = d.has_intent ? d.intent.id : FenceId{};
      push_lineage(e);
      if (type == RecordType::DecisionCommit) ++state_.total_decisions;
      break;
    }
    case RecordType::FenceIntentCommit:
    case RecordType::FenceAckCommit:
    case RecordType::FenceEffectCommit:
    case RecordType::FenceRevokeCommit: {
      FenceStatePayload f;
      Outcome o = decode(r, f);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      auto it = std::find_if(state_.fences.begin(), state_.fences.end(),
                             [&](const FenceStatePayload& x) {
                               return x.intent.id == f.intent.id;
                             });
      if (it == state_.fences.end()) {
        if (state_.fences.size() >= config_.max_prior_fences) return Outcome::Exhausted;
        state_.fences.push_back(f);
      } else {
        *it = f;
      }
      if (type == RecordType::FenceIntentCommit) ++state_.total_fences;
      LineageEntry e;
      e.type = type;
      e.seq = seq;
      e.at = f.intent.issued_at;
      e.decision = f.intent.decision;
      e.fence = f.intent.id;
      e.scope = f.intent.scope;
      e.gens = f.intent.gens;
      e.outcome = Outcome::Ok;
      e.classification = Classification::NoEvidence;
      e.reason = f.terminal_reason != ReasonCode::None ? f.terminal_reason : f.intent.reason;
      e.boot = f.intent.boot;
      e.incarnation = f.intent.incarnation;
      push_lineage(e);
      break;
    }
    case RecordType::InterruptionCommit: {
      InterruptionPayload p;
      Outcome o = decode(r, p);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      ++state_.total_interruptions;
      LineageEntry e;
      e.type = type;
      e.seq = seq;
      e.at = p.at;
      e.decision = p.decision;
      e.fence = p.fence;
      e.scope = p.scope;
      e.gens.epoch = p.prior_epoch;
      e.outcome = Outcome::Interrupted;
      e.classification = Classification::Unknown;
      e.reason = p.reason;
      e.boot = p.prior_boot;
      e.incarnation = p.prior_incarnation;
      e.from_prior_incarnation = true;
      push_lineage(e);
      break;
    }
    case RecordType::EpochAdvance: {
      CoordinatorEpoch e;
      Outcome o = decode(r, e);
      if (!is_affirmative(o)) return o;
      o = r.finish();
      if (!is_affirmative(o)) return o;
      if (e <= state_.epoch) return Outcome::Corrupt;
      state_.epoch = e;
      break;
    }
  }
  last_seq_ = seq;
  return Outcome::Ok;
}

void Store::push_lineage(const LineageEntry& e) {
  ++state_.lineage_seen;
  if (state_.lineage.size() >= config_.max_lineage_records) {
    state_.lineage.erase(state_.lineage.begin());
    ++state_.lineage_dropped;
  }
  state_.lineage.push_back(e);
}

Outcome Store::append_commit(RecordType type, std::span<const std::byte> payload,
                             RecordSequence& out_seq) {
  if (!opened_) return Outcome::Invalid;
  if (journal_bytes_ >= config_.max_journal_bytes) return Outcome::Exhausted;
  const RecordSequence seq{last_seq_.value() + 1};
  const Outcome ao = journal_.append(type, seq, payload);
  if (!is_affirmative(ao)) return ao;
  const Outcome co = journal_.commit();
  if (!is_affirmative(co)) return co;
  journal_bytes_ = journal_.bytes_written();
  last_seq_ = seq;
  out_seq = seq;
  return Outcome::Ok;
}

Outcome Store::commit_policy(const FencePolicy& policy) {
  Writer w(1024);
  encode(w, policy);
  if (!w.ok()) return Outcome::Exhausted;
  RecordSequence seq{};
  const Outcome o = append_commit(RecordType::PolicyCommit, w.span(), seq);
  if (!is_affirmative(o)) return o;
  state_.policy = policy.version;
  state_.policy_fingerprint = policy_fingerprint(policy);
  return Outcome::Ok;
}

Outcome Store::commit_decision(const Decision& d) {
  Writer w(kDefaultMaxDocumentBytes);
  encode(w, d);
  if (!w.ok()) return Outcome::Exhausted;
  RecordSequence seq{};
  const Outcome o = append_commit(RecordType::DecisionCommit, w.span(), seq);
  if (!is_affirmative(o)) return o;
  ++state_.total_decisions;
  LineageEntry e;
  e.type = RecordType::DecisionCommit;
  e.seq = seq;
  e.at = d.decided_at;
  e.decision = d.id;
  e.scope = d.scope;
  e.gens = d.gens;
  e.outcome = d.outcome;
  e.classification = d.classification;
  e.reason = d.primary_reason;
  e.boot = d.boot;
  e.incarnation = d.incarnation;
  e.fence = d.has_intent ? d.intent.id : FenceId{};
  push_lineage(e);
  return Outcome::Ok;
}

Outcome Store::commit_fence_state(RecordType type, const FenceStatePayload& f) {
  Writer w(1024);
  encode(w, f);
  if (!w.ok()) return Outcome::Exhausted;
  RecordSequence seq{};
  const Outcome o = append_commit(type, w.span(), seq);
  if (!is_affirmative(o)) return o;
  auto it = std::find_if(state_.fences.begin(), state_.fences.end(),
                         [&](const FenceStatePayload& x) { return x.intent.id == f.intent.id; });
  if (it == state_.fences.end()) {
    if (state_.fences.size() >= config_.max_prior_fences) return Outcome::Exhausted;
    state_.fences.push_back(f);
    if (type == RecordType::FenceIntentCommit) ++state_.total_fences;
  } else {
    *it = f;
  }
  LineageEntry e;
  e.type = type;
  e.seq = seq;
  e.at = f.intent.issued_at;
  e.decision = f.intent.decision;
  e.fence = f.intent.id;
  e.scope = f.intent.scope;
  e.gens = f.intent.gens;
  e.outcome = Outcome::Ok;
  e.reason = f.terminal_reason != ReasonCode::None ? f.terminal_reason : f.intent.reason;
  e.boot = f.intent.boot;
  e.incarnation = f.intent.incarnation;
  push_lineage(e);
  return Outcome::Ok;
}

Outcome Store::commit_interruption(const InterruptionPayload& p) {
  Writer w(512);
  encode(w, p);
  if (!w.ok()) return Outcome::Exhausted;
  RecordSequence seq{};
  const Outcome o = append_commit(RecordType::InterruptionCommit, w.span(), seq);
  if (!is_affirmative(o)) return o;
  ++state_.total_interruptions;
  LineageEntry e;
  e.type = RecordType::InterruptionCommit;
  e.seq = seq;
  e.at = p.at;
  e.decision = p.decision;
  e.fence = p.fence;
  e.scope = p.scope;
  e.gens.epoch = p.prior_epoch;
  e.outcome = Outcome::Interrupted;
  e.classification = Classification::Unknown;
  e.reason = p.reason;
  e.boot = p.prior_boot;
  e.incarnation = p.prior_incarnation;
  // An interruption record is always about authority minted by a previous
  // incarnation, whether it is replayed or written during this open.
  e.from_prior_incarnation = true;
  push_lineage(e);
  return Outcome::Ok;
}

DurableState Store::build_snapshot_state() const {
  DurableState s = state_;
  s.snapshot_seq = last_seq_;
  return s;
}

Outcome Store::checkpoint(WallNs now) {
  if (!opened_) return Outcome::Invalid;
  (void)now;
  DurableState snap = build_snapshot_state();

  // Snapshot lineage is bounded; keep the newest records and preserve the exact
  // dropped/seen accounting so the closure identity survives the snapshot.
  Writer w(config_.max_snapshot_payload);
  encode(w, snap);
  if (!w.ok()) return Outcome::Exhausted;
  const Outcome so = write_snapshot_atomic(snapshot_path_, snap.snapshot_seq, w.span());
  if (!is_affirmative(so)) return so;

  const Outcome co = journal_.close();
  if (!is_affirmative(co)) return co;
  const Outcome ro = journal_.open_append(journal_path_, true);
  if (!is_affirmative(ro)) return ro;
  journal_bytes_ = 0;
  last_seq_ = snap.snapshot_seq;

  CheckpointPayload cp;
  cp.snapshot_seq = snap.snapshot_seq;
  cp.epoch = state_.epoch;
  Writer cw(128);
  encode(cw, cp);
  if (!cw.ok()) return Outcome::Exhausted;
  RecordSequence seq{};
  const Outcome ao = append_commit(RecordType::Checkpoint, cw.span(), seq);
  if (!is_affirmative(ao)) return ao;
  return Outcome::Ok;
}

Outcome Store::close() {
  const Outcome o = journal_.close();
  opened_ = false;
  return o;
}

}  // namespace bhg
