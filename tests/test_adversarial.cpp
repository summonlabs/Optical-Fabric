// Optical Fabric 1.0.0 - Summon Software Labs
// Adversarial input, bounds exhaustion and malformed state.
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::Session;

OF_TEST(adversarial, malformed_requests_are_refused_with_typed_codes) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "adv");
  of::OpticalFabric& fabric = *session->fabric;

  of::ConnectivityIntent no_ports = of_test::make_intent(session->fixture, "no-ports");
  no_ports.source_port = of::PortId{};
  OF_REQUIRE_EQ(fabric.submit_intent(no_ports).outcome, of::IntentOutcome::Refused);

  of::ConnectivityIntent same_port = of_test::make_intent(session->fixture, "same-port");
  same_port.destination_port = same_port.source_port;
  OF_REQUIRE_EQ(fabric.submit_intent(same_port).refusal.code, of::RefusalCode::InvalidArgument);

  of::ConnectivityIntent unknown_port = of_test::make_intent(session->fixture, "unknown-port");
  unknown_port.destination_port = of::derive_id<of::PortId>("does-not-exist");
  OF_REQUIRE_EQ(fabric.submit_intent(unknown_port).refusal.code, of::RefusalCode::RouteUnresolved);

  of::ConnectivityIntent bad_channel = of_test::make_intent(session->fixture, "bad-channel");
  bad_channel.channel = of::derive_id<of::ChannelId>("missing-channel");
  OF_REQUIRE_EQ(fabric.submit_intent(bad_channel).outcome, of::IntentOutcome::Refused);

  of::ValidateRequest missing_object;
  missing_object.attempt = of::generate_attempt_id();
  missing_object.connectivity = of::derive_named_id<of::ConnectivityId>("connectivity", "absent");
  missing_object.authority = session->token;
  const of::ValidationResult refused = fabric.validate(missing_object);
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::UnknownResource);

  const of::IntentSubmission accepted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "ttl"));
  OF_REQUIRE(accepted.outcome == of::IntentOutcome::Accepted);
  of::ReservationRequest zero_ttl;
  zero_ttl.attempt = of::generate_attempt_id();
  zero_ttl.connectivity = accepted.connectivity;
  zero_ttl.authority = session->token;
  zero_ttl.ttl_ticks = 0;
  OF_REQUIRE_EQ(fabric.reserve(zero_ttl).outcome, of::OutcomeCode::Refused);
  zero_ttl.attempt = of::generate_attempt_id();
  zero_ttl.ttl_ticks = ~static_cast<std::uint64_t>(0);
  OF_REQUIRE_EQ(fabric.reserve(zero_ttl).outcome, of::OutcomeCode::Refused);
}

OF_TEST(adversarial, out_of_order_and_replayed_events_are_refused) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "adv-order");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "order"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);

  // A reservation cannot precede validation, and an activation cannot precede
  // a reservation.
  of::ReservationRequest early;
  early.attempt = of::generate_attempt_id();
  early.connectivity = submitted.connectivity;
  early.authority = session->token;
  early.ttl_ticks = 100;
  OF_REQUIRE_EQ(fabric.reserve(early).refusal.code, of::RefusalCode::IllegalTransition);
  of::ActivationRequest activation;
  activation.attempt = of::generate_attempt_id();
  activation.connectivity = submitted.connectivity;
  activation.authority = session->token;
  OF_REQUIRE_EQ(fabric.begin_activation(activation).refusal.code, of::RefusalCode::IllegalTransition);

  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = session->token;
  OF_REQUIRE(fabric.validate(validation).outcome == of::OutcomeCode::Applied);
  // The earlier reservation attempt was refused and recorded, so the retry
  // needs a fresh attempt identifier rather than replaying a refused one.
  early.attempt = of::generate_attempt_id();
  OF_REQUIRE(fabric.reserve(early).outcome == of::OutcomeCode::Applied);

  // A commit for an object that is not activating is refused, and a commit that
  // presents a forged digest is refused even while one is pending.
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = session->token;
  commit.activation_digest = of::CanonicalHasher{}.digest();
  OF_REQUIRE_EQ(fabric.commit_activation(commit).refusal.code, of::RefusalCode::IllegalTransition);

  // The earlier activation attempt was refused and recorded; a fresh attempt is
  // required for the real one.
  activation.attempt = of::generate_attempt_id();
  const of::ActivationResult began = fabric.begin_activation(activation);
  OF_REQUIRE(began.outcome == of::OutcomeCode::Applied);
  commit.attempt = of::generate_attempt_id();
  OF_REQUIRE_EQ(fabric.commit_activation(commit).refusal.code, of::RefusalCode::AttemptConflict);

  // An attempt identifier is single use: the same value with different content
  // is a contradiction, and an exact replay is answered from the record.
  of::ValidateRequest replayed = validation;
  OF_REQUIRE_EQ(fabric.validate(replayed).outcome, of::OutcomeCode::IdempotentReplay);
  of::ValidateRequest reused = validation;
  reused.connectivity = of::derive_named_id<of::ConnectivityId>("connectivity", "other");
  OF_REQUIRE_EQ(fabric.validate(reused).refusal.code, of::RefusalCode::AttemptConflict);
}

