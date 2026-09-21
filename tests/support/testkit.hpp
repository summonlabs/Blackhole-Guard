#pragma once

/// Zero-dependency test harness used by every Blackhole Guard suite.
///
/// Design notes:
///   * no timeouts, no watchdogs, no sleeps used as synchronisation;
///   * deterministic registry order (registration order is source order);
///   * a failing assertion records and continues unless it is a REQUIRE;
///   * process exit code is the number of failed tests, so ctest sees truth.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

namespace bhg::test {

using TestFn = void (*)();

struct TestCase {
  const char* suite;
  const char* name;
  TestFn fn;
};

class Registry {
 public:
  static Registry& instance();

  void add(const char* suite, const char* name, TestFn fn);
  int run_all(const char* suite_filter);
  void fail(const char* file, int line, const std::string& message);
  void require_failed();

  [[nodiscard]] int current_failures() const noexcept { return failures_; }
  [[nodiscard]] bool current_required_failed() const noexcept { return required_failed_; }

 private:
  std::vector<TestCase> tests_;
  int failures_{0};
  bool required_failed_{false};
};

struct Registrar {
  Registrar(const char* suite, const char* name, TestFn fn) {
    Registry::instance().add(suite, name, fn);
  }
};

/// Deterministic pseudo-random source (SplitMix64). Same seed, same stream on every
/// platform and every run: reproduction parameters are the seed alone.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  std::uint32_t below(std::uint32_t bound) noexcept {
    if (bound == 0) return 0;
    return static_cast<std::uint32_t>(next() % bound);
  }
  bool coin() noexcept { return (next() & 1u) != 0u; }
  [[nodiscard]] std::uint64_t state() const noexcept { return state_; }

 private:
  std::uint64_t state_{0};
};

/// Unique scratch directory, removed recursively on destruction.
class TempDir {
 public:
  explicit TempDir(const char* tag);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string child(const std::string& name) const;

 private:
  std::string path_;
};

std::string unique_suffix();

/// Spins (yielding the CPU) until the predicate holds. Bounded by an iteration count
/// rather than a wall clock: this is a synchronisation wait, not a watchdog.
template <class Predicate>
bool wait_until(Predicate pred, std::uint32_t max_spins = 5000u) {
  for (std::uint32_t i = 0; i < max_spins; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

/// Absolute path of the running executable. Used to locate sibling helper
/// executables without depending on build-system string escaping.
std::string current_executable_path();

/// Absolute path of a sibling executable next to the running one.
std::string sibling_executable(const std::string& name);

}  // namespace bhg::test

/// Assertion macros deliberately evaluate possibly-constant conditions; MSVC's
/// C4127 ("conditional expression is constant") is noise in that position.
#if defined(_MSC_VER)
#define BHG_SUPPRESS_CONSTANT_CONDITION __pragma(warning(suppress : 4127))
#else
#define BHG_SUPPRESS_CONSTANT_CONDITION
#endif

#define BHG_TEST(suite, name)                                                        \
  static void bhg_test_##suite##_##name();                                           \
  static ::bhg::test::Registrar bhg_registrar_##suite##_##name(#suite, #name,        \
                                                              &bhg_test_##suite##_##name); \
  static void bhg_test_##suite##_##name()

#define BHG_CHECK(cond)                                                              \
  do {                                                                               \
    BHG_SUPPRESS_CONSTANT_CONDITION                                                  \
    if (!(cond)) {                                                                   \
      ::bhg::test::Registry::instance().fail(__FILE__, __LINE__, "CHECK failed: " #cond); \
    }                                                                                \
  } while (false)

#define BHG_CHECK_EQ(a, b)                                                           \
  do {                                                                               \
    BHG_SUPPRESS_CONSTANT_CONDITION                                                  \
    if (!((a) == (b))) {                                                             \
      ::bhg::test::Registry::instance().fail(                                        \
          __FILE__, __LINE__,                                                        \
          std::string("CHECK_EQ failed: " #a " == " #b " (") + ::bhg::test::detail(a) + \
              " vs " + ::bhg::test::detail(b) + ")");                                \
    }                                                                                \
  } while (false)

#define BHG_REQUIRE(cond)                                                            \
  do {                                                                               \
    BHG_SUPPRESS_CONSTANT_CONDITION                                                  \
    if (!(cond)) {                                                                   \
      ::bhg::test::Registry::instance().fail(__FILE__, __LINE__,                     \
                                             "REQUIRE failed: " #cond);              \
      ::bhg::test::Registry::instance().require_failed();                            \
      return;                                                                        \
    }                                                                                \
  } while (false)

namespace bhg::test {

std::string detail(bool v);
std::string detail(int v);
std::string detail(unsigned v);
std::string detail(long long v);
std::string detail(unsigned long long v);
std::string detail(const char* v);
std::string detail(const std::string& v);
std::string detail(std::string_view v);

template <class T>
std::string detail(const T& v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else {
    return std::to_string(v);
  }
}

}  // namespace bhg::test
