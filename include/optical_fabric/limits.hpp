// Optical Fabric 1.0.0 - Summon Software Labs
// Bounded resources.
//
// Every externally reachable collection, payload and history in the runtime is
// bounded by an explicit limit. Limits are validated once at construction and
// every insertion path checks the bound before allocating, so a hostile or
// buggy caller cannot grow runtime state without bound.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "optical_fabric/error.hpp"

namespace optical_fabric {

struct Limits {
  // Topology
  std::size_t max_sites = 4096;
  std::size_t max_optical_nodes = 16384;
  std::size_t max_ports = 262144;
  std::size_t max_line_systems = 16384;
  std::size_t max_spans = 65536;
  std::size_t max_cross_connects = 262144;
  std::size_t max_channels = 65536;
  std::size_t max_wavelength_references = 65536;
  std::size_t max_name_length = 128;

  // Evidence
  std::size_t max_evidence_records = 131072;
  std::size_t max_evidence_sources = 64;
  std::size_t max_evidence_detail_length = 512;

  // Connectivity
  std::size_t max_connectivity_objects = 65536;
  std::size_t max_path_segments = 256;
  std::size_t max_reservations = 65536;
  std::size_t max_attempt_history_per_object = 64;
  std::size_t max_attempt_history_total = 65536;
  std::size_t max_authority_grants = 4096;
  std::size_t max_controllers = 1024;
  std::size_t max_diagnostics = 8192;
  std::size_t max_fence_reasons_history = 1024;

  // Persistence
  std::size_t max_record_payload_bytes = 4u * 1024u * 1024u;
  std::size_t max_journal_bytes = 64u * 1024u * 1024u;
  std::size_t max_journal_records = 200000;
  std::size_t max_persisted_attempts = 65536;

  // Transport
  std::size_t max_frame_payload_bytes = 1u * 1024u * 1024u;
  std::size_t max_connections = 16;
  std::size_t max_outbound_queue = 256;

  /// Rejects degenerate limits (zero) that would make the runtime unusable and
  /// would turn a bound into a denial of service against the caller.
  [[nodiscard]] Status validate() const;
};

/// Checked narrowing. Externally derived sizes are validated before allocation;
/// a size that does not fit the target type is refused, never truncated.
[[nodiscard]] bool fits_u32(std::size_t value) noexcept;
[[nodiscard]] bool fits_u16(std::size_t value) noexcept;

}  // namespace optical_fabric
