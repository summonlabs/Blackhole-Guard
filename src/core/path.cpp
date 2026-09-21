#include "blackhole/core/path.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace bhg {

bool is_safe_path_component(std::string_view name) noexcept {
  if (name.empty()) return false;
  if (name.size() > kMaxPathComponentBytes) return false;
  if (name == "." || name == "..") return false;
  for (const char c : name) {
    if (c == '\0') return false;
    if (c == '/' || c == '\\') return false;
    if (c == ':') return false;
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u) return false;
  }
  return true;
}

std::string join_path(std::string_view dir, std::string_view component) {
  std::string out(dir);
  if (!out.empty()) {
    const char last = out.back();
    if (last != '/' && last != '\\') out.push_back('/');
  }
  out.append(component);
  return out;
}

std::string parent_dir(std::string_view path) {
  const std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string_view::npos) return std::string();
  return std::string(path.substr(0, pos));
}

bool ensure_directory(const std::string& dir) {
  if (dir.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir), ec);
  if (ec) return false;
  return std::filesystem::is_directory(std::filesystem::path(dir), ec);
}

bool is_directory(const std::string& path) {
  std::error_code ec;
  return std::filesystem::is_directory(std::filesystem::path(path), ec);
}

bool is_regular_file(const std::string& path) {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

bool remove_file(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(std::filesystem::path(path), ec);
  std::error_code ec2;
  return !std::filesystem::exists(std::filesystem::path(path), ec2);
}

void sync_directory(const std::string& dir) {
#if defined(_WIN32)
  (void)dir;
#else
  // Opening a directory and fsync-ing it makes a preceding rename durable.
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
#endif
}

}  // namespace bhg
