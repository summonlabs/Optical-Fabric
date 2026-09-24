// Optical Fabric 1.0.0 - Summon Software Labs
// Path provenance and explanation.
//
// Every path record carries provenance for every segment it asserts: which
// registered resource the segment names, the generation it was validated
// against, how it claims the resource, where the route came from, and which
// observations were used to justify it.
#pragma once

#include <string>
#include <vector>

#include "optical_fabric/evidence.hpp"
#include "optical_fabric/lifecycle.hpp"
#include "optical_fabric/path.hpp"

namespace optical_fabric {

struct SegmentProvenance {
  PathSegment segment{};
  std::string resource_name;
  SharingMode sharing = SharingMode::Exclusive;
  Generation registered_generation{};
  /// "exclusive" or "channelized:<channel>" - how this segment claims the
  /// resource, which is what decides compatibility with other paths.
  std::string claim;
  EvidenceState evidence_state = EvidenceState::Unknown;
  std::vector<EvidenceEvaluation> evidence;
  /// True when the segment's generation still matches the registered resource.
  bool generation_current = false;
};

struct PathExplanation {
  ConnectivityId connectivity{};
  std::string name;
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  Digest128 identity{};
  std::string canonical_path;
  Direction direction = Direction::Forward;
  ChannelId channel{};
  std::string route_origin;
  std::vector<SegmentProvenance> segments;
  EvidenceState evidence_aggregate = EvidenceState::Unknown;
  bool evidence_healthy = false;
  bool authority_confirmed = false;
  AuthorityToken authority{};
  ReservationId reservation{};
  bool has_reservation = false;
  Tick reservation_expiry{};
  RefusalCode last_refusal = RefusalCode::None;
  std::string last_refusal_detail;
  std::vector<TransitionRecord> history;
  /// Statements the runtime can prove about this object right now, including
  /// the negative ones ("no producer exists for span operational state").
  std::vector<std::string> notes;
};

/// Where a resolved route came from. Distinguishing these is required for an
/// honest explanation: a planner proposal is not an observation.
enum class RouteOrigin : std::uint8_t {
  DeclaredRoute = 0,   ///< Resolved from registered cross-connect opportunities.
  PlannerProposal,     ///< Supplied by an attached Optical Path Planner.
  DirectRequest,       ///< The intent named both ports and a single declared opportunity.
};

[[nodiscard]] std::string_view to_string(RouteOrigin origin) noexcept;

}  // namespace optical_fabric
