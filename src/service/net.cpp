#include "blackhole/service/net.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstring>

namespace bhg {

namespace {

#if defined(_WIN32)
int last_socket_error() noexcept { return ::WSAGetLastError(); }
#else
int last_socket_error() noexcept { return errno; }
#endif

std::string socket_error_text(int code) {
  return "socket error " + std::to_string(code);
}

/// Process-wide socket runtime. Every public socket entry point goes through this,
/// so the raw API is safe to use without constructing a Client or a Server first.
NetRuntime& shared_net_runtime() {
  static NetRuntime runtime;
  return runtime;
}

}  // namespace

NetRuntime::NetRuntime() {
#if defined(_WIN32)
  WSADATA data{};
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  ok_ = true;
#endif
}

NetRuntime::~NetRuntime() {
#if defined(_WIN32)
  if (ok_) ::WSACleanup();
#endif
}

ListenResult net_listen_loopback(std::uint16_t port, std::uint32_t backlog) {
  ListenResult r;
  if (!shared_net_runtime().ok()) {
    r.detail = "socket runtime unavailable";
    return r;
  }
  const socket_handle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocketHandle) {
    r.detail = socket_error_text(last_socket_error());
    return r;
  }
  int reuse = 1;
  (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
#if defined(_WIN32)
                     reinterpret_cast<const char*>(&reuse),
#else
                     &reuse,
#endif
                     static_cast<int>(sizeof(reuse)));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0) {
    r.detail = "bind failed: " + socket_error_text(last_socket_error());
    socket_handle tmp = s;
    net_close(tmp);
    return r;
  }
  if (::listen(s, static_cast<int>(backlog)) != 0) {
    r.detail = "listen failed: " + socket_error_text(last_socket_error());
    socket_handle tmp = s;
    net_close(tmp);
    return r;
  }
  std::uint16_t bound = 0;
  if (!is_affirmative(net_local_port(s, bound))) {
    r.detail = "getsockname failed";
    socket_handle tmp = s;
    net_close(tmp);
    return r;
  }
  r.outcome = Outcome::Ok;
  r.handle = s;
  r.port = bound;
  r.detail = "listening";
  return r;
}

Outcome net_connect_loopback(std::uint16_t port, socket_handle& out, std::string& detail) {
  out = kInvalidSocketHandle;
  if (!shared_net_runtime().ok()) {
    detail = "socket runtime unavailable";
    return Outcome::Unavailable;
  }
  const socket_handle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kInvalidSocketHandle) {
    detail = socket_error_text(last_socket_error());
    return Outcome::Unavailable;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), static_cast<int>(sizeof(addr))) != 0) {
    detail = "connect failed: " + socket_error_text(last_socket_error());
    socket_handle tmp = s;
    net_close(tmp);
    return Outcome::Unavailable;
  }
  out = s;
  detail = "connected";
  return Outcome::Ok;
}

Outcome net_accept(socket_handle listener, socket_handle& out, std::string& detail) {
  out = kInvalidSocketHandle;
  const socket_handle s = ::accept(listener, nullptr, nullptr);
  if (s == kInvalidSocketHandle) {
    detail = socket_error_text(last_socket_error());
    return Outcome::Unavailable;
  }
  out = s;
  detail = "accepted";
  return Outcome::Ok;
}

Outcome net_send_all(socket_handle s, std::span<const std::byte> data, std::string& detail) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
#if defined(_WIN32)
    const int n = ::send(s, reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
#else
    const ssize_t n = ::send(s, data.data() + sent, static_cast<std::size_t>(chunk), 0);
#endif
    if (n <= 0) {
      detail = "send failed: " + socket_error_text(last_socket_error());
      return Outcome::Rejected;
    }
    sent += static_cast<std::size_t>(n);
  }
  return Outcome::Ok;
}

Outcome net_recv_some(socket_handle s, std::span<std::byte> buffer, std::size_t& bytes,
                      std::string& detail) {
  bytes = 0;
  if (buffer.empty()) {
    detail = "empty receive buffer";
    return Outcome::Invalid;
  }
  const int chunk = static_cast<int>(buffer.size() > 1u << 20 ? 1u << 20 : buffer.size());
#if defined(_WIN32)
  const int n = ::recv(s, reinterpret_cast<char*>(buffer.data()), chunk, 0);
#else
  const ssize_t n = ::recv(s, buffer.data(), static_cast<std::size_t>(chunk), 0);
#endif
  if (n < 0) {
    detail = "recv failed: " + socket_error_text(last_socket_error());
    return Outcome::Rejected;
  }
  bytes = static_cast<std::size_t>(n);
  return Outcome::Ok;
}

Outcome net_wait_readable(socket_handle s, std::uint32_t timeout_ms, bool& readable) {
  readable = false;
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(s, &read_set);
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout_ms / 1000u);
  tv.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
#if defined(_WIN32)
  const int rc = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
  const int rc = ::select(s + 1, &read_set, nullptr, nullptr, &tv);
#endif
  if (rc < 0) return Outcome::Rejected;
  readable = rc > 0 && FD_ISSET(s, &read_set) != 0;
  return Outcome::Ok;
}

void net_shutdown(socket_handle s) {
  if (s == kInvalidSocketHandle) return;
#if defined(_WIN32)
  (void)::shutdown(s, SD_BOTH);
#else
  (void)::shutdown(s, SHUT_RDWR);
#endif
}

void net_close(socket_handle& s) {
  if (s == kInvalidSocketHandle) return;
#if defined(_WIN32)
  (void)::closesocket(s);
#else
  (void)::close(s);
#endif
  s = kInvalidSocketHandle;
}

Outcome net_local_port(socket_handle s, std::uint16_t& port) {
  sockaddr_in addr{};
#if defined(_WIN32)
  int len = static_cast<int>(sizeof(addr));
#else
  socklen_t len = static_cast<socklen_t>(sizeof(addr));
#endif
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return Outcome::Unavailable;
  }
  port = ntohs(addr.sin_port);
  return Outcome::Ok;
}

}  // namespace bhg
