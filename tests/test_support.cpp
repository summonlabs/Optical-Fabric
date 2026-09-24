// Optical Fabric 1.0.0 - Summon Software Labs
#include "test_support.hpp"

#include <utility>

namespace of_test {

namespace of = optical_fabric;

LineFixture build_line(of::OpticalFabric& fabric, const std::string& prefix) {
  LineFixture fixture;
  of::SiteRegistration site;
  site.name = prefix + ".site";
  site.region = "synthetic-region";
  // A recovered store already contains the topology, so a fixture that is
  // rebuilt after a restart must accept an idempotent re-registration.
  const of::RegistrationResult site_result = fabric.register_site(site);
  OF_REQUIRE_MSG(site_result.outcome != of::TopologyOutcome::Refused,
                 std::string(of::to_string(site_result.refusal.code)) + " " + site_result.refusal.detail);
  fixture.site = of::SiteId::from_value(site_result.resource.id);

  of::OpticalNodeRegistration node_a;
  node_a.name = prefix + ".node-a";
  node_a.site = fixture.site;
  node_a.role = "terminal";
  const of::RegistrationResult node_a_result = fabric.register_optical_node(node_a);
  OF_REQUIRE(node_a_result.outcome != of::TopologyOutcome::Refused);
  fixture.node_a = of::OpticalNodeId::from_value(node_a_result.resource.id);

  of::OpticalNodeRegistration node_b;
  node_b.name = prefix + ".node-b";
  node_b.site = fixture.site;
  node_b.role = "terminal";
  const of::RegistrationResult node_b_result = fabric.register_optical_node(node_b);
  OF_REQUIRE(node_b_result.outcome != of::TopologyOutcome::Refused);
  fixture.node_b = of::OpticalNodeId::from_value(node_b_result.resource.id);

  const auto add_port = [&fabric, &prefix](const char* suffix, of::OpticalNodeId node) {
    of::PortRegistration port;
    port.name = prefix + "." + suffix;
    port.node = node;
    port.role = "line";
    const of::RegistrationResult result = fabric.register_port(port);
    OF_REQUIRE_MSG(result.outcome != of::TopologyOutcome::Refused, result.refusal.detail);
    return of::PortId::from_value(result.resource.id);
  };
  fixture.port_a = add_port("port-a", fixture.node_a);
  fixture.port_a_far = add_port("port-a-far", fixture.node_a);
  fixture.port_b = add_port("port-b", fixture.node_b);
  fixture.port_b_far = add_port("port-b-far", fixture.node_b);

  of::SpanRegistration span;
  span.name = prefix + ".span";
  span.endpoint_a = fixture.port_a;
  span.endpoint_b = fixture.port_b;
  span.declared_length_metres = 40000;
  const of::RegistrationResult span_result = fabric.register_span(span);
  OF_REQUIRE(span_result.outcome != of::TopologyOutcome::Refused);
  fixture.span = of::SpanId::from_value(span_result.resource.id);

  of::CrossConnectRegistration cross_a;
  cross_a.name = prefix + ".cross-a";
  cross_a.node = fixture.node_a;
  cross_a.ingress = fixture.port_a_far;
  cross_a.egress = fixture.port_a;
  const of::RegistrationResult cross_a_result = fabric.register_cross_connect(cross_a);
  OF_REQUIRE(cross_a_result.outcome != of::TopologyOutcome::Refused);
  fixture.cross_a = of::CrossConnectId::from_value(cross_a_result.resource.id);

  of::CrossConnectRegistration cross_b;
  cross_b.name = prefix + ".cross-b";
  cross_b.node = fixture.node_b;
  cross_b.ingress = fixture.port_b;
  cross_b.egress = fixture.port_b_far;
  const of::RegistrationResult cross_b_result = fabric.register_cross_connect(cross_b);
  OF_REQUIRE(cross_b_result.outcome != of::TopologyOutcome::Refused);
  fixture.cross_b = of::CrossConnectId::from_value(cross_b_result.resource.id);

  const auto add_channel = [&fabric, &prefix](const char* suffix, std::uint32_t index) {
    of::ChannelRegistration channel;
    channel.name = prefix + "." + suffix;
    channel.channel_index = index;
    channel.nominal_frequency_ghz = 191300 + index * 50;
    channel.band = "C";
    const of::RegistrationResult result = fabric.register_channel(channel);
    OF_REQUIRE(result.outcome != of::TopologyOutcome::Refused);
    return of::ChannelId::from_value(result.resource.id);
  };
  fixture.channel = add_channel("ch-0", 0);
  fixture.channel_alt = add_channel("ch-1", 1);
  return fixture;
}

of::SiteId build_site(of::OpticalFabric& fabric, const std::string& prefix) {
  of::SiteRegistration site;
  site.name = prefix + ".site";
  const of::RegistrationResult result = fabric.register_site(site);
  OF_REQUIRE(result.outcome == of::TopologyOutcome::Registered);
  return of::SiteId::from_value(result.resource.id);
}

of::ConnectivityIntent make_intent(const LineFixture& fixture, const std::string& name) {
  of::ConnectivityIntent intent;
  intent.name = name;
  intent.owner = "controller-a";
  intent.source_port = fixture.port_a_far;
  intent.destination_port = fixture.port_b_far;
  intent.channel = fixture.channel;
  intent.direction = of::Direction::Forward;
  intent.reservation_ttl_ticks = 256;
  return intent;
}

of::AuthorityToken acquire_site_authority(of::OpticalFabric& fabric, const std::string& holder,
                                          of::SiteId site, std::uint64_t lease_ticks) {
  of::AuthorityRequest request;
  request.attempt = of::generate_attempt_id();
  request.holder = of::derive_named_id<of::ControllerId>("controller", holder);
  request.holder_label = holder;
  request.scope = of::AuthorityScope::of_site(site);
  request.lease_ticks = lease_ticks;
  request.reason = "test";
  const of::AuthorityResult result = fabric.acquire_authority(request);
  OF_REQUIRE_MSG(result.outcome != of::OutcomeCode::Refused,
                 std::string(of::to_string(result.refusal.code)) + " " + result.refusal.detail);
  return result.token;
}SyntheticSource::SyntheticSource(std::string runtime, std::string instance,
                                 std::vector<of::EvidenceKind> produced)
    : runtime_(std::move(runtime)), instance_(std::move(instance)), produced_(std::move(produced)) {
  source_ = of::derive_named_id<of::SourceId>("source", runtime_ + "|" + instance_);
}

of::SourceDescriptor SyntheticSource::describe() const {
  of::SourceDescriptor descriptor;
  descriptor.id = source_;
  descriptor.runtime = runtime_;
  descriptor.instance = instance_;
  descriptor.synthetic = true;
  descriptor.produced = produced_;
  descriptor.unsupported = unsupported_;
  return descriptor;
}

void SyntheticSource::declare_unsupported(std::vector<of::EvidenceKind> kinds) {
  unsupported_ = std::move(kinds);
}

void SyntheticSource::set_state(of::EvidenceKind kind, of::EvidenceState state) {
  states_.emplace_back(kind, state);
}

void SyntheticSource::set_digest_salt(std::uint64_t salt) { digest_salt_ = salt; }
void SyntheticSource::set_validity_span_override(std::uint64_t span) { validity_override_ = span; }
void SyntheticSource::set_polls_before_answer(std::size_t polls) { polls_before_answer_ = polls; }

of::Result<of::EvidenceBundle> SyntheticSource::poll(const of::EvidencePollRequest& request) {
  poll_count_ += 1;
  of::EvidenceBundle bundle;
  bundle.source_runtime = runtime_;
  bundle.source_instance = instance_;
  bundle.source_id = source_;
  if (poll_count_ <= polls_before_answer_) {
    return of::Result<of::EvidenceBundle>::success(std::move(bundle));
  }
  const std::uint64_t span = validity_override_ == 0 ? request.validity_span.value : validity_override_;
  for (const of::ResourceRef subject : request.subjects) {
    for (const of::EvidenceKind kind : request.kinds) {
      bool produces = false;
      for (const of::EvidenceKind candidate : produced_) {
        if (candidate == kind) {
          produces = true;
          break;
        }
      }
      if (!produces) {
        continue;
      }
      of::EvidenceState state = of::EvidenceState::Known;
      for (const auto& entry : states_) {
        if (entry.first == kind) {
          state = entry.second;
        }
      }
      of::EvidenceRecord record;
      record.subject = subject;
      record.kind = kind;
      record.state = state;
      record.advisory = kind == of::EvidenceKind::OpticalPowerTelemetry ||
                        kind == of::EvidenceKind::AlignmentQuality;
      record.detail = std::string("synthetic ") + std::string(of::to_string(kind));
      record.provenance.source_runtime = runtime_;
      record.provenance.source_instance = instance_;
      record.provenance.source_id = source_;
      record.provenance.source_sequence = poll_count_;
      record.provenance.observed_tick = request.now;
      record.provenance.valid_until_tick = request.now.advanced_by(span);
      of::CanonicalHasher hasher;
      hasher.add_field("source", runtime_);
      hasher.add_field("kind", of::to_string(kind));
      hasher.add_field("subject", subject.to_string());
      hasher.add_field("state", of::to_string(state));
      hasher.add_field("salt", digest_salt_);
      record.provenance.content_digest = hasher.digest();
      bundle.records.push_back(std::move(record));
    }
  }
  return of::Result<of::EvidenceBundle>::success(std::move(bundle));
}

void ScriptedPlanner::set_route(std::vector<of::ResourceRef> resources, of::ChannelId channel) {
  resources_ = std::move(resources);
  channel_ = channel;
  failing_ = false;
}

void ScriptedPlanner::fail_with(of::ErrorCode code, std::string message) {
  failing_ = true;
  failure_code_ = code;
  failure_message_ = std::move(message);
}

of::Result<of::RouteProposal> ScriptedPlanner::propose_route(const of::RouteRequest& request) {
  calls_ += 1;
  (void)request;
  if (failing_) {
    return of::Result<of::RouteProposal>::failure(failure_code_, failure_message_);
  }
  of::RouteProposal proposal;
  proposal.resources = resources_;
  proposal.channel = channel_;
  proposal.origin = name_;
  proposal.note = "synthetic planner proposal";
  return of::Result<of::RouteProposal>::success(std::move(proposal));
}

of::EvidenceRecord make_record(of::ResourceRef subject, of::EvidenceKind kind, of::EvidenceState state,
                               const std::string& runtime, of::SourceId source, std::uint64_t sequence,
                               of::Tick now, std::uint64_t validity_span) {
  of::EvidenceRecord record;
  record.subject = subject;
  record.kind = kind;
  record.state = state;
  record.detail = "synthetic observation";
  record.provenance.source_runtime = runtime;
  record.provenance.source_instance = runtime + "-instance";
  record.provenance.source_id = source;
  record.provenance.source_sequence = sequence;
  record.provenance.observed_tick = now;
  record.provenance.valid_until_tick = now.advanced_by(validity_span);
  of::CanonicalHasher hasher;
  hasher.add_field("runtime", runtime);
  hasher.add_field("kind", of::to_string(kind));
  hasher.add_field("subject", subject.to_string());
  hasher.add_field("state", of::to_string(state));
  hasher.add_field("sequence", sequence);
  record.provenance.content_digest = hasher.digest();
  return record;
}

std::shared_ptr<SyntheticSource> attach_full_evidence(of::OpticalFabric& fabric,
                                                      const std::string& instance) {
  const std::vector<of::EvidenceKind> kinds = {
      of::EvidenceKind::PortCapability,
      of::EvidenceKind::ChannelCapability,
      of::EvidenceKind::LineSystemCapability,
      of::EvidenceKind::SpanCapability,
      of::EvidenceKind::CrossConnectCapability,
      of::EvidenceKind::PortOperationalState,
      of::EvidenceKind::SpanOperationalState,
      of::EvidenceKind::LineSystemOperationalState,
      of::EvidenceKind::CrossConnectOperationalState,
      of::EvidenceKind::ChannelAvailability,
      of::EvidenceKind::WavelengthAvailability,
  };
  auto source = std::make_shared<SyntheticSource>("synthetic-line-model", instance, kinds);
  fabric.register_evidence_source(source);
  return source;
}

of::ActivationResult activate_path(of::OpticalFabric& fabric, const LineFixture& fixture,
                                   const std::string& name, const of::AuthorityToken& token,
                                   std::uint64_t ttl_ticks) {
  const of::ConnectivityIntent intent = make_intent(fixture, name);
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  OF_REQUIRE_MSG(submitted.outcome == of::IntentOutcome::Accepted,
                 std::string(of::to_string(submitted.refusal.code)) + " " + submitted.refusal.detail);

  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = token;
  const of::ValidationResult validated = fabric.validate(validation);
  OF_REQUIRE_MSG(validated.outcome != of::OutcomeCode::Refused,
                 std::string(of::to_string(validated.refusal.code)) + " " + validated.refusal.detail);

  of::ReservationRequest reservation;
  reservation.attempt = of::generate_attempt_id();
  reservation.connectivity = submitted.connectivity;
  reservation.authority = token;
  reservation.ttl_ticks = ttl_ticks;
  const of::ReservationResult reserved = fabric.reserve(reservation);
  OF_REQUIRE_MSG(reserved.outcome != of::OutcomeCode::Refused,
                 std::string(of::to_string(reserved.refusal.code)) + " " + reserved.refusal.detail);

  of::ActivationRequest activation;
  activation.attempt = of::generate_attempt_id();
  activation.connectivity = submitted.connectivity;
  activation.authority = token;
  const of::ActivationResult beginning = fabric.begin_activation(activation);
  OF_REQUIRE_MSG(beginning.outcome != of::OutcomeCode::Refused,
                 std::string(of::to_string(beginning.refusal.code)) + " " + beginning.refusal.detail);
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = token;
  commit.activation_digest = beginning.activation_digest;
  return fabric.commit_activation(commit);
}


std::vector<of::ResourceRef> fixture_path_resources(const LineFixture& fixture) {
  return {of::as_ref(fixture.port_a_far), of::as_ref(fixture.cross_a), of::as_ref(fixture.port_a),
          of::as_ref(fixture.span),       of::as_ref(fixture.port_b),  of::as_ref(fixture.cross_b),
          of::as_ref(fixture.port_b_far), of::as_ref(fixture.channel)};
}

std::vector<of::EvidenceKind> fixture_evidence_kinds() {
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

std::unique_ptr<Session> make_session(const of::FabricOptions& options, const std::string& prefix,
                                      const std::string& holder) {
  auto session = std::make_unique<Session>();
  session->fabric = std::make_unique<of::OpticalFabric>(options);
  session->fixture = build_line(*session->fabric, prefix);
  session->source = attach_full_evidence(*session->fabric, prefix + "-synthetic");
  session->holder = holder;
  // A long lease keeps the authority out of the way of tests that advance the
  // logical clock to age evidence; authority expiry has its own suite.
  session->token = acquire_site_authority(*session->fabric, holder, session->fixture.site, 1000000);
  satisfy_evidence(*session);
  return session;
}

void satisfy_evidence(Session& session, std::uint64_t validity_span_ticks) {
  const of::Result<std::size_t> refreshed = session.fabric->refresh_from_source(
      session.source->describe().id, fixture_path_resources(session.fixture),
      fixture_evidence_kinds(), validity_span_ticks);
  OF_REQUIRE_MSG(refreshed.ok(), refreshed.status.message);
}

}  // namespace of_test