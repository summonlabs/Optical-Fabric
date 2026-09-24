// Optical Fabric 1.0.0 - Summon Software Labs
// Connectivity lifecycle: proposal, validation, reservation, activation,
// withdrawal, refusal and replay.
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::LineFixture;
using of_test::Session;

namespace {

/// Asserts that an operation was not refused, printing the typed refusal.
template <typename Result>
void expect_applied(const Result& result, const char* operation) {
  OF_REQUIRE_MSG(result.outcome != of::OutcomeCode::Refused,
                 std::string(operation) + " was refused: " + std::string(of::to_string(result.refusal.code)) +
                     " " + result.refusal.detail);
}


of::ValidateRequest make_validation(const Session& session, of::ConnectivityId id, of::AttemptId attempt) {
  of::ValidateRequest request;
  request.attempt = attempt;
  request.connectivity = id;
  request.authority = session.token;
  return request;
}

of::ReservationRequest make_reservation(const Session& session, of::ConnectivityId id,
                                       of::AttemptId attempt, std::uint64_t ttl) {
  of::ReservationRequest request;
  request.attempt = attempt;
  request.connectivity = id;
  request.authority = session.token;
  request.ttl_ticks = ttl;
  return request;
}

of::ActivationRequest make_activation(const Session& session, of::ConnectivityId id,
                                     of::AttemptId attempt) {
  of::ActivationRequest request;
  request.attempt = attempt;
  request.connectivity = id;
  request.authority = session.token;
  return request;
}

of::WithdrawalRequest make_withdrawal(const Session& session, of::ConnectivityId id,
                                     of::AttemptId attempt) {
  of::WithdrawalRequest request;
  request.attempt = attempt;
  request.connectivity = id;
  request.authority = session.token;
  request.reason = "unit test";
  return request;
}

}  // namespace

OF_TEST(lifecycle, happy_path_reaches_active_and_authorizes_exactly_one_path) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-happy");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "happy"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  OF_REQUIRE_EQ(submitted.state, of::ConnectivityState::Proposed);
  OF_REQUIRE(fabric.active_paths().active.empty());

  const of::ValidationResult validated =
      fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id()));
  expect_applied(validated, "validated");
  OF_REQUIRE_EQ(validated.state, of::ConnectivityState::Validated);
  OF_REQUIRE(validated.assessment.healthy());

  // Activation before a reservation is refused: a validated proposal is not
  // authority to touch anything.
  const of::ActivationResult premature = fabric.begin_activation(
      make_activation(*session, submitted.connectivity, of::generate_attempt_id()));
  OF_REQUIRE_EQ(premature.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(premature.refusal.code, of::RefusalCode::IllegalTransition);

  const of::ReservationResult reserved = fabric.reserve(
      make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 256));
  expect_applied(reserved, "reserved");
  OF_REQUIRE_EQ(reserved.reservation.resources.size(), 7u);
  OF_REQUIRE_EQ(reserved.reservation.holder,
                of::derive_named_id<of::ControllerId>("controller", "controller-a").to_string());

  const of::ActivationResult began = fabric.begin_activation(
      make_activation(*session, submitted.connectivity, of::generate_attempt_id()));
  expect_applied(began, "began");
  OF_REQUIRE_EQ(began.state, of::ConnectivityState::Activating);
  OF_REQUIRE(!began.activation_digest.is_nil());

  // A commit that does not present the pending activation digest is inert.
  of::CommitRequest wrong;
  wrong.attempt = of::generate_attempt_id();
  wrong.connectivity = submitted.connectivity;
  wrong.authority = session->token;
  wrong.activation_digest = of::CanonicalHasher{}.digest();
  const of::ActivationResult refused_commit = fabric.commit_activation(wrong);
  OF_REQUIRE_EQ(refused_commit.refusal.code, of::RefusalCode::AttemptConflict);
  OF_REQUIRE(fabric.active_paths().active.empty());

  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = session->token;
  commit.activation_digest = began.activation_digest;
  const of::ActivationResult activated = fabric.commit_activation(commit);
  expect_applied(activated, "activated");
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  OF_REQUIRE_EQ(activated.committed.size(), 7u);

  const of::ActivePathReport report = fabric.active_paths();
  OF_REQUIRE_EQ(report.active.size(), 1u);
  OF_REQUIRE_EQ(report.authorized.size(), 1u);
  OF_REQUIRE(report.active.front().authorized);
  OF_REQUIRE(report.active.front().authority_confirmed);
  OF_REQUIRE(report.diagnostics.empty());

  // The commit boundary is the only way to ACTIVE, so a repeated commit is a
  // satisfied no-op rather than a second activation.
  of::CommitRequest replay = commit;
  replay.attempt = of::generate_attempt_id();
  OF_REQUIRE(fabric.commit_activation(replay).outcome == of::OutcomeCode::AlreadySatisfied);

  const of::ClaimReport claims = fabric.claims();
  OF_REQUIRE_EQ(claims.claims.size(), 7u);
  OF_REQUIRE_EQ(claims.exclusive_resources_claimed, 6u);
  OF_REQUIRE_EQ(claims.channelized_resources_claimed, 1u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);
}

