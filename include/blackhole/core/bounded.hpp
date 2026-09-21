#pragma once

#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "blackhole/core/checked.hpp"
#include "blackhole/core/outcome.hpp"

namespace bhg {

/// Bounded retention helpers. Every internal table, history, queue and explanation is
/// explicitly bounded; overflow is a deterministic Outcome, never unchecked growth.

inline constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

/// Append honouring a hard capacity. Returns Exhausted without mutating v when full.
template <class T>
inline Outcome push_bounded(std::vector<T>& v, const T& value, std::size_t capacity) {
  if (v.size() >= capacity) return Outcome::Exhausted;
  v.push_back(value);
  return Outcome::Ok;
}

template <class T>
inline Outcome push_bounded(std::vector<T>& v, T&& value, std::size_t capacity) {
  if (v.size() >= capacity) return Outcome::Exhausted;
  v.push_back(std::move(value));
  return Outcome::Ok;
}

/// Bounded history: keeps the newest N entries and counts every drop so accounting
/// stays exact (seen == retained + dropped).
template <class T>
class BoundedHistory {
 public:
  explicit BoundedHistory(std::size_t capacity = 0) noexcept : capacity_(capacity) {}

  Outcome push(const T& value) {
    ++seen_;
    if (items_.size() >= capacity_) {
      ++dropped_;
      if (capacity_ == 0) return Outcome::Exhausted;
      items_.erase(items_.begin());
    }
    items_.push_back(value);
    return Outcome::Ok;
  }

  [[nodiscard]] const std::vector<T>& items() const noexcept { return items_; }
  [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t seen() const noexcept { return seen_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }
  [[nodiscard]] bool accounting_closed() const noexcept {
    return seen_ == static_cast<std::uint64_t>(items_.size()) + dropped_;
  }
  void clear() noexcept {
    items_.clear();
    // seen_/dropped_ intentionally preserved: they are lifetime accounting.
  }

 private:
  std::vector<T> items_;
  std::size_t capacity_{0};
  std::uint64_t seen_{0};
  std::uint64_t dropped_{0};
};

/// Truncate a string to a maximum length, appending a marker when truncated so the
/// fact of truncation is observable rather than silent.
inline void truncate_bounded(std::string& s, std::size_t max_len) {
  if (s.size() <= max_len) return;
  if (max_len <= 3) {
    s.resize(max_len);
    return;
  }
  s.resize(max_len - 3);
  s += "...";
}

/// Bounded map with deterministic (ordered) iteration.
template <class K, class V, class Cmp = std::less<K>>
class BoundedMap {
 public:
  explicit BoundedMap(std::size_t capacity = 0, Cmp cmp = Cmp{}) noexcept
      : capacity_(capacity), map_(cmp) {}

  Outcome insert(const K& key, const V& value) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second = value;
      return Outcome::Ok;
    }
    if (map_.size() >= capacity_) {
      ++rejected_;
      return Outcome::Exhausted;
    }
    map_.emplace(key, value);
    return Outcome::Ok;
  }

  [[nodiscard]] const V* find(const K& key) const noexcept {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  V* find(const K& key) noexcept {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  Outcome erase(const K& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return Outcome::NotFound;
    map_.erase(it);
    return Outcome::Ok;
  }

  [[nodiscard]] const std::map<K, V, Cmp>& items() const noexcept { return map_; }
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }
  [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
  void clear() noexcept { map_.clear(); }

 private:
  std::size_t capacity_{0};
  std::map<K, V, Cmp> map_;
  std::uint64_t rejected_{0};
};

}  // namespace bhg
