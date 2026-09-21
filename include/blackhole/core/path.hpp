#pragma once

#include <string>
#include <string_view>

namespace bhg {

/// Filesystem safety helpers. All runtime-generated names are fixed literals; user
/// supplied names are validated before use. No component may escape the root.

inline constexpr std::size_t kMaxPathComponentBytes = 128;

/// True when name is a single safe relative path component: non-empty, bounded,
/// no NUL, no path separators, no drive prefix, not "." or "..".
bool is_safe_path_component(std::string_view name) noexcept;

/// Joins a directory and a validated component using the platform separator.
std::string join_path(std::string_view dir, std::string_view component);

/// Returns the parent directory of a path ("" when there is none).
std::string parent_dir(std::string_view path);

/// Creates a directory (and its parents) if missing. Returns false on failure.
bool ensure_directory(const std::string& dir);

/// True when the path exists and is a directory.
bool is_directory(const std::string& path);

/// True when the path exists as a regular file.
bool is_regular_file(const std::string& path);

/// Removes a file if present. Returns true when the file is absent afterwards.
bool remove_file(const std::string& path);

/// Flushes a directory entry if the platform supports it (POSIX). No-op elsewhere.
void sync_directory(const std::string& dir);

}  // namespace bhg
