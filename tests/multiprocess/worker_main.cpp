/// Multiprocess worker used by the multiprocess proofs.
///
/// This is a real independent executable: it owns its own boot identity, opens its
/// own sockets and can be hard-killed at any point. It never links test code.

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/process.hpp"

using namespace bhg;

namespace {

struct Args {
  std::map<std::string, std::string> values;
  bool has(const std::string& key) const { return values.find(key) != values.end(); }
  std::string get(const std::string& key, const std::string& fallback = std::string()) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
  }
  std::uint64_t number(const std::string& key, std::uint64_t fallback) const {
    const std::string v = get(key);
    if (v.empty()) return fallback;
    return static_cast<std::uint64_t>(std::strtoull(v.c_str(), nullptr, 10));
  }
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 2; i + 1 < argc; i += 2) {
    a.values[argv[i]] = argv[i + 1];
  }
  return a;
}

int usage() {
  std::fprintf(stderr,
               "usage: bhg_worker <serve|submit|evaluate|hold|noise> [--key value ...]\n");
  return 2;
}

Scope make_scope(const Args& a) {
  Scope s;
  s.kind = ScopeKind::Path;
  s.path = PathId{a.number("--path", 1)};
  return s;
}

GenerationVector make_gens_from(const Args& a) {
  GenerationVector g;
  g.path = PathGeneration{a.number("--path-gen", 1)};
  g.topology = TopologyGeneration{a.number("--topo-gen", 1)};
  g.link_state = LinkStateGeneration{a.number("--link-gen", 1)};
  g.epoch = CoordinatorEpoch{a.number("--epoch", 1)};
  return g;
}

/// Blocks the calling thread until the process is terminated, unless the argument
/// --stop-file names a file whose appearance ends the wait. Termination is the normal
/// path for this worker: the multiprocess proofs hard-kill it at chosen boundaries.
void block_until_stopped(const Args& a) {
  const std::string stop_file = a.get("--stop-file");
  if (!stop_file.empty()) {
    while (!std::ifstream(stop_file).good()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return;
  }
  std::mutex mutex;
  std::condition_variable cv;
  bool stop = false;
  std::unique_lock<std::mutex> lock(mutex);
  cv.wait(lock, [&stop] { return stop; });
}

Outcome connect_and_hello(Client& client, const char* name, std::uint16_t port) {
  if (!is_affirmative(client.connect_loopback(port))) return Outcome::Unavailable;
  HelloResponse hello;
  return client.hello(name, hello);
}

DeliveryEvidence build_evidence(const Args& a, const Scope& scope, const GenerationVector& gens,
                                std::uint64_t seq, bool failure) {
  // Default to the current wall clock so observations are fresh with respect to the
  // coordinator's clock; an explicit value is only used for skew experiments.
  const std::int64_t wall_ns =
      a.has("--wall-ns") ? static_cast<std::int64_t>(a.number("--wall-ns", 0))
                         : SystemClock().wall_now().ns;
  const WallNs at{wall_ns};
  const std::uint64_t attempts = a.number("--attempts", 32);
  DeliveryEvidence e;
  e.source.source = EvidenceSourceId{a.number("--source", 1)};
  e.source.boot = BootId{a.number("--boot", a.number("--source", 1) * 31u + 1u)};
  e.source.incarnation = IncarnationId{a.number("--incarnation", 1)};
  e.source.epoch = SourceEpoch{1};
  e.scope = scope;
  e.gens = gens;
  e.seq = EvidenceSequence{seq};
  e.attempt = AttemptId{seq};
  e.kind = failure ? EvidenceKind::DeliveryFailure : EvidenceKind::DeliverySuccess;
  e.quality = EvidenceQuality::Exact;
  e.provenance = Provenance::Synthetic;
  e.attempts = attempts;
  e.failures = failure ? attempts : 0;
  e.loss_ppm = failure ? kLossTotalPpm : 0;
  e.observed_at = at;
  e.fresh.open = at;
  e.fresh.close = WallNs{at.ns + 600 * kNsPerSecond};
  e.id = derive_evidence_id(e);
  return e;
}

int run_serve(const Args& a) {
  EngineConfig config;
  config.store.root = a.get("--root", "./bhg-worker-state");
  config.store.max_journal_bytes = a.number("--journal-bytes", 8ull * 1024ull * 1024ull);
  config.store.max_lineage_records = static_cast<std::uint32_t>(a.number("--lineage", 4096));
  config.label = Provenance::Synthetic;
  SystemClock clock;
  SystemNonceSource nonces;
  Engine engine(config, clock, nonces);
  const Outcome o = engine.open();
  if (!is_affirmative(o)) {
    std::fprintf(stderr, "open failed: %s\n", std::string(to_string(o)).c_str());
    const std::string ready = a.get("--ready");
    if (!ready.empty()) {
      (void)bhg::test::write_text_file(
          ready, "ERROR open " + std::string(to_string(o)) + " " + engine.open_report().detail);
    }
    return 3;
  }
  ServerConfig sc;
  sc.port = static_cast<std::uint16_t>(a.number("--port", 0));
  Server server(engine, sc);
  std::uint16_t bound = 0;
  if (!is_affirmative(server.start(bound))) {
    std::fprintf(stderr, "listen failed\n");
    const std::string ready = a.get("--ready");
    if (!ready.empty()) (void)bhg::test::write_text_file(ready, "ERROR listen");
    return 4;
  }
  const std::string ready = a.get("--ready");
  if (!ready.empty()) {
    if (!bhg::test::write_text_file(ready, std::to_string(bound) + "\n")) {
      std::fprintf(stderr, "ready file not writable\n");
      return 5;
    }
  }
  block_until_stopped(a);
  (void)server.stop();
  (void)engine.close();
  return 0;
}

int run_submit(const Args& a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.number("--port", 0));
  Client client(Provenance::Synthetic);
  if (!is_affirmative(connect_and_hello(client, "sender", port))) return 6;
  const Scope scope = make_scope(a);
  const GenerationVector gens = make_gens_from(a);
  const std::uint64_t count = a.number("--count", 1);
  const bool failure = a.get("--outcome", "failure") == "failure";
  const std::string progress = a.get("--progress");
  std::uint64_t accepted = 0;
  std::string last_reason = "NONE";
  for (std::uint64_t i = 1; i <= count; ++i) {
    SubmitEvidenceResponse resp;
    const DeliveryEvidence e = build_evidence(a, scope, gens, i, failure);
    if (is_affirmative(client.submit(e, resp)) &&
        resp.admission == EvidenceAdmission::Accepted) {
      ++accepted;
    } else {
      last_reason = std::string(to_string(resp.reason));
    }
    if (!progress.empty() && i == 1) {
      (void)bhg::test::write_text_file(progress, "1\n");
    }
  }
  const std::string out = a.get("--out");
  if (!out.empty()) {
    (void)bhg::test::write_text_file(out, "accepted " + std::to_string(accepted) + " of " +
                                              std::to_string(count) + " last_reason " +
                                              last_reason + "\n");
  }
  client.close();
  return accepted == count ? 0 : 7;
}

