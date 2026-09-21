# Blackhole Guard

Blackhole Guard is a C++20 infrastructure runtime that answers one question
deterministically and truthfully:

> Given a path that is structurally legal but authoritative delivery evidence is
> missing or contradictory: **is traffic being blackholed now, where is the failure
> localized, what authority must be fenced, and when may the path be restored?**

It is built by Summon Software Labs and released as version 1.0.0.

The defining distinction of the product is that **path legality is not delivery
proof**. A path that exists in the topology, is administratively up and has a valid
route is *legal*; that says nothing about whether packets reach the far end. Blackhole
Guard consumes delivery observations from adjacent owners, classifies what they
actually support, localizes the failure to hop-level elements, and emits a *bounded,
revocable fencing intent* -- never a route change, never a congestion verdict, never a
telemetry stream.

---

## 1. Exact systems boundary

### What this runtime owns

* Classification of *delivery* state for a subject (path / node / link / hop segment)
  into HEALTHY, NO_EVIDENCE, UNKNOWN, LOSSY, CONGESTED, PARTITIONED or BLACKHOLE.
* Localization of a delivery failure to a minimum-cardinality set of hop elements,
  with an explicit ambiguity, infeasibility and search-limit story.
* Corroboration evaluation against policy before any authority is granted.
* Bounded fencing **intent** for affected path authority, its lifecycle
  (intent -> acknowledgement -> reported effect -> revocation), and its revocation when
  any authority-bearing dependency moves.
* The restoration gate: fresh positive delivery evidence under the current generation
  vector, or nothing.
* Durable, integrity-checked lineage: definitions, policy, completed outcomes, fences
  and interruptions.

### What this runtime explicitly does **not** own

* It does not compute routes, and it never removes a route. A fencing intent is an
  input to the route/convergence owner, who decides whether and how to apply it.
* It does not own congestion state. Congestion arrives as a typed signal from the
  congestion owner and is used only to *explain* loss.
* It does not collect telemetry. Observations arrive as typed evidence records.
* It does not forward packets, program dataplanes, or touch NIC/ASIC/DPU state.
* It does not authenticate or encrypt transport (see the trust boundary in section 11).

Adjacent runtime owners integrate through explicit typed inputs, generation-bound
references and authority boundaries. Nothing is absorbed to make a demonstration easier.

---

## 2. Authority and generation model

Authority is a strictly ordered ladder. Each rung is a different kind of statement and
they are never conflated:

| Rung | Meaning | Confers |
|---|---|---|
| OBSERVATION | a raw measurement was received | nothing |
| ELIGIBILITY | policy says this subject *could* be fenced if corroborated | nothing |
| AUTHORIZATION | corroboration requirements are satisfied | permission to mint an intent |
| INTENT | this runtime emitted a bounded fencing intent | a request to the route owner |
| ACKNOWLEDGEMENT | the route owner received it | receipt only |
| VERIFIED_EFFECT | the route owner reported the intent applied | a downstream claim, still not proof of route removal |

Positive authority is exactly: stage >= AUTHORIZATION **and** not revoked **and** not
expired **and** bound to the current generation vector **and** bound to the current
coordinator boot/incarnation. Nothing weaker is authority.

Every authority-bearing claim is bound to a `GenerationVector`:

```
GenerationVector { path, topology, link_state, epoch }
```

* `path`, `topology`, `link_state` are generations owned by the adjacent owners.
* `epoch` is this coordinator's term and advances on every process open.

Evidence additionally carries a `SourceRef { source, boot, incarnation, epoch }`. A
restarted source is a *different authority* even under the same source id.

**Matching identity is not matching generation. Persistence is not liveness.
Observation is not authority. Eligibility is not authorization. Authorization is not
application. Acknowledgement is not verified effect.**

---

## 3. Major invariants

These are enforced in code and asserted by the property suite after *every* operation:

1. **Ambiguity is never promoted to blackhole.** Staleness, generation mismatch,
   quality refusal, contradictory observations and structural withdrawal all degrade to
   UNKNOWN, or to the owner-supplied explanation (CONGESTED / PARTITIONED). Only an
   unexplained, fresh, generation-matched, complete delivery failure classifies as
   BLACKHOLE.
