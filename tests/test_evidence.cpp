// Optical Fabric 1.0.0 - Summon Software Labs
// Evidence composition, freshness and provenance; topology registration.
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::SyntheticSource;

OF_TEST(evidence, composition_never_upgrades_an_indeterminate_state) {
  const std::vector<of::EvidenceState> indeterminate = {
      of::EvidenceState::Incomplete, of::EvidenceState::Stale, of::EvidenceState::Unknown,
      of::EvidenceState::Unsupported, of::EvidenceState::Conflicting, of::EvidenceState::Invalid};
  for (const of::EvidenceState state : indeterminate) {
    OF_REQUIRE_EQ(of::compose(of::EvidenceState::Known, state), state);
    OF_REQUIRE_EQ(of::compose(state, of::EvidenceState::Known), state);
    OF_REQUIRE(of::is_indeterminate(state));
    OF_REQUIRE(!of::is_healthy(state));
  }
  OF_REQUIRE_EQ(of::compose(of::EvidenceState::Known, of::EvidenceState::Known), of::EvidenceState::Known);
  OF_REQUIRE(of::is_healthy(of::EvidenceState::Known));
  OF_REQUIRE_EQ(of::compose(of::EvidenceState::Stale, of::EvidenceState::Conflicting),
                of::EvidenceState::Conflicting);
  OF_REQUIRE_EQ(of::compose(of::EvidenceState::Unknown, of::EvidenceState::Stale),
                of::EvidenceState::Unknown);
  // Composition is commutative and associative on every pair.
  for (const of::EvidenceState first : indeterminate) {
    for (const of::EvidenceState second : indeterminate) {
      OF_REQUIRE_EQ(of::compose(first, second), of::compose(second, first));
    }
  }
  OF_REQUIRE_EQ(of::compose_all({}), of::EvidenceState::Unknown);
  OF_REQUIRE_EQ(of::compose_all({of::EvidenceState::Known}), of::EvidenceState::Known);
}

OF_TEST(evidence, policy_marks_foreign_and_expired_observations_stale) {
  const of::EvidencePolicy policy = of::EvidencePolicy::conservative();
  of::Incarnation current;
  current.boot_sequence = 4;
  of::EvidenceRecord record;
  record.provenance.ingested_incarnation.boot_sequence = 3;
  record.provenance.observed_generation = of::Generation{2};
  record.provenance.observed_tick = of::Tick{1};
  record.provenance.valid_until_tick = of::Tick{100};
  record.state = of::EvidenceState::Known;

  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{2}, of::Tick{10}),
                of::EvidenceState::Stale);
  record.provenance.ingested_incarnation = current;
  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{3}, of::Tick{10}),
                of::EvidenceState::Stale);
  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{2}, of::Tick{10}),
                of::EvidenceState::Known);
  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{2}, of::Tick{100}),
                of::EvidenceState::Stale);
  record.provenance.valid_until_tick = record.provenance.observed_tick;
  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{2}, of::Tick{1}),
                of::EvidenceState::Invalid);
  record.provenance.valid_until_tick = of::Tick{99};
  OF_REQUIRE_EQ(of::apply_policy(record, policy, current, of::Generation{2}, of::Tick{99}),
                of::EvidenceState::Stale);
}

OF_TEST(evidence, ingestion_validates_subject_producer_and_window) {
  of::FabricOptions options;
  of::OpticalFabric fabric(options);
  const of_test::LineFixture fixture = of_test::build_line(fabric, "ev");
  const of::SourceId source = of::derive_named_id<of::SourceId>("source", "unit-test");

  of::EvidenceRecord unknown_subject = of_test::make_record(
      of::as_ref(of::derive_id<of::PortId>("missing")), of::EvidenceKind::PortCapability,
      of::EvidenceState::Known, "unit-test", source, 1, fabric.current_tick(), 100);
  OF_REQUIRE(!fabric.ingest_evidence(unknown_subject).ok());

  of::EvidenceRecord no_producer = of_test::make_record(
      of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known, "", source,
      1, fabric.current_tick(), 100);
  OF_REQUIRE(!fabric.ingest_evidence(no_producer).ok());

  of::EvidenceRecord bad_window = of_test::make_record(
      of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known,
      "unit-test", source, 1, fabric.current_tick(), 0);
  OF_REQUIRE(!fabric.ingest_evidence(bad_window).ok());

  const of::EvidenceRecord good = of_test::make_record(
      of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known,
      "unit-test", source, 5, fabric.current_tick(), 100);
  OF_REQUIRE(fabric.ingest_evidence(good).ok());
  // An identical re-ingestion is idempotent and changes nothing.
  const of::EvidenceId first = fabric.ingest_evidence(good).value;
  const of::EvidenceId second = fabric.ingest_evidence(good).value;
  OF_REQUIRE_EQ(first, second);
  OF_REQUIRE_EQ(fabric.snapshot().evidence_records, 1u);

  // The same producer sequence with different content is a contradiction.
  of::EvidenceRecord contradictory = good;
  contradictory.detail = "different";
  contradictory.provenance.content_digest = of::CanonicalHasher{}.digest();
  OF_REQUIRE(!fabric.ingest_evidence(contradictory).ok());

  // An older observation from the same producer must never replace a newer one.
  of::EvidenceRecord older = of_test::make_record(
      of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known,
      "unit-test", source, 4, fabric.current_tick(), 100);
  const of::Result<of::EvidenceId> stale = fabric.ingest_evidence(older);
  OF_REQUIRE(!stale.ok());
  OF_REQUIRE_EQ(stale.status.code, of::ErrorCode::StaleEvidence);

  // A subject that exists and a producer that answers is accepted.
  of::EvidenceRecord newer = of_test::make_record(
      of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known,
      "unit-test", source, 9, fabric.current_tick(), 100);
  OF_REQUIRE(fabric.ingest_evidence(newer).ok());
  const of::EvidenceAssessment assessment = [&]() {
    of::EvidenceRequirement requirement;
    requirement.subject = of::as_ref(fixture.port_a);
    requirement.kind = of::EvidenceKind::PortCapability;
    requirement.reason = "test";
    of::EvidenceAssessment result;
    result.evaluations.push_back(fabric.evaluate_requirement(requirement));
    result.aggregate = result.evaluations.front().state;
    return result;
  }();
  OF_REQUIRE(assessment.healthy());
}

