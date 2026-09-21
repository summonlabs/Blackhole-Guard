#include "support/testkit.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace bhg::test {

namespace {

std::atomic<std::uint64_t> g_counter{0};

}  // namespace

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(const char* suite, const char* name, TestFn fn) {
  tests_.push_back(TestCase{suite, name, fn});
}

void Registry::fail(const char* file, int line, const std::string& message) {
  ++failures_;
  std::fprintf(stderr, "    %s:%d: %s\n", file, line, message.c_str());
}

void Registry::require_failed() { required_failed_ = true; }

int Registry::run_all(const char* suite_filter) {
  int failed_tests = 0;
  int ran = 0;
  for (const TestCase& t : tests_) {
    if (suite_filter != nullptr) {
      const std::string suite_name(t.suite);
      const std::string full = suite_name + "." + t.name;
      if (suite_name != suite_filter && full != suite_filter) continue;
    }
    ++ran;
    const int before = failures_;
    required_failed_ = false;
    std::fprintf(stdout, "[ RUN  ] %s.%s\n", t.suite, t.name);
    std::fflush(stdout);
    t.fn();
    if (failures_ != before || required_failed_) {
      ++failed_tests;
      std::fprintf(stdout, "[ FAIL ] %s.%s\n", t.suite, t.name);
    } else {
      std::fprintf(stdout, "[  OK  ] %s.%s\n", t.suite, t.name);
    }
    std::fflush(stdout);
  }
  std::fprintf(stdout, "\n%d test(s) run, %d failed, %d assertion failure(s)\n", ran,
               failed_tests, failures_);
  return failed_tests;
}

TempDir::TempDir(const char* tag) {
  std::error_code ec;
  const auto base = std::filesystem::temp_directory_path(ec);
  std::string root = ec ? std::string(".") : base.string();
  std::ostringstream name;
  name << "bhg-" << tag << "-" << unique_suffix();
  std::filesystem::path p = std::filesystem::path(root) / name.str();
  std::filesystem::create_directories(p, ec);
  path_ = p.string();
}

TempDir::~TempDir() {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(path_), ec);
}

std::string TempDir::child(const std::string& name) const {
  return (std::filesystem::path(path_) / name).string();
}

std::string current_executable_path() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH * 4];
  const DWORD n = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(MAX_PATH * 4));
  if (n == 0) return std::string();
  const std::wstring wide(buffer, buffer + n);
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  (void)::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(),
                              needed, nullptr, nullptr);
  return out;
#else
  char buffer[PATH_MAX * 2];
  const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n <= 0) return std::string();
  return std::string(buffer, static_cast<std::size_t>(n));
#endif
}

std::string sibling_executable(const std::string& name) {
  const std::string self = current_executable_path();
  if (self.empty()) return name;
  const std::filesystem::path p(self);
  const std::filesystem::path dir = p.parent_path();
  std::string leaf = name;
#if defined(_WIN32)
  if (p.extension() == ".exe" && leaf.size() < 4u) leaf += ".exe";
  if (p.extension() == ".exe" && leaf.compare(leaf.size() - 4u, 4u, ".exe") != 0) leaf += ".exe";
#endif
  return (dir / leaf).string();
}

std::string unique_suffix() {
  const std::uint64_t n = ++g_counter;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream s;
  s << static_cast<unsigned long long>(now) << "-" << n;
  return s.str();
}

std::string detail(bool v) { return v ? "true" : "false"; }
std::string detail(int v) { return std::to_string(v); }
std::string detail(unsigned v) { return std::to_string(v); }
std::string detail(long long v) { return std::to_string(v); }
std::string detail(unsigned long long v) { return std::to_string(v); }
std::string detail(const char* v) { return std::string(v); }
std::string detail(const std::string& v) { return v; }
std::string detail(std::string_view v) { return std::string(v); }

}  // namespace bhg::test

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  return ::bhg::test::Registry::instance().run_all(filter);
}
