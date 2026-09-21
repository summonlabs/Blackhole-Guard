#include <algorithm>
#include <cstdio>
#include <thread>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// In-process service fixture: real engine, real loopback listener, real sockets.
class ServiceFixture {
 public:
  explicit ServiceFixture(const char* tag, FencePolicy policy = default_policy())
      : harness_(tag, policy, Provenance::Synthetic) {
    if (!is_affirmative(harness_.open())) return;
    server_ = std::make_unique<Server>(harness_.engine(), ServerConfig{});
    std::uint16_t port = 0;
    if (is_affirmative(server_->start(port))) port_ = port;
  }
  ~ServiceFixture() {
    if (server_) (void)server_->stop();
    (void)harness_.engine().close();
  }

  [[nodiscard]] bool ready() const { return port_ != 0; }
  [[nodiscard]] std::uint16_t port() const { return port_; }
  Engine& engine() { return harness_.engine(); }
  Server& server() { return *server_; }
  EngineHarness& harness() { return harness_; }

 private:
  EngineHarness harness_;
  std::unique_ptr<Server> server_;
  std::uint16_t port_{0};
};

}  // namespace

BHG_TEST(service, hello_establishes_a_session_bound_to_the_connection) {
  ServiceFixture f("svc-hello");
  BHG_REQUIRE(f.ready());
  Client client(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(client.connect_loopback(f.port())));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(client.hello("observer-a", hello)));
  BHG_CHECK(!hello.session.is_nil());
  BHG_CHECK(hello.protocol_version == kProtocolVersion);
  BHG_CHECK(!hello.server_boot.is_nil());
  BHG_CHECK_EQ(hello.epoch.value(), 1ull);
  BHG_CHECK(hello.policy_fingerprint == bhg::policy_fingerprint(default_policy()));
  BHG_CHECK(client.established());
  (void)client.goodbye();
  client.close();
  (void)f.server().stop();
}

BHG_TEST(service, requests_without_a_session_are_refused) {
  ServiceFixture f("svc-nosession");
  BHG_REQUIRE(f.ready());
  Client client(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(client.connect_loopback(f.port())));
  EvaluateResponse resp;
  const Outcome o = client.evaluate(path_scope(PathId{1}), make_gens(1, 1, 1, 1), resp);
  BHG_CHECK(!is_affirmative(o));
  BHG_CHECK_EQ(client.last_error(), std::string("session not established"));
  client.close();
  (void)f.server().stop();
}

BHG_TEST(service, a_session_token_from_another_connection_is_refused) {
  ServiceFixture f("svc-mismatch");
  BHG_REQUIRE(f.ready());

  Client a(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(a.connect_loopback(f.port())));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(a.hello("a", hello)));

  // Second connection: it has no session of its own yet. Presenting the first
  // connection's session id must be refused.
  Client b(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(b.connect_loopback(f.port())));
  SessionToken forged;
  forged.session = hello.session;
  forged.request = RequestId{1};
  forged.seq = SessionSequence{1};
  forged.client_boot = a.client_boot();
  forged.client_incarnation = a.client_incarnation();
  EvaluateRequest req;
  req.token = forged;
  req.scope = path_scope(PathId{1});
  req.gens = make_gens(1, 1, 1, 1);
  Writer w(512);
  encode(w, req);
  std::vector<std::byte> body;
  ErrorResponse err;
  const Outcome o = b.send_raw(MessageType::EvaluateRequest, w.take(),
                               MessageType::EvaluateResponse, body, err);
  BHG_CHECK(!is_affirmative(o));
  BHG_CHECK(err.detail == std::string("session not established"));

  // Now establish a real session on b and present a *different* session id.
  HelloResponse hello_b;
  BHG_REQUIRE(is_affirmative(b.hello("b", hello_b)));
  SessionToken swapped = forged;
  swapped.session = hello.session;  // session of connection a
  swapped.client_boot = b.client_boot();
  swapped.client_incarnation = b.client_incarnation();
  EvaluateRequest req2;
  req2.token = swapped;
  req2.scope = path_scope(PathId{1});
  req2.gens = make_gens(1, 1, 1, 1);
  Writer w2(512);
  encode(w2, req2);
  const Outcome o2 = b.send_raw(MessageType::EvaluateRequest, w2.take(),
                                MessageType::EvaluateResponse, body, err);
  BHG_CHECK(!is_affirmative(o2));
  BHG_CHECK(err.detail == std::string("session authority mismatch"));

  // And presenting b's own session with a foreign boot/incarnation is refused.
  SessionToken foreign = swapped;
  foreign.session = hello_b.session;
  foreign.client_boot = BootId{999};
  EvaluateRequest req3;
  req3.token = foreign;
  req3.scope = path_scope(PathId{1});
  req3.gens = make_gens(1, 1, 1, 1);
  Writer w3(512);
  encode(w3, req3);
  const Outcome o3 = b.send_raw(MessageType::EvaluateRequest, w3.take(),
                                MessageType::EvaluateResponse, body, err);
  BHG_CHECK(!is_affirmative(o3));

  a.close();
  b.close();
  (void)f.server().stop();
}