OF_TEST(lifecycle, withdrawal_releases_every_claim_and_closes_accounting) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-withdraw");
  of::OpticalFabric& fabric = *session->fabric;
  const of::ActivationResult activated =
      of_test::activate_path(fabric, session->fixture, "withdraw", session->token);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  const of::ConnectivityId id = [&fabric]() {
    const std::vector<of::ConnectivityView> objects = fabric.connectivity_objects();
    OF_REQUIRE_EQ(objects.size(), 1u);
    return objects.front().id;
  }();

  const of::WithdrawalResult started =
      fabric.withdraw(make_withdrawal(*session, id, of::generate_attempt_id()));
  expect_applied(started, "started");
  OF_REQUIRE_EQ(started.state, of::ConnectivityState::Withdrawing);
  OF_REQUIRE(!started.terminal);
  OF_REQUIRE_EQ(started.released.size(), 7u);
  // Withdrawal releases the commit index immediately.
  OF_REQUIRE_EQ(fabric.claims().claims.size(), 0u);
  OF_REQUIRE(fabric.active_paths().active.empty());

  const of::WithdrawalResult completed =
      fabric.withdraw(make_withdrawal(*session, id, of::generate_attempt_id()));
  expect_applied(completed, "completed");
  OF_REQUIRE_EQ(completed.state, of::ConnectivityState::Retired);
  OF_REQUIRE(completed.terminal);

  const of::WithdrawalResult again =
      fabric.withdraw(make_withdrawal(*session, id, of::generate_attempt_id()));
  OF_REQUIRE(again.outcome == of::OutcomeCode::AlreadySatisfied);

  const of::AccountingReport accounting = fabric.accounting();
  OF_REQUIRE_EQ(accounting.claims, 0u);
  OF_REQUIRE_EQ(accounting.reservations_live, 0u);
  OF_REQUIRE_EQ(accounting.reservations_consumed, 1u);
  OF_REQUIRE_EQ(accounting.reservations_released, 0u);
  OF_REQUIRE_EQ(accounting.objects_by_state[static_cast<std::size_t>(of::ConnectivityState::Retired)], 1u);
  OF_REQUIRE(accounting.balanced);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(lifecycle, missing_evidence_prevents_promotion) {
  of::FabricOptions options;
  of::OpticalFabric fabric(options);
  const LineFixture fixture = of_test::build_line(fabric, "lc-noev");
  const of::AuthorityToken token =
      of_test::acquire_site_authority(fabric, "controller-a", fixture.site);
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(fixture, "no-evidence"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);

  // No producer is registered at this boundary, so the requirements are
  // UNSUPPORTED and the object must not be promoted.
  const of::ValidationResult validated =
      fabric.validate(make_validation(*[&]() {
        static Session placeholder;
        placeholder.token = token;
        return &placeholder;
      }(), submitted.connectivity, of::generate_attempt_id()));
  OF_REQUIRE_EQ(validated.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(validated.refusal.code, of::RefusalCode::UnsupportedEvidenceKind);
  OF_REQUIRE(!validated.assessment.healthy());
  OF_REQUIRE_EQ(validated.state, of::ConnectivityState::Proposed);
  const of::Result<of::ConnectivityView> view = fabric.describe_connectivity(submitted.connectivity);
  OF_REQUIRE(view.ok());
  OF_REQUIRE_EQ(view.value.last_refusal, of::RefusalCode::UnsupportedEvidenceKind);
}

OF_TEST(lifecycle, evidence_loss_between_begin_and_commit_blocks_activation) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-loss");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "evidence-loss"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");
  expect_applied(fabric.reserve(make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 256)), "reserve");
  const of::ActivationResult began = fabric.begin_activation(
      make_activation(*session, submitted.connectivity, of::generate_attempt_id()));
  expect_applied(began, "began");

  // The clock passes the producer's validity window without a re-attestation.
  const of::Result<of::Tick> advanced = fabric.advance_tick(9000);
  OF_REQUIRE(advanced.ok());
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = session->token;
  commit.activation_digest = began.activation_digest;
  const of::ActivationResult refused = fabric.commit_activation(commit);
  OF_REQUIRE_MSG(refused.outcome == of::OutcomeCode::Refused,
                 std::string(of::to_string(refused.outcome)) + " " +
                     std::string(of::to_string(refused.refusal.code)) + " " + refused.refusal.detail);
  OF_REQUIRE_MSG(refused.refusal.code == of::RefusalCode::StaleEvidence,
                 std::string(of::to_string(refused.refusal.code)) + " " + refused.refusal.detail);
  OF_REQUIRE(fabric.active_paths().active.empty());

  // Re-attesting the producer lets the pending activation commit, but the
  // commit needs a fresh attempt: the refused attempt is recorded and replaying
  // it would faithfully return the same refusal.
  of_test::satisfy_evidence(*session);
  of::CommitRequest retry = commit;
  retry.attempt = of::generate_attempt_id();
  OF_REQUIRE_EQ(fabric.commit_activation(commit).refusal.code, of::RefusalCode::StaleEvidence);
  const of::ActivationResult activated = fabric.commit_activation(retry);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
}

