#include "blackhole/protocol/messages.hpp"

namespace bhg {

namespace {

Outcome enum8(Reader& r, std::uint8_t& raw, bool (*valid)(std::uint8_t)) noexcept {
  Outcome o = r.u8(raw);
  if (!is_affirmative(o)) return o;
  if (!valid(raw)) return Outcome::Invalid;
  return Outcome::Ok;
}

Outcome enum16(Reader& r, std::uint16_t& raw, bool (*valid)(std::uint16_t)) noexcept {
  Outcome o = r.u16(raw);
  if (!is_affirmative(o)) return o;
  if (!valid(raw)) return Outcome::Invalid;
  return Outcome::Ok;
}

}  // namespace

void encode(Writer& w, const SessionToken& v) noexcept {
  encode(w, v.session);
  encode(w, v.request);
  encode(w, v.seq);
  encode(w, v.client_boot);
  encode(w, v.client_incarnation);
}

Outcome decode(Reader& r, SessionToken& v) noexcept {
  SessionToken out{};
  Outcome o = decode(r, out.session);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.request);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.seq);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.client_boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.client_incarnation);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const HelloRequest& v) noexcept {
  encode(w, v.token);
  w.string(v.client_name, kMaxClientNameBytes);
  w.u8(static_cast<std::uint8_t>(v.label));
}

Outcome decode(Reader& r, HelloRequest& v) noexcept {
  HelloRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = r.string(out.client_name, kMaxClientNameBytes);
  if (!is_affirmative(o)) return o;
  std::uint8_t label = 0;
  o = enum8(r, label, &is_valid_provenance);
  if (!is_affirmative(o)) return o;
  out.label = static_cast<Provenance>(label);
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const HelloResponse& v) noexcept {
  encode(w, v.session);
  encode(w, v.server_boot);
  encode(w, v.server_incarnation);
  encode(w, v.epoch);
  encode(w, v.policy);
  w.u64(v.policy_fingerprint);
  w.u16(v.protocol_version);
  w.u8(static_cast<std::uint8_t>(v.label));
}

Outcome decode(Reader& r, HelloResponse& v) noexcept {
  HelloResponse out{};
  Outcome o = decode(r, out.session);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.server_boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.server_incarnation);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.epoch);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.policy);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.policy_fingerprint);
  if (!is_affirmative(o)) return o;
  o = r.u16(out.protocol_version);
  if (!is_affirmative(o)) return o;
  std::uint8_t label = 0;
  o = enum8(r, label, &is_valid_provenance);
  if (!is_affirmative(o)) return o;
  out.label = static_cast<Provenance>(label);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const SubmitEvidenceRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.evidence);
}

Outcome decode(Reader& r, SubmitEvidenceRequest& v) noexcept {
  SubmitEvidenceRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.evidence);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const SubmitEvidenceResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u8(static_cast<std::uint8_t>(v.admission));
  w.u16(static_cast<std::uint16_t>(v.reason));
}

Outcome decode(Reader& r, SubmitEvidenceResponse& v) noexcept {
  SubmitEvidenceResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint8_t admission = 0;
  o = enum8(r, admission, &is_valid_admission);
  if (!is_affirmative(o)) return o;
  out.admission = static_cast<EvidenceAdmission>(admission);
  std::uint16_t reason = 0;
  o = enum16(r, reason, &is_valid_reason_code);
  if (!is_affirmative(o)) return o;
  out.reason = static_cast<ReasonCode>(reason);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const EvaluateRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.scope);
  encode(w, v.gens);
}

Outcome decode(Reader& r, EvaluateRequest& v) noexcept {
  EvaluateRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const EvaluateResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  encode(w, v.decision);
}

Outcome decode(Reader& r, EvaluateResponse& v) noexcept {
  EvaluateResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const LocalizeRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.scope);
  encode(w, v.gens);
  w.u32(v.hop_count);
  if (v.probes.size() > kMaxProbesPerRequest) {
    w.u32(0xFFFFFFFFu);
    return;
  }
  w.u32(static_cast<std::uint32_t>(v.probes.size()));
  for (const HopProbe& p : v.probes) {
    w.u32(p.begin_hop);
    w.u32(p.end_hop);
    w.boolean(p.success);
  }
}

Outcome decode(Reader& r, LocalizeRequest& v) noexcept {
  LocalizeRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.hop_count);
  if (!is_affirmative(o)) return o;
  std::uint32_t count = 0;
  o = r.u32(count);
  if (!is_affirmative(o)) return o;
  if (count > kMaxProbesPerRequest) return Outcome::Oversized;
  if (r.remaining() < static_cast<std::size_t>(count) * 9u) return Outcome::Malformed;
  out.probes.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    HopProbe p;
    o = r.u32(p.begin_hop);
    if (!is_affirmative(o)) return o;
    o = r.u32(p.end_hop);
    if (!is_affirmative(o)) return o;
    o = r.boolean(p.success);
    if (!is_affirmative(o)) return o;
    out.probes.push_back(p);
  }
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const LocalizeResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  encode(w, v.decision);
}

