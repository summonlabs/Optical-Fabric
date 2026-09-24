// Optical Fabric 1.0.0 - Summon Software Labs
// Durable record store.
//
// Layout:
//   header  : magic u32 | format version u32 | created boot u64 | header crc u32 | reserved u32
//   record* : magic u32 | kind u16 | reserved u16 | payload length u32 | crc u32 | payload
// Every record is written with a single write call and is applied only when its
// frame is complete and its checksum matches. A commit is always one record, so
// a torn write can only lose a whole commit, never half of one.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/limits.hpp"
#include "optical_fabric/persistence.hpp"

namespace optical_fabric::detail {

struct StoreRecord {
  RecordKind kind = RecordKind::Unknown;
  std::string payload;
};

class Store {
 public:
  struct Impl;

  Store(std::filesystem::path path, SalvagePolicy salvage, bool durable_writes, bool exclusive_lock,
        Limits limits);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  [[nodiscard]] bool memory_only() const noexcept { return memory_only_; }

  /// Opens (creating when absent), takes the writer lock and reads every intact
  /// record. The recovery report is always filled in, including on refusal.
  [[nodiscard]] Status open(RecoveryReport& report);

  /// Appends one commit record. Fails when the record cannot be written
  /// completely, and reports the failure rather than dropping it.
  [[nodiscard]] Status append(RecordKind kind, const std::string& payload);

  /// Replaces the store with a single checkpoint record through a temporary
  /// file and an atomic rename, bounding growth.
  [[nodiscard]] Status rewrite(RecordKind kind, const std::string& payload);

  [[nodiscard]] Status flush();

  /// Releases the writer lock and closes the handle. Idempotent.
  void close();

  [[nodiscard]] const std::vector<StoreRecord>& records() const noexcept { return records_; }
  [[nodiscard]] std::uint64_t valid_bytes() const noexcept { return valid_bytes_; }
  /// Bytes and records appended since the last checkpoint. Growth, not absolute
  /// file size, is what decides when to compact: otherwise a checkpoint larger
  /// than the threshold would trigger a rewrite on every single commit.
  [[nodiscard]] std::uint64_t appended_bytes() const noexcept { return appended_bytes_; }
  [[nodiscard]] std::size_t appended_records() const noexcept { return appended_records_; }

 private:
  std::unique_ptr<Impl> impl_;
  std::vector<StoreRecord> records_;
  std::uint64_t valid_bytes_ = 0;
  std::uint64_t appended_bytes_ = 0;
  std::size_t appended_records_ = 0;
  bool memory_only_ = false;
  bool open_ = false;
  bool durable_writes_ = true;
  Limits limits_{};
};

}  // namespace optical_fabric::detail
