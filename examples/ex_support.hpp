// Optical Fabric 1.0.0 - Summon Software Labs
// Shared example fixtures. Every producer here is SYNTHETIC: it observes a
// synthetic model, says so in its provenance, and never touches optical
// hardware.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"

namespace of = optical_fabric;

struct ExampleLine {
  of::SiteId site{};
  of::PortId client_a{};
  of::PortId client_b{};
  of::PortId port_a{};
  of::PortId port_b{};
  of::CrossConnectId cross_a{};
  of::CrossConnectId cross_b{};
  of::SpanId span{};
  of::ChannelId channel{};
};

/// Registers a two-node line with one span and one cross connect per node.
[[nodiscard]] ExampleLine build_example_line(of::OpticalFabric& fabric, const std::string& prefix);

/// A SYNTHETIC capability and operational-state producer.
class ExampleEvidenceSource : public of::IEvidenceSource {
 public:
  explicit ExampleEvidenceSource(std::string instance);

  [[nodiscard]] of::SourceDescriptor describe() const override;
  [[nodiscard]] of::Result<of::EvidenceBundle> poll(const of::EvidencePollRequest& request) override;

 private:
  std::string instance_;
  of::SourceId source_{};
  std::uint64_t sequence_ = 0;
};

[[nodiscard]] std::vector<of::ResourceRef> example_path_resources(const ExampleLine& line);
[[nodiscard]] std::vector<of::EvidenceKind> example_kinds();
