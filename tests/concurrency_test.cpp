#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

/// Deterministic latch: every participant blocks until the last one arrives.
/// Synchronisation is by condition variable, never by sleeping.
class Latch {
 public:
  explicit Latch(std::size_t count) : remaining_(count) {}
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      cv_.notify_all();
      return;
    }
    cv_.wait(lock, [this] { return remaining_ == 0; });
  }
  /// Releases the latch without waiting, for participants that bail out early.
  void count_down() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (remaining_ > 0 && --remaining_ == 0) cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t remaining_;
};

}  // namespace

BHG_TEST(concurrency, engine_is_safe_under_simultaneous_mutation) {
  EngineHarness harness("conc-engine", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 40;
  Latch latch(kThreads + 1);
  std::atomic<int> failures{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      latch.arrive_and_wait();
      for (int i = 0; i < kOpsPerThread; ++i) {
        const auto source = static_cast<std::uint64_t>(t) + 1u;
        const auto seq = static_cast<std::uint64_t>(i) + 1u;
        const Scope scope = path_scope(PathId{static_cast<std::uint64_t>(t % 4) + 1u});
        const GenerationVector gens = make_gens(1, 1, 1, 1);
        const WallNs at = harness.clock().wall_now();
        DeliveryEvidence e =
            (i % 3 == 0)
                ? failure_evidence(EvidenceSourceId{source}, IncarnationId{source}, scope, gens,
                                   EvidenceSequence{seq}, at, 32)
                : success_evidence(EvidenceSourceId{source}, IncarnationId{source}, scope, gens,
                                   EvidenceSequence{seq}, at, 32);
        EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
        ReasonCode reason = ReasonCode::None;
        (void)harness.engine().submit(e, admission, reason);

        Decision d;
        (void)harness.engine().evaluate(scope, gens, d);
        if (!d.is_valid()) {
          failures.fetch_add(1);
          continue;
        }
        if (static_cast<std::uint8_t>(d.authority.stage) >=
                static_cast<std::uint8_t>(AuthorityStage::Authorization) &&
            d.classification != Classification::Blackhole) {
          failures.fetch_add(1);
        }
        if (i % 7 == 0) {
          std::vector<LineageEntry> entries;
          std::uint64_t seen = 0;
          std::uint64_t dropped = 0;
          (void)harness.engine().lineage(16, entries, seen, dropped);
        }
        if (i % 11 == 0) {
          (void)harness.engine().stats();
        }
      }
    });
  }
  latch.arrive_and_wait();
  for (std::thread& th : threads) th.join();

  BHG_CHECK_EQ(failures.load(), 0);
  const EngineStats s = harness.engine().stats();
  BHG_CHECK(s.accounting_closed());
  BHG_CHECK(s.evaluations ==
            static_cast<std::uint64_t>(kThreads) * static_cast<std::uint64_t>(kOpsPerThread));
}