2. **UNKNOWN / STALE / NO_EVIDENCE / CONFLICT can never authorize fencing.** The
   property suite asserts this over randomized operation sequences, and the fail-closed
   engine paths are exercised under journal exhaustion.
3. **Severe loss is not total loss.** `loss_ppm < 1000000` classifies as LOSSY no
   matter how severe. Only complete failure, with the same generation vector and no
   competing explanation, classifies as BLACKHOLE.
4. **Corroboration is required before fencing.** By default: >= 2 distinct source ids,
   >= 2 distinct incarnations, >= 32 total delivery attempts, exact quality, full
   generation agreement.
5. **Every externally visible decision names its subject, its generations, its
   authority rung, and its reasons**, and is deterministic: the same inputs produce the
   same `DecisionId` on any host.
6. **Authorization is revocable.** Any change to path/topology/link-state generation,
   coordinator boot/incarnation, or expiry fences the intent on the next evaluation.
7. **Restoration requires fresh positive delivery evidence.** Absence of blackhole
   evidence is never evidence of restoration.
8. **Persistence never converts old dynamic state into current authority.** A restart
   mints a new boot id, a new incarnation and a higher epoch, fences every open
   pre-restart intent, and starts with an empty evidence ledger.
9. **Every table, history, queue and explanation is bounded**, and every bound has an
   exact accounting identity that the tests assert.

---

## 4. Evidence admission

Observations enter through `Engine::submit` / the `SubmitEvidence` message. Admission
is explicit and every refusal carries a reason code:

| Admission | Cause |
|---|---|
| Accepted | fresh, structurally valid, in-order |
| RolloverAccepted | a counter wrap that looks like a wrap (previous value in the high half, new value in the low half), bounded per source |
| DuplicateAttempt | same sequence or same attempt id as the last accepted record |
| Regressed | a backward jump that is not a plausible wrap |
| StructurallyInvalid | broken invariants (failures > attempts, loss > 1e6, degenerate window, ...) |
| OutOfFreshnessWindow | the freshness window does not contain the local clock |
| ClockSkewExceeded | observed more than the tolerated skew into the future |
| CapacityExhausted | bounded per-scope evidence or per-scope source table is full |

A newly seen source is refused when the per-scope source table is full; existing cursors
are never evicted, because forgetting a source would let it be re-admitted later with a
regressed sequence.

---

## 5. Failure localization

Localization is an exact, bounded optimization over an explicitly stated problem class:

* The subject is a path decomposed into hop elements `0..n-1`.
* Successful segment probes prove every element in their range healthy.
* Failed segment probes require at least one element in their range to be faulty.
* Derived: candidates `C` = elements not proven healthy; constraint sets
  `S_i` = failing probe range intersect `C`.
* A localization is a set `H` subset of `C` hitting every `S_i`.
  It is **valid** (subset + covers), **feasible** (one exists) and **optimal**
  (minimum cardinality).

Objective: minimize `|H|`. Tie-break: the lexicographically smallest ascending element
list. Output is independent of container, hash, insertion and discovery order.

Outcomes are explicit and never conflated:

* `Resolved` -- optimal size proven, unique minimum.
* `Ambiguous` -- optimal size proven, more than one minimum exists.
* `ProvenInfeasible` -- only with a certificate (a constraint set no candidate can hit).
* `Indeterminate` -- the bounded search did not prove optimality. Failure to find is
  never reported as proof that none exists.
* `InvalidInput` -- outside the supported class (bounded at 64 candidates, 64 sets).

Every emitted result is re-validated by an independent verifier. For instances with at
most 18 candidates the verifier uses a separate exhaustive reference solver; larger
instances are checked with an independently instantiated feasibility oracle. If the
verification fails, the decision degrades to `Indeterminate` and an
`INTERNAL_INVARIANT_VIOLATION` reason instead of publishing a localization.

---

## 6. Lifecycle and restart semantics

### Durable (survives restart)

Definitions, the effective policy and its fingerprint, committed lineage, completed
outcomes, fence records, epoch/boot counters.

### Never durable (re-established after restart)

