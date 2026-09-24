// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/persistence.hpp"

namespace optical_fabric {

std::string_view to_string(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::Unknown: return "unknown";
    case RecordKind::Checkpoint: return "checkpoint";
    case RecordKind::RuntimeState: return "runtime-state";
    case RecordKind::TopologyRegistered: return "topology-registered";
    case RecordKind::EvidenceIngested: return "evidence-ingested";
    case RecordKind::ConnectivityTransition: return "connectivity-transition";
    case RecordKind::ReservationChanged: return "reservation-changed";
    case RecordKind::AuthorityGranted: return "authority-granted";
    case RecordKind::AuthorityFenced: return "authority-fenced";
    case RecordKind::AuthorityReleased: return "authority-released";
    case RecordKind::AttemptRecorded: return "attempt-recorded";
  }
  return "unknown";
}

std::string_view to_string(SalvagePolicy policy) noexcept {
  switch (policy) {
    case SalvagePolicy::Reject: return "reject";
    case SalvagePolicy::DiscardTail: return "discard-tail";
  }
  return "reject";
}

std::string_view to_string(RecoveryStatus status) noexcept {
  switch (status) {
    case RecoveryStatus::MemoryOnly: return "memory-only";
    case RecoveryStatus::Created: return "created";
    case RecoveryStatus::Clean: return "clean";
    case RecoveryStatus::TailDiscarded: return "tail-discarded";
    case RecoveryStatus::Rejected: return "rejected";
  }
  return "rejected";
}

}  // namespace optical_fabric