OF_TEST(lifecycle, conflicting_claims_cannot_both_become_authoritative) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-conflict");
  of::OpticalFabric& fabric = *session->fabric;
  const of::ActivationResult first =
      of_test::activate_path(fabric, session->fixture, "first", session->token);
  OF_REQUIRE_EQ(first.state, of::ConnectivityState::Active);

  // A second intent over the same exclusive endpoints resolves to the same
  // canonical path, so it is the same logical decision and is reported as a
  // duplicate rather than as a conflict.
  const of::IntentSubmission duplicate =
      fabric.submit_intent(of_test::make_intent(session->fixture, "second"));
  OF_REQUIRE_EQ(duplicate.outcome, of::IntentOutcome::Duplicate);
  OF_REQUIRE(!duplicate.created);

  // Reversing the path makes it a genuinely different path that needs the same
  // exclusive resources, and that must be refused at reservation time.
  of::ConnectivityIntent reversed = of_test::make_intent(session->fixture, "reversed");
  reversed.source_port = session->fixture.port_b_far;
  reversed.destination_port = session->fixture.port_a_far;
  reversed.direction = of::Direction::Reverse;
  const of::IntentSubmission submitted = fabric.submit_intent(reversed);
  OF_REQUIRE_EQ(submitted.outcome, of::IntentOutcome::Accepted);
  OF_REQUIRE_NE(submitted.path_identity, first.committed.empty() ? of::Digest128{} : duplicate.path_identity);
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");
  const of::ReservationResult refused = fabric.reserve(
      make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 256));
  OF_REQUIRE_MSG(refused.outcome == of::OutcomeCode::Refused,
                 std::string(of::to_string(refused.outcome)) + " " +
                     std::string(of::to_string(refused.refusal.code)) + " " + refused.refusal.detail);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::ConflictingClaim);
  OF_REQUIRE_EQ(fabric.active_paths().authorized.size(), 1u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(lifecycle, channelized_resources_allow_a_second_channel) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-channel");
  of::OpticalFabric& fabric = *session->fabric;
  OF_REQUIRE_EQ(of_test::activate_path(fabric, session->fixture, "channel-0", session->token).state,
                of::ConnectivityState::Active);

  // The same span carrying a different channel is a different claim on a
  // channelized resource. The exclusive ports and cross connects still belong
  // to the first path, so this intent must be refused for that reason.
  of::ConnectivityIntent other = of_test::make_intent(session->fixture, "channel-1");
  other.channel = session->fixture.channel_alt;
  // The second channel is a different subject and needs its own observation.
  OF_REQUIRE(fabric
                 .refresh_from_source(session->source->describe().id,
                                      {of::as_ref(session->fixture.channel_alt)},
                                      {of::EvidenceKind::ChannelAvailability}, 8192)
                 .ok());
  const of::IntentSubmission submitted = fabric.submit_intent(other);
  OF_REQUIRE_EQ(submitted.outcome, of::IntentOutcome::Accepted);
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");
  const of::ReservationResult refused = fabric.reserve(
      make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 256));
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::ConflictingClaim);
}