Evidence freshness, source sequencing cursors, live leases, in-flight attempts, open
session authority, and any backend effect. The live evidence ledger is **empty** after a
restart; a durable record never repopulates it.

### Restart sequence

1. Read the snapshot (integrity-checked, version-checked, length-checked).
2. Replay the journal from `snapshot_seq + 1`, validating magic, version, type,
   length, header CRC, record CRC and sequence continuity on every record.
3. Reclaim a genuine torn tail (a trailing prefix of a valid record header). Never
   truncate an integrity failure, a bad magic, an unsupported version, an out-of-domain
   type, an impossible length, or non-prefix trailing bytes -- those are refused and the
   store does not open.
4. Advance `boot_count`, `open_count` and the coordinator epoch; mint a new boot id and
   incarnation id; append a boot record.
5. Re-commit policy if the effective policy differs from what was persisted.
6. Fence **every** open pre-restart intent and record an interruption for each, with
   reason `RESTART_FENCED_PRIOR_AUTHORITY`.
7. Mark all previously persisted lineage as `from_prior_incarnation`.

### Ordering contract

Every durable mutation is append -> flush -> fsync -> publish. Nothing is reported
durable before the fsync returns. Snapshots are written to a staging file, flushed,
fsynced and atomically renamed over the live path. Journal rotation happens *after* the
snapshot is durable.

### Failure modes

* Corrupt snapshot or journal: the store refuses to open (fail closed) and reports the
  exact reason. It never silently truncates or skips.
* Journal bound reached: durable writes return `Exhausted`; a fencing intent is not
  allowed to exist in memory without its durable record.
* Failed decision commit after a fence intent commit: the in-memory intent is revoked.

---

## 7. Wire protocol

Bounded, checksummed framing over TCP (loopback by default):

```
u32 magic 'B','H','G','1' | u16 version | u16 type | u32 flags | u32 length
u32 integrity = CRC-32C(header[0,16) || payload) | payload
```

* The declared length is checked against the configured maximum **before** any buffer is
  sized, so an oversized frame is refused without allocating.
* Decoding is total and sticky-failing: bad magic, unsupported version, out-of-domain
  type, non-zero flags, integrity mismatch, oversized length and trailing bytes inside a
  declared frame all latch a permanent failure and the connection is torn down. The
  decoder never attempts to resynchronise.
* Every request carries `{ session, request, seq, client_boot, client_incarnation }`.
  A session is bound to exactly one connection; a request presenting another
  connection's session, or a foreign boot/incarnation, is refused. Sequences must be
  strictly increasing; replays and gaps are refused and do not advance the sequence.

Messages: Hello, SubmitEvidence, Evaluate, Localize, AckFence, ReportEffect, Restore,
Lineage, Checkpoint, Stats, Goodbye, Error.

### Concurrency and lifecycle

* The engine has exactly one internal mutex and performs no lock re-entry, acquires no
  other lock while holding it, and invokes no callback, log sink or foreign code beneath
  it.
* The server uses one dedicated thread per served connection with a hard
  `max_connections` bound. There is no shared worker pool, so a client that holds its
  session open can never block another client (head-of-line blocking was found and
  removed during the concurrency audit).
* `stop()` wakes the accept thread with a loopback self-connect, shuts down every live
  connection so blocked reads return, and joins every thread -- never while holding a
  lock those threads need. `stop()` is idempotent.

### Concurrency and ownership audit

The audit was performed by inspecting ownership and call paths, not only by running
tests. Each hazard class was examined explicitly and its disposition recorded:

