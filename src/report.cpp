// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/diagnostics.hpp"

#include "optical_fabric/explanation.hpp"

namespace optical_fabric {

std::string_view to_string(DiagnosticKind kind) noexcept {
  switch (kind) {
    case DiagnosticKind::StaleGeneration: return "stale-generation";
    case DiagnosticKind::StaleEvidence: return "stale-evidence";
    case DiagnosticKind::StaleAuthority: return "stale-authority";
    case DiagnosticKind::StaleIncarnation: return "stale-incarnation";
    case DiagnosticKind::UnsupportedEvidence: return "unsupported-evidence";
    case DiagnosticKind::MissingEvidence: return "missing-evidence";
    case DiagnosticKind::ConflictingEvidence: return "conflicting-evidence";
    case DiagnosticKind::ResourceConflict: return "resource-conflict";
    case DiagnosticKind::UnconfirmedAuthority: return "unconfirmed-authority";
    case DiagnosticKind::ReservationExpiry: return "reservation-expiry";
    case DiagnosticKind::IllegalTransitionRefused: return "illegal-transition-refused";
    case DiagnosticKind::InvariantViolation: return "invariant-violation";
    case DiagnosticKind::CapacityPressure: return "capacity-pressure";
    case DiagnosticKind::UnsupportedBoundary: return "unsupported-boundary";
  }
  return "unknown";
}

std::size_t DiagnosticsReport::count_of(DiagnosticKind kind) const noexcept {
  std::size_t count = 0;
  for (const Diagnostic& diagnostic : diagnostics) {
    if (diagnostic.kind == kind) {
      ++count;
    }
  }
  return count;
}

std::string_view to_string(RouteOrigin origin) noexcept {
  switch (origin) {
    case RouteOrigin::DeclaredRoute: return "declared-route";
    case RouteOrigin::PlannerProposal: return "planner-proposal";
    case RouteOrigin::DirectRequest: return "direct-request";
  }
  return "unknown";
}

}  // namespace optical_fabric
