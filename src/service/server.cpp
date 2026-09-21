#include "blackhole/service/server.hpp"

#include <algorithm>
#include <vector>

#include "blackhole/protocol/frame.hpp"
#include "blackhole/version.hpp"

namespace bhg {

namespace {

constexpr std::size_t kRecvChunk = 16 * 1024;

/// Readiness poll slice. A blocked reader observes a stop request within this bound
/// on every platform, because shutdown() alone is not a portable wake-up for a
/// pending recv.
constexpr std::uint32_t kReadinessSliceMs = 50;

template <class T>
Outcome decode_message(const Frame& frame, T& out) {
  Reader reader(std::span<const std::byte>(frame.payload.data(), frame.payload.size()));
  const Outcome o = decode(reader, out);
  if (!is_affirmative(o)) return o;
  return reader.finish();
}

}  // namespace

Server::Server(Engine& engine, ServerConfig config)
    : engine_(engine),
      config_(config),
      sessions_(engine.policy()),
      max_frame_bytes_(engine.policy().max_frame_bytes) {}

Server::~Server() { (void)stop(); }

ServerStats Server::stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

std::size_t Server::live_connections() const {
  std::lock_guard<std::mutex> lock(connections_mutex_);
  // A finished connection may still occupy a slot until the accept loop reaps it, so
  // liveness is decided by the per-connection completion flag, not by slot presence.
  std::size_t live = 0;
  for (const ConnectionSlot& slot : slots_) {
    if (slot.done == nullptr || !slot.done->load()) ++live;
  }
  return live;
}

Outcome Server::start(std::uint16_t& bound_port) {
  bound_port = 0;
  if (running_.load()) return Outcome::Invalid;
  if (!net_.ok()) return Outcome::Unavailable;
  const ListenResult lr = net_listen_loopback(config_.port, config_.backlog);
  if (!is_affirmative(lr.outcome)) return lr.outcome;
  listener_ = lr.handle;
  port_ = lr.port;
  bound_port = lr.port;
  stopping_.store(false);
  running_.store(true);
  accept_thread_ = std::thread([this] { accept_loop(); });
  return Outcome::Ok;
}

void Server::reap_finished() {
  std::vector<std::thread> finished;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto it = slots_.begin(); it != slots_.end();) {
      if (it->done != nullptr && it->done->load()) {
        finished.push_back(std::move(it->thread));
        it = slots_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Joining happens with no lock held: the joined thread never needs this lock to
  // finish, but we never take the risk.
  for (std::thread& t : finished) {
    if (t.joinable()) t.join();
  }
}

void Server::join_all_connections() {
  std::vector<std::thread> all;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (ConnectionSlot& slot : slots_) {
      all.push_back(std::move(slot.thread));
    }
    slots_.clear();
  }
  for (std::thread& t : all) {
    if (t.joinable()) t.join();
  }
}

void Server::accept_loop() {
  while (!stopping_.load()) {
    socket_handle s = kInvalidSocketHandle;
    std::string detail;
    const Outcome o = net_accept(listener_, s, detail);
    if (!is_affirmative(o)) {
      if (stopping_.load()) break;
      continue;
    }
    if (stopping_.load()) {
      net_shutdown(s);
      net_close(s);
      break;
    }
    reap_finished();
    bool over_capacity = false;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      over_capacity = slots_.size() >= config_.max_connections;
    }
    if (over_capacity) {
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.accept_rejected;
      }
      net_shutdown(s);
      net_close(s);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.accepted;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    ConnectionSlot slot;
    slot.socket = s;
    slot.done = done;
    slot.thread = std::thread([this, s, done] { serve_connection(s, done); });
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      slots_.push_back(std::move(slot));
    }
  }
}