| Hazard | Finding |
|---|---|
| Read-lock then write-lock re-entry on the same lock | None. The engine has a single mutex and never re-acquires it; `Engine::localize` originally nested a locked call inside a locked call during development and was restructured so the bounded search runs outside the lock. |
| Write lock held across helper/callback re-entry | None. No callback, sink or foreign code is invoked while the engine mutex is held; the server holds no engine lock while dispatching. |
| Event/log/callback invocation beneath internal locks | No log sink or callback exists in the runtime. Server statistics are plain counters under their own mutex, never combined with engine state. |
| Joining workers while holding state they need | Fixed by construction: every join (accept thread, connection threads, reaped threads) happens with no lock held; the thread list is moved out under the lock and joined after it is released. |
| Cancellation/shutdown with reversed lock ordering | Single documented order: `connections_mutex_` and `queue`-equivalent state are never taken while holding `stats_mutex_`; conversely `stats_mutex_` is never taken while `connections_mutex_` is held. The earlier accept-loop capacity check that read `connections_` under `stats_mutex_` was a real defect and was fixed. |
| Blocked socket/thread teardown | `stop()` shuts down every live connection (which releases blocked `recv` on both Windows and POSIX) and wakes the accept thread with a loopback self-connect, because closing a listening socket does not reliably wake a blocked `accept`. Proven by a test that holds a session open and then stops the server. |
| Cross-object mutex order inversion | The only cross-object interaction is engine -> store -> file, and the store has no locks. No inversion exists. |
| Moved-from handle ownership | Sockets are moved through a single `socket_handle` that `net_close` sets to the invalid sentinel exactly once; `Client` and `Server` both make `close`/`stop` idempotent, so double-close is impossible. |
| Close/shutdown races and double-close | `Client::close`, `Server::stop`, `Store::close`, `JournalWriter::close` and `Engine::close` are all idempotent and tested as such. |
| Callbacks retaining references to mutable state beyond lock lifetime | No callbacks exist. Public accessors that would have returned interior pointers (`Engine::fence_state`) return values instead. |
| Dangling reference members | Found during hardening: `EvidenceLedger`, `FenceRegistry`, `SessionTable` and `Store` held `const FencePolicy&`. A caller binding that reference to a temporary produced a live but dangling policy. All four now own their policy by value. |
| Head-of-line blocking in the connection model | Found by a hanging concurrency test (not a timeout): a small fixed worker pool meant that N long-lived sessions starved every other client. Replaced with a hard-bounded, one-thread-per-connection model, which removes the failure mode entirely rather than enlarging the pool. |
| Server refusing capacity | Connections beyond `max_connections` are refused at accept time and counted; the refusal is bounded and deterministic rather than an unbounded thread fan-out. |
| Shared session table data race | Found by an intermittently failing concurrent-client test and confirmed by inspection: the server shares one session table between connection threads and it had no internal synchronisation. `SessionTable` now serialises every entry point on an internal leaf mutex. |
| Blocked reader not released on Windows | Found by a hanging test, not a timeout. `shutdown()` releases a blocked `recv` on POSIX but not reliably on Windows, so a connection thread could never exit and `stop()` blocked forever. The receive loop now waits for readiness with a bounded slice and re-checks the stop flag, which makes shutdown prompt on every platform. |
| Live-connection accounting | `live_connections()` originally reported unreaped slots, so a connection that had already finished still counted as live. It now reports connections whose completion flag is unset. |
| Socket runtime initialisation | The raw `net_*` entry points assumed the caller had already constructed a `Client` or `Server` (which own the socket runtime). Found by a multiprocess worker that used `net_connect_loopback` directly and failed with an uninitialised-Winsock error. Every public socket entry point now goes through a process-wide runtime accessor. |
| Child process handle inheritance | The multiprocess harness spawned children with handle inheritance enabled, so a leaked long-lived worker held the harness's stdio pipe open and hung the enclosing shell. Children are now spawned without inheritance and long-lived children are owned by a kill-on-destruction guard. |

---

## 8. Build, install and use

### Requirements

* CMake >= 3.20
* A C++20 compiler: MSVC 19.3x (Visual Studio 2022), GCC 11+, or Clang 14+
* No third-party runtime dependencies. Threads and (on Windows) Winsock2 only.

### Build and test

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

Options: `BHG_BUILD_TESTS`, `BHG_BUILD_TOOLS`, `BHG_BUILD_EXAMPLES`,
`BHG_WARNINGS_AS_ERRORS` (default ON), `BHG_ENABLE_ASAN`.

### Install and consume

```sh
cmake --install build/release --prefix /opt/blackhole-guard
```

An independent downstream project then does:

```cmake
find_package(BlackholeGuard CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE BlackholeGuard::blackhole_guard)
```

