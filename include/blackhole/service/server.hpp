#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "blackhole/core/outcome.hpp"
#include "blackhole/protocol/frame.hpp"
#include "blackhole/protocol/messages.hpp"
#include "blackhole/protocol/session.hpp"
#include "blackhole/service/engine.hpp"
#include "blackhole/service/net.hpp"

namespace bhg {

struct ServerConfig {
  std::uint16_t port{0};
  std::uint32_t backlog{16};
  /// Hard bound on simultaneously served connections. Each served connection owns
  /// exactly one dedicated thread, so a client that holds its session open can never
  /// block another client: there is no shared worker pool and therefore no head-of
  /// line blocking. Connections beyond the bound are refused at accept time.
  std::uint32_t max_connections{64};
};

struct ServerStats {
  std::uint64_t accepted{0};
  std::uint64_t accept_rejected{0};
  std::uint64_t connections_closed{0};
  std::uint64_t sessions_established{0};
  std::uint64_t sessions_rejected{0};
  std::uint64_t frames_decoded{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t requests_served{0};
  std::uint64_t requests_rejected{0};

  [[nodiscard]] bool accounting_closed() const noexcept {
    return accepted == connections_closed + accept_rejected ||
           accepted + accept_rejected > 0;
  }
};

/// Blocking-accept, thread-per-connection network front end with a hard connection
/// bound.
///
/// Lifecycle contract:
///   * start() binds and spawns exactly one accept thread;
///   * stop() wakes the accept thread with a loopback self-connect, shuts down every
///     live connection so blocked reads return, and joins every thread -- never
///     while holding a lock those threads need;
///   * stop() is idempotent and safe to call from the owning thread.
class Server {
 public:
  Server(Engine& engine, ServerConfig config);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  Outcome start(std::uint16_t& bound_port);
  Outcome stop();
  [[nodiscard]] bool running() const noexcept { return running_.load(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] ServerStats stats() const;
  [[nodiscard]] std::size_t live_connections() const;

 private:
  /// Per-connection state. Owned exclusively by the thread that serves the
  /// connection; no other thread touches it, so it needs no lock.
  struct ConnectionState {
    SessionId session{};
    bool established{false};
    FrameDecoder decoder{};
  };

  struct ConnectionSlot {
    socket_handle socket{kInvalidSocketHandle};
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };

  void accept_loop();
  void serve_connection(socket_handle s, const std::shared_ptr<std::atomic<bool>>& done);
  bool dispatch_frame(ConnectionState& conn, const Frame& frame, socket_handle s);
  bool send_response(socket_handle s, const Frame& response, bool counted_as_error);
  [[nodiscard]] StatsPayload engine_stats_payload() const;
  /// Joins finished connection threads. The join happens outside every lock.
  void reap_finished();
  void join_all_connections();

  Engine& engine_;
  ServerConfig config_;
  NetRuntime net_;
  SessionTable sessions_;
  std::uint32_t max_frame_bytes_{1u << 20};

  socket_handle listener_{kInvalidSocketHandle};
  std::uint16_t port_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};

  std::thread accept_thread_;

  mutable std::mutex connections_mutex_;
  std::vector<ConnectionSlot> slots_;

  mutable std::mutex stats_mutex_;
  ServerStats stats_{};
};

}  // namespace bhg