void Server::serve_connection(socket_handle s, const std::shared_ptr<std::atomic<bool>>& done) {
  ConnectionState conn;
  conn.decoder = FrameDecoder(max_frame_bytes_);
  std::vector<std::byte> buffer(kRecvChunk);
  bool closed = false;

  while (!stopping_.load() && !closed) {
    bool readable = false;
    const Outcome wo = net_wait_readable(s, kReadinessSliceMs, readable);
    if (!is_affirmative(wo)) break;
    if (!readable) continue;  // timeout slice: re-check the stop flag
    std::size_t got = 0;
    std::string detail;
    const Outcome ro =
        net_recv_some(s, std::span<std::byte>(buffer.data(), buffer.size()), got, detail);
    if (!is_affirmative(ro) || got == 0) break;
    const Outcome fo = conn.decoder.feed(std::span<const std::byte>(buffer.data(), got));
    if (!is_affirmative(fo)) {
      std::lock_guard<std::mutex> lock(stats_mutex_);
      ++stats_.frames_rejected;
      closed = true;
      break;
    }

    Frame frame;
    while (!closed) {
      const Outcome no = conn.decoder.next(frame);
      if (no == Outcome::NotFound) break;
      if (!is_affirmative(no)) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.frames_rejected;
        closed = true;
        break;
      }
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.frames_decoded;
      }
      if (!dispatch_frame(conn, frame, s)) {
        closed = true;
        break;
      }
    }
  }

  net_shutdown(s);
  if (conn.established) sessions_.release(conn.session);
  net_close(s);
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.connections_closed;
  }
  if (done != nullptr) done->store(true);
}

bool Server::send_response(socket_handle s, const Frame& response, bool counted_as_error) {
  Writer out(kDefaultMaxDocumentBytes);
  encode_frame(out, response);
  if (!out.ok()) return false;
  std::string detail;
  if (!is_affirmative(net_send_all(s, out.span(), detail))) return false;
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    if (counted_as_error) {
      ++stats_.requests_rejected;
    } else {
      ++stats_.requests_served;
    }
  }
  return true;
}

