/// Real multiprocess proofs.
///
/// Every participant here is an independent operating-system process launched from
/// bhBHG_WORKER, communicating over real loopback TCP sockets. Processes are killed
/// with an uncatchable hard kill at meaningful boundaries: before a durable commit,
/// after a durable commit but before downstream acknowledgement, and while blocked.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/process.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// The worker is a sibling executable in the same output directory; locating it via
/// the running executable avoids any build-system string escaping.
const std::string& worker_path() {
  static const std::string path = sibling_executable("bhg_worker");
  return path;
}
#define BHG_WORKER worker_path()

struct WorkerArgs {
  std::vector<std::string> items;
  WorkerArgs& add(const std::string& positional) {
    items.push_back(positional);
    return *this;
  }
  WorkerArgs& add(const std::string& k, const std::string& v) {
    items.push_back(k);
    items.push_back(v);
    return *this;
  }
  WorkerArgs& add(const std::string& k, std::uint64_t v) {
    return add(k, std::to_string(v));
  }
};

/// Spawns a worker and waits for its readiness file. Synchronisation is a bounded
/// yield-spin on the file, never a wall-clock timeout.
bool spawn_ready_worker(TempDir& dir, const std::string& tag, WorkerArgs args,
                        ChildProcess& child, std::string& ready_text, std::uint16_t& port) {
  const std::string ready = dir.child(tag + ".ready");
  args.add("--ready", ready);
  std::string detail;
  if (!is_affirmative(spawn_process(BHG_WORKER, args.items, child, detail))) return false;
  if (!wait_for_file(ready)) return false;
  ready_text = read_text_file(ready);
  if (ready_text.empty()) return false;
  if (ready_text.rfind("ERROR", 0) == 0) return false;
  port = static_cast<std::uint16_t>(std::strtoul(ready_text.c_str(), nullptr, 10));
  return port != 0;
}

std::map<std::string, std::string> parse_kv(const std::string& text) {
  std::map<std::string, std::string> out;
  std::string line;
  for (const char c : text) {
    if (c == '\n') {
      const std::size_t eq = line.find('=');
      if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  return out;
}

}  // namespace

BHG_TEST(multiprocess, worker_executable_exists_and_reports_usage) {
  ChildProcess child;
  std::string detail;
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, {}, child, detail)));
  std::uint32_t code = 0;
  BHG_REQUIRE(is_affirmative(wait_process(child, code, detail)));
  BHG_CHECK_EQ(code, 2u);  // usage
  close_process(child);
}

BHG_TEST(multiprocess, killed_sender_does_not_produce_a_confirmed_blackhole) {
  TempDir dir("mp-kill-sender");
  ChildProcess server;
  std::string ready;
  std::uint16_t port = 0;
  WorkerArgs server_args;
  server_args.add("serve").add("--root", dir.child("state")).add("--port", 0);
  BHG_REQUIRE(spawn_ready_worker(dir, "server", server_args, server, ready, port));

  const std::string scope_args_ready = dir.child("sender2.ready");

  // Sender 1 runs to completion: one authoritative source instance.
  ChildProcess sender1;
  WorkerArgs s1;
  s1.add("submit")
      .add("--port", port)
      .add("--path", 1)
      .add("--source", 1)
      .add("--incarnation", 1)
      .add("--count", 8)
      .add("--outcome", "failure")
      .add("--out", dir.child("sender1.out"));
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, s1.items, sender1, ready)));
  std::uint32_t code = 0;
  BHG_REQUIRE(is_affirmative(wait_process(sender1, code, ready)));
  BHG_CHECK_EQ(code, 0u);
  close_process(sender1);

  // Sender 2 is a second source instance that is killed before it reports anything.
  ChildProcess sender2;
  WorkerArgs s2;
  s2.add("hold").add("--port", port).add("--ready", scope_args_ready);
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, s2.items, sender2, ready)));
  BHG_REQUIRE(wait_for_file(scope_args_ready));
  BHG_REQUIRE(process_running(sender2));
  kill_process_hard(sender2);
  BHG_REQUIRE(is_affirmative(wait_process(sender2, code, ready)));
  close_process(sender2);

  // An independent observer process evaluates the subject.
  ChildProcess observer;
  WorkerArgs obs;
  obs.add("evaluate")
      .add("--port", port)
      .add("--path", 1)
      .add("--out", dir.child("observer.out"));
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, obs.items, observer, ready)));
  BHG_REQUIRE(is_affirmative(wait_process(observer, code, ready)));
  BHG_CHECK_EQ(code, 0u);
  close_process(observer);

  auto kv = parse_kv(read_text_file(dir.child("observer.out")));
  BHG_REQUIRE(!kv.empty());
  // The surviving evidence supports a blackhole *classification* but only one
  // source instance observed it, so no authority to fence may be claimed.
  BHG_CHECK_EQ(kv["classification"], std::string("BLACKHOLE"));
  BHG_CHECK_EQ(kv["corroborated"], std::string("0"));
  BHG_CHECK_EQ(kv["has_intent"], std::string("0"));
  BHG_CHECK_EQ(kv["authority"], std::string("ELIGIBILITY"));
  BHG_CHECK_EQ(kv["reason"], std::string("COMPLETE_DELIVERY_FAILURE"));

  kill_process_hard(server);
  (void)wait_process(server, code, ready);
  close_process(server);
}

