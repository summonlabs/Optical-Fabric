// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/topology.hpp"

namespace optical_fabric {

std::string_view to_string(TopologyOutcome outcome) noexcept {
  switch (outcome) {
    case TopologyOutcome::Registered: return "registered";
    case TopologyOutcome::AlreadyRegistered: return "already_registered";
    case TopologyOutcome::Refused: return "refused";
  }
  return "unknown";
}

}  // namespace optical_fabric