`tests/consumer` is a complete out-of-tree consumer used as a release gate.

### Minimal embedding

```cpp
#include <blackhole/blackhole.hpp>
using namespace bhg;

EngineConfig config;
config.store.root = "./state";
SystemClock clock;
SystemNonceSource nonces;
Engine engine(config, clock, nonces);
engine.open();

// An adjacent owner reports what it observed. This is an observation, not authority.
engine.submit(observation, admission, reason);

Decision decision;
engine.evaluate(scope, current_generations, decision);
if (decision.has_intent) {
  // The route/convergence owner decides whether to apply it.
  route_owner.offer(decision.intent);
}
```

---

## 9. Tools and examples

| Artifact | Purpose |
|---|---|
| `bhg_cli version` | print version, durable format version and protocol version |
| `bhg_cli demo --root DIR [--label synthetic\|real]` | full detect -> localize -> fence -> restore cycle, printing each decision and the accounting closure |
| `bhg_cli report --root DIR` | reopen a durable root and print the open report, restart facts and bounded lineage |
| `bhg_cli serve --root DIR [--port N]` | run the network service until stdin closes |
| `bhg_example_fence` | library embedding example |
| `bhg_example_lineage` | persistence-is-not-liveness example |

`bhg_worker` (built with the tests) is the multiprocess participant used by the
multiprocess proofs: a real, independent, hard-killable executable.

---

## 10. Evidence classes used in this repository

| Label | Meaning |
|---|---|
| **REAL** | exercised on this host: real processes, real loopback TCP sockets, real filesystem persistence, real process termination, real fsync |
| **SYNTHETIC** | deterministic fabricated inputs standing in for adjacent-owner fabric data. All evidence fixtures are SYNTHETIC and are labelled as such in code |
| **UNSUPPORTED** | claimed nowhere and not exercised: see section 12 |

There is no hardware validation in this repository. No physical switch, NIC, RDMA,
InfiniBand, RoCE, DPU, SmartNIC or multi-node behaviour is claimed.

---

## 11. Trust boundary

* Transport is **not** authenticated and **not** encrypted. The frame integrity field is
  CRC-32C, an error-detecting checksum, not a MAC. It detects accidental corruption; it
  does not authenticate peers.
* The service binds IPv4 loopback by default. Running it on a routable interface is an
  operator decision that requires an authenticating proxy or an authenticated transport
  in front of it; neither is provided here.
* `NonceSource` produces uniqueness tokens, not secrets. Boot, incarnation, session and
  fence identities are unique in practice but are not unpredictable, and nothing depends
  on their unpredictability for security.
* Evidence freshness is compared in wall-clock nanoseconds supplied by the reporting
  source. The runtime bounds this with a configured `max_clock_skew` and refuses
  observations from beyond it, but it cannot prove that a peer's clock is honest. A
  hostile peer inside the trust boundary can lie about its own observations; the
  corroboration policy is the mitigation, not authentication.
* No cryptographic security is invented anywhere in this codebase.

---

## 12. Genuine limitations

* **SYNTHETIC fabric inputs.** All adjacent-owner inputs (path structure, generations,
  congestion and partition signals, delivery observations) are supplied by the caller.
  This repository ships deterministic synthetic producers and clearly-labelled
  fixtures. No real fabric was instrumented.
* **No hardware proof.** No switch ASIC, NIC, RDMA, DPU, NVLink, InfiniBand, RoCE or
  multi-node validation. Those are **UNSUPPORTED on this host**.
* **Sanitizer coverage.** AddressSanitizer was verified as available and is wired through
  `BHG_ENABLE_ASAN`; the sanitizer run recorded in the release report is the one that
  actually executed on this host, and the exact scope is stated there. Anything not run
  is marked UNSUPPORTED rather than implied.
* **POSIX paths are implemented but not executed here.** The process-spawn/kill helpers,
  socket layer and directory-fsync paths have POSIX implementations guarded by `_WIN32`
  that were not exercised on this Windows host.
* **Wall-clock trust.** Freshness uses source-supplied wall time bounded by a configured
  skew; it is not a secure time source.