bool Server::dispatch_frame(ConnectionState& conn, const Frame& frame, socket_handle s) {
  ErrorResponse err;
  bool is_error = false;
  Frame response;

  const auto reject = [&](Outcome outcome, ReasonCode reason, const char* text) {
    err.outcome = outcome;
    err.reason = reason;
    err.detail = text;
    is_error = true;
  };
  const auto bad_payload = [&](Outcome o) {
    reject(o == Outcome::Oversized ? Outcome::Oversized : Outcome::Malformed,
           ReasonCode::InvalidEvidenceRejected, "malformed request payload");
  };

  const auto require_session = [&](const SessionToken& token) -> bool {
    if (!conn.established) {
      reject(Outcome::Rejected, ReasonCode::PolicyRefused, "session not established");
      return false;
    }
    if (token.session != conn.session) {
      // One connection may never act under another connection's session.
      reject(Outcome::Rejected, ReasonCode::PolicyRefused, "session authority mismatch");
      return false;
    }
    ReasonCode reason = ReasonCode::None;
    const Outcome vo = sessions_.validate(token.session, token.client_boot,
                                          token.client_incarnation, token.seq, reason);
    if (!is_affirmative(vo)) {
      reject(vo, reason, "session token refused");
      return false;
    }
    return true;
  };

  switch (frame.type) {
    case MessageType::HelloRequest: {
      HelloRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (conn.established) {
        reject(Outcome::Rejected, ReasonCode::DuplicateEvidence, "session already established");
        break;
      }
      SessionId id{};
      const Outcome eo = sessions_.establish(req.token.client_boot, req.token.client_incarnation,
                                             req.label, engine_.now_wall(), id);
      if (!is_affirmative(eo)) {
        {
          std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.sessions_rejected;
        }
        reject(eo, ReasonCode::BudgetExhausted, "session not established");
        break;
      }
      conn.session = id;
      conn.established = true;
      {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.sessions_established;
      }
      const OpenReport report = engine_.open_report();
      HelloResponse resp;
      resp.session = id;
      resp.server_boot = report.boot;
      resp.server_incarnation = report.incarnation;
      resp.epoch = engine_.epoch();
      resp.policy = engine_.policy_version();
      resp.policy_fingerprint = engine_.policy_fingerprint();
      resp.protocol_version = kProtocolVersion;
      resp.label = engine_.label();
      Writer w(kDefaultMaxDocumentBytes);
      encode(w, resp);
      if (!w.ok()) {
        reject(Outcome::Exhausted, ReasonCode::BudgetExhausted, "response not encodable");
        break;
      }
      response.type = MessageType::HelloResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::SubmitEvidenceRequest: {
      SubmitEvidenceRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      SubmitEvidenceResponse resp;
      EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
      ReasonCode reason = ReasonCode::None;
      resp.outcome = engine_.submit(req.evidence, admission, reason);
      resp.admission = admission;
      resp.reason = reason;
      // Rejections are reported through the typed admission/reason pair so the
      // caller sees exactly why an observation was refused.
      Writer w(256);
      encode(w, resp);
      response.type = MessageType::SubmitEvidenceResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::EvaluateRequest: {
      EvaluateRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      EvaluateResponse resp;
      Decision decision;
      resp.outcome = engine_.evaluate(req.scope, req.gens, decision);
      resp.decision = decision;
      Writer w(kDefaultMaxDocumentBytes);
      encode(w, resp);
      if (!w.ok()) {
        reject(Outcome::Exhausted, ReasonCode::BudgetExhausted, "response not encodable");
        break;
      }
      response.type = MessageType::EvaluateResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::LocalizeRequest: {
      LocalizeRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      LocalizeResponse resp;
      Decision decision;
      resp.outcome = engine_.localize(req.scope, req.gens, req.hop_count,
                                      std::span<const HopProbe>(req.probes.data(),
                                                               req.probes.size()),
                                      decision);
      resp.decision = decision;
      Writer w(kDefaultMaxDocumentBytes);
      encode(w, resp);
      if (!w.ok()) {
        reject(Outcome::Exhausted, ReasonCode::BudgetExhausted, "response not encodable");
        break;
      }
      response.type = MessageType::LocalizeResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::AckFenceRequest: {
      AckFenceRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      AckFenceResponse resp;
      ReasonCode reason = ReasonCode::None;
      resp.outcome = engine_.acknowledge_fence(req.fence, req.downstream, reason);
      resp.reason = reason;
      Writer w(256);
      encode(w, resp);
      response.type = MessageType::AckFenceResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::ReportEffectRequest: {
      ReportEffectRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      ReportEffectResponse resp;
      ReasonCode reason = ReasonCode::None;
      resp.outcome = engine_.report_effect(req.fence, req.downstream, req.verified, reason);
      resp.reason = reason;
      Writer w(256);
      encode(w, resp);
      response.type = MessageType::ReportEffectResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::RestoreRequest: {
      RestoreRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      RestoreResponse resp;
      Decision decision;
      resp.outcome = engine_.restore(req.scope, req.gens, decision);
      resp.decision = decision;
      Writer w(kDefaultMaxDocumentBytes);
      encode(w, resp);
      if (!w.ok()) {
        reject(Outcome::Exhausted, ReasonCode::BudgetExhausted, "response not encodable");
        break;
      }
      response.type = MessageType::RestoreResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::LineageRequest: {
      LineageRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      LineageResponse resp;
      std::vector<LineageEntry> entries;
      std::uint64_t seen = 0;
      std::uint64_t dropped = 0;
      resp.outcome = engine_.lineage(req.limit, entries, seen, dropped);
      resp.entries = std::move(entries);
      resp.lineage_seen = seen;
      resp.lineage_dropped = dropped;
      Writer w(kDefaultMaxDocumentBytes);
      encode(w, resp);
      if (!w.ok()) {
        reject(Outcome::Exhausted, ReasonCode::BudgetExhausted, "response not encodable");
        break;
      }
      response.type = MessageType::LineageResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::CheckpointRequest: {
      CheckpointRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      CheckpointResponse resp;
      RecordSequence seq{};
      resp.outcome = engine_.checkpoint(seq);
      resp.snapshot_seq = seq;
      Writer w(128);
      encode(w, resp);
      response.type = MessageType::CheckpointResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::StatsRequest: {
      StatsRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      if (!require_session(req.token)) break;
      StatsResponse resp;
      resp.outcome = Outcome::Ok;
      resp.stats = engine_stats_payload();
      Writer w(1024);
      encode(w, resp);
      response.type = MessageType::StatsResponse;
      response.payload = w.take();
      break;
    }
    case MessageType::GoodbyeRequest: {
      GoodbyeRequest req;
      const Outcome o = decode_message(frame, req);
      if (!is_affirmative(o)) {
        bad_payload(o);
        break;
      }
      GoodbyeResponse resp;
      resp.outcome = Outcome::Ok;
      Writer w(64);
      encode(w, resp);
      response.type = MessageType::GoodbyeResponse;
      response.payload = w.take();
      // The session is released by the connection teardown that follows.
      break;
    }
    default:
      reject(Outcome::Unsupported, ReasonCode::UnsupportedInput, "unsupported message type");
      break;
  }

  if (is_error) {
    Writer w(512);
    encode(w, err);
    response.type = MessageType::ErrorResponse;
    response.payload = w.take();
  }
  if (response.payload.empty() && response.type != MessageType::GoodbyeResponse &&
      response.type != MessageType::CheckpointResponse) {
    // Every branch above either filled a payload or set an error.
    if (!is_error) {
      ErrorResponse fallback;
      fallback.outcome = Outcome::Invalid;
      fallback.reason = ReasonCode::InternalInvariantViolation;
      fallback.detail = "handler produced no response";
      Writer w(256);
      encode(w, fallback);
      response.type = MessageType::ErrorResponse;
      response.payload = w.take();
      is_error = true;
    }
  }
  return send_response(s, response, is_error);
}

StatsPayload Server::engine_stats_payload() const {
  const EngineStats es = engine_.stats();
  const ServerStats ss = stats();
  const OpenReport report = engine_.open_report();
  StatsPayload p;
  p.evidence_submitted = es.evidence_submitted;
  p.evidence_admitted = es.evidence_admitted;
  p.evidence_rejected = es.evidence_rejected;
  p.evaluations = es.evaluations;
  p.decisions_committed = es.decisions_committed;
  p.durable_commit_failures = es.durable_commit_failures;
  p.fence_intents_issued = es.fence_intents_issued;
  p.restorations_authorized = es.restorations_authorized;
  p.restorations_refused = es.restorations_refused;
  p.refusals_fail_closed = es.refusals_fail_closed;
  p.sessions_established = ss.sessions_established;
  p.sessions_rejected = ss.sessions_rejected;
  p.requests_served = ss.requests_served;
  p.requests_rejected = ss.requests_rejected;
  p.frames_decoded = ss.frames_decoded;
  p.frames_rejected = ss.frames_rejected;
  p.boot_count = report.boot_count;
  p.open_count = report.open_count;
  p.epoch = report.epoch;
  p.policy = engine_.policy_version();
  p.policy_fingerprint = engine_.policy_fingerprint();
  p.last_record = engine_.last_record();
  p.label = engine_.label();
  return p;
}

Outcome Server::stop() {
  if (!running_.load() && listener_ == kInvalidSocketHandle) return Outcome::Ok;
  stopping_.store(true);

  // Wake the accept thread with a loopback self-connect; closing a listening
  // socket does not reliably wake a blocked accept on every platform.
  {
    socket_handle wake = kInvalidSocketHandle;
    std::string detail;
    if (is_affirmative(net_connect_loopback(port_, wake, detail))) {
      net_close(wake);
    }
  }
  // Wake blocked readers on every live connection so their threads can finish.
  {
    std::vector<socket_handle> live;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      live.reserve(slots_.size());
      for (const ConnectionSlot& slot : slots_) live.push_back(slot.socket);
    }
    for (const socket_handle h : live) net_shutdown(h);
  }

  // Joining never happens while holding a lock the joined threads need.
  if (accept_thread_.joinable()) accept_thread_.join();
  join_all_connections();

  net_close(listener_);
  sessions_.clear();
  running_.store(false);
  return Outcome::Ok;
}

}  // namespace bhg
