// Optical Fabric 1.0.0 - Summon Software Labs
// Independent consumer of the installed package.
//
// It exercises the documented consumption surface only: find the package, link
// the exported target, register a synthetic topology, ingest evidence, acquire
// authority, and drive one connectivity object to ACTIVE. Every step is
// asserted and the process exits non-zero on any failure.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "optical_fabric/optical_fabric.hpp"

namespace of = optical_fabric;

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "consumer failed: %s\n", message);
    std::exit(1);
  }
}

/// A SYNTHETIC producer: it says so, and it never claims hardware behaviour.
class ConsumerEvidence : public of::IEvidenceSource {
 public:
  ConsumerEvidence() {
    source_ = of::derive_named_id<of::SourceId>("source", "consumer-synthetic");
    kinds_ = {of::EvidenceKind::PortCapability,       of::EvidenceKind::SpanCapability,
              of::EvidenceKind::CrossConnectCapability, of::EvidenceKind::ChannelCapability,
              of::EvidenceKind::PortOperationalState,  of::EvidenceKind::SpanOperationalState,
              of::EvidenceKind::CrossConnectOperationalState,
              of::EvidenceKind::ChannelAvailability};
  }

  [[nodiscard]] of::SourceDescriptor describe() const override {
    of::SourceDescriptor descriptor;
    descriptor.id = source_;
    descriptor.runtime = "consumer-synthetic-model";
    descriptor.instance = "consumer";
    descriptor.synthetic = true;
    descriptor.produced = kinds_;
    return descriptor;
  }

