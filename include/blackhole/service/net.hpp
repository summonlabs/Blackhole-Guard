#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "blackhole/core/outcome.hpp"

namespace bhg {

/// Minimal portable socket wrapper (Winsock2 / POSIX). Only what the runtime needs:
/// loopback listen/connect/accept, blocking send/recv, orderly shutdown.
///
/// Trust boundary: the service binds loopback by default and speaks an
/// error-detecting checksummed framing with NO authentication and NO encryption.
/// The checksum detects accidental corruption; it does not authenticate peers.

#if defined(_WIN32)
using socket_handle = std::uintptr_t;
inline constexpr socket_handle kInvalidSocketHandle = static_cast<socket_handle>(~std::uintptr_t{0});
#else
using socket_handle = int;
inline constexpr socket_handle kInvalidSocketHandle = -1;
#endif

/// Process-wide socket runtime lifetime (WSAStartup/WSACleanup on Windows).
class NetRuntime {
 public:
  NetRuntime();
  ~NetRuntime();
  NetRuntime(const NetRuntime&) = delete;
  NetRuntime& operator=(const NetRuntime&) = delete;
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  bool ok_{false};
};

struct ListenResult {
  Outcome outcome{Outcome::Rejected};
  socket_handle handle{kInvalidSocketHandle};
  std::uint16_t port{0};
  std::string detail;
};

/// Binds an IPv4 loopback listener. Port 0 selects an ephemeral port.
ListenResult net_listen_loopback(std::uint16_t port, std::uint32_t backlog);

Outcome net_connect_loopback(std::uint16_t port, socket_handle& out, std::string& detail);
Outcome net_accept(socket_handle listener, socket_handle& out, std::string& detail);
Outcome net_send_all(socket_handle s, std::span<const std::byte> data, std::string& detail);
/// Blocking receive. A return value of Ok with bytes == 0 means the peer closed.
Outcome net_recv_some(socket_handle s, std::span<std::byte> buffer, std::size_t& bytes,
                      std::string& detail);

/// Waits up to timeout_ms for the socket to become readable, or for it to be closed
/// or errored. This is the primitive that makes shutdown release a blocked reader
/// portably: POSIX releases a blocked recv via shutdown(), Windows does not reliably
/// do so, and polling readiness re-checks the stop flag on every platform.
Outcome net_wait_readable(socket_handle s, std::uint32_t timeout_ms, bool& readable);
/// Wakes any thread blocked in recv on this socket. Safe to call repeatedly.
void net_shutdown(socket_handle s);
/// Closes the socket exactly once; subsequent calls are no-ops.
void net_close(socket_handle& s);
/// Port actually bound to the socket.
Outcome net_local_port(socket_handle s, std::uint16_t& port);

}  // namespace bhg
