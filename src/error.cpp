// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/error.hpp"

namespace optical_fabric {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "ok";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NameInvalid: return "name_invalid";
    case ErrorCode::IdentityCollision: return "identity_collision";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::CapacityExceeded: return "capacity_exceeded";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::UnsupportedBoundary: return "unsupported_boundary";
    case ErrorCode::IllegalTransition: return "illegal_transition";
    case ErrorCode::StaleGeneration: return "stale_generation";
    case ErrorCode::StaleEpoch: return "stale_epoch";
    case ErrorCode::StaleIncarnation: return "stale_incarnation";
    case ErrorCode::StaleEvidence: return "stale_evidence";
    case ErrorCode::MissingEvidence: return "missing_evidence";
    case ErrorCode::ConflictingEvidence: return "conflicting_evidence";
    case ErrorCode::ConflictingClaim: return "conflicting_claim";
    case ErrorCode::ReservationExpired: return "reservation_expired";
    case ErrorCode::ReservationNotHeld: return "reservation_not_held";
    case ErrorCode::AttemptConflict: return "attempt_conflict";
    case ErrorCode::NotAuthoritative: return "not_authoritative";
    case ErrorCode::InvariantViolation: return "invariant_violation";
    case ErrorCode::StoreLocked: return "store_locked";
    case ErrorCode::StoreCorrupt: return "store_corrupt";
    case ErrorCode::StoreVersionUnsupported: return "store_version_unsupported";
    case ErrorCode::StoreIo: return "store_io";
    case ErrorCode::Closed: return "closed";
    case ErrorCode::Internal: return "internal";
  }
  return "unknown";
}

std::string_view to_string(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::None: return "none";
    case RefusalCode::InvalidArgument: return "invalid_argument";
    case RefusalCode::UnknownResource: return "unknown_resource";
    case RefusalCode::NameInvalid: return "name_invalid";
    case RefusalCode::IdentityCollision: return "identity_collision";
    case RefusalCode::UnsupportedCapability: return "unsupported_capability";
    case RefusalCode::UnsupportedEvidenceKind: return "unsupported_evidence_kind";
    case RefusalCode::MissingEvidence: return "missing_evidence";
    case RefusalCode::StaleEvidence: return "stale_evidence";
    case RefusalCode::ConflictingEvidence: return "conflicting_evidence";
    case RefusalCode::InvalidEvidence: return "invalid_evidence";
    case RefusalCode::StaleGeneration: return "stale_generation";
    case RefusalCode::StaleEpoch: return "stale_epoch";
    case RefusalCode::StaleIncarnation: return "stale_incarnation";
    case RefusalCode::StaleReservation: return "stale_reservation";
    case RefusalCode::ReservationExpired: return "reservation_expired";
    case RefusalCode::ReservationNotHeld: return "reservation_not_held";
    case RefusalCode::ReservationConflict: return "reservation_conflict";
    case RefusalCode::ConflictingClaim: return "conflicting_claim";
    case RefusalCode::DuplicateClaim: return "duplicate_claim";
    case RefusalCode::IllegalTransition: return "illegal_transition";
    case RefusalCode::ObjectRetired: return "object_retired";
    case RefusalCode::ObjectActive: return "object_active";
    case RefusalCode::ObjectNotActive: return "object_not_active";
    case RefusalCode::NotAuthoritative: return "not_authoritative";
    case RefusalCode::AttemptConflict: return "attempt_conflict";
    case RefusalCode::PlannerUnavailable: return "planner_unavailable";
    case RefusalCode::RouteUnresolved: return "route_unresolved";
    case RefusalCode::CapacityExceeded: return "capacity_exceeded";
    case RefusalCode::InvariantViolation: return "invariant_violation";
    case RefusalCode::PersistenceFailure: return "persistence_failure";
    case RefusalCode::BoundaryNotImplemented: return "boundary_not_implemented";
  }
  return "unknown";
}

