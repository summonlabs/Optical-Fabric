// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/interfaces.hpp"

namespace optical_fabric {

// Out-of-line virtual destructors keep the polymorphic boundary in one
// translation unit and give the vtable a single definition.
IEvidenceSource::~IEvidenceSource() = default;
IPlannerPort::~IPlannerPort() = default;

}  // namespace optical_fabric