  [[nodiscard]] of::Result<of::EvidenceBundle> poll(const of::EvidencePollRequest& request) override {
    of::EvidenceBundle bundle;
    bundle.source_runtime = "consumer-synthetic-model";
    bundle.source_instance = "consumer";
    bundle.source_id = source_;
    for (const of::ResourceRef subject : request.subjects) {
      for (const of::EvidenceKind kind : request.kinds) {
        of::EvidenceRecord record;
        record.subject = subject;
        record.kind = kind;
        record.state = of::EvidenceState::Known;
        record.detail = "synthetic observation from the downstream consumer";
        record.provenance.source_runtime = bundle.source_runtime;
        record.provenance.source_instance = bundle.source_instance;
        record.provenance.source_id = source_;
        record.provenance.source_sequence = request.now.value;
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

 private:
  of::SourceId source_{};
  std::vector<of::EvidenceKind> kinds_;
};

}  // namespace

int main() {
  std::printf("optical fabric consumer built against %s\n", std::string(of::version_string()).c_str());
  of::FabricOptions options;
  of::OpticalFabric fabric(options);
  require(!fabric.closed(), "the memory-only runtime refused to start");

  of::SiteRegistration site;
  site.name = "consumer.site";
  const of::RegistrationResult site_result = fabric.register_site(site);
  require(site_result.outcome != of::TopologyOutcome::Refused, "site registration failed");
  const of::SiteId site_id = of::SiteId::from_value(site_result.resource.id);

  const auto add_node = [&fabric, site_id](const char* name) {
    of::OpticalNodeRegistration node;
    node.name = name;
    node.site = site_id;
    return of::OpticalNodeId::from_value(fabric.register_optical_node(node).resource.id);
  };
  const of::OpticalNodeId node_a = add_node("consumer.node-a");
  const of::OpticalNodeId node_b = add_node("consumer.node-b");
  const auto add_port = [&fabric](const char* name, of::OpticalNodeId node) {
    of::PortRegistration port;
    port.name = name;
    port.node = node;
    return of::PortId::from_value(fabric.register_port(port).resource.id);
  };
  const of::PortId port_a = add_port("consumer.port-a", node_a);
  const of::PortId port_b = add_port("consumer.port-b", node_b);
  const of::PortId client_a = add_port("consumer.client-a", node_a);
  const of::PortId client_b = add_port("consumer.client-b", node_b);
  of::SpanRegistration span;
  span.name = "consumer.span";
  span.endpoint_a = port_a;
  span.endpoint_b = port_b;
  const of::SpanId span_id = of::SpanId::from_value(fabric.register_span(span).resource.id);
  of::CrossConnectRegistration cross_a;
  cross_a.name = "consumer.cross-a";
  cross_a.node = node_a;
  cross_a.ingress = client_a;
  cross_a.egress = port_a;
  const of::CrossConnectId cross_a_id =
      of::CrossConnectId::from_value(fabric.register_cross_connect(cross_a).resource.id);
  of::CrossConnectRegistration cross_b;
  cross_b.name = "consumer.cross-b";
  cross_b.node = node_b;
  cross_b.ingress = port_b;
  cross_b.egress = client_b;
  const of::CrossConnectId cross_b_id =
      of::CrossConnectId::from_value(fabric.register_cross_connect(cross_b).resource.id);
  of::ChannelRegistration channel;
  channel.name = "consumer.channel-0";
  channel.channel_index = 0;
  channel.nominal_frequency_ghz = 191300;
  const of::ChannelId channel_id = of::ChannelId::from_value(fabric.register_channel(channel).resource.id);

  auto evidence = std::make_shared<ConsumerEvidence>();
  fabric.register_evidence_source(evidence);
  const std::vector<of::ResourceRef> subjects = {of::as_ref(client_a), of::as_ref(cross_a_id),
                                                 of::as_ref(port_a),   of::as_ref(span_id),
                                                 of::as_ref(port_b),   of::as_ref(cross_b_id),
                                                 of::as_ref(client_b), of::as_ref(channel_id)};
  const of::Result<std::size_t> refreshed = fabric.refresh_from_source(
      evidence->describe().id, subjects, evidence->describe().produced, 4096);
  require(refreshed.ok(), "the evidence producer was refused");

  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "consumer");
  authority.scope = of::AuthorityScope::of_site(site_id);
  authority.lease_ticks = 4096;
  const of::AuthorityToken token = fabric.acquire_authority(authority).token;
  require(!token.is_nil(), "no authority was granted");

  of::ConnectivityIntent intent;
  intent.name = "consumer.path";
  intent.owner = "consumer";
  intent.source_port = client_a;
  intent.destination_port = client_b;
  intent.channel = channel_id;
  intent.reservation_ttl_ticks = 4096;
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  require(submitted.outcome == of::IntentOutcome::Accepted, "the intent was refused");

  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = token;
  const of::ValidationResult validated = fabric.validate(validation);
  require(validated.outcome != of::OutcomeCode::Refused, "validation was refused");
  require(validated.assessment.healthy(), "the evidence assessment is not healthy");

  of::ReservationRequest reservation;
  reservation.attempt = of::generate_attempt_id();
  reservation.connectivity = submitted.connectivity;
  reservation.authority = token;
  reservation.ttl_ticks = 4096;
  require(fabric.reserve(reservation).outcome == of::OutcomeCode::Applied, "the reservation was refused");

  of::ActivationRequest activation;
  activation.attempt = of::generate_attempt_id();
  activation.connectivity = submitted.connectivity;
  activation.authority = token;
  const of::ActivationResult began = fabric.begin_activation(activation);
  require(began.outcome == of::OutcomeCode::Applied, "activation did not begin");
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = token;
  commit.activation_digest = began.activation_digest;
  const of::ActivationResult activated = fabric.commit_activation(commit);
  require(activated.state == of::ConnectivityState::Active, "activation did not commit");

  const of::ActivePathReport paths = fabric.active_paths();
  require(paths.active.size() == 1, "the active path was not reported");
  require(paths.authorized.size() == 1, "the active path is not authorized");
  const of::InvariantReport invariants = fabric.verify_invariants();
  require(invariants.all_hold, "an invariant is violated");
  require(fabric.accounting().balanced, "the accounting drifted");
  const of::Result<of::PathExplanation> explanation = fabric.explain(submitted.connectivity);
  require(explanation.ok(), "the path could not be explained");
  require(explanation.value.segments.size() == 7, "the path lost a segment");

  std::printf("consumer ok: path=%s segments=%zu authorized=%zu digest=%s\n",
              explanation.value.identity.to_string().c_str(), explanation.value.segments.size(),
              paths.authorized.size(), fabric.snapshot().digest.to_string().c_str());
  return 0;
}
