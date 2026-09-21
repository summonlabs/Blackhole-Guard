/// Example: reopen a durable root and read its lineage.
///
/// Demonstrates that persistence is not liveness: the durable lineage is reported,
/// the epoch has advanced, and every pre-restart fence is fenced.
///
/// Run: bhg_example_lineage <state-root>

#include <cstdio>
#include <string>
#include <vector>

#include "blackhole/blackhole.hpp"

using namespace bhg;

int main(int argc, char** argv) {
  const std::string root = argc > 1 ? argv[1] : "./bhg-example-state";
  FencePolicy policy = default_policy();
  StoreConfig config;
  config.root = root;
  SystemClock clock;
  SystemNonceSource nonces;
  Store store(config, policy);
  const Outcome opened = store.open(nonces, clock.wall_now(), Provenance::Synthetic);
  if (!is_affirmative(opened)) {
    std::fprintf(stderr, "store open failed: %s\n", std::string(to_string(opened)).c_str());
    return 1;
  }
  const OpenReport r = store.report();
  std::printf("boot_count=%llu open_count=%llu epoch=%llu fenced_prior_fences=%llu\n",
              static_cast<unsigned long long>(r.boot_count),
              static_cast<unsigned long long>(r.open_count),
              static_cast<unsigned long long>(r.epoch.value()),
              static_cast<unsigned long long>(r.fenced_prior_fences));
  std::printf("snapshot_present=%d restart_detected=%d torn_tail=%d reclaimed=%llu\n",
              r.snapshot_present ? 1 : 0, r.restart_detected ? 1 : 0,
              r.torn_tail_recovered ? 1 : 0,
              static_cast<unsigned long long>(r.reclaimed_tail_bytes));
  std::printf("lineage retained=%zu seen=%llu dropped=%llu closed=%d\n",
              store.state().lineage.size(),
              static_cast<unsigned long long>(store.state().lineage_seen),
              static_cast<unsigned long long>(store.state().lineage_dropped),
              store.lineage_accounting_closed() ? 1 : 0);
  (void)store.close();
  return 0;
}