Outcome decode(Reader& r, LocalizeResponse& v) noexcept {
  LocalizeResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const AckFenceRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.fence);
  encode(w, v.downstream);
}

Outcome decode(Reader& r, AckFenceRequest& v) noexcept {
  AckFenceRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.fence);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.downstream);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const AckFenceResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u16(static_cast<std::uint16_t>(v.reason));
}

Outcome decode(Reader& r, AckFenceResponse& v) noexcept {
  AckFenceResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint16_t reason = 0;
  o = enum16(r, reason, &is_valid_reason_code);
  if (!is_affirmative(o)) return o;
  out.reason = static_cast<ReasonCode>(reason);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const ReportEffectRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.fence);
  encode(w, v.downstream);
  w.boolean(v.verified);
}

Outcome decode(Reader& r, ReportEffectRequest& v) noexcept {
  ReportEffectRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.fence);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.downstream);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.verified);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const ReportEffectResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u16(static_cast<std::uint16_t>(v.reason));
}

Outcome decode(Reader& r, ReportEffectResponse& v) noexcept {
  ReportEffectResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint16_t reason = 0;
  o = enum16(r, reason, &is_valid_reason_code);
  if (!is_affirmative(o)) return o;
  out.reason = static_cast<ReasonCode>(reason);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const RestoreRequest& v) noexcept {
  encode(w, v.token);
  encode(w, v.scope);
  encode(w, v.gens);
}

Outcome decode(Reader& r, RestoreRequest& v) noexcept {
  RestoreRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.scope);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.gens);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const RestoreResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  encode(w, v.decision);
}

Outcome decode(Reader& r, RestoreResponse& v) noexcept {
  RestoreResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  o = decode(r, out.decision);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const LineageEntry& v) noexcept {
  w.u16(static_cast<std::uint16_t>(v.type));
  encode(w, v.seq);
  encode(w, v.at);
  encode(w, v.decision);
  encode(w, v.fence);
  encode(w, v.scope);
  encode(w, v.gens);
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u16(static_cast<std::uint16_t>(v.classification));
  w.u16(static_cast<std::uint16_t>(v.reason));
  encode(w, v.boot);
  encode(w, v.incarnation);
  w.boolean(v.from_prior_incarnation);
}

Outcome decode(Reader& r, LineageEntry& v) noexcept {
  LineageEntry out{};
  std::uint16_t type = 0;
  Outcome o = enum16(r, type, &is_valid_record_type);
  if (!is_affirmative(o)) return o;
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
  o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint16_t cls = 0;
  o = enum16(r, cls, &is_valid_classification);
  if (!is_affirmative(o)) return o;
  out.classification = static_cast<Classification>(cls);
  std::uint16_t reason = 0;
  o = enum16(r, reason, &is_valid_reason_code);
  if (!is_affirmative(o)) return o;
  out.reason = static_cast<ReasonCode>(reason);
  o = decode(r, out.boot);
  if (!is_affirmative(o)) return o;
  o = decode(r, out.incarnation);
  if (!is_affirmative(o)) return o;
  o = r.boolean(out.from_prior_incarnation);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const LineageRequest& v) noexcept {
  encode(w, v.token);
  w.u32(v.limit);
}

Outcome decode(Reader& r, LineageRequest& v) noexcept {
  LineageRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  o = r.u32(out.limit);
  if (!is_affirmative(o)) return o;
  if (out.limit > kMaxLineagePage) return Outcome::Oversized;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const LineageResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u32(static_cast<std::uint32_t>(v.entries.size()));
  for (const LineageEntry& e : v.entries) encode(w, e);
  w.u64(v.lineage_seen);
  w.u64(v.lineage_dropped);
}

Outcome decode(Reader& r, LineageResponse& v) noexcept {
  LineageResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint32_t count = 0;
  o = r.u32(count);
  if (!is_affirmative(o)) return o;
  if (count > kMaxLineagePage) return Outcome::Oversized;
  if (r.remaining() < static_cast<std::size_t>(count) * 32u) return Outcome::Malformed;
  out.entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    LineageEntry e;
    o = decode(r, e);
    if (!is_affirmative(o)) return o;
    out.entries.push_back(e);
  }
  o = r.u64(out.lineage_seen);
  if (!is_affirmative(o)) return o;
  o = r.u64(out.lineage_dropped);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

