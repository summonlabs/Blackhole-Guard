#pragma once

#include <cstdint>
#include <map>
#include <mutex>

#include "blackhole/core/ids.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/core/time.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/policy.hpp"

namespace bhg {

/// Established session authority.
///
/// A session binds a client boot/incarnation identity to exactly one connection.
/// Every request must carry the matching token and a strictly increasing sequence;
/// a mismatch is refused rather than reinterpreted, so one session can never act
/// under another session's identity, boot or epoch.
struct SessionRecord {
  SessionId id{};
  BootId client_boot{};
  IncarnationId client_incarnation{};
  SessionSequence last_seq{};
  WallNs established{};
  Provenance label{Provenance::Synthetic};
  std::uint64_t requests{0};
  std::uint64_t rejected{0};
  bool active{false};

  [[nodiscard]] bool accounting_closed() const noexcept {
    return requests + rejected > 0;
  }
};

/// Thread-safe by construction: a server serves each connection on its own thread
/// and shares one session table between them, so every entry point serialises on an
/// internal mutex. The mutex is a leaf lock: no other lock is ever acquired while it
/// is held, and no callback is invoked beneath it.
class SessionTable {
 public:
  explicit SessionTable(const FencePolicy& policy) : policy_(policy) {}

  Outcome establish(BootId boot, IncarnationId incarnation, Provenance label, WallNs now,
                    SessionId& out_id);
  /// Validates token authority and sequence continuity, then advances the sequence.
  Outcome validate(SessionId id, BootId boot, IncarnationId incarnation, SessionSequence seq,
                   ReasonCode& reason);
  void release(SessionId id);
  void clear();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::size_t capacity() const noexcept { return policy_.max_sessions; }
  [[nodiscard]] std::uint64_t established() const;
  [[nodiscard]] std::uint64_t rejected() const;
  [[nodiscard]] bool accounting_closed() const;

 private:
  FencePolicy policy_;
  mutable std::mutex mutex_;
  std::map<SessionId, SessionRecord> sessions_;
  std::uint64_t established_{0};
  std::uint64_t rejected_{0};
  std::uint64_t counter_{0};
};

}  // namespace bhg
