// Optical Fabric 1.0.0 - Summon Software Labs
// Control-plane server.
//
// The server exists so that authority, epochs and incarnations can be fenced
// across real operating-system processes. It accepts a bounded number of
// loopback connections, dispatches framed requests to the runtime, and stops
// for real: no new connections, existing sockets shut down, worker threads
// joined, writer lock released.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "optical_fabric/fabric.hpp"
#include "optical_fabric/protocol.hpp"

namespace optical_fabric {

struct NodeOptions {
  FabricOptions fabric;
  /// Loopback only. Optical Fabric does not claim multi-host operation.
  std::string bind_host = "127.0.0.1";
  /// 0 selects an ephemeral port, which the announced line reports.
  std::uint16_t port = 0;
  Limits limits{};
  /// Emit the announcement line on stdout so a supervising process can learn
  /// the bound port and the incarnation.
  bool announce = true;
};

class FabricNode {
 public:
  explicit FabricNode(NodeOptions options);
  ~FabricNode();

  FabricNode(const FabricNode&) = delete;
  FabricNode& operator=(const FabricNode&) = delete;

  /// Binds, announces, and starts accepting. Idempotent failure: a node that
  /// fails to start leaves no thread and no socket behind.
  [[nodiscard]] Status start();
  /// Serves until stop() is called or the shutdown request arrives. Blocks.
  [[nodiscard]] Status serve();
  void stop();
  [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
  [[nodiscard]] OpticalFabric& fabric() noexcept { return *fabric_; }
  [[nodiscard]] bool running() const noexcept { return running_.load(); }

 private:
  struct Impl;

  std::unique_ptr<Impl> impl_;
  std::string bind_host_;
  std::uint16_t port_ = 0;
  bool announce_ = true;
  Limits limits_;
  std::string store_label_;
  std::uint16_t bound_port_ = 0;
  std::atomic<bool> running_{false};
  std::unique_ptr<OpticalFabric> fabric_;
};

/// Applies one framed request to a runtime. Shared by the server and the
/// tests, so the transport path and the in-process path cannot drift.
[[nodiscard]] ProtocolMessage dispatch_request(OpticalFabric& fabric, const ProtocolMessage& request);

}  // namespace optical_fabric