BHG_TEST(multiprocess, server_killed_after_durable_commit_before_acknowledgement) {
  TempDir dir("mp-kill-commit");
  const std::string root = dir.child("state");
  ChildGuard server1;
  std::string detail;
  {
    WorkerArgs args;
    args.add("serve").add("--root", root).add("--port", 0);
    std::uint16_t port = 0;
    std::string ready;
    BHG_REQUIRE(spawn_ready_worker(dir, "server1", args, server1.get(), ready, port));

    // Two independent sender processes create a corroborated blackhole; the server
    // durably commits the fencing intent. The downstream owner never acknowledges
    // it, and the server is then hard-killed.
    for (std::uint64_t source = 1; source <= 2; ++source) {
      ChildProcess sender;
      WorkerArgs s;
      s.add("submit")
          .add("--port", port)
          .add("--path", 1)
          .add("--source", source)
          .add("--incarnation", source)
          .add("--count", 4)
          .add("--outcome", "failure");
      BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, s.items, sender, detail)));
      std::uint32_t code = 0;
      BHG_REQUIRE(is_affirmative(wait_process(sender, code, detail)));
      BHG_CHECK_EQ(code, 0u);
      close_process(sender);
    }
    ChildProcess observer;
    WorkerArgs obs;
    obs.add("evaluate")
        .add("--port", port)
        .add("--path", 1)
        .add("--out", dir.child("before-kill.out"));
    BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, obs.items, observer, detail)));
    std::uint32_t code = 0;
    BHG_REQUIRE(is_affirmative(wait_process(observer, code, detail)));
    close_process(observer);
    auto kv = parse_kv(read_text_file(dir.child("before-kill.out")));
    BHG_REQUIRE(!kv.empty());
    BHG_CHECK_EQ(kv["corroborated"], std::string("1"));
    BHG_CHECK_EQ(kv["has_intent"], std::string("1"));
    BHG_CHECK_EQ(kv["authority"], std::string("INTENT"));
  }

  // Hard kill: no shutdown path runs, no acknowledgement was ever sent.
  kill_process_hard(server1.get());
  std::uint32_t code = 0;
  BHG_REQUIRE(is_affirmative(wait_process(server1.get(), code, detail)));
  server1.disarm();

  // A brand-new server process over the same durable root must conservatively fence
  // everything the dead incarnation had claimed.
  ChildProcess server2;
  std::string ready;
  std::uint16_t port2 = 0;
  WorkerArgs args2;
  args2.add("serve").add("--root", root).add("--port", 0);
  BHG_REQUIRE(spawn_ready_worker(dir, "server2", args2, server2, ready, port2));

  ChildProcess observer2;
  WorkerArgs obs2;
  obs2.add("evaluate")
      .add("--port", port2)
      .add("--path", 1)
      .add("--out", dir.child("after-kill.out"));
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, obs2.items, observer2, detail)));
  BHG_REQUIRE(is_affirmative(wait_process(observer2, code, detail)));
  close_process(observer2);
  auto after = parse_kv(read_text_file(dir.child("after-kill.out")));
  BHG_REQUIRE(!after.empty());
  // Liveness was not restored: the new incarnation has observed nothing.
  BHG_CHECK_EQ(after["classification"], std::string("NO_EVIDENCE"));
  BHG_CHECK_EQ(after["authority"], std::string("NONE"));
  BHG_CHECK_EQ(after["has_intent"], std::string("0"));

  kill_process_hard(server2);
  (void)wait_process(server2, code, detail);
  close_process(server2);

  // Inspect the durable root in-process: the prior authority is fenced, and the
  // prior claim survives only as lineage.
  FencePolicy policy = default_policy();
  StoreConfig config;
  config.root = root;
  config.max_journal_bytes = 8ull * 1024ull * 1024ull;
  SeededNonceSource nonces(1234);
  SystemClock clock;
  Store store(config, policy);
  BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  const OpenReport report = store.report();
  BHG_CHECK(report.restart_detected);
  // The second server already consumed and fenced the dead incarnation's intent when
  // it opened, so this third open has nothing left to fence -- which is itself the
  // proof that fencing happened exactly once.
  BHG_CHECK_EQ(report.fenced_prior_fences, 0u);
  BHG_CHECK(report.boot_count >= 3u);
  std::uint64_t decisions = 0;
  std::uint64_t interruptions = 0;
  bool saw_prior = false;
  bool saw_fenced = false;
  for (const LineageEntry& e : store.state().lineage) {
    if (e.type == RecordType::DecisionCommit) {
      ++decisions;
      if (e.from_prior_incarnation) saw_prior = true;
    }
    if (e.type == RecordType::InterruptionCommit) {
      ++interruptions;
      BHG_CHECK(e.reason == ReasonCode::RestartFencedPriorAuthority);
    }
  }
  for (const FenceStatePayload& f : store.state().fences) {
    if (f.lifecycle == static_cast<std::uint8_t>(FenceLifecycle::FencedByRestart)) saw_fenced = true;
  }
  BHG_CHECK(decisions >= 1u);
  BHG_CHECK_EQ(interruptions, 1u);
  BHG_CHECK(saw_prior);
  BHG_CHECK(saw_fenced);
  BHG_CHECK(store.lineage_accounting_closed());
  BHG_REQUIRE(is_affirmative(store.close()));
}

