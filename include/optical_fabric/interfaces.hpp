// Optical Fabric 1.0.0 - Summon Software Labs
// Typed consumption boundary for adjacent runtimes.
//
// Optical Fabric owns the governed connectivity object and its lifecycle and
// authority. It does NOT own, embed or reimplement:
//   * Transceiver Registry      -> transceiver/port capability facts
//   * Cable & Attachment Registry -> attachment facts for physical endpoints
//   * Wavelength Fabric         -> channel/wavelength availability facts
//   * Optical Path Planner      -> candidate routes (proposals, not facts)
//   * Link Quality Fabric       -> span/line-system operational observations
// Each of those is consumed through the typed interfaces below. Facts arrive as
// evidence with provenance; proposals arrive as routes. Neither ever carries
// authority, and neither can promote a connectivity object by itself.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/evidence.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/path.hpp"

namespace optical_fabric {

struct SourceDescriptor {
  SourceId id{};
  /// Producing runtime name, for example "link-quality-fabric".
  std::string runtime;
  /// Producing instance label. Diagnostics only.
  std::string instance;
  /// True when the producer observes a synthetic model rather than hardware.
  /// The runtime records this in provenance; it never converts synthetic
  /// observations into claims about real optics.
  bool synthetic = true;
  /// Evidence kinds this producer can answer for.
  std::vector<EvidenceKind> produced;
  /// Kinds the producer explicitly cannot answer for. A kind listed here is
  /// reported as UNSUPPORTED, which blocks promotion exactly like a missing
  /// observation does.
  std::vector<EvidenceKind> unsupported;
};

struct EvidencePollRequest {
  Tick now{};
  /// Validity window the producer should attach to its observations.
  Tick validity_span{};
  std::vector<ResourceRef> subjects;
  std::vector<EvidenceKind> kinds;
};

/// A producer of observations. poll() is invoked by the runtime outside every
/// internal lock, and its result is validated and bounded before ingestion.
class IEvidenceSource {
 public:
  IEvidenceSource() = default;
  virtual ~IEvidenceSource();
  IEvidenceSource(const IEvidenceSource&) = delete;
  IEvidenceSource& operator=(const IEvidenceSource&) = delete;

  [[nodiscard]] virtual SourceDescriptor describe() const = 0;
  [[nodiscard]] virtual Result<EvidenceBundle> poll(const EvidencePollRequest& request) = 0;
};

struct RouteRequest {
  PortId source{};
  PortId destination{};
  ChannelId channel{};
  Direction direction = Direction::Forward;
  /// Generation of the topology the caller observed, so a planner can refuse to
  /// plan against a topology that has since changed.
  Generation topology_generation{};
};

struct RouteProposal {
  /// Ordered resource references, source endpoint first. The runtime validates
  /// every reference against the registered topology and refuses unknown ones.
  std::vector<ResourceRef> resources;
  ChannelId channel{};
  /// Which planner produced the proposal. Recorded as route provenance.
  std::string origin;
  std::string note;
};

/// A source of candidate routes. A proposal is never evidence and never
/// authority: it only tells the runtime which resources to consider.
class IPlannerPort {
 public:
  IPlannerPort() = default;
  virtual ~IPlannerPort();
  IPlannerPort(const IPlannerPort&) = delete;
  IPlannerPort& operator=(const IPlannerPort&) = delete;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual Result<RouteProposal> propose_route(const RouteRequest& request) = 0;
};

}  // namespace optical_fabric
