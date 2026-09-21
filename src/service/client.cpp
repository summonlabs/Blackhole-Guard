#include "blackhole/service/client.hpp"

#include <algorithm>

namespace bhg {

Client::Client(Provenance label, NonceSource* nonces)
    : nonces_(nonces != nullptr ? nonces : &default_nonces_), label_(label) {
  boot_ = BootId{nonces_->next()};
  incarnation_ = IncarnationId{nonces_->next()};
}

Client::~Client() { close(); }

void Client::close() {
  if (socket_ != kInvalidSocketHandle) {
    net_shutdown(socket_);
    net_close(socket_);
  }
  connected_ = false;
  established_ = false;
}

Outcome Client::connect_loopback(std::uint16_t port) {
  close();
  std::string detail;
  const Outcome o = net_connect_loopback(port, socket_, detail);
  if (!is_affirmative(o)) {
    last_error_ = detail;
    return o;
  }
  decoder_ = FrameDecoder();
  connected_ = true;
  seq_ = 0;
  request_ = 0;
  return Outcome::Ok;
}

SessionToken Client::next_token() {
  SessionToken t;
  t.session = hello_.session;
  t.request = RequestId{++request_};
  t.seq = SessionSequence{++seq_};
  t.client_boot = boot_;
  t.client_incarnation = incarnation_;
  return t;
}

Outcome Client::exchange(MessageType request_type, const std::vector<std::byte>& body,
                         MessageType expected, std::vector<std::byte>& response_body,
                         ErrorResponse& error) {
  response_body.clear();
  error = ErrorResponse{};
  if (!connected_) {
    last_error_ = "not connected";
    return Outcome::Unavailable;
  }
  Frame request;
  request.type = request_type;
  request.payload = body;
  Writer out(kDefaultMaxDocumentBytes);
  encode_frame(out, request);
  if (!out.ok()) {
    last_error_ = "request too large";
    return Outcome::Oversized;
  }
  std::string detail;
  Outcome o = net_send_all(socket_, out.span(), detail);
  if (!is_affirmative(o)) {
    last_error_ = detail;
    return o;
  }

  std::vector<std::byte> buffer(16 * 1024);
  while (true) {
    Frame response;
    const Outcome no = decoder_.next(response);
    if (no == Outcome::Ok) {
      if (response.type == MessageType::ErrorResponse) {
        Reader r(std::span<const std::byte>(response.payload.data(), response.payload.size()));
        o = decode(r, error);
        if (!is_affirmative(o)) return o;
        o = r.finish();
        if (!is_affirmative(o)) return o;
        last_error_ = error.detail;
        return error.outcome;
      }
      if (response.type != expected) {
        last_error_ = "unexpected response type";
        return Outcome::Malformed;
      }
      response_body = std::move(response.payload);
      return Outcome::Ok;
    }
    if (no != Outcome::NotFound) {
      last_error_ = "frame rejected";
      return no;
    }
    std::size_t got = 0;
    o = net_recv_some(socket_, std::span<std::byte>(buffer.data(), buffer.size()), got, detail);
    if (!is_affirmative(o)) {
      last_error_ = detail;
      return o;
    }
    if (got == 0) {
      last_error_ = "peer closed";
      connected_ = false;
      return Outcome::Unavailable;
    }
    o = decoder_.feed(std::span<const std::byte>(buffer.data(), got));
    if (!is_affirmative(o)) {
      last_error_ = "decoder refused bytes";
      return o;
    }
  }
}

Outcome Client::send_raw(MessageType type, const std::vector<std::byte>& body, MessageType expected,
                         std::vector<std::byte>& response_body, ErrorResponse& error) {
  return exchange(type, body, expected, response_body, error);
}

Outcome Client::hello(std::string_view name, HelloResponse& out) {
  HelloRequest req;
  req.token.session = SessionId{};
  req.token.request = RequestId{++request_};
  req.token.seq = SessionSequence{0};
  req.token.client_boot = boot_;
  req.token.client_incarnation = incarnation_;
  req.client_name.assign(name.substr(0, std::min<std::size_t>(name.size(), kMaxClientNameBytes)));
  req.label = label_;

  Writer w(256);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  const std::vector<std::byte> body = w.take();

  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::HelloRequest, body, MessageType::HelloResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  d = r.finish();
  if (!is_affirmative(d)) return d;
  hello_ = out;
  established_ = true;
  seq_ = 0;
  return Outcome::Ok;
}

Outcome Client::submit(const DeliveryEvidence& evidence, SubmitEvidenceResponse& out) {
  SubmitEvidenceRequest req;
  req.token = next_token();
  req.evidence = evidence;
  Writer w(2048);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::SubmitEvidenceRequest, w.take(),
                             MessageType::SubmitEvidenceResponse, response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::evaluate(const Scope& scope, const GenerationVector& gens, EvaluateResponse& out) {
  EvaluateRequest req;
  req.token = next_token();
  req.scope = scope;
  req.gens = gens;
  Writer w(512);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::EvaluateRequest, w.take(), MessageType::EvaluateResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::localize(const Scope& scope, const GenerationVector& gens, std::uint32_t hop_count,
                         std::span<const HopProbe> probes, LocalizeResponse& out) {
  LocalizeRequest req;
  req.token = next_token();
  req.scope = scope;
  req.gens = gens;
  req.hop_count = hop_count;
  req.probes.assign(probes.begin(), probes.end());
  Writer w(4096);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::LocalizeRequest, w.take(), MessageType::LocalizeResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::ack_fence(FenceId id, EvidenceSourceId downstream, AckFenceResponse& out) {
  AckFenceRequest req;
  req.token = next_token();
  req.fence = id;
  req.downstream = downstream;
  Writer w(256);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::AckFenceRequest, w.take(), MessageType::AckFenceResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::report_effect(FenceId id, EvidenceSourceId downstream, bool verified,
                              ReportEffectResponse& out) {
  ReportEffectRequest req;
  req.token = next_token();
  req.fence = id;
  req.downstream = downstream;
  req.verified = verified;
  Writer w(256);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::ReportEffectRequest, w.take(),
                             MessageType::ReportEffectResponse, response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::restore(const Scope& scope, const GenerationVector& gens, RestoreResponse& out) {
  RestoreRequest req;
  req.token = next_token();
  req.scope = scope;
  req.gens = gens;
  Writer w(512);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::RestoreRequest, w.take(), MessageType::RestoreResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::lineage(std::uint32_t limit, LineageResponse& out) {
  LineageRequest req;
  req.token = next_token();
  req.limit = limit;
  Writer w(256);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::LineageRequest, w.take(), MessageType::LineageResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::checkpoint(CheckpointResponse& out) {
  CheckpointRequest req;
  req.token = next_token();
  Writer w(128);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::CheckpointRequest, w.take(),
                             MessageType::CheckpointResponse, response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::stats(StatsResponse& out) {
  StatsRequest req;
  req.token = next_token();
  Writer w(128);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::StatsRequest, w.take(), MessageType::StatsResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, out);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

Outcome Client::goodbye() {
  GoodbyeRequest req;
  req.token = next_token();
  Writer w(128);
  encode(w, req);
  if (!w.ok()) return Outcome::Exhausted;
  std::vector<std::byte> response_body;
  ErrorResponse error;
  const Outcome o = exchange(MessageType::GoodbyeRequest, w.take(), MessageType::GoodbyeResponse,
                             response_body, error);
  if (!is_affirmative(o)) return o;
  GoodbyeResponse resp;
  Reader r(std::span<const std::byte>(response_body.data(), response_body.size()));
  Outcome d = decode(r, resp);
  if (!is_affirmative(d)) return d;
  return r.finish();
}

}  // namespace bhg
