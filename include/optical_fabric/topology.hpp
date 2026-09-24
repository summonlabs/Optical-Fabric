// Optical Fabric 1.0.0 - Summon Software Labs
// Topology registration: physical and structural identity only.
//
// Registration records describe what exists. They never describe whether it is
// healthy, and they never grant authority. Capability and operational facts
// arrive separately as evidence.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"

namespace optical_fabric {

struct SiteRegistration {
  std::string name;
  std::string region;
  SharingMode sharing = SharingMode::Exclusive;
  std::string description;
};

struct OpticalNodeRegistration {
  std::string name;
  SiteId site{};
  /// Free-form structural role label ("terminal", "roadm", "amplifier-site").
  /// It is a label, not a claimed hardware capability.
  std::string role;
  SharingMode sharing = SharingMode::Exclusive;
};

struct PortRegistration {
  std::string name;
  OpticalNodeId node{};
  /// Structural label of the port ("line", "add", "drop", "client").
  std::string role;
  SharingMode sharing = SharingMode::Exclusive;
};

struct SpanRegistration {
  std::string name;
  PortId endpoint_a{};
  PortId endpoint_b{};
  /// Declared span length in metres. Declared, not measured.
  std::uint64_t declared_length_metres = 0;
  SharingMode sharing = SharingMode::Channelized;
};

struct LineSystemRegistration {
  std::string name;
  std::vector<SpanId> spans;
  std::vector<PortId> endpoints;
  SharingMode sharing = SharingMode::Channelized;
};

struct CrossConnectRegistration {
  std::string name;
  OpticalNodeId node{};
  PortId ingress{};
  PortId egress{};
  SharingMode sharing = SharingMode::Exclusive;
};

struct ChannelRegistration {
  std::string name;
  /// Index in a declared channel plan. Declared, not measured.
  std::uint32_t channel_index = 0;
  /// Nominal centre frequency in GHz. Declared, not measured.
  std::uint64_t nominal_frequency_ghz = 0;
  /// Free-form band label ("C", "L", "O").
  std::string band;
  SharingMode sharing = SharingMode::Channelized;
};

enum class TopologyOutcome : std::uint8_t {
  Registered = 0,
  AlreadyRegistered,   ///< Byte-identical re-registration; idempotent.
  Refused,
};

[[nodiscard]] std::string_view to_string(TopologyOutcome outcome) noexcept;

struct RegistrationResult {
  TopologyOutcome outcome = TopologyOutcome::Refused;
  Refusal refusal{};
  ResourceRef resource{};
  Generation generation{};
};

/// Read-only view of one registered resource.
struct ResourceView {
  ResourceRef resource{};
  std::string name;
  SharingMode sharing = SharingMode::Exclusive;
  Generation generation{};
  Tick registered_tick{};
  /// Structural neighbours and members (site -> nodes, node -> ports,
  /// span -> endpoints, line system -> spans/endpoints, cross connect ->
  /// ingress/egress, channel -> nothing).
  std::vector<ResourceRef> related;
  /// Declared attributes, canonical order ("role=line", "index=17", ...).
  std::vector<std::string> attributes;
};

struct TopologySnapshot {
  std::vector<ResourceView> resources;
  Generation topology_generation{};
  Digest128 digest{};
};

struct CrossConnectOpportunity {
  CrossConnectId id{};
  OpticalNodeId node{};
  PortId ingress{};
  PortId egress{};
  Generation generation{};
};

}  // namespace optical_fabric
