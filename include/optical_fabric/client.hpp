// Optical Fabric 1.0.0 - Summon Software Labs
// Control-plane client over real loopback TCP.
#pragma once

#include <cstdint>
#include <string>

#include "optical_fabric/limits.hpp"
#include "optical_fabric/protocol.hpp"

namespace optical_fabric {

struct ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  Limits limits{};
  std::string request_prefix;
};

class FabricClient {
 public:
  explicit FabricClient(ClientOptions options);
  ~FabricClient();

  FabricClient(const FabricClient&) = delete;
  FabricClient& operator=(const FabricClient&) = delete;

  [[nodiscard]] Status connect();
  void disconnect();
  [[nodiscard]] bool connected() const noexcept { return socket_ != kInvalidSocket; }

  /// Sends one request and waits for its response frame. Requests are matched
  /// to responses by request id; an out-of-order or mismatched response is a
  /// protocol failure, not a silent mismatch.
  [[nodiscard]] Result<ProtocolMessage> call(const ProtocolMessage& request);

  /// Sends a request without waiting for the response.
  [[nodiscard]] Status send(const ProtocolMessage& request);

  /// Reads and decodes the next response frame.
  [[nodiscard]] Result<ProtocolMessage> receive();

 private:
  static constexpr std::intptr_t kInvalidSocket = -1;

  ClientOptions options_;
  std::intptr_t socket_ = kInvalidSocket;
  FrameCodec codec_;
  std::uint64_t next_sequence_ = 1;
  std::string request_prefix_;
  std::uint64_t request_counter_ = 0;
};

/// Fresh, process-unique request id prefix.
[[nodiscard]] std::string make_request_prefix();

}  // namespace optical_fabric