OF_TEST(evidence, missing_producers_are_unsupported_not_healthy) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of_test::LineFixture fixture = of_test::build_line(fabric, "uns");

  of::EvidenceRequirement topo;
  topo.subject = of::as_ref(fixture.span);
  topo.kind = of::EvidenceKind::TopologyPresence;
  // Topology presence is self-knowledge and is KNOWN without any producer.
  OF_REQUIRE(fabric.evaluate_requirement(topo).state == of::EvidenceState::Known);

  of::EvidenceRequirement telemetry;
  telemetry.subject = of::as_ref(fixture.span);
  telemetry.kind = of::EvidenceKind::SpanOperationalState;
  OF_REQUIRE(fabric.evaluate_requirement(telemetry).state == of::EvidenceState::Unsupported);

  auto source = std::make_shared<SyntheticSource>("synthetic-partial", "one",
                                                  std::vector<of::EvidenceKind>{
                                                      of::EvidenceKind::SpanOperationalState});
  fabric.register_evidence_source(source);
  // A registered producer that has not answered yet is UNKNOWN, not UNSUPPORTED.
  OF_REQUIRE(fabric.evaluate_requirement(telemetry).state == of::EvidenceState::Unknown);

  const of::Result<std::size_t> refreshed = fabric.refresh_from_source(
      source->describe().id, {of::as_ref(fixture.span)}, {of::EvidenceKind::SpanOperationalState}, 50);
  OF_REQUIRE(refreshed.ok());
  OF_REQUIRE_EQ(refreshed.value, 1u);
  OF_REQUIRE(fabric.evaluate_requirement(telemetry).state == of::EvidenceState::Known);

  // A producer that declares a kind unsupported keeps it unsupported even when
  // another producer exists for a different kind.
  auto unsupported = std::make_shared<SyntheticSource>("synthetic-none", "two",
                                                      std::vector<of::EvidenceKind>{});
  unsupported->declare_unsupported({of::EvidenceKind::OpticalPowerTelemetry});
  fabric.register_evidence_source(unsupported);
  of::EvidenceRequirement power;
  power.subject = of::as_ref(fixture.span);
  power.kind = of::EvidenceKind::OpticalPowerTelemetry;
  OF_REQUIRE(fabric.evaluate_requirement(power).state == of::EvidenceState::Unsupported);
  OF_REQUIRE(of::is_hardware_telemetry_kind(of::EvidenceKind::OpticalPowerTelemetry));
}