* **Journal growth is bounded, not unbounded-durable.** When the configured journal bound
  is reached the store refuses further durable writes until a checkpoint rotates the
  journal. The refusal is explicit (`Exhausted`), and fencing authority is never granted
  without a durable record.
* **Localization is bounded to 64 candidates and 64 constraint sets.** Larger subjects
  are refused with `Unsupported` rather than answered approximately.
* **Single coordinator.** Epoch/term handling models one coordinator at a time. There is
  no quorum, leader election or multi-coordinator consensus; two coordinators over the
  same durable root are not supported and would be unsafe.

---

## 13. Testing

Every suite is a plain executable registered with ctest. **No test sets or relies on a
timeout.** A hanging test is treated as a defect to diagnose, not to hide behind a
watchdog -- which is how the head-of-line blocking defect in the server was found.

| Suite | Proves |
|---|---|
| `bhg_tests_core` | strongly typed identities, generations, checked arithmetic, canonical codec totality and boundedness, CRC-32C vectors, policy validation and round-trip, evidence structural validation, scope semantics |
| `bhg_tests_evidence` | admission, sequencing, replay/regression/rollover, freshness and clock-skew refusal, bounded retention with exact eviction accounting, deterministic ordering |
| `bhg_tests_diagnosis` | every classification, congestion vs. complete failure, partition precedence, severe-loss vs. total-loss, contradiction -> UNKNOWN, stale/generation mismatch, quality policy, corroboration rules |
| `bhg_tests_authority` | the authority ladder, binding rules, fence lifecycle, expiry, revocation on generation change, capacity bounds, decision identity and encoding, restoration gate |
| `bhg_tests_localizer` | exact solver vs. an independent exhaustive reference solver over thousands of seeded instances, a constructed greedy counterexample, ambiguity, infeasibility certificates, search-limit honesty, determinism under permutation, tamper rejection |
| `bhg_tests_property` | invariants asserted after every operation: no authority without a corroborated blackhole, accounting closure, insertion-order independence, fence dependency revocation, restoration gating, mutation-fuzzed decision decoding |
| `bhg_tests_protocol` | framing round-trip, every split point, every truncated prefix, corrupt magic/version/type/flags/integrity, oversized refusal before allocation, sticky failure, bounded buffering, message codec totality, session authority rules |
| `bhg_tests_persistence` | journal and snapshot adversarial cases (torn tails at every prefix length, all-zero tails, trailing garbage, corrupt fields, sequence gaps), transactional replacement, store open/refusal, rotation and bounds |
| `bhg_tests_restart` | epoch/incarnation advance, prior fences fenced, liveness not restored, prior diagnoses demoted to lineage, repeated restarts, snapshot replay, closed-engine refusal |
| `bhg_tests_service` | end-to-end over real loopback sockets: session establishment, foreign-session refusal, replay refusal, the full detect -> fence -> ack -> effect -> restore path, lineage/stats/checkpoint, malformed frame teardown, concurrency, start/stop lifecycle |
| `bhg_tests_concurrency` | simultaneous engine mutation, server start/stop races, blocked-read release on shutdown, close-during-use refusal, engine isolation, concurrent sessions |
| `bhg_tests_breakit` | evidence floods, split-brain disagreement, generation flapping, replay/future evidence, journal exhaustion fail-closed, localization boundary inputs, extreme values, nil identities, post-revocation acknowledgement, hostile wire input |
| `bhg_tests_scale` | completed work at multiple scales with work-counter ratios, localization effort bounds, retention bounds under long runs |
| `bhg_tests_multiprocess` | real independent processes and real loopback sockets: worker usage, hard-killed sender causing evidence loss without a false blackhole, server hard-killed after a durable commit but before acknowledgement, server killed before any commit, blocked and noisy processes, durable state shared across process restarts |

### Multiprocess and crash proof (REAL)

`bhg_tests_multiprocess` spawns `bhg_worker` as independent OS processes and terminates
them with `TerminateProcess` (an uncatchable hard kill, so no cleanup path runs):

