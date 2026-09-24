// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/protocol.hpp"

#include <cstring>

#include "crc_stream.hpp"
#include "text_codec.hpp"

namespace optical_fabric {

namespace {

void append_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void append_u32(std::string& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

void append_u64(std::string& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<char>((value >> (8 * index)) & 0xFFu));
  }
}

[[nodiscard]] std::uint16_t read_u16(const char* data) {
  return static_cast<std::uint16_t>(static_cast<unsigned char>(data[0]) |
                                    (static_cast<unsigned char>(data[1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(const char* data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[index])) << (8 * index);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(const char* data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[index])) << (8 * index);
  }
  return value;
}

}  // namespace

void ProtocolMessage::set(std::string key, std::string value) {
  for (auto& field : fields) {
    if (field.first == key) {
      field.second = std::move(value);
      return;
    }
  }
  fields.emplace_back(std::move(key), std::move(value));
}

const std::string* ProtocolMessage::find(std::string_view key) const noexcept {
  for (const auto& field : fields) {
    if (field.first == key) {
      return &field.second;
    }
  }
  return nullptr;
}

bool ProtocolMessage::has(std::string_view key) const noexcept { return find(key) != nullptr; }

std::string ProtocolMessage::get(std::string_view key, std::string fallback) const {
  const std::string* found = find(key);
  return found == nullptr ? std::move(fallback) : *found;
}

std::string ProtocolMessage::serialize() const {
  std::string out;
  out.append(operation);
  out.push_back('\n');
  out.append("request_id=").append(escape_value(request_id)).push_back('\n');
  for (const auto& field : fields) {
    out.append(field.first).push_back('=');
    out.append(escape_value(field.second)).push_back('\n');
  }
  return out;
}

Result<ProtocolMessage> parse_message(std::string_view text, const Limits& limits) {
  const Result<detail::TextReader> parsed = detail::TextReader::parse(text, limits);
  if (!parsed.ok()) {
    return Result<ProtocolMessage>::failure(parsed.status.code, parsed.status.message);
  }
  detail::TextReader reader = parsed.value;
  ProtocolMessage message;
  message.operation = reader.token();
  const Result<std::string> request_id = reader.require_string("request_id");
  if (!request_id.ok()) {
    return Result<ProtocolMessage>::failure(request_id.status.code, request_id.status.message);
  }
  message.request_id = request_id.value;
  message.fields = reader.take_remaining();
  return Result<ProtocolMessage>::success(std::move(message));
}

std::string FrameCodec::encode(std::uint64_t sequence, std::string_view payload) const {
  std::string frame;
  frame.reserve(kFrameHeaderBytes + payload.size());
  append_u32(frame, kFrameMagic);
  append_u16(frame, kFrameVersion);
  append_u16(frame, 0);
  append_u64(frame, sequence);
  append_u32(frame, static_cast<std::uint32_t>(payload.size()));
  detail::Crc32cStream stream;
  stream.update(payload.data(), payload.size());
  append_u32(frame, stream.finish());
  frame.append(payload);
  return frame;
}

Status FrameCodec::feed(const char* data, std::size_t size, std::vector<Frame>& out) {
  if (failed_) {
    return Status::failure(ErrorCode::InvalidArgument, "the frame stream already failed");
  }
  buffer_.append(data, size);
  std::size_t offset = 0;
  while (buffer_.size() - offset >= kFrameHeaderBytes) {
    const char* header = buffer_.data() + offset;
    if (read_u32(header) != kFrameMagic) {
      failed_ = true;
      return Status::failure(ErrorCode::InvalidArgument, "the frame magic is wrong");
    }
    if (read_u16(header + 4) != kFrameVersion) {
      failed_ = true;
      return Status::failure(ErrorCode::InvalidArgument, "the frame version is not supported");
    }
    const std::uint64_t sequence = read_u64(header + 8);
    const std::uint32_t length = read_u32(header + 16);
    const std::uint32_t checksum = read_u32(header + 20);
    if (length > limits_.max_frame_payload_bytes) {
      // The declared length is validated before anything is sized from it.
      failed_ = true;
      return Status::failure(ErrorCode::CapacityExceeded,
                             "a frame declares a payload larger than the configured bound");
    }
    const std::size_t total = kFrameHeaderBytes + length;
    if (buffer_.size() - offset < total) {
      break;
    }
    detail::Crc32cStream stream;
    stream.update(buffer_.data() + offset + kFrameHeaderBytes, length);
    if (stream.finish() != checksum) {
      failed_ = true;
      return Status::failure(ErrorCode::InvalidArgument, "a frame checksum does not match");
    }
    Frame frame;
    frame.sequence = sequence;
    frame.payload.assign(buffer_.data() + offset + kFrameHeaderBytes, length);
    out.push_back(std::move(frame));
    offset += total;
  }
  if (offset > 0) {
    buffer_.erase(0, offset);
  }
  if (buffer_.size() > limits_.max_frame_payload_bytes + kFrameHeaderBytes) {
    failed_ = true;
    return Status::failure(ErrorCode::CapacityExceeded, "the frame buffer exceeds the configured bound");
  }
  return Status{};
}

}  // namespace optical_fabric