BHG_TEST(multiprocess, server_killed_before_any_commit_restarts_conservatively) {
  TempDir dir("mp-kill-early");
  const std::string root = dir.child("state");
  ChildProcess server1;
  std::string ready;
  std::uint16_t port = 0;
  WorkerArgs args;
  args.add("serve").add("--root", root).add("--port", 0);
  BHG_REQUIRE(spawn_ready_worker(dir, "early", args, server1, ready, port));
  // Kill immediately, before any request has been served.
  kill_process_hard(server1);
  std::uint32_t code = 0;
  BHG_REQUIRE(is_affirmative(wait_process(server1, code, ready)));
  close_process(server1);

  FencePolicy policy = default_policy();
  StoreConfig config;
  config.root = root;
  SeededNonceSource nonces(99);
  SystemClock clock;
  Store store(config, policy);
  BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  BHG_CHECK_EQ(store.report().fenced_prior_fences, 0u);
  BHG_CHECK_EQ(store.report().boot_count, 2u);
  BHG_CHECK(store.report().epoch.value() >= 2u);
  BHG_CHECK_EQ(store.state().total_decisions, 0u);
  BHG_REQUIRE(is_affirmative(store.close()));
}

BHG_TEST(multiprocess, blocked_and_noisy_processes_do_not_stop_the_service) {
  TempDir dir("mp-noise");
  ChildProcess server;
  std::string ready;
  std::uint16_t port = 0;
  WorkerArgs args;
  args.add("serve").add("--root", dir.child("state")).add("--port", 0);
  BHG_REQUIRE(spawn_ready_worker(dir, "noisy", args, server, ready, port));

  // A process that holds a session open and blocks forever.
  ChildProcess holder;
  std::string detail;
  WorkerArgs h;
  h.add("hold").add("--port", port).add("--ready", dir.child("holder.ready"));
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, h.items, holder, detail)));
  BHG_REQUIRE(wait_for_file(dir.child("holder.ready")));

  // A process that sends garbage at the framing layer.
  ChildProcess noise;
  WorkerArgs n;
  n.add("noise").add("--port", port).add("--count", 16).add("--ready", dir.child("noise.ready"));
  BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, n.items, noise, detail)));
  BHG_REQUIRE(wait_for_file(dir.child("noise.ready")));
  std::uint32_t code = 0;
  BHG_REQUIRE(is_affirmative(wait_process(noise, code, detail)));
  close_process(noise);

  // Hard-kill the holder: the server must release the connection and keep serving.
  kill_process_hard(holder);
  BHG_REQUIRE(is_affirmative(wait_process(holder, code, detail)));
  close_process(holder);

  Client client(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(client.connect_loopback(port)));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(client.hello("after-noise", hello)));
  StatsResponse stats;
  BHG_REQUIRE(is_affirmative(client.stats(stats)));
  BHG_CHECK(stats.stats.frames_rejected >= 1u);
  BHG_CHECK(stats.stats.sessions_established >= 2u);
  client.close();

  kill_process_hard(server);
  (void)wait_process(server, code, detail);
  close_process(server);
}