* **Killed sender, no false blackhole.** One sender process completes normally; a second
  process is hard-killed before it reports anything. The observer process then reports
  `BLACKHOLE` for the classification with `corroborated=0`, authority `ELIGIBILITY` and
  no fence intent. Evidence loss never fabricates a confirmed blackhole.
* **Killed after durable commit, before acknowledgement.** Two sender processes produce a
  corroborated blackhole, the server durably commits the fencing intent, the downstream
  owner never acknowledges, and the server is hard-killed. A brand-new server process over
  the same durable root reports `NO_EVIDENCE` / authority `NONE` / no intent, the store
  reports one fenced prior intent, and the durable lineage contains exactly one
  interruption with reason `RESTART_FENCED_PRIOR_AUTHORITY`.
* **Killed before any commit.** The server is hard-killed immediately after binding.
  Restarting shows `boot_count = 2`, an advanced epoch, zero fenced intents, zero
  committed decisions.
* **Blocked and noisy processes.** A process that holds a session open is hard-killed; a
  process that sends framing garbage is rejected. The service keeps serving and reports
  both facts in its stats.

---

## 14. Performance

`bhg_tests_scale` measures **completed work**, not submission latency: every evaluation
in the measured loop includes a durable, fsynced commit, and every 64th iteration
includes a full checkpoint (snapshot + journal rotation + fsync).

The suite reports nanoseconds per decision at three scales and asserts on *deterministic
work counters* rather than on wall-clock ratios, because wall-clock ratios are not
reproducible across hosts:

* durable record count grows strictly proportionally with completed work (no re-writing
  of history, no accidental quadratic behaviour);
* localization search-node counts stay bounded across hop counts;
* retained lineage, ledger entries and fences stay within their configured bounds under
  a 4000-observation run.

Absolute timings from the release run are recorded in the engineering report, with the
ratio between scales.

---

## 15. Release evidence summary

Measured on the release host (Windows 11 x64, MSVC 19.44.35222, CMake 4.3.2, Ninja
1.13.2). Numbers are from the run recorded in the release report; nothing here is
projected.

| Gate | Result |
|---|---|
| Release build (MSVC `/W4 /WX /permissive- /std:c++20`) | clean, zero warnings |
| Release ctest | 14/14 suites, 159 tests, 0 failures, 8.7 s |
| Debug build (`/W4 /WX /permissive-`) | clean |
| Debug ctest | 14/14 suites, 0 failures, 10.7 s |
| AddressSanitizer build (`/fsanitize=address`, RelWithDebInfo) | clean |
| AddressSanitizer ctest | 14/14 suites, 0 failures, 15.5 s |
| ASan capability check | a deliberate heap-buffer-overflow in a scratch program is reported by AddressSanitizer, and `clang_rt.asan`/`__asan_` are present in the built test binaries |
| Static analysis | **UNSUPPORTED on this host** -- no separate analyzer toolchain is installed; the strict warning set build is the only static check that ran |
| `cmake --install` | succeeds; headers, static library, `bhg_cli`, and CMake package files installed |
| Out-of-tree consumer | copied outside the source tree, configured with `find_package(BlackholeGuard CONFIG REQUIRED)`, built, linked and run: `consumer ok: version 1.0.0 decisions=4 fences=1` |
| CLI and examples | `bhg_cli version|demo|report` and both examples run against real built artifacts and report consistent state |
| Scale (completed work, fsync per decision) | 256 decisions: 1.02 ms/decision, 262 durable records; 512: 0.93 ms/decision, 522 records; 1024: 0.94 ms/decision, 1042 records -- record count ratios 1.99x and 2.00x for 2x work, i.e. strictly linear |
| Localization effort | 4 hops: 896 search nodes; 8 hops: 2816; 16 hops: 9728, all bounded by the configured 200000-node budget |

## 16. Repository layout

```
include/blackhole/     public headers (core, domain, evidence, diagnosis, authority, localize, persist, protocol, service)
src/                   implementation
tools/                 bhg_cli
examples/              embedding examples
tests/                 test suites, test harness, multiprocess worker
tests/consumer/        independent out-of-tree consumer project
cmake/                 package config template
```

## License
Apache License 2.0. Copyright 2026 Summon Software Labs.