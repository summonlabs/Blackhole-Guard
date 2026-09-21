#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "blackhole/core/ids.hpp"
#include "blackhole/core/nonce.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/protocol/frame.hpp"
#include "blackhole/protocol/messages.hpp"
#include "blackhole/service/net.hpp"

namespace bhg {

/// Synchronous protocol client. One connection, one session, strictly increasing
/// request sequence. The client never fabricates session identity: it presents the
/// boot/incarnation minted by its nonce source and the server's session id.
class Client {
 public:
  explicit Client(Provenance label = Provenance::Synthetic, NonceSource* nonces = nullptr);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  Outcome connect_loopback(std::uint16_t port);
  Outcome hello(std::string_view name, HelloResponse& out);
  Outcome submit(const DeliveryEvidence& evidence, SubmitEvidenceResponse& out);
  Outcome evaluate(const Scope& scope, const GenerationVector& gens, EvaluateResponse& out);
  Outcome localize(const Scope& scope, const GenerationVector& gens, std::uint32_t hop_count,
                   std::span<const HopProbe> probes, LocalizeResponse& out);
  Outcome ack_fence(FenceId id, EvidenceSourceId downstream, AckFenceResponse& out);
  Outcome report_effect(FenceId id, EvidenceSourceId downstream, bool verified,
                        ReportEffectResponse& out);
  Outcome restore(const Scope& scope, const GenerationVector& gens, RestoreResponse& out);
  Outcome lineage(std::uint32_t limit, LineageResponse& out);
  Outcome checkpoint(CheckpointResponse& out);
  Outcome stats(StatsResponse& out);
  Outcome goodbye();
  void close();

  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] bool established() const noexcept { return established_; }
  [[nodiscard]] const HelloResponse& hello_info() const noexcept { return hello_; }
  [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }
  [[nodiscard]] BootId client_boot() const noexcept { return boot_; }
  [[nodiscard]] IncarnationId client_incarnation() const noexcept { return incarnation_; }

  /// Sends a request under an arbitrary token. Used by adversarial tests to prove
  /// that a foreign or replayed session token is refused.
  Outcome send_raw(MessageType type, const std::vector<std::byte>& body, MessageType expected,
                   std::vector<std::byte>& response_body, ErrorResponse& error);

 private:
  SessionToken next_token();
  Outcome exchange(MessageType request_type, const std::vector<std::byte>& body,
                   MessageType expected, std::vector<std::byte>& response_body,
                   ErrorResponse& error);

  SystemNonceSource default_nonces_;
  NonceSource* nonces_;
  Provenance label_;
  NetRuntime net_;
  socket_handle socket_{kInvalidSocketHandle};
  FrameDecoder decoder_;
  HelloResponse hello_{};
  bool connected_{false};
  bool established_{false};
  std::uint64_t seq_{0};
  std::uint64_t request_{0};
  BootId boot_{};
  IncarnationId incarnation_{};
  std::string last_error_;
};

}  // namespace bhg
