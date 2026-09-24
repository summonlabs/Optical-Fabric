// Optical Fabric 1.0.0 - Summon Software Labs
// Read-only store inspection.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "crc_stream.hpp"
#include "optical_fabric/persistence.hpp"
#include "optical_fabric/version.hpp"
#include "state.hpp"
#include "state_codec.hpp"
#include "store.hpp"

namespace optical_fabric {

namespace {

constexpr std::size_t kHeaderBytes = 24;
constexpr std::size_t kRecordHeaderBytes = 16;
/// Inspection decodes record contents up to this many records; beyond it, only
/// frames are counted, so inspecting a large store stays bounded.
constexpr std::size_t kDecodeLimit = 20000;

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

[[nodiscard]] bool writer_active(const std::filesystem::path& path) {
  std::filesystem::path lock_path = path;
  lock_path += ".lock";
  std::error_code error;
  if (!std::filesystem::exists(lock_path, error)) {
    return false;
  }
#if defined(_WIN32)
  // On Windows an exclusive open is the test: a live writer holds the lock file
  // with no sharing.
  std::FILE* handle = nullptr;
  if (::fopen_s(&handle, lock_path.string().c_str(), "rb") != 0 || handle == nullptr) {
    return true;
  }
  std::fclose(handle);
  return false;
#else
  return false;
#endif
}

}  // namespace

Result<PersistenceInspection> inspect_persistence(const std::filesystem::path& path, const Limits& limits) {
  PersistenceInspection inspection;
  inspection.path = path;
  std::error_code error;
  inspection.exists = std::filesystem::exists(path, error);
  if (error) {
    return Result<PersistenceInspection>::failure(ErrorCode::StoreIo, "cannot stat the store path");
  }
  if (!inspection.exists) {
    inspection.detail = "the store does not exist";
    return Result<PersistenceInspection>::success(std::move(inspection));
  }
  inspection.locked_by_writer = writer_active(path);
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) {
    return Result<PersistenceInspection>::failure(ErrorCode::StoreIo, "cannot size the store");
  }
  inspection.file_bytes = static_cast<std::uint64_t>(size);
  if (size > static_cast<std::uintmax_t>(limits.max_journal_bytes) + (1u << 20)) {
    return Result<PersistenceInspection>::failure(
        ErrorCode::CapacityExceeded, "the store is larger than the configured inspection bound");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Result<PersistenceInspection>::failure(ErrorCode::StoreIo, "cannot open the store");
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
      return Result<PersistenceInspection>::failure(ErrorCode::StoreIo, "cannot read the store");
    }
  }

  if (bytes.size() < kHeaderBytes) {
    inspection.detail = "the store header is incomplete";
    inspection.tail_torn = true;
    inspection.trailing_bytes = bytes.size();
    return Result<PersistenceInspection>::success(std::move(inspection));
  }
  if (read_u32(bytes.data()) != kStoreMagic) {
    return Result<PersistenceInspection>::failure(ErrorCode::StoreCorrupt, "the store magic is wrong");
  }
  inspection.format_version = read_u32(bytes.data() + 4);
  inspection.header_flags = read_u32(bytes.data() + 8);
  if (inspection.format_version != kStateFormatVersion) {
    return Result<PersistenceInspection>::failure(
        ErrorCode::StoreVersionUnsupported,
        "the store was written by format version " + std::to_string(inspection.format_version));
  }
  inspection.header_valid = read_u32(bytes.data() + 12) == crc32c(bytes.data(), 12);

  CanonicalHasher hasher;
  std::size_t offset = kHeaderBytes;
  bool torn = false;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < kRecordHeaderBytes) {
      torn = true;
      inspection.detail = "the store ends with an incomplete record header";
      break;
    }
    const char* header = bytes.data() + offset;
    if (read_u32(header) != kRecordMagic) {
      torn = true;
      inspection.detail = "a record frame does not begin with the record magic";
      break;
    }
    const auto kind = static_cast<RecordKind>(read_u16(header + 4));
    const std::uint32_t length = read_u32(header + 8);
    const std::uint32_t checksum = read_u32(header + 12);
    if (length > limits.max_record_payload_bytes) {
      return Result<PersistenceInspection>::failure(
          ErrorCode::StoreCorrupt, "a record declares a payload larger than the configured bound");
    }
    if (bytes.size() - offset < kRecordHeaderBytes + length) {
      torn = true;
      inspection.detail = "the store ends inside a record payload";
      break;
    }
    const std::string payload = bytes.substr(offset + kRecordHeaderBytes, length);
    if (frame_crc(static_cast<std::uint16_t>(kind), length, payload) != checksum) {
      torn = true;
      inspection.detail = "a record does not match its checksum";
      break;
    }
    bool counted = false;
    for (RecordKindCount& entry : inspection.record_counts) {
      if (entry.kind == kind) {
        entry.count += 1;
        counted = true;
        break;
      }
    }
    if (!counted) {
      inspection.record_counts.push_back(RecordKindCount{kind, 1});
    }
    inspection.records += 1;
    if (kind == RecordKind::Checkpoint) {
      inspection.checkpoints += 1;
    }
    if (inspection.records <= kDecodeLimit) {
      hasher.add_bytes(payload);
      const Result<detail::DecodedCommit> decoded = detail::CommitBundle::decode(payload, limits);
      if (decoded.ok()) {
        for (const detail::DecodedCommitItem& item : decoded.value.items) {
          if (item.type != "runtime") {
            continue;
          }
          detail::TextReader reader = item.fields;
          const Result<detail::RuntimeState> runtime = detail::decode_runtime(reader);
          if (runtime.ok()) {
            inspection.last_epoch = runtime.value.epoch.value;
            inspection.last_tick = runtime.value.tick.value;
            inspection.stored_boot_sequence = runtime.value.boot_sequence;
          }
        }
      }
    }
    offset += kRecordHeaderBytes + length;
  }
  inspection.valid_bytes = offset;
  inspection.trailing_bytes = bytes.size() - offset;
  inspection.tail_torn = torn;
  inspection.content_digest = hasher.digest();
  if (!inspection.header_valid) {
    inspection.detail = "the store header checksum does not match";
  }
  return Result<PersistenceInspection>::success(std::move(inspection));
}

}  // namespace optical_fabric
