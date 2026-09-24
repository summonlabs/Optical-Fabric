// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/limits.hpp"

#include <utility>

namespace optical_fabric {

Status Limits::validate() const {
  const auto require_positive = [](std::size_t value, const char* name) -> Status {
    if (value == 0) {
      return Status::failure(ErrorCode::InvalidArgument, std::string(name) + " must be greater than zero");
    }
    return Status{};
  };
  const std::pair<std::size_t, const char*> checks[] = {
      {max_sites, "max_sites"},
      {max_optical_nodes, "max_optical_nodes"},
      {max_ports, "max_ports"},
      {max_line_systems, "max_line_systems"},
      {max_spans, "max_spans"},
      {max_cross_connects, "max_cross_connects"},
      {max_channels, "max_channels"},
      {max_wavelength_references, "max_wavelength_references"},
      {max_name_length, "max_name_length"},
      {max_evidence_records, "max_evidence_records"},
      {max_evidence_sources, "max_evidence_sources"},
      {max_evidence_detail_length, "max_evidence_detail_length"},
      {max_connectivity_objects, "max_connectivity_objects"},
      {max_path_segments, "max_path_segments"},
      {max_reservations, "max_reservations"},
      {max_attempt_history_per_object, "max_attempt_history_per_object"},
      {max_attempt_history_total, "max_attempt_history_total"},
      {max_authority_grants, "max_authority_grants"},
      {max_controllers, "max_controllers"},
      {max_diagnostics, "max_diagnostics"},
      {max_fence_reasons_history, "max_fence_reasons_history"},
      {max_record_payload_bytes, "max_record_payload_bytes"},
      {max_journal_bytes, "max_journal_bytes"},
      {max_journal_records, "max_journal_records"},
      {max_persisted_attempts, "max_persisted_attempts"},
      {max_frame_payload_bytes, "max_frame_payload_bytes"},
      {max_connections, "max_connections"},
      {max_outbound_queue, "max_outbound_queue"},
  };
  for (const auto& [value, name] : checks) {
    const Status status = require_positive(value, name);
    if (!status.ok()) {
      return status;
    }
  }
  if (max_path_segments > 4096) {
    return Status::failure(ErrorCode::InvalidArgument, "max_path_segments exceeds the supported maximum of 4096");
  }
  if (max_evidence_detail_length > 65536) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "max_evidence_detail_length exceeds the supported maximum of 65536");
  }
  if (max_record_payload_bytes > (1ull << 30)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "max_record_payload_bytes exceeds the supported maximum of 1 GiB");
  }
  if (max_frame_payload_bytes > (1ull << 30)) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "max_frame_payload_bytes exceeds the supported maximum of 1 GiB");
  }
  if (max_attempt_history_per_object > max_attempt_history_total) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "max_attempt_history_per_object exceeds max_attempt_history_total");
  }
  return Status{};
}

bool fits_u32(std::size_t value) noexcept { return value <= static_cast<std::size_t>(0xFFFFFFFFull); }

bool fits_u16(std::size_t value) noexcept { return value <= static_cast<std::size_t>(0xFFFFull); }

}  // namespace optical_fabric