OF_TEST(evidence, disagreeing_producers_are_conflicting) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of_test::LineFixture fixture = of_test::build_line(fabric, "conf");
  auto first = std::make_shared<SyntheticSource>("synthetic-a", "a",
                                                 std::vector<of::EvidenceKind>{
                                                     of::EvidenceKind::PortCapability});
  auto second = std::make_shared<SyntheticSource>("synthetic-b", "b",
                                                  std::vector<of::EvidenceKind>{
                                                      of::EvidenceKind::PortCapability});
  second->set_digest_salt(0xABCDEF);
  fabric.register_evidence_source(first);
  fabric.register_evidence_source(second);
  const std::vector<of::ResourceRef> subjects = {of::as_ref(fixture.port_a)};
  OF_REQUIRE(fabric
                 .refresh_from_source(first->describe().id, subjects,
                                      {of::EvidenceKind::PortCapability}, 100)
                 .ok());
  of::EvidenceRequirement requirement;
  requirement.subject = of::as_ref(fixture.port_a);
  requirement.kind = of::EvidenceKind::PortCapability;
  OF_REQUIRE(fabric.evaluate_requirement(requirement).state == of::EvidenceState::Known);
  OF_REQUIRE(fabric
                 .refresh_from_source(second->describe().id, subjects,
                                      {of::EvidenceKind::PortCapability}, 100)
                 .ok());
  OF_REQUIRE(fabric.evaluate_requirement(requirement).state == of::EvidenceState::Conflicting);

  // A producer that reports STALE directly is preserved as STALE.
  auto stale_source = std::make_shared<SyntheticSource>("synthetic-c", "c",
                                                       std::vector<of::EvidenceKind>{
                                                           of::EvidenceKind::SpanCapability});
  stale_source->set_state(of::EvidenceKind::SpanCapability, of::EvidenceState::Stale);
  fabric.register_evidence_source(stale_source);
  OF_REQUIRE(fabric
                 .refresh_from_source(stale_source->describe().id, {of::as_ref(fixture.span)},
                                      {of::EvidenceKind::SpanCapability}, 100)
                 .ok());
  of::EvidenceRequirement span_requirement;
  span_requirement.subject = of::as_ref(fixture.span);
  span_requirement.kind = of::EvidenceKind::SpanCapability;
  OF_REQUIRE(fabric.evaluate_requirement(span_requirement).state == of::EvidenceState::Stale);
}

OF_TEST(topology, registration_is_idempotent_and_collision_safe) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of_test::LineFixture fixture = of_test::build_line(fabric, "topo");
  of::SiteRegistration site;
  site.name = "topo.site";
  site.region = "synthetic-region";
  OF_REQUIRE(fabric.register_site(site).outcome == of::TopologyOutcome::AlreadyRegistered);
  site.region = "changed";
  const of::RegistrationResult collision = fabric.register_site(site);
  OF_REQUIRE(collision.outcome == of::TopologyOutcome::Refused);
  OF_REQUIRE_EQ(collision.refusal.code, of::RefusalCode::IdentityCollision);

  of::SiteRegistration bad;
  bad.name = "has space";
  OF_REQUIRE(fabric.register_site(bad).refusal.code == of::RefusalCode::NameInvalid);

  of::PortRegistration orphan;
  orphan.name = "topo.orphan";
  orphan.node = of::derive_id<of::OpticalNodeId>("missing-node");
  OF_REQUIRE(fabric.register_port(orphan).refusal.code == of::RefusalCode::UnknownResource);

  of::SpanRegistration bad_span;
  bad_span.name = "topo.bad-span";
  bad_span.endpoint_a = fixture.port_a;
  bad_span.endpoint_b = fixture.port_a;
  OF_REQUIRE(fabric.register_span(bad_span).refusal.code == of::RefusalCode::InvalidArgument);

  of::CrossConnectRegistration foreign;
  foreign.name = "topo.foreign-cross";
  foreign.node = fixture.node_a;
  foreign.ingress = fixture.port_b;
  foreign.egress = fixture.port_a;
  OF_REQUIRE(fabric.register_cross_connect(foreign).refusal.code == of::RefusalCode::InvalidArgument);

  const of::Result<of::ResourceView> view = fabric.describe_resource(of::as_ref(fixture.span));
  OF_REQUIRE(view.ok());
  OF_REQUIRE_EQ(view.value.name, std::string("topo.span"));
  OF_REQUIRE_EQ(view.value.related.size(), 2u);
  OF_REQUIRE(!fabric.describe_resource(of::as_ref(of::derive_id<of::SpanId>("nope"))).ok());

  const of::TopologySnapshot snapshot = fabric.topology();
  OF_REQUIRE_EQ(snapshot.resources.size(), 12u);
  const of::TopologySnapshot again = fabric.topology();
  OF_REQUIRE_EQ(snapshot.digest, again.digest);
  OF_REQUIRE(!snapshot.digest.is_nil());
}

OF_TEST(topology, capacity_bounds_are_enforced_before_allocation) {
  of::FabricOptions options;
  options.limits.max_sites = 2;
  options.limits.max_ports = 3;
  of::OpticalFabric fabric(options);
  const of::SiteId site = of_test::build_site(fabric, "cap");
  (void)site;
  of::SiteRegistration extra;
  extra.name = "cap.extra";
  OF_REQUIRE(fabric.register_site(extra).outcome == of::TopologyOutcome::Registered);
  of::SiteRegistration overflow;
  overflow.name = "cap.overflow";
  const of::RegistrationResult refused = fabric.register_site(overflow);
  OF_REQUIRE(refused.outcome == of::TopologyOutcome::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::CapacityExceeded);
}
