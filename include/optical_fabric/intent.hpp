// Optical Fabric 1.0.0 - Summon Software Labs
// Connectivity intents.
//
// An intent states desired connectivity. It is a request, never an authority
// and never an observation: submitting one creates a PROPOSED object and
// nothing more.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/evidence.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/lifecycle.hpp"
#include "optical_fabric/path.hpp"

namespace optical_fabric {

/// Evidence the runtime must hold before it will promote a path. The runtime
/// derives the concrete requirement list from the path itself; these switches
/// only let a caller raise the bar, never lower it.
struct IntentRequirements {
  /// Structural capability evidence for every segment (from a capability
  /// producer such as the transceiver registry, or an operator attestation).
  bool require_capability_evidence = true;
  /// Operational-state evidence for optical spans and line systems. Without a
  /// producer these requirements evaluate to UNSUPPORTED and block promotion.
  bool require_operational_evidence = true;
  /// Attachment evidence for physical endpoints (cable/attachment registry).
  bool require_attachment_evidence = false;
  /// Additional kinds the caller insists on, on top of the derived set.
  std::vector<EvidenceKind> additional_required_kinds;
  /// When false, a caller that requires hardware telemetry it cannot obtain is
  /// refused at submission instead of at activation.
  bool allow_unsupported_advisory = true;
};

struct ConnectivityIntent {
  /// Validated canonical name; the connectivity identity is derived from it.
  std::string name;
  std::string owner;
  PortId source_port{};
  PortId destination_port{};
  /// Optional requested channel. A nil channel asks the runtime to use the
  /// channel declared by the resolved route.
  ChannelId channel{};
  Direction direction = Direction::Forward;
  IntentRequirements requirements{};
  /// Lifetime of the reservation created for this intent, in ticks.
  std::uint64_t reservation_ttl_ticks = 64;
};

enum class IntentOutcome : std::uint8_t {
  /// A new connectivity object was created in PROPOSED.
  Accepted = 0,
  /// An object with the same path identity already exists; no new object was
  /// created and no state changed. Identical input yields the same answer.
  Duplicate,
  /// The intent could not be turned into a path.
  Refused,
};

[[nodiscard]] std::string_view to_string(IntentOutcome outcome) noexcept;

struct IntentSubmission {
  IntentOutcome outcome = IntentOutcome::Refused;
  Refusal refusal{};
  ConnectivityId connectivity{};
  Digest128 path_identity{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  /// True when this submission created the object; false on Duplicate.
  bool created = false;
};

}  // namespace optical_fabric