bool is_conservative_refusal(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::UnsupportedCapability:
    case RefusalCode::UnsupportedEvidenceKind:
    case RefusalCode::MissingEvidence:
    case RefusalCode::StaleEvidence:
    case RefusalCode::ConflictingEvidence:
    case RefusalCode::InvalidEvidence:
    case RefusalCode::StaleGeneration:
    case RefusalCode::StaleEpoch:
    case RefusalCode::StaleIncarnation:
    case RefusalCode::StaleReservation:
    case RefusalCode::ReservationExpired:
    case RefusalCode::NotAuthoritative:
    case RefusalCode::PlannerUnavailable:
    case RefusalCode::RouteUnresolved:
    case RefusalCode::BoundaryNotImplemented:
      return true;
    default:
      return false;
  }
}

ErrorCode to_error_code(RefusalCode code) noexcept {
  switch (code) {
    case RefusalCode::None: return ErrorCode::Ok;
    case RefusalCode::InvalidArgument: return ErrorCode::InvalidArgument;
    case RefusalCode::UnknownResource: return ErrorCode::NotFound;
    case RefusalCode::NameInvalid: return ErrorCode::NameInvalid;
    case RefusalCode::IdentityCollision: return ErrorCode::IdentityCollision;
    case RefusalCode::UnsupportedCapability:
    case RefusalCode::UnsupportedEvidenceKind: return ErrorCode::Unsupported;
    case RefusalCode::MissingEvidence: return ErrorCode::MissingEvidence;
    case RefusalCode::StaleEvidence: return ErrorCode::StaleEvidence;
    case RefusalCode::ConflictingEvidence: return ErrorCode::ConflictingEvidence;
    case RefusalCode::InvalidEvidence: return ErrorCode::InvalidArgument;
    case RefusalCode::StaleGeneration: return ErrorCode::StaleGeneration;
    case RefusalCode::StaleEpoch: return ErrorCode::StaleEpoch;
    case RefusalCode::StaleIncarnation: return ErrorCode::StaleIncarnation;
    case RefusalCode::StaleReservation:
    case RefusalCode::ReservationExpired: return ErrorCode::ReservationExpired;
    case RefusalCode::ReservationNotHeld: return ErrorCode::ReservationNotHeld;
    case RefusalCode::ReservationConflict: return ErrorCode::ConflictingClaim;
    case RefusalCode::ConflictingClaim: return ErrorCode::ConflictingClaim;
    case RefusalCode::DuplicateClaim: return ErrorCode::AlreadyExists;
    case RefusalCode::IllegalTransition: return ErrorCode::IllegalTransition;
    case RefusalCode::ObjectRetired:
    case RefusalCode::ObjectActive:
    case RefusalCode::ObjectNotActive: return ErrorCode::IllegalTransition;
    case RefusalCode::NotAuthoritative: return ErrorCode::NotAuthoritative;
    case RefusalCode::AttemptConflict: return ErrorCode::AttemptConflict;
    case RefusalCode::PlannerUnavailable: return ErrorCode::Unsupported;
    case RefusalCode::RouteUnresolved: return ErrorCode::NotFound;
    case RefusalCode::CapacityExceeded: return ErrorCode::CapacityExceeded;
    case RefusalCode::InvariantViolation: return ErrorCode::InvariantViolation;
    case RefusalCode::PersistenceFailure: return ErrorCode::StoreIo;
    case RefusalCode::BoundaryNotImplemented: return ErrorCode::UnsupportedBoundary;
  }
  return ErrorCode::Internal;
}

std::string_view to_string(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::Applied: return "applied";
    case OutcomeCode::IdempotentReplay: return "idempotent_replay";
    case OutcomeCode::AlreadySatisfied: return "already_satisfied";
    case OutcomeCode::Refused: return "refused";
  }
  return "unknown";
}

}  // namespace optical_fabric