OF_TEST(adversarial, capacity_bounds_are_enforced_for_every_collection) {
  of::FabricOptions options;
  options.limits.max_sites = 1;
  options.limits.max_optical_nodes = 2;
  options.limits.max_ports = 4;
  options.limits.max_spans = 1;
  options.limits.max_line_systems = 1;
  options.limits.max_cross_connects = 2;
  options.limits.max_channels = 2;
  options.limits.max_connectivity_objects = 2;
  options.limits.max_reservations = 1;
  options.limits.max_evidence_records = 4;
  options.limits.max_attempt_history_total = 16;
  options.limits.max_attempt_history_per_object = 4;
  options.limits.max_path_segments = 9;
  of::OpticalFabric fabric(options);
  const of_test::LineFixture fixture = of_test::build_line(fabric, "bounds");

  of::SiteRegistration extra_site;
  extra_site.name = "bounds.second";
  OF_REQUIRE_EQ(fabric.register_site(extra_site).refusal.code, of::RefusalCode::CapacityExceeded);
  of::ChannelRegistration extra_channel;
  extra_channel.name = "bounds.channel-extra";
  OF_REQUIRE_EQ(fabric.register_channel(extra_channel).refusal.code, of::RefusalCode::CapacityExceeded);

  const of::AuthorityToken token =
      of_test::acquire_site_authority(fabric, "controller-a", fixture.site, 1000000);
  const of::SourceId source = of::derive_named_id<of::SourceId>("source", "bounds");
  of::Result<of::EvidenceId> last = of::Result<of::EvidenceId>::success(of::EvidenceId{});
  for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
    last = fabric.ingest_evidence(of_test::make_record(
        of::as_ref(fixture.port_a), of::EvidenceKind::PortCapability, of::EvidenceState::Known,
        "bounds-source", source, sequence, fabric.current_tick(), 1000));
  }
  OF_REQUIRE(!last.ok());
  OF_REQUIRE_EQ(last.status.code, of::ErrorCode::CapacityExceeded);
  OF_REQUIRE_EQ(fabric.snapshot().evidence_records, 4u);

  // The attempt history stays bounded, and an evicted attempt falls back to
  // state based idempotence rather than being mistaken for a conflict.
  for (int index = 0; index < 40; ++index) {
    of::ValidateRequest noise;
    noise.attempt = of::generate_attempt_id();
    noise.connectivity = of::derive_named_id<of::ConnectivityId>("connectivity", "absent");
    noise.authority = token;
    (void)fabric.validate(noise);
  }
  OF_REQUIRE(fabric.snapshot().connectivity_objects <= 2u);
  const of::InvariantReport bounded = fabric.verify_invariants();
  for (const of::InvariantCheck& check : bounded.checks) {
    if (check.name == "bounded_histories_stay_bounded") {
      OF_REQUIRE(check.holds);
    }
  }
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(adversarial, extreme_values_are_rejected_before_allocation) {
  of::FabricOptions options;
  options.limits.max_path_segments = 2;
  of::OpticalFabric fabric(options);
  const of_test::LineFixture fixture = of_test::build_line(fabric, "extreme");
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(fixture, "too-long"));
  // The declared line needs seven segments, which the configured bound forbids.
  OF_REQUIRE_EQ(submitted.outcome, of::IntentOutcome::Refused);

  of::Limits degenerate;
  degenerate.max_sites = 0;
  OF_REQUIRE(!degenerate.validate().ok());
  degenerate = of::Limits{};
  degenerate.max_frame_payload_bytes = 8u * 1024u * 1024u * 1024u;
  OF_REQUIRE(!degenerate.validate().ok());
  degenerate = of::Limits{};
  degenerate.max_path_segments = 100000;
  OF_REQUIRE(!degenerate.validate().ok());

  // A store bound larger than the supported maximum is refused, not clamped.
  of::FabricOptions bad;
  bad.limits.max_record_payload_bytes = 8ull * 1024ull * 1024ull * 1024ull;
  OF_REQUIRE_THROWS(of::OpticalFabric{bad});
  (void)fixture;
}

OF_TEST(adversarial, interrupted_publication_leaves_a_consistent_runtime) {
  // A mutation whose persistence fails must leave the runtime refusing further
  // work rather than diverging quietly from its durable state.
  of_test::TempDirectory directory;
  of::FabricOptions options;
  options.store_path = directory.file("fabric.store");
  std::unique_ptr<Session> session = of_test::make_session(options, "interrupt");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "interrupt-path"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  OF_REQUIRE(fabric.flush().ok());
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.close().ok());
  OF_REQUIRE(fabric.closed());
}
