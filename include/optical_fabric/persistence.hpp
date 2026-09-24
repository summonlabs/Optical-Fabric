// Optical Fabric 1.0.0 - Summon Software Labs
// Durable state: framing, recovery and inspection.
//
// The store is a single append-only record stream. Every record is framed with
// its type, length and CRC-32C, and the file header is itself checksummed. A
// torn tail produced by a process that died mid-write is recovered by
// discarding exactly the bytes that do not form a complete, checksummed record;
// anything else is refused rather than guessed at.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "optical_fabric/digest.hpp"
#include "optical_fabric/error.hpp"
#include "optical_fabric/limits.hpp"

namespace optical_fabric {

inline constexpr std::uint32_t kStoreMagic = 0x4241464Fu;  // "OFAB" little-endian
inline constexpr std::uint32_t kRecordMagic = 0x4352464Fu; // "OFRC" little-endian

enum class SalvagePolicy : std::uint8_t {
  /// Refuse to open a store that is not completely intact. The file is left
  /// untouched so an operator can inspect it.
  Reject = 0,
  /// Discard a trailing run of bytes that does not form complete, checksummed
  /// records, and open the state as of the last intact record. Bytes are only
  /// ever discarded from the tail; a mid-file corruption is always a refusal.
  DiscardTail,
};

[[nodiscard]] std::string_view to_string(SalvagePolicy policy) noexcept;

struct StoreOptions {
  std::filesystem::path path;
  SalvagePolicy salvage = SalvagePolicy::DiscardTail;
  /// Flush to the operating system and, where supported, to stable storage at
  /// every commit boundary. Disabling this weakens kill/restart durability.
  bool durable_writes = true;
  /// Take the sidecar lock that makes this process the only writer.
  bool exclusive_lock = true;
  Limits limits{};
};

enum class RecoveryStatus : std::uint8_t {
  /// No store path was configured; the runtime is memory-only.
  MemoryOnly = 0,
  /// The store path did not exist and was created.
  Created,
  /// The store was opened and every record was intact.
  Clean,
  /// Trailing bytes did not form an intact record and were discarded.
  TailDiscarded,
  /// The store was refused: wrong magic, unsupported version, or corruption
  /// that cannot be attributed to a torn tail.
  Rejected,
};

[[nodiscard]] std::string_view to_string(RecoveryStatus status) noexcept;

struct RecoveryReport {
  RecoveryStatus status = RecoveryStatus::MemoryOnly;
  std::string detail;
  std::uint64_t file_bytes = 0;
  std::uint64_t valid_bytes = 0;
  std::uint64_t discarded_bytes = 0;
  std::size_t records_read = 0;
  std::size_t records_applied = 0;
  std::size_t records_discarded = 0;
  std::size_t checkpoints = 0;
  std::uint64_t previous_boot_sequence = 0;
  std::uint64_t boot_sequence = 0;
  /// Dynamic evidence that the new incarnation refused to treat as fresh.
  std::size_t evidence_marked_stale = 0;
  /// Objects that were live in a previous incarnation and are not authorized
  /// in this one until they are revalidated.
  std::size_t authority_unconfirmed = 0;
  /// Objects left mid-activation by a previous incarnation.
  std::size_t incomplete_activations = 0;
  /// Withdrawals resumed to completion by recovery.
  std::size_t resumed_withdrawals = 0;

  [[nodiscard]] bool ok() const noexcept {
    return status != RecoveryStatus::Rejected;
  }
};

/// Record kinds present in a store, for inspection and diagnostics.
enum class RecordKind : std::uint8_t {
  Unknown = 0,
  Checkpoint,
  RuntimeState,
  TopologyRegistered,
  EvidenceIngested,
  ConnectivityTransition,
  ReservationChanged,
  AuthorityGranted,
  AuthorityFenced,
  AuthorityReleased,
  AttemptRecorded,
};

/// Number of record kinds including Unknown. Derived so that adding a kind can
/// never silently fall outside the decode loop.
inline constexpr std::size_t kRecordKindCount =
    static_cast<std::size_t>(RecordKind::AttemptRecorded) + 1;

[[nodiscard]] std::string_view to_string(RecordKind kind) noexcept;

struct RecordKindCount {
  RecordKind kind = RecordKind::Unknown;
  std::size_t count = 0;
};

struct PersistenceInspection {
  std::filesystem::path path;
  bool exists = false;
  bool locked_by_writer = false;
  std::uint64_t file_bytes = 0;
  std::uint32_t format_version = 0;
  std::uint32_t header_flags = 0;
  bool header_valid = false;
  std::uint64_t valid_bytes = 0;
  std::uint64_t trailing_bytes = 0;
  bool tail_torn = false;
  std::size_t records = 0;
  std::vector<RecordKindCount> record_counts;
  std::uint64_t last_epoch = 0;
  std::uint64_t last_tick = 0;
  std::uint64_t stored_boot_sequence = 0;
  std::size_t checkpoints = 0;
  Digest128 content_digest{};
  std::string detail;
};

/// Parses a store without applying it and without taking the writer lock.
/// Never mutates the file.
[[nodiscard]] Result<PersistenceInspection> inspect_persistence(const std::filesystem::path& path,
                                                               const Limits& limits);

/// Recomputes the CRC of a serialized record frame. Exposed so tests can
/// corrupt a store with a precisely placed bit flip and prove the runtime
/// detects it.
[[nodiscard]] std::uint32_t frame_crc(std::uint16_t kind, std::uint32_t payload_length,
                                      const std::string& payload) noexcept;

}  // namespace optical_fabric
