// Optical Fabric 1.0.0 - Summon Software Labs
#include "ex_support.hpp"

#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "example failed: %s\n", message);
    std::exit(1);
  }
}

}  // namespace

ExampleLine build_example_line(of::OpticalFabric& fabric, const std::string& prefix) {
  ExampleLine line;
  of::SiteRegistration site;
  site.name = prefix + ".site";
  site.region = "synthetic";
  const of::RegistrationResult site_result = fabric.register_site(site);
  require(site_result.outcome != of::TopologyOutcome::Refused, "site registration refused");
  line.site = of::SiteId::from_value(site_result.resource.id);

  const auto add_node = [&fabric, &line, &prefix](const char* suffix) {
    of::OpticalNodeRegistration node;
    node.name = prefix + "." + suffix;
    node.site = line.site;
    node.role = "terminal";
    const of::RegistrationResult result = fabric.register_optical_node(node);
    require(result.outcome != of::TopologyOutcome::Refused, "node registration refused");
    return of::OpticalNodeId::from_value(result.resource.id);
  };
  const of::OpticalNodeId node_a = add_node("node-a");
  const of::OpticalNodeId node_b = add_node("node-b");

  const auto add_port = [&fabric, &prefix](const char* suffix, of::OpticalNodeId node) {
    of::PortRegistration port;
    port.name = prefix + "." + suffix;
    port.node = node;
    port.role = "line";
    const of::RegistrationResult result = fabric.register_port(port);
    require(result.outcome != of::TopologyOutcome::Refused, "port registration refused");
    return of::PortId::from_value(result.resource.id);
  };
  const of::PortId port_a = add_port("port-a", node_a);
  const of::PortId port_b = add_port("port-b", node_b);
  line.client_a = add_port("client-a", node_a);
  line.client_b = add_port("client-b", node_b);

  of::SpanRegistration span;
  span.name = prefix + ".span";
  span.endpoint_a = port_a;
  span.endpoint_b = port_b;
  span.declared_length_metres = 20000;
  const of::RegistrationResult span_result = fabric.register_span(span);
  require(span_result.outcome != of::TopologyOutcome::Refused, "span registration refused");
  line.span = of::SpanId::from_value(span_result.resource.id);
  line.port_a = port_a;
  line.port_b = port_b;

  of::CrossConnectRegistration cross_a;
  cross_a.name = prefix + ".cross-a";
  cross_a.node = node_a;
  cross_a.ingress = line.client_a;
  cross_a.egress = port_a;
  const of::RegistrationResult cross_a_result = fabric.register_cross_connect(cross_a);
  require(cross_a_result.outcome != of::TopologyOutcome::Refused, "cross connect refused");
  line.cross_a = of::CrossConnectId::from_value(cross_a_result.resource.id);
  of::CrossConnectRegistration cross_b;
  cross_b.name = prefix + ".cross-b";
  cross_b.node = node_b;
  cross_b.ingress = port_b;
  cross_b.egress = line.client_b;
  const of::RegistrationResult cross_b_result = fabric.register_cross_connect(cross_b);
  require(cross_b_result.outcome != of::TopologyOutcome::Refused, "cross connect refused");
  line.cross_b = of::CrossConnectId::from_value(cross_b_result.resource.id);

  of::ChannelRegistration channel;
  channel.name = prefix + ".channel-0";
  channel.channel_index = 0;
  channel.nominal_frequency_ghz = 191300;
  channel.band = "C";
  const of::RegistrationResult channel_result = fabric.register_channel(channel);
  require(channel_result.outcome != of::TopologyOutcome::Refused, "channel registration refused");
  line.channel = of::ChannelId::from_value(channel_result.resource.id);
  return line;
}

ExampleEvidenceSource::ExampleEvidenceSource(std::string instance) : instance_(std::move(instance)) {
  // Producer identity includes the instance: a restarted producer is a new
  // producer with its own sequence space, which is what lets a fresh process
  // re-attest facts that an earlier process already reported.
  source_ = of::derive_named_id<of::SourceId>("source", "example-synthetic|" + instance_);
}

of::SourceDescriptor ExampleEvidenceSource::describe() const {
  of::SourceDescriptor descriptor;
  descriptor.id = source_;
  descriptor.runtime = "example-synthetic-model";
  descriptor.instance = instance_;
  descriptor.synthetic = true;
  descriptor.produced = example_kinds();
  return descriptor;
}

of::Result<of::EvidenceBundle> ExampleEvidenceSource::poll(const of::EvidencePollRequest& request) {
  of::EvidenceBundle bundle;
  bundle.source_runtime = "example-synthetic-model";
  bundle.source_instance = instance_;
  bundle.source_id = source_;
  for (const of::ResourceRef subject : request.subjects) {
    for (const of::EvidenceKind kind : request.kinds) {
      of::EvidenceRecord record;
      record.subject = subject;
      record.kind = kind;
      record.state = of::EvidenceState::Known;
      record.detail = "synthetic observation";
      record.provenance.source_runtime = bundle.source_runtime;
      record.provenance.source_instance = instance_;
      record.provenance.source_id = source_;
      // A producer sequence must advance on every attestation; re-using one
      // would make a genuine re-attestation indistinguishable from a replay.
      record.provenance.source_sequence = ++sequence_;
      record.provenance.observed_tick = request.now;
      record.provenance.valid_until_tick = request.now.advanced_by(request.validity_span.value);
      of::CanonicalHasher hasher;
      hasher.add_field("subject", subject.to_string());
      hasher.add_field("kind", of::to_string(kind));
      record.provenance.content_digest = hasher.digest();
      bundle.records.push_back(std::move(record));
    }
  }
  return of::Result<of::EvidenceBundle>::success(std::move(bundle));
}

std::vector<of::ResourceRef> example_path_resources(const ExampleLine& line) {
  return {of::as_ref(line.client_a), of::as_ref(line.cross_a), of::as_ref(line.port_a),
          of::as_ref(line.span),     of::as_ref(line.port_b),  of::as_ref(line.cross_b),
          of::as_ref(line.client_b), of::as_ref(line.channel)};
}

std::vector<of::EvidenceKind> example_kinds() {
  return {of::EvidenceKind::PortCapability,
          of::EvidenceKind::ChannelCapability,
          of::EvidenceKind::SpanCapability,
          of::EvidenceKind::CrossConnectCapability,
          of::EvidenceKind::PortOperationalState,
          of::EvidenceKind::SpanOperationalState,
          of::EvidenceKind::CrossConnectOperationalState,
          of::EvidenceKind::ChannelAvailability,
          of::EvidenceKind::WavelengthAvailability};
}