BHG_TEST(multiprocess, durable_state_is_shared_across_process_restarts) {
  TempDir dir("mp-lineage");
  const std::string root = dir.child("state");
  std::vector<std::uint16_t> ports;
  for (int cycle = 0; cycle < 3; ++cycle) {
    ChildProcess server;
    std::string ready;
    std::uint16_t port = 0;
    WorkerArgs args;
    args.add("serve").add("--root", root).add("--port", 0);
    BHG_REQUIRE(spawn_ready_worker(dir, "cycle" + std::to_string(cycle), args, server, ready, port));
    ports.push_back(port);

    ChildProcess sender;
    std::string detail;
    WorkerArgs s;
    s.add("submit")
        .add("--port", port)
        .add("--path", 1)
        .add("--source", 1)
        .add("--incarnation", 1)
        .add("--count", 2)
        .add("--outcome", "failure");
    BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, s.items, sender, detail)));
    std::uint32_t code = 0;
    BHG_REQUIRE(is_affirmative(wait_process(sender, code, detail)));
    BHG_CHECK_EQ(code, 0u);
    close_process(sender);

    // An evaluation process commits a decision record in each cycle, so the durable
    // lineage must accumulate across restarts.
    ChildProcess evaluator;
    WorkerArgs e;
    e.add("evaluate").add("--port", port).add("--path", 1).add("--out", dir.child("cycle.out"));
    BHG_REQUIRE(is_affirmative(spawn_process(BHG_WORKER, e.items, evaluator, detail)));
    BHG_REQUIRE(is_affirmative(wait_process(evaluator, code, detail)));
    BHG_CHECK_EQ(code, 0u);
    close_process(evaluator);

    kill_process_hard(server);
    BHG_REQUIRE(is_affirmative(wait_process(server, code, detail)));
    close_process(server);
  }

  FencePolicy policy = default_policy();
  StoreConfig config;
  config.root = root;
  config.max_journal_bytes = 8ull * 1024ull * 1024ull;
  SeededNonceSource nonces(4242);
  SystemClock clock;
  Store store(config, policy);
  BHG_REQUIRE(is_affirmative(store.open(nonces, clock.wall_now(), Provenance::Synthetic)));
  const OpenReport report = store.report();
  BHG_CHECK(report.restart_detected);
  BHG_CHECK(report.boot_count >= 4u);
  BHG_CHECK(report.epoch.value() >= 4u);
  BHG_CHECK(store.state().total_decisions >= 3u);
  BHG_CHECK(store.lineage_accounting_closed());
  BHG_REQUIRE(is_affirmative(store.close()));
}