void encode(Writer& w, const CheckpointRequest& v) noexcept { encode(w, v.token); }

Outcome decode(Reader& r, CheckpointRequest& v) noexcept {
  CheckpointRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const CheckpointResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  encode(w, v.snapshot_seq);
}

Outcome decode(Reader& r, CheckpointResponse& v) noexcept {
  CheckpointResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  o = decode(r, out.snapshot_seq);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const StatsRequest& v) noexcept { encode(w, v.token); }

Outcome decode(Reader& r, StatsRequest& v) noexcept {
  StatsRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const StatsResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  const StatsPayload& s = v.stats;
  w.u64(s.evidence_submitted);
  w.u64(s.evidence_admitted);
  w.u64(s.evidence_rejected);
  w.u64(s.evaluations);
  w.u64(s.decisions_committed);
  w.u64(s.durable_commit_failures);
  w.u64(s.fence_intents_issued);
  w.u64(s.restorations_authorized);
  w.u64(s.restorations_refused);
  w.u64(s.refusals_fail_closed);
  w.u64(s.ledger_seen);
  w.u64(s.ledger_admitted);
  w.u64(s.ledger_rejected);
  w.u64(s.ledger_retained);
  w.u64(s.ledger_dropped);
  w.u64(s.fences_issue_attempts);
  w.u64(s.fences_issued);
  w.u64(s.fences_open);
  w.u64(s.fences_revoked);
  w.u64(s.fences_expired);
  w.u64(s.fences_fenced_by_restart);
  w.u64(s.sessions_established);
  w.u64(s.sessions_rejected);
  w.u64(s.requests_served);
  w.u64(s.requests_rejected);
  w.u64(s.frames_decoded);
  w.u64(s.frames_rejected);
  w.u64(s.boot_count);
  w.u64(s.open_count);
  encode(w, s.epoch);
  encode(w, s.policy);
  w.u64(s.policy_fingerprint);
  encode(w, s.last_record);
  w.u8(static_cast<std::uint8_t>(s.label));
}

Outcome decode(Reader& r, StatsResponse& v) noexcept {
  StatsResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  StatsPayload& s = out.stats;
  o = r.u64(s.evidence_submitted);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.evidence_admitted);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.evidence_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.evaluations);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.decisions_committed);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.durable_commit_failures);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fence_intents_issued);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.restorations_authorized);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.restorations_refused);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.refusals_fail_closed);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.ledger_seen);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.ledger_admitted);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.ledger_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.ledger_retained);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.ledger_dropped);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_issue_attempts);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_issued);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_open);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_revoked);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_expired);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.fences_fenced_by_restart);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.sessions_established);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.sessions_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.requests_served);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.requests_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.frames_decoded);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.frames_rejected);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.boot_count);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.open_count);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.epoch);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.policy);
  if (!is_affirmative(o)) return o;
  o = r.u64(s.policy_fingerprint);
  if (!is_affirmative(o)) return o;
  o = decode(r, s.last_record);
  if (!is_affirmative(o)) return o;
  std::uint8_t label = 0;
  o = enum8(r, label, &is_valid_provenance);
  if (!is_affirmative(o)) return o;
  s.label = static_cast<Provenance>(label);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const GoodbyeRequest& v) noexcept { encode(w, v.token); }

Outcome decode(Reader& r, GoodbyeRequest& v) noexcept {
  GoodbyeRequest out{};
  Outcome o = decode(r, out.token);
  if (!is_affirmative(o)) return o;
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const GoodbyeResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
}

Outcome decode(Reader& r, GoodbyeResponse& v) noexcept {
  GoodbyeResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  v = out;
  return Outcome::Ok;
}

void encode(Writer& w, const ErrorResponse& v) noexcept {
  w.u8(static_cast<std::uint8_t>(v.outcome));
  w.u16(static_cast<std::uint16_t>(v.reason));
  w.string(v.detail, kMaxDetailBytes);
}

Outcome decode(Reader& r, ErrorResponse& v) noexcept {
  ErrorResponse out{};
  std::uint8_t outcome = 0;
  Outcome o = enum8(r, outcome, &is_valid_outcome);
  if (!is_affirmative(o)) return o;
  out.outcome = static_cast<Outcome>(outcome);
  std::uint16_t reason = 0;
  o = enum16(r, reason, &is_valid_reason_code);
  if (!is_affirmative(o)) return o;
  out.reason = static_cast<ReasonCode>(reason);
  o = r.string(out.detail, kMaxDetailBytes);
  if (!is_affirmative(o)) return o;
  v = std::move(out);
  return Outcome::Ok;
}

}  // namespace bhg
