// Optical Fabric 1.0.0 - Summon Software Labs
// Framed control-plane transport.
//
// Real loopback TCP with an explicit frame: magic, version, sequence, length
// and CRC-32C. The codec never allocates from an unvalidated length: a frame
// whose declared length exceeds the configured bound is refused before any
// buffer is sized from it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/limits.hpp"

namespace optical_fabric {

inline constexpr std::uint32_t kFrameMagic = 0x3150464Fu;  // "OFP1" little-endian
inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 24;

struct Frame {
  std::uint64_t sequence = 0;
  std::string payload;
};

/// A control-plane message is an ordered list of key/value fields with a
/// canonical serialization. Field order is preserved so that two messages that
/// differ only in order are still distinguishable by digest.
struct ProtocolMessage {
  std::string operation;
  std::string request_id;
  std::vector<std::pair<std::string, std::string>> fields;

  void set(std::string key, std::string value);
  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;
  [[nodiscard]] std::string get(std::string_view key, std::string fallback = std::string()) const;
  [[nodiscard]] bool has(std::string_view key) const noexcept;
  [[nodiscard]] std::string serialize() const;
};

[[nodiscard]] Result<ProtocolMessage> parse_message(std::string_view text, const Limits& limits);

class FrameCodec {
 public:
  explicit FrameCodec(Limits limits) : limits_(limits) {}

  [[nodiscard]] std::string encode(std::uint64_t sequence, std::string_view payload) const;

  /// Appends every complete frame contained in the fed bytes. A partial frame
  /// stays buffered; a frame that violates the framing contract fails the
  /// stream permanently (the connection is then closed by the caller).
  [[nodiscard]] Status feed(const char* data, std::size_t size, std::vector<Frame>& out);

  [[nodiscard]] std::size_t pending_bytes() const noexcept { return buffer_.size(); }
  [[nodiscard]] bool stream_failed() const noexcept { return failed_; }

 private:
  Limits limits_;
  std::string buffer_;
  bool failed_ = false;
};

/// Field-level escaping shared by the transport and the store. Values may
/// contain any byte except NUL; newlines and backslashes are escaped.
[[nodiscard]] std::string escape_value(std::string_view value);
[[nodiscard]] bool unescape_value(std::string_view text, std::string& out) noexcept;

}  // namespace optical_fabric
