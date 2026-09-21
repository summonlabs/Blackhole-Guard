#pragma once

#include <cstdint>

namespace bhg {

/// Source of uniqueness tokens for boot/incarnation/session identity.
///
/// This is explicitly NOT a cryptographic random source and nothing in this
/// runtime depends on its unpredictability for security. It exists so that two
/// process incarnations are distinguishable, and so that tests can be fully
/// deterministic.
class NonceSource {
 public:
  NonceSource() = default;
  virtual ~NonceSource();
  NonceSource(const NonceSource&) = delete;
  NonceSource& operator=(const NonceSource&) = delete;
  virtual std::uint64_t next() noexcept = 0;
};

/// Production source: mixes wall time, process id, a process-local counter and a
/// stack address. Unique in practice, not secret.
class SystemNonceSource final : public NonceSource {
 public:
  SystemNonceSource() noexcept;
  std::uint64_t next() noexcept override;

 private:
  std::uint64_t counter_{0};
  std::uint64_t seed_{0};
};

/// Deterministic source for tests and replay. Same seed, same sequence.
class SeededNonceSource final : public NonceSource {
 public:
  explicit SeededNonceSource(std::uint64_t seed) noexcept : state_(seed) {}
  std::uint64_t next() noexcept override;

 private:
  std::uint64_t state_{0};
};

/// Current process id, used for diagnostics and multiprocess observability.
std::uint64_t current_process_id() noexcept;

}  // namespace bhg