int run_evaluate(const Args& a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.number("--port", 0));
  Client client(Provenance::Synthetic);
  if (!is_affirmative(connect_and_hello(client, "observer", port))) return 6;
  const Scope scope = make_scope(a);
  const GenerationVector gens = make_gens_from(a);
  EvaluateResponse resp;
  const Outcome o = client.evaluate(scope, gens, resp);
  std::string text = "outcome=" + std::string(to_string(o)) + "\n";
  text += "classification=" + std::string(to_string(resp.decision.classification)) + "\n";
  text += "authority=" + std::string(to_string(resp.decision.authority.stage)) + "\n";
  text += "corroborated=" + std::string(resp.decision.corroborated ? "1" : "0") + "\n";
  text += "has_intent=" + std::string(resp.decision.has_intent ? "1" : "0") + "\n";
  text += "reason=" + std::string(to_string(resp.decision.primary_reason)) + "\n";
  const std::string out = a.get("--out");
  if (!out.empty()) (void)bhg::test::write_text_file(out, text);
  std::fputs(text.c_str(), stdout);
  client.close();
  return 0;
}

int run_stat(const Args& a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.number("--port", 0));
  Client client(Provenance::Synthetic);
  if (!is_affirmative(connect_and_hello(client, "stat", port))) return 6;
  StatsResponse stats;
  if (!is_affirmative(client.stats(stats))) return 8;
  std::string text;
  text += "frames_decoded=" + std::to_string(stats.stats.frames_decoded) + "\n";
  text += "frames_rejected=" + std::to_string(stats.stats.frames_rejected) + "\n";
  text += "sessions=" + std::to_string(stats.stats.sessions_established) + "\n";
  text += "requests_served=" + std::to_string(stats.stats.requests_served) + "\n";
  const std::string out = a.get("--out");
  if (!out.empty()) (void)bhg::test::write_text_file(out, text);
  std::fputs(text.c_str(), stdout);
  client.close();
  return 0;
}

int run_hold(const Args& a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.number("--port", 0));
  Client client(Provenance::Synthetic);
  if (!is_affirmative(connect_and_hello(client, "holder", port))) return 6;
  const std::string ready = a.get("--ready");
  if (!ready.empty()) (void)bhg::test::write_text_file(ready, "held\n");
  block_until_stopped(a);
  client.close();
  return 0;
}

int run_noise(const Args& a) {
  const std::uint16_t port = static_cast<std::uint16_t>(a.number("--port", 0));
  socket_handle h = kInvalidSocketHandle;
  std::string detail;
  if (!is_affirmative(net_connect_loopback(port, h, detail))) return 6;
  const std::uint64_t count = a.number("--count", 8);
  const std::string ready = a.get("--ready");
  for (std::uint64_t i = 0; i < count; ++i) {
    std::vector<std::byte> junk(64, static_cast<std::byte>(i + 1u));
    (void)net_send_all(h, std::span<const std::byte>(junk.data(), junk.size()), detail);
  }
  if (!ready.empty()) (void)bhg::test::write_text_file(ready, "sent\n");
  net_shutdown(h);
  net_close(h);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string mode = argv[1];
  const Args args = parse(argc, argv);
  if (mode == "serve") return run_serve(args);
  if (mode == "submit") return run_submit(args);
  if (mode == "evaluate") return run_evaluate(args);
  if (mode == "stat") return run_stat(args);
  if (mode == "hold") return run_hold(args);
  if (mode == "noise") return run_noise(args);
  return usage();
}
