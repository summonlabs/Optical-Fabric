// Optical Fabric 1.0.0 - Summon Software Labs
// Typed error and refusal vocabulary.
//
// The runtime never signals a refusal by returning a default-constructed value:
// every operation that can be refused returns an explicit code, and every code
// distinguishes "not known" from "known to be bad".
#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace optical_fabric {

enum class ErrorCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  NameInvalid,
  IdentityCollision,
  NotFound,
  AlreadyExists,
  CapacityExceeded,
  Unsupported,
  UnsupportedBoundary,
  IllegalTransition,
  StaleGeneration,
  StaleEpoch,
  StaleIncarnation,
  StaleEvidence,
  MissingEvidence,
  ConflictingEvidence,
  ConflictingClaim,
  ReservationExpired,
  ReservationNotHeld,
  AttemptConflict,
  NotAuthoritative,
  InvariantViolation,
  StoreLocked,
  StoreCorrupt,
  StoreVersionUnsupported,
  StoreIo,
  Closed,
  Internal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// Refusal codes are the lifecycle-facing vocabulary. They are recorded on the
/// connectivity object, returned to the caller, and persisted so that a restart
/// never loses the reason an object was refused.
enum class RefusalCode : std::uint8_t {
  None = 0,
  InvalidArgument,
  UnknownResource,
  NameInvalid,
  IdentityCollision,
  UnsupportedCapability,
  UnsupportedEvidenceKind,
  MissingEvidence,
  StaleEvidence,
  ConflictingEvidence,
  InvalidEvidence,
  StaleGeneration,
  StaleEpoch,
  StaleIncarnation,
  StaleReservation,
  ReservationExpired,
  ReservationNotHeld,
  ReservationConflict,
  ConflictingClaim,
  DuplicateClaim,
  IllegalTransition,
  ObjectRetired,
  ObjectActive,
  ObjectNotActive,
  NotAuthoritative,
  AttemptConflict,
  PlannerUnavailable,
  RouteUnresolved,
  CapacityExceeded,
  InvariantViolation,
  PersistenceFailure,
  BoundaryNotImplemented,
};

[[nodiscard]] std::string_view to_string(RefusalCode code) noexcept;

/// True when the refusal means "the runtime does not have the evidence or the
/// authority required to make a positive claim".
[[nodiscard]] bool is_conservative_refusal(RefusalCode code) noexcept;

/// Refusals map onto the transport-level error vocabulary so a remote caller
/// observes the same distinction a local caller does.
[[nodiscard]] ErrorCode to_error_code(RefusalCode code) noexcept;

struct Status {
  ErrorCode code = ErrorCode::Ok;
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }

  static Status success() { return Status{}; }
  static Status failure(ErrorCode code, std::string message) { return Status{code, std::move(message)}; }
};

[[nodiscard]] inline Status ok_status() { return Status{}; }

template <typename T>
struct Result {
  Status status;
  T value{};

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }

  static Result success(T value) { return Result{Status{}, std::move(value)}; }
  static Result failure(ErrorCode code, std::string message) {
    return Result{Status::failure(code, std::move(message)), T{}};
  }
};

/// Every refusal carries a code and a human-readable explanation; the
/// explanation text is never used for control flow and never parsed.
struct Refusal {
  RefusalCode code = RefusalCode::None;
  std::string detail;

  [[nodiscard]] bool refused() const noexcept { return code != RefusalCode::None; }
  static Refusal none() { return Refusal{}; }
  static Refusal of(RefusalCode code, std::string detail) { return Refusal{code, std::move(detail)}; }
};

/// Outcome of a mutating request.
enum class OutcomeCode : std::uint8_t {
  Applied = 0,         ///< The request changed authoritative state.
  IdempotentReplay,    ///< An exact replay of an already-applied request.
  AlreadySatisfied,    ///< A new request that the current state already satisfies.
  Refused,             ///< The request was rejected; see the refusal code.
};

[[nodiscard]] std::string_view to_string(OutcomeCode code) noexcept;

}  // namespace optical_fabric
