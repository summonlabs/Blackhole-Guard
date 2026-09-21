#include "blackhole/protocol/session.hpp"

#include "blackhole/core/hash.hpp"

namespace bhg {

Outcome SessionTable::establish(BootId boot, IncarnationId incarnation, Provenance label,
                                WallNs now, SessionId& out_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  out_id = SessionId{};
  if (boot.is_nil() || incarnation.is_nil()) {
    ++rejected_;
    return Outcome::Invalid;
  }
  if (sessions_.size() >= policy_.max_sessions) {
    ++rejected_;
    return Outcome::Exhausted;
  }
  ++counter_;
  const SessionId id{mix64(mix64(boot.value(), incarnation.value()), counter_)};
  SessionRecord rec;
  rec.id = id;
  rec.client_boot = boot;
  rec.client_incarnation = incarnation;
  rec.last_seq = SessionSequence{0};
  rec.established = now;
  rec.label = label;
  rec.active = true;
  sessions_.emplace(id, rec);
  ++established_;
  out_id = id;
  return Outcome::Ok;
}

Outcome SessionTable::validate(SessionId id, BootId boot, IncarnationId incarnation,
                               SessionSequence seq, ReasonCode& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  reason = ReasonCode::None;
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    reason = ReasonCode::PolicyRefused;
    return Outcome::Rejected;
  }
  SessionRecord& rec = it->second;
  if (!rec.active) {
    reason = ReasonCode::PolicyRefused;
    return Outcome::Rejected;
  }
  if (boot != rec.client_boot || incarnation != rec.client_incarnation) {
    // Never let one session act under another session's identity.
    reason = ReasonCode::SourceIncarnationChanged;
    ++rec.rejected;
    ++rejected_;
    return Outcome::Rejected;
  }
  if (seq.value() != rec.last_seq.value() + 1u) {
    reason = seq.value() <= rec.last_seq.value() ? ReasonCode::ReplayedSequence
                                                 : ReasonCode::WindowViolation;
    ++rec.rejected;
    ++rejected_;
    return Outcome::Rejected;
  }
  rec.last_seq = seq;
  ++rec.requests;
  return Outcome::Ok;
}

void SessionTable::release(SessionId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = sessions_.find(id);
  if (it != sessions_.end()) {
    it->second.active = false;
    sessions_.erase(it);
  }
}

void SessionTable::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.clear();
}

std::size_t SessionTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

std::uint64_t SessionTable::established() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return established_;
}

std::uint64_t SessionTable::rejected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rejected_;
}

bool SessionTable::accounting_closed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t active = 0;
  for (const auto& kv : sessions_) {
    if (kv.second.active) ++active;
  }
  return established_ >= active;
}

}  // namespace bhg