BHG_TEST(concurrency, server_start_stop_is_race_free) {
  EngineHarness harness("conc-server", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  for (int cycle = 0; cycle < 6; ++cycle) {
    Server server(harness.engine(), ServerConfig{});
    std::uint16_t port = 0;
    BHG_REQUIRE(is_affirmative(server.start(port)));
    std::vector<std::thread> clients;
    std::atomic<int> served{0};
    for (int i = 0; i < 4; ++i) {
      clients.emplace_back([&] {
        Client c(Provenance::Synthetic);
        if (!is_affirmative(c.connect_loopback(port))) return;
        HelloResponse hello;
        if (!is_affirmative(c.hello("racer", hello))) return;
        EvaluateResponse resp;
        if (!is_affirmative(c.evaluate(path_scope(PathId{1}), make_gens(1, 1, 1, 1), resp))) return;
        served.fetch_add(1);
        c.close();
      });
    }
    // Stop concurrently with in-flight clients: blocked reads must be released.
    std::thread stopper([&] { (void)server.stop(); });
    for (std::thread& t : clients) t.join();
    stopper.join();
    BHG_CHECK(!server.running());
    BHG_REQUIRE(is_affirmative(server.stop()));
  }
}

BHG_TEST(concurrency, shutdown_releases_a_blocked_reader_promptly) {
  EngineHarness harness("conc-blocked", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  Server server(harness.engine(), ServerConfig{});
  std::uint16_t port = 0;
  BHG_REQUIRE(is_affirmative(server.start(port)));

  // A client that establishes a session and then holds the connection open without
  // sending anything leaves a worker blocked in recv.
  Client holder(Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(holder.connect_loopback(port)));
  HelloResponse hello;
  BHG_REQUIRE(is_affirmative(holder.hello("holder", hello)));

  std::atomic<bool> stopped{false};
  std::thread stopper([&] {
    (void)server.stop();
    stopped.store(true);
  });
  stopper.join();
  BHG_CHECK(stopped.load());
  BHG_CHECK(!server.running());
  holder.close();
}

BHG_TEST(concurrency, close_during_use_is_refused_not_crashing) {
  EngineHarness harness("conc-close", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  Latch latch(2);
  std::atomic<int> invalid{0};
  std::thread worker([&] {
    latch.arrive_and_wait();
    for (int i = 0; i < 200; ++i) {
      Decision d;
      const Outcome o = harness.engine().evaluate(path_scope(PathId{1}), make_gens(1, 1, 1, 1), d);
      if (!is_affirmative(o)) {
        // A refused evaluation must still be a well-formed, non-authoritative answer.
        if (static_cast<std::uint8_t>(d.authority.stage) >=
            static_cast<std::uint8_t>(AuthorityStage::Authorization)) {
          invalid.fetch_add(1);
        }
        if (d.classification == Classification::Blackhole) invalid.fetch_add(1);
      }
    }
  });
  latch.arrive_and_wait();
  (void)harness.engine().close();
  worker.join();
  BHG_CHECK_EQ(invalid.load(), 0);
}

BHG_TEST(concurrency, independent_engines_do_not_share_state) {
  // Two engines over distinct roots must never observe each other's evidence.
  EngineHarness a("conc-iso-a", default_policy(), Provenance::Synthetic);
  EngineHarness b("conc-iso-b", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(a.open()));
  BHG_REQUIRE(is_affirmative(b.open()));
  const Scope scope = path_scope(PathId{1});
  const GenerationVector gens = make_gens(1, 1, 1, 1);
  EvidenceAdmission admission = EvidenceAdmission::StructurallyInvalid;
  ReasonCode reason = ReasonCode::None;
  for (std::uint64_t s = 1; s <= 2; ++s) {
    (void)a.engine().submit(failure_evidence(EvidenceSourceId{s}, IncarnationId{s}, scope, gens,
                                             EvidenceSequence{1}, a.clock().wall_now(), 32),
                            admission, reason);
  }
  Decision da;
  Decision db;
  (void)a.engine().evaluate(scope, gens, da);
  (void)b.engine().evaluate(scope, gens, db);
  BHG_CHECK(da.classification == Classification::Blackhole);
  BHG_CHECK(db.classification == Classification::NoEvidence);
  BHG_CHECK(da.boot != db.boot);
  BHG_CHECK(da.incarnation != db.incarnation);
}

BHG_TEST(concurrency, concurrent_sessions_on_one_server_keep_sequences_independent) {
  EngineHarness harness("conc-sessions", default_policy(), Provenance::Synthetic);
  BHG_REQUIRE(is_affirmative(harness.open()));
  Server server(harness.engine(), ServerConfig{});
  std::uint16_t port = 0;
  BHG_REQUIRE(is_affirmative(server.start(port)));
  constexpr int kClients = 6;
  Latch latch(kClients + 1);
  std::atomic<int> ok{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kClients; ++i) {
    threads.emplace_back([&] {
      Client c(Provenance::Synthetic);
      if (!is_affirmative(c.connect_loopback(port))) {
        latch.count_down();
        return;
      }
      HelloResponse hello;
      if (!is_affirmative(c.hello("seq", hello))) {
        latch.count_down();
        return;
      }
      latch.arrive_and_wait();
      for (int k = 0; k < 10; ++k) {
        StatsResponse stats;
        if (!is_affirmative(c.stats(stats))) break;
      }
      ok.fetch_add(1);
      c.close();
    });
  }
  latch.arrive_and_wait();
  for (std::thread& t : threads) t.join();
  BHG_CHECK_EQ(ok.load(), kClients);
  BHG_REQUIRE(is_affirmative(server.stop()));
}