OF_TEST(lifecycle, replay_and_attempt_reuse_are_answered_precisely) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-replay");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "replay"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);

  const of::ValidateRequest validation =
      make_validation(*session, submitted.connectivity, of::generate_attempt_id());
  const of::ValidationResult first = fabric.validate(validation);
  expect_applied(first, "first");
  const of::ValidationResult replay = fabric.validate(validation);
  OF_REQUIRE_EQ(replay.outcome, of::OutcomeCode::IdempotentReplay);
  OF_REQUIRE_EQ(replay.generation, first.generation);
  const of::Generation after = fabric.describe_connectivity(submitted.connectivity).value.generation;
  OF_REQUIRE_EQ(after, first.generation);

  // The same attempt identifier with different content is a contradiction.
  of::ValidateRequest conflicting = validation;
  conflicting.connectivity = of::derive_named_id<of::ConnectivityId>("connectivity", "other");
  const of::ValidationResult refused = fabric.validate(conflicting);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::AttemptConflict);

  // A fresh attempt against an already validated object is a satisfied no-op.
  const of::ValidationResult fresh =
      fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id()));
  OF_REQUIRE_EQ(fresh.outcome, of::OutcomeCode::AlreadySatisfied);
  OF_REQUIRE_EQ(fresh.state, of::ConnectivityState::Validated);
}

OF_TEST(lifecycle, expired_reservation_prevents_activation) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-expiry");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "expiry"));
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");
  const of::ReservationResult reserved = fabric.reserve(
      make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 10));
  expect_applied(reserved, "reserved");
  OF_REQUIRE(fabric.advance_tick(11).ok());

  const of::ActivationResult refused = fabric.begin_activation(
      make_activation(*session, submitted.connectivity, of::generate_attempt_id()));
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::ReservationExpired);

  // Renewal is refused once the lease has lapsed; the holder must reserve again.
  of::ReservationRenewal renewal;
  renewal.attempt = of::generate_attempt_id();
  renewal.reservation = reserved.reservation.id;
  renewal.authority = session->token;
  renewal.extend_ticks = 10;
  OF_REQUIRE_EQ(fabric.renew_reservation(renewal).refusal.code, of::RefusalCode::ReservationExpired);
}

OF_TEST(lifecycle, release_returns_the_object_to_validated_and_frees_resources) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-release");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "release"));
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");
  const of::ReservationResult reserved = fabric.reserve(
      make_reservation(*session, submitted.connectivity, of::generate_attempt_id(), 100));
  expect_applied(reserved, "reserved");
  OF_REQUIRE_EQ(fabric.accounting().reservations_live, 1u);

  of::ReleaseRequest release;
  release.attempt = of::generate_attempt_id();
  release.reservation = reserved.reservation.id;
  release.authority = session->token;
  release.reason = "unit test";
  const of::ReleaseResult released = fabric.release_reservation(release);
  expect_applied(released, "released");
  OF_REQUIRE_EQ(released.released.size(), 7u);
  OF_REQUIRE_EQ(fabric.accounting().reservations_released, 1u);
  OF_REQUIRE_EQ(fabric.accounting().reservations_live, 0u);
  const of::Result<of::ConnectivityView> view = fabric.describe_connectivity(submitted.connectivity);
  OF_REQUIRE_EQ(view.value.state, of::ConnectivityState::Validated);
  OF_REQUIRE(!view.value.has_reservation);
  // An exact replay of the release is recognised as such; a fresh attempt is a
  // satisfied no-op.
  OF_REQUIRE(fabric.release_reservation(release).outcome == of::OutcomeCode::IdempotentReplay);
  of::ReleaseRequest second = release;
  second.attempt = of::generate_attempt_id();
  OF_REQUIRE(fabric.release_reservation(second).outcome == of::OutcomeCode::AlreadySatisfied);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(lifecycle, degradation_and_failure_are_reported_not_inferred) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-health");
  of::OpticalFabric& fabric = *session->fabric;
  const of::ActivationResult activated =
      of_test::activate_path(fabric, session->fixture, "health", session->token);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  const of::ConnectivityId id = fabric.connectivity_objects().front().id;

  of::HealthReport degraded;
  degraded.attempt = of::generate_attempt_id();
  degraded.connectivity = id;
  degraded.authority = session->token;
  degraded.observed = of::EvidenceState::Stale;
  degraded.detail = "synthetic degradation";
  const of::HealthResult result = fabric.report_degraded(degraded);
  expect_applied(result, "result");
  OF_REQUIRE_EQ(result.state, of::ConnectivityState::Degraded);
  const of::ActivePathReport report = fabric.active_paths();
  OF_REQUIRE_EQ(report.active.size(), 1u);
  OF_REQUIRE_EQ(report.authorized.size(), 0u);
  OF_REQUIRE(!report.active.front().authorized);
  OF_REQUIRE_EQ(report.active.front().unauthorized_reason, std::string("the path is degraded"));

  of::HealthReport failed = degraded;
  failed.attempt = of::generate_attempt_id();
  const of::HealthResult failed_result = fabric.report_failed(failed);
  OF_REQUIRE_EQ(failed_result.state, of::ConnectivityState::Failed);
  // A failed path keeps its claims until it is withdrawn: the runtime does not
  // assume the resources were released by the failure.
  OF_REQUIRE_MSG(fabric.claims().claims.size() == 7u,
                 "claims after failure: " + std::to_string(fabric.claims().claims.size()));
  const of::WithdrawalResult withdrawn =
      fabric.withdraw(make_withdrawal(*session, id, of::generate_attempt_id()));
  OF_REQUIRE_EQ(withdrawn.state, of::ConnectivityState::Withdrawing);
  OF_REQUIRE_EQ(fabric.claims().claims.size(), 0u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(lifecycle, stale_segment_generation_refuses_the_object_terminally) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-stale");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "stale"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);

  // A new port on the same node changes membership but not the definition of
  // any resource on the path, so the recorded generations stay current.
  of::PortRegistration extra;
  extra.name = "lc-stale.port-extra";
  extra.node = session->fixture.node_a;
  OF_REQUIRE(fabric.register_port(extra).outcome == of::TopologyOutcome::Registered);
  expect_applied(fabric.validate(make_validation(*session, submitted.connectivity, of::generate_attempt_id())), "validate");

  const of::DiagnosticsReport diagnostics = fabric.diagnose(of::DiagnosticQuery{});
  OF_REQUIRE(diagnostics.diagnostics.empty() ||
             diagnostics.count_of(of::DiagnosticKind::StaleGeneration) == 0u);
}