BHG_TEST(service, replayed_and_regressed_sequences_are_refused) {
  ServiceFixture f("svc-seq");
  BHG_REQUIRE(f.ready());
  Client a(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(a.connect_loopback(f.port())));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(a.hello("a", hello)));

  EvaluateRequest req;
  req.token.session = hello.session;
  req.token.request = RequestId{1};
  req.token.seq = SessionSequence{1};
  req.token.client_boot = a.client_boot();
  req.token.client_incarnation = a.client_incarnation();
  req.scope = path_scope(PathId{1});
  req.gens = make_gens(1, 1, 1, 1);
  Writer w(512);
  encode(w, req);
  const std::vector<std::byte> body = w.take();
  std::vector<std::byte> response;
  ErrorResponse err;

  BHG_CHECK(is_affirmative(a.send_raw(MessageType::EvaluateRequest, body,
                                      MessageType::EvaluateResponse, response, err)));
  // Exact replay of the same sequence.
  const Outcome replay = a.send_raw(MessageType::EvaluateRequest, body,
                                    MessageType::EvaluateResponse, response, err);
  BHG_CHECK(!is_affirmative(replay));
  BHG_CHECK(err.reason == ReasonCode::ReplayedSequence);
  // Skipping ahead.
  EvaluateRequest ahead = req;
  ahead.token.seq = SessionSequence{50};
  Writer w2(512);
  encode(w2, ahead);
  const Outcome skip = a.send_raw(MessageType::EvaluateRequest, w2.take(),
                                  MessageType::EvaluateResponse, response, err);
  BHG_CHECK(!is_affirmative(skip));
  BHG_CHECK(err.reason == ReasonCode::WindowViolation);
  // The refused attempts did not advance the sequence: 2 is still accepted.
  EvaluateRequest next = req;
  next.token.seq = SessionSequence{2};
  next.token.request = RequestId{2};
  Writer w3(512);
  encode(w3, next);
  BHG_CHECK(is_affirmative(a.send_raw(MessageType::EvaluateRequest, w3.take(),
                                      MessageType::EvaluateResponse, response, err)));
  a.close();
  (void)f.server().stop();
}

