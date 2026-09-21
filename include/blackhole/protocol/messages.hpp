#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "blackhole/authority/authority.hpp"
#include "blackhole/core/canonical.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/generation.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/domain/scope.hpp"
#include "blackhole/evidence/ledger.hpp"
#include "blackhole/localize/localizer.hpp"
#include "blackhole/persist/store.hpp"

namespace bhg {

/// Session authority carried by every request. A request is only honoured when the
/// token matches the session established on *this* connection: one session can
/// never act under another session's identity, boot or incarnation.
struct SessionToken {
  SessionId session{};
  RequestId request{};
  SessionSequence seq{};
  BootId client_boot{};
  IncarnationId client_incarnation{};

  friend bool operator==(const SessionToken&, const SessionToken&) = default;
};

struct HelloRequest {
  SessionToken token{};
  std::string client_name{};
  Provenance label{Provenance::Synthetic};
};

struct HelloResponse {
  SessionId session{};
  BootId server_boot{};
  IncarnationId server_incarnation{};
  CoordinatorEpoch epoch{};
  PolicyVersion policy{};
  std::uint64_t policy_fingerprint{0};
  std::uint16_t protocol_version{0};
  Provenance label{Provenance::Synthetic};
};

struct SubmitEvidenceRequest {
  SessionToken token{};
  DeliveryEvidence evidence{};
};

struct SubmitEvidenceResponse {
  Outcome outcome{Outcome::Invalid};
  EvidenceAdmission admission{EvidenceAdmission::StructurallyInvalid};
  ReasonCode reason{ReasonCode::None};
};

struct EvaluateRequest {
  SessionToken token{};
  Scope scope{};
  GenerationVector gens{};
};

struct EvaluateResponse {
  Outcome outcome{Outcome::Invalid};
  Decision decision{};
};

struct LocalizeRequest {
  SessionToken token{};
  Scope scope{};
  GenerationVector gens{};
  std::uint32_t hop_count{0};
  std::vector<HopProbe> probes{};
};

struct LocalizeResponse {
  Outcome outcome{Outcome::Invalid};
  Decision decision{};
};

struct AckFenceRequest {
  SessionToken token{};
  FenceId fence{};
  EvidenceSourceId downstream{};
};

struct AckFenceResponse {
  Outcome outcome{Outcome::Invalid};
  ReasonCode reason{ReasonCode::None};
};

struct ReportEffectRequest {
  SessionToken token{};
  FenceId fence{};
  EvidenceSourceId downstream{};
  bool verified{false};
};

struct ReportEffectResponse {
  Outcome outcome{Outcome::Invalid};
  ReasonCode reason{ReasonCode::None};
};

struct RestoreRequest {
  SessionToken token{};
  Scope scope{};
  GenerationVector gens{};
};

struct RestoreResponse {
  Outcome outcome{Outcome::Invalid};
  Decision decision{};
};

struct LineageRequest {
  SessionToken token{};
  std::uint32_t limit{0};
};

struct LineageResponse {
  Outcome outcome{Outcome::Invalid};
  std::vector<LineageEntry> entries{};
  std::uint64_t lineage_seen{0};
  std::uint64_t lineage_dropped{0};
};

struct CheckpointRequest {
  SessionToken token{};
};

struct CheckpointResponse {
  Outcome outcome{Outcome::Invalid};
  RecordSequence snapshot_seq{};
};

struct StatsPayload {
  std::uint64_t evidence_submitted{0};
  std::uint64_t evidence_admitted{0};
  std::uint64_t evidence_rejected{0};
  std::uint64_t evaluations{0};
  std::uint64_t decisions_committed{0};
  std::uint64_t durable_commit_failures{0};
  std::uint64_t fence_intents_issued{0};
  std::uint64_t restorations_authorized{0};
  std::uint64_t restorations_refused{0};
  std::uint64_t refusals_fail_closed{0};

  std::uint64_t ledger_seen{0};
  std::uint64_t ledger_admitted{0};
  std::uint64_t ledger_rejected{0};
  std::uint64_t ledger_retained{0};
  std::uint64_t ledger_dropped{0};

  std::uint64_t fences_issue_attempts{0};
  std::uint64_t fences_issued{0};
  std::uint64_t fences_open{0};
  std::uint64_t fences_revoked{0};
  std::uint64_t fences_expired{0};
  std::uint64_t fences_fenced_by_restart{0};

  std::uint64_t sessions_established{0};
  std::uint64_t sessions_rejected{0};
  std::uint64_t requests_served{0};
  std::uint64_t requests_rejected{0};
  std::uint64_t frames_decoded{0};
  std::uint64_t frames_rejected{0};

  std::uint64_t boot_count{0};
  std::uint64_t open_count{0};
  CoordinatorEpoch epoch{};
  PolicyVersion policy{};
  std::uint64_t policy_fingerprint{0};
  RecordSequence last_record{};
  Provenance label{Provenance::Synthetic};

  [[nodiscard]] bool accounting_closed() const noexcept {
    return evidence_submitted == evidence_admitted + evidence_rejected &&
           evaluations == decisions_committed + durable_commit_failures &&
           ledger_seen == ledger_admitted + ledger_rejected &&
           requests_served + requests_rejected > 0;
  }
};

struct StatsRequest {
  SessionToken token{};
};

struct StatsResponse {
  Outcome outcome{Outcome::Invalid};
  StatsPayload stats{};
};

struct GoodbyeRequest {
  SessionToken token{};
};

struct GoodbyeResponse {
  Outcome outcome{Outcome::Ok};
};

struct ErrorResponse {
  Outcome outcome{Outcome::Invalid};
  ReasonCode reason{ReasonCode::None};
  std::string detail{};
};

// ---- codecs --------------------------------------------------------------------

inline constexpr std::size_t kMaxDetailBytes = 192;
inline constexpr std::size_t kMaxClientNameBytes = 64;
inline constexpr std::uint32_t kMaxLineagePage = 512;
inline constexpr std::uint32_t kMaxProbesPerRequest = kMaxLocalizationSets;

void encode(Writer& w, const SessionToken& v) noexcept;
Outcome decode(Reader& r, SessionToken& v) noexcept;
void encode(Writer& w, const HelloRequest& v) noexcept;
Outcome decode(Reader& r, HelloRequest& v) noexcept;
void encode(Writer& w, const HelloResponse& v) noexcept;
Outcome decode(Reader& r, HelloResponse& v) noexcept;
void encode(Writer& w, const SubmitEvidenceRequest& v) noexcept;
Outcome decode(Reader& r, SubmitEvidenceRequest& v) noexcept;
void encode(Writer& w, const SubmitEvidenceResponse& v) noexcept;
Outcome decode(Reader& r, SubmitEvidenceResponse& v) noexcept;
void encode(Writer& w, const EvaluateRequest& v) noexcept;
Outcome decode(Reader& r, EvaluateRequest& v) noexcept;
void encode(Writer& w, const EvaluateResponse& v) noexcept;
Outcome decode(Reader& r, EvaluateResponse& v) noexcept;
void encode(Writer& w, const LocalizeRequest& v) noexcept;
Outcome decode(Reader& r, LocalizeRequest& v) noexcept;
void encode(Writer& w, const LocalizeResponse& v) noexcept;
Outcome decode(Reader& r, LocalizeResponse& v) noexcept;
void encode(Writer& w, const AckFenceRequest& v) noexcept;
Outcome decode(Reader& r, AckFenceRequest& v) noexcept;
void encode(Writer& w, const AckFenceResponse& v) noexcept;
Outcome decode(Reader& r, AckFenceResponse& v) noexcept;
void encode(Writer& w, const ReportEffectRequest& v) noexcept;
Outcome decode(Reader& r, ReportEffectRequest& v) noexcept;
void encode(Writer& w, const ReportEffectResponse& v) noexcept;
Outcome decode(Reader& r, ReportEffectResponse& v) noexcept;
void encode(Writer& w, const RestoreRequest& v) noexcept;
Outcome decode(Reader& r, RestoreRequest& v) noexcept;
void encode(Writer& w, const RestoreResponse& v) noexcept;
Outcome decode(Reader& r, RestoreResponse& v) noexcept;
void encode(Writer& w, const LineageRequest& v) noexcept;
Outcome decode(Reader& r, LineageRequest& v) noexcept;
void encode(Writer& w, const LineageResponse& v) noexcept;
Outcome decode(Reader& r, LineageResponse& v) noexcept;
void encode(Writer& w, const CheckpointRequest& v) noexcept;
Outcome decode(Reader& r, CheckpointRequest& v) noexcept;
void encode(Writer& w, const CheckpointResponse& v) noexcept;
Outcome decode(Reader& r, CheckpointResponse& v) noexcept;
void encode(Writer& w, const StatsRequest& v) noexcept;
Outcome decode(Reader& r, StatsRequest& v) noexcept;
void encode(Writer& w, const StatsResponse& v) noexcept;
Outcome decode(Reader& r, StatsResponse& v) noexcept;
void encode(Writer& w, const GoodbyeRequest& v) noexcept;
Outcome decode(Reader& r, GoodbyeRequest& v) noexcept;
void encode(Writer& w, const GoodbyeResponse& v) noexcept;
Outcome decode(Reader& r, GoodbyeResponse& v) noexcept;
void encode(Writer& w, const ErrorResponse& v) noexcept;
Outcome decode(Reader& r, ErrorResponse& v) noexcept;
void encode(Writer& w, const LineageEntry& v) noexcept;
Outcome decode(Reader& r, LineageEntry& v) noexcept;

}  // namespace bhg