OF_TEST(lifecycle, same_name_with_a_different_path_is_an_identity_collision) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-name");
  of::OpticalFabric& fabric = *session->fabric;
  of::ConnectivityIntent first = of_test::make_intent(session->fixture, "named");
  OF_REQUIRE(fabric.submit_intent(first).outcome == of::IntentOutcome::Accepted);
  of::ConnectivityIntent second = first;
  second.channel = session->fixture.channel_alt;
  const of::IntentSubmission collision = fabric.submit_intent(second);
  OF_REQUIRE_EQ(collision.outcome, of::IntentOutcome::Refused);
  OF_REQUIRE_EQ(collision.refusal.code, of::RefusalCode::IdentityCollision);
  of::ConnectivityIntent invalid = second;
  invalid.name = "bad name";
  OF_REQUIRE_EQ(fabric.submit_intent(invalid).refusal.code, of::RefusalCode::NameInvalid);
}

OF_TEST(lifecycle, explain_reports_provenance_for_every_segment) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "lc-explain");
  of::OpticalFabric& fabric = *session->fabric;
  const of::ActivationResult activated =
      of_test::activate_path(fabric, session->fixture, "explain", session->token);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  const of::ConnectivityId id = fabric.connectivity_objects().front().id;
  const of::Result<of::PathExplanation> explained = fabric.explain(id);
  OF_REQUIRE(explained.ok());
  const of::PathExplanation& explanation = explained.value;
  OF_REQUIRE_EQ(explanation.segments.size(), 7u);
  OF_REQUIRE_EQ(explanation.route_origin, std::string("declared-route"));
  OF_REQUIRE(explanation.evidence_healthy);
  OF_REQUIRE(explanation.authority_confirmed);
  OF_REQUIRE(!explanation.canonical_path.empty());
  for (const of::SegmentProvenance& segment : explanation.segments) {
    OF_REQUIRE(!segment.resource_name.empty());
    OF_REQUIRE(segment.generation_current);
    OF_REQUIRE(!segment.claim.empty());
    OF_REQUIRE_EQ(segment.evidence_state, of::EvidenceState::Known);
    OF_REQUIRE(!segment.evidence.empty());
  }
  OF_REQUIRE(!explanation.notes.empty());
  OF_REQUIRE_EQ(explanation.history.size(), 5u);
}