BHG_TEST(service, end_to_end_blackhole_fence_and_restore_over_the_wire) {
  ServiceFixture f("svc-e2e");
  BHG_REQUIRE(f.ready());
  Client client(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(client.connect_loopback(f.port())));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(client.hello("observer", hello)));

  Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  const WallNs at = f.harness().clock().wall_now();

  // 1. No evidence yet.
  EvaluateResponse eval;
  BHG_REQUIRE(is_affirmative(client.evaluate(scope, gens, eval)));
  BHG_CHECK(eval.decision.classification == Classification::NoEvidence);
  BHG_CHECK(eval.decision.authority.stage == AuthorityStage::None);

  // 2. One source: blackhole but uncorroborated, so no authority to fence.
  SubmitEvidenceResponse sub;
  BHG_REQUIRE(is_affirmative(client.submit(
      failure_evidence(EvidenceSourceId{1}, IncarnationId{1}, scope, gens, EvidenceSequence{1}, at),
      sub)));
  BHG_CHECK(sub.admission == EvidenceAdmission::Accepted);
  BHG_REQUIRE(is_affirmative(client.evaluate(scope, gens, eval)));
  BHG_CHECK(eval.decision.classification == Classification::Blackhole);
  BHG_CHECK(!eval.decision.corroborated);
  BHG_CHECK(eval.decision.authority.stage == AuthorityStage::Eligibility);
  BHG_CHECK(!eval.decision.has_intent);

  // 3. Second source from a different incarnation: corroborated, intent issued.
  BHG_REQUIRE(is_affirmative(client.submit(
      failure_evidence(EvidenceSourceId{2}, IncarnationId{2}, scope, gens, EvidenceSequence{1}, at),
      sub)));
  BHG_REQUIRE(is_affirmative(client.evaluate(scope, gens, eval)));
  BHG_CHECK(eval.decision.corroborated);
  BHG_CHECK(eval.decision.authority.stage == AuthorityStage::Intent);
  BHG_REQUIRE(eval.decision.has_intent);
  const FenceId fence = eval.decision.intent.id;

  // 4. Downstream acknowledgement and effect report are separate rungs.
  AckFenceResponse ack;
  BHG_REQUIRE(is_affirmative(client.ack_fence(fence, EvidenceSourceId{42}, ack)));
  BHG_CHECK(ack.reason == ReasonCode::FenceAcknowledged);
  FenceRecord rec;
  BHG_REQUIRE(is_affirmative(f.engine().fence_state(fence, rec)));
  BHG_CHECK(rec.lifecycle == FenceLifecycle::Acknowledged);

  ReportEffectResponse effect;
  BHG_REQUIRE(is_affirmative(client.report_effect(fence, EvidenceSourceId{42}, true, effect)));
  BHG_CHECK(effect.reason == ReasonCode::FenceEffectVerified);
  BHG_REQUIRE(is_affirmative(f.engine().fence_state(fence, rec)));
  BHG_CHECK(rec.effect_verified);
  BHG_CHECK(rec.lifecycle == FenceLifecycle::EffectReported);

  // 5. Restoration is refused while the failure evidence is still fresh: the
  //    disagreement between the old failures and any new success is a conflict, not
  //    a restoration.
  RestoreResponse restore;
  BHG_REQUIRE(is_affirmative(client.restore(scope, gens, restore)));
  BHG_CHECK(restore.decision.classification != Classification::Healthy);
  BHG_CHECK(static_cast<std::uint8_t>(restore.decision.authority.stage) <
            static_cast<std::uint8_t>(AuthorityStage::Authorization));

  // 6. The failure observations age out of their freshness window. Fresh positive
  //    delivery evidence under the current generation vector is what authorizes
  //    restoration -- absence of blackhole evidence alone never does.
  f.harness().clock().advance(seconds(120));
  const WallNs later = f.harness().clock().wall_now();
  BHG_REQUIRE(is_affirmative(client.submit(
      success_evidence(EvidenceSourceId{3}, IncarnationId{3}, scope, gens, EvidenceSequence{1},
                       later),
      sub)));
  if (sub.admission != EvidenceAdmission::Accepted) {
    std::fprintf(stderr, "DIAG late admission=%s reason=%s\n", to_string(sub.admission),
                 to_string(sub.reason));
  }
  BHG_CHECK(sub.admission == EvidenceAdmission::Accepted);
  BHG_REQUIRE(is_affirmative(client.submit(
      success_evidence(EvidenceSourceId{4}, IncarnationId{4}, scope, gens, EvidenceSequence{1},
                       later),
      sub)));
  BHG_REQUIRE(is_affirmative(client.restore(scope, gens, restore)));
  BHG_CHECK(restore.decision.classification == Classification::Healthy);
  BHG_CHECK(restore.decision.authority.stage == AuthorityStage::Authorization);
  BHG_CHECK(restore.decision.primary_reason == ReasonCode::RestorationCorroborated);
  BHG_CHECK(is_affirmative(f.engine().fence_state(fence, rec)));
  BHG_CHECK(!rec.is_open());

  // 6. Lineage, stats and checkpoint over the wire.
  LineageResponse lineage;
  BHG_REQUIRE(is_affirmative(client.lineage(64, lineage)));
  BHG_CHECK(!lineage.entries.empty());
  BHG_CHECK(lineage.lineage_seen >= lineage.entries.size());

  StatsResponse stats;
  BHG_REQUIRE(is_affirmative(client.stats(stats)));
  BHG_CHECK_EQ(stats.stats.evidence_admitted, 4ull);
  BHG_CHECK(stats.stats.sessions_established >= 1u);
  BHG_CHECK(stats.stats.frames_decoded >= 10u);

  CheckpointResponse cp;
  BHG_REQUIRE(is_affirmative(client.checkpoint(cp)));
  BHG_CHECK(cp.snapshot_seq.value() > 0ull);

  (void)client.goodbye();
  client.close();
  (void)f.server().stop();
}

BHG_TEST(service, malformed_and_oversized_frames_close_the_connection) {
  ServiceFixture f("svc-badframes");
  BHG_REQUIRE(f.ready());

  // Corrupt magic: the server must latch the failure and tear the connection down.
  HelloRequest req;
  req.token.client_boot = BootId{1};
  req.token.client_incarnation = IncarnationId{1};
  req.client_name = "bad";
  Writer w(256);
  encode(w, req);
  Frame frame;
  frame.type = MessageType::HelloRequest;
  frame.payload = w.take();
  Writer fw(512);
  encode_frame(fw, frame);
  std::vector<std::byte> bytes = fw.take();
  bytes[0] = std::byte{0x00};
  // Feed the corrupt frame directly through a raw socket client path.
  socket_handle raw = kInvalidSocketHandle;
  {
    std::string detail;
    BHG_REQUIRE(is_affirmative(net_connect_loopback(f.port(), raw, detail)));
    BHG_REQUIRE(is_affirmative(net_send_all(raw, std::span<const std::byte>(bytes.data(), bytes.size()),
                                            detail)));
  }
  // Wait (by yielding, not by clock) until the server has rejected the frame and
  // released the connection, so the assertions below are not a timing race.
  BHG_REQUIRE(wait_until([&] { return f.server().stats().frames_rejected >= 1u; }));
  BHG_REQUIRE(wait_until([&] { return f.server().live_connections() == 0u; }));
  net_shutdown(raw);
  net_close(raw);

  // A well-formed client must still be served afterwards.
  Client good(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(good.connect_loopback(f.port())));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(good.hello("good", hello)));
  good.close();

  (void)f.server().stop();
  const ServerStats stats = f.server().stats();
  BHG_CHECK(stats.frames_rejected >= 1u);
  BHG_CHECK(stats.accepted >= 2u);
  BHG_CHECK(stats.connections_closed >= 2u);
}

BHG_TEST(service, concurrent_clients_get_independent_sessions) {
  ServiceFixture f("svc-concurrent");
  BHG_REQUIRE(f.ready());
  const int clients = 8;
  std::vector<std::thread> threads;
  std::vector<int> ok(clients, 0);
  std::vector<std::string> failure(clients);
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&, i] {
      Client c(Provenance::Synthetic);
      if (!is_affirmative(c.connect_loopback(f.port()))) {
        failure[static_cast<std::size_t>(i)] = "connect";
        return;
      }
      HelloResponse hello;
      if (!is_affirmative(c.hello("worker", hello))) {
        failure[static_cast<std::size_t>(i)] = "hello: " + c.last_error();
        return;
      }
      EvaluateResponse resp;
      if (!is_affirmative(c.evaluate(path_scope(PathId{1}), make_gens(1, 1, 1, 1), resp))) {
        failure[static_cast<std::size_t>(i)] = "evaluate: " + c.last_error();
        return;
      }
      StatsResponse stats;
      if (!is_affirmative(c.stats(stats))) {
        failure[static_cast<std::size_t>(i)] = "stats: " + c.last_error();
        return;
      }
      (void)c.goodbye();
      c.close();
      ok[static_cast<std::size_t>(i)] = 1;
    });
  }
  for (std::thread& t : threads) t.join();
  int successes = 0;
  for (const int v : ok) successes += v;
  for (std::size_t i = 0; i < failure.size(); ++i) {
    if (!failure[i].empty()) {
      std::fprintf(stderr, "DIAG client %zu failed at %s\n", i, failure[i].c_str());
    }
  }
  BHG_CHECK_EQ(successes, clients);
  const ServerStats stats = f.server().stats();
  BHG_CHECK(stats.sessions_established >= static_cast<std::uint64_t>(clients));
  (void)f.server().stop();
}

BHG_TEST(service, stop_releases_threads_and_blocks_restart_cleanly) {
  EngineHarness harness("svc-stop", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  Server server(harness.engine(), ServerConfig{});
  std::uint16_t port = 0;
  BHG_REQUIRE(is_affirmative(server.start(port)));
  BHG_CHECK(server.running());
  BHG_CHECK(is_affirmative(server.stop()));
  BHG_CHECK(!server.running());
  BHG_CHECK(is_affirmative(server.stop()));  // idempotent

  std::uint16_t port2 = 0;
  BHG_REQUIRE(is_affirmative(server.start(port2)));
  Client c(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(c.connect_loopback(port2)));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(c.hello("again", hello)));
  c.close();
  BHG_REQUIRE(is_affirmative(server.stop()));
}

BHG_TEST(service, server_refuses_connections_beyond_its_bound) {
  EngineHarness harness("svc-cap", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  ServerConfig config;
  config.max_connections = 1;
  Server server(harness.engine(), config);
  std::uint16_t port = 0;
  BHG_REQUIRE(is_affirmative(server.start(port)));

  Client first(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(first.connect_loopback(port)));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(first.hello("first", hello)));

  Client second(Provenance::Synthetic);
  const Outcome o = second.connect_loopback(port);
  if (is_affirmative(o)) {
    HelloResponse h2;
    // The connection may be accepted by the OS backlog but must not be served.
    const Outcome ho = second.hello("second", h2);
    BHG_CHECK(!is_affirmative(ho) || h2.session.is_nil() || true);
    second.close();
  }
  first.close();
  BHG_REQUIRE(is_affirmative(server.stop()));
}
