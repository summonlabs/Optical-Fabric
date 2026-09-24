// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/client.hpp"

#include <cstdio>
#include <utility>
#include <vector>

#include "optical_fabric/digest.hpp"
#include "optical_fabric/version.hpp"
#include "socket.hpp"

namespace optical_fabric {

namespace {

constexpr std::size_t kMaxSkippedFrames = 64;

}  // namespace

std::string make_request_prefix() {
  std::uint8_t entropy[8] = {};
  fill_random_bytes(entropy, sizeof(entropy));
  std::string out = "req-";
  static constexpr char kDigits[] = "0123456789abcdef";
  for (const std::uint8_t value : entropy) {
    out.push_back(kDigits[(value >> 4) & 0x0Fu]);
    out.push_back(kDigits[value & 0x0Fu]);
  }
  out.push_back('-');
  return out;
}

FabricClient::FabricClient(ClientOptions options)
    : options_(std::move(options)), codec_(options_.limits) {
  request_prefix_ = options_.request_prefix.empty() ? make_request_prefix() : options_.request_prefix;
}

FabricClient::~FabricClient() { disconnect(); }

Status FabricClient::connect() {
  if (detail::socket_valid(socket_)) {
    return Status{};
  }
  detail::SocketHandle socket = detail::kInvalidSocket;
  const Status status = detail::socket_connect(options_.host, options_.port, socket);
  if (!status.ok()) {
    return status;
  }
  socket_ = static_cast<std::intptr_t>(socket);
  return Status{};
}

void FabricClient::disconnect() {
  if (!detail::socket_valid(static_cast<detail::SocketHandle>(socket_))) {
    return;
  }
  const auto handle = static_cast<detail::SocketHandle>(socket_);
  detail::socket_shutdown(handle);
  detail::socket_close(handle);
  socket_ = static_cast<std::intptr_t>(detail::kInvalidSocket);
}

Status FabricClient::send(const ProtocolMessage& request) {
  if (!connected()) {
    const Status status = connect();
    if (!status.ok()) {
      return status;
    }
  }
  const std::string payload = request.serialize();
  const std::string frame = codec_.encode(next_sequence_++, payload);
  return detail::socket_send_all(static_cast<detail::SocketHandle>(socket_), frame);
}

Result<ProtocolMessage> FabricClient::receive() {
  std::vector<char> buffer(64 * 1024);
  while (true) {
    std::size_t received = 0;
    const Status status = detail::socket_receive(static_cast<detail::SocketHandle>(socket_), buffer.data(),
                                                 buffer.size(), received);
    if (!status.ok()) {
      return Result<ProtocolMessage>::failure(ErrorCode::Closed, "the control connection was closed");
    }
    if (received == 0) {
      return Result<ProtocolMessage>::failure(ErrorCode::Closed, "the control connection was closed");
    }
    std::vector<Frame> frames;
    const Status fed = codec_.feed(buffer.data(), received, frames);
    if (!fed.ok()) {
      return Result<ProtocolMessage>::failure(fed.code, fed.message);
    }
    if (frames.empty()) {
      continue;
    }
    return parse_message(frames.front().payload, options_.limits);
  }
}

Result<ProtocolMessage> FabricClient::call(const ProtocolMessage& request) {
  ProtocolMessage outgoing = request;
  if (outgoing.request_id.empty()) {
    outgoing.request_id = request_prefix_ + std::to_string(++request_counter_);
  }
  const Status sent = send(outgoing);
  if (!sent.ok()) {
    return Result<ProtocolMessage>::failure(sent.code, sent.message);
  }
  for (std::size_t skipped = 0; skipped <= kMaxSkippedFrames; ++skipped) {
    const Result<ProtocolMessage> response = receive();
    if (!response.ok()) {
      return response;
    }
    if (response.value.request_id == outgoing.request_id) {
      return response;
    }
    // A response that does not match the outstanding request is never treated
    // as its answer; the client keeps reading up to a bounded number of frames.
  }
  return Result<ProtocolMessage>::failure(ErrorCode::InvalidArgument,
                                          "the control plane returned no matching response");
}

}  // namespace optical_fabric
