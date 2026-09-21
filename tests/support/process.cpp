#include "support/process.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bhg::test {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
  return std::wstring(s.begin(), s.end());
}

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"') out += "\\\"";
    out.push_back(c);
  }
  out += "\"";
  return out;
}

}  // namespace

Outcome spawn_process(const std::string& executable, const std::vector<std::string>& args,
                      ChildProcess& out, std::string& detail) {
  out = ChildProcess{};
  std::string command = quote(executable);
  for (const std::string& a : args) {
    command.push_back(' ');
    command += quote(a);
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring wcommand = widen(command);
  // bInheritHandles is FALSE on purpose: a long-lived child must not hold the parent
  // harness's stdio pipes open, which would hang anything reading them.
  if (::CreateProcessW(nullptr, wcommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                       &pi) == 0) {
    detail = "CreateProcess failed";
    return Outcome::Unavailable;
  }
  ::CloseHandle(pi.hThread);
  out.handle = pi.hProcess;
  out.pid = pi.dwProcessId;
  out.valid = true;
  detail = "spawned";
  return Outcome::Ok;
}

void kill_process_hard(ChildProcess& child) {
  if (!child.valid || child.handle == nullptr) return;
  (void)::TerminateProcess(static_cast<HANDLE>(child.handle), 0xDEADu);
}

Outcome wait_process(ChildProcess& child, std::uint32_t& exit_code, std::string& detail) {
  exit_code = 0;
  if (!child.valid || child.handle == nullptr) {
    detail = "invalid child";
    return Outcome::Invalid;
  }
  const DWORD r = ::WaitForSingleObject(static_cast<HANDLE>(child.handle), INFINITE);
  if (r != WAIT_OBJECT_0) {
    detail = "wait failed";
    return Outcome::Rejected;
  }
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code) == 0) {
    detail = "GetExitCodeProcess failed";
    return Outcome::Rejected;
  }
  exit_code = static_cast<std::uint32_t>(code);
  child.reaped = true;
  detail = "exited";
  return Outcome::Ok;
}

bool process_running(ChildProcess& child) {
  if (!child.valid || child.handle == nullptr) return false;
  DWORD code = 0;
  if (::GetExitCodeProcess(static_cast<HANDLE>(child.handle), &code) == 0) return false;
  return code == STILL_ACTIVE;
}

void close_process(ChildProcess& child) {
  if (child.handle != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(child.handle));
    child.handle = nullptr;
  }
  child.valid = false;
}

#else  // POSIX

Outcome spawn_process(const std::string& executable, const std::vector<std::string>& args,
                      ChildProcess& out, std::string& detail) {
  out = ChildProcess{};
  std::vector<std::string> storage;
  storage.push_back(executable);
  for (const std::string& a : args) storage.push_back(a);
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1u);
  for (std::string& s : storage) argv.push_back(s.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    detail = "fork failed";
    return Outcome::Unavailable;
  }
  if (pid == 0) {
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  out.pid = static_cast<int>(pid);
  out.valid = true;
  detail = "spawned";
  return Outcome::Ok;
}

void kill_process_hard(ChildProcess& child) {
  if (!child.valid) return;
  (void)::kill(static_cast<pid_t>(child.pid), SIGKILL);
}

Outcome wait_process(ChildProcess& child, std::uint32_t& exit_code, std::string& detail) {
  exit_code = 0;
  if (!child.valid) {
    detail = "invalid child";
    return Outcome::Invalid;
  }
  int status = 0;
  if (::waitpid(static_cast<pid_t>(child.pid), &status, 0) < 0) {
    detail = "waitpid failed";
    return Outcome::Rejected;
  }
  exit_code = static_cast<std::uint32_t>(status);
  child.reaped = true;
  detail = "exited";
  return Outcome::Ok;
}

bool process_running(ChildProcess& child) {
  if (!child.valid || child.reaped) return false;
  int status = 0;
  const pid_t r = ::waitpid(static_cast<pid_t>(child.pid), &status, WNOHANG);
  if (r == 0) return true;
  child.reaped = true;
  return false;
}

void close_process(ChildProcess& child) { child.valid = false; }

#endif

std::string read_text_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::string();
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool write_text_file(const std::string& path, const std::string& text) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text;
  f.flush();
  return static_cast<bool>(f);
}

bool wait_for_file(const std::string& path, std::uint32_t max_spins) {
  std::error_code ec;
  for (std::uint32_t i = 0; i < max_spins; ++i) {
    if (std::filesystem::exists(std::filesystem::path(path), ec)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

bool wait_for_exit(ChildProcess& child, std::uint32_t max_spins) {
  for (std::uint32_t i = 0; i < max_spins; ++i) {
    if (!process_running(child)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return !process_running(child);
}

}  // namespace bhg::test
