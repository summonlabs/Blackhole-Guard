#pragma once

/// Real OS process control for the multiprocess proofs.
///
/// These helpers spawn independent executables, observe them, and terminate them
/// with an uncatchable hard kill. Threads are never used as a substitute here.

#include <cstdint>
#include <string>
#include <vector>

#include "blackhole/core/outcome.hpp"

namespace bhg::test {

struct ChildProcess {
#if defined(_WIN32)
  void* handle{nullptr};
  std::uint64_t pid{0};
#else
  int pid{0};
#endif
  bool valid{false};
  bool reaped{false};
};

/// Spawns an executable with the given arguments. The child inherits the parent's
/// standard handles.
Outcome spawn_process(const std::string& executable, const std::vector<std::string>& args,
                      ChildProcess& out, std::string& detail);

/// Hard kill: TerminateProcess / SIGKILL. Uncatchable, so no cleanup runs in the
/// child -- exactly what a crash boundary needs.
void kill_process_hard(ChildProcess& child);

/// Waits for the process to exit and reports its exit code.
Outcome wait_process(ChildProcess& child, std::uint32_t& exit_code, std::string& detail);

/// True while the process is still running.
bool process_running(ChildProcess& child);

/// Releases OS handles. Does not kill the process.
void close_process(ChildProcess& child);

/// Owns a child process and guarantees it is dead and reaped when the guard goes out
/// of scope, including on an early test return. A leaked long-lived child would keep
/// the harness's stdio pipe open and hang the enclosing shell, so this is not
/// optional hygiene.
class ChildGuard {
 public:
  ChildGuard() = default;
  explicit ChildGuard(ChildProcess child) : child_(child) {}
  ~ChildGuard() { reap(); }
  ChildGuard(const ChildGuard&) = delete;
  ChildGuard& operator=(const ChildGuard&) = delete;

  Outcome spawn(const std::string& executable, const std::vector<std::string>& args,
                std::string& detail) {
    reap();
    return spawn_process(executable, args, child_, detail);
  }

  [[nodiscard]] ChildProcess& get() noexcept { return child_; }
  [[nodiscard]] bool valid() const noexcept { return child_.valid; }
  void disarm() { close_process(child_); }

  void reap() {
    if (!child_.valid) return;
    kill_process_hard(child_);
    if (!child_.reaped) {
      std::uint32_t code = 0;
      std::string detail;
      (void)wait_process(child_, code, detail);
    }
    close_process(child_);
  }

 private:
  ChildProcess child_{};
};

/// Reads a whole file as text; empty string when the file does not exist.
std::string read_text_file(const std::string& path);

/// Writes text to a file, creating parent directories.
bool write_text_file(const std::string& path, const std::string& text);

/// Spins (yielding the CPU) until the file exists, bounded by an iteration count
/// rather than by a wall clock. Returns false if the bound is reached.
bool wait_for_file(const std::string& path, std::uint32_t max_spins = 5000u);

/// Waits until the named process has exited.
bool wait_for_exit(ChildProcess& child, std::uint32_t max_spins = 5000u);

}  // namespace bhg::test
