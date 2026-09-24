// Optical Fabric 1.0.0 - Summon Software Labs
// Concurrent mutation: reservation races, shutdown during work, invariants.
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::Session;

namespace {

of::IntentSubmission submit_ready(of::OpticalFabric& fabric, const of_test::LineFixture& fixture,
                                  const std::string& name, const of::AuthorityToken& token) {
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(fixture, name));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = token;
  OF_REQUIRE(fabric.validate(validation).outcome == of::OutcomeCode::Applied);
  return submitted;
}

}  // namespace

OF_TEST(concurrency, a_reservation_race_has_exactly_one_winner) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "race");
  of::OpticalFabric& fabric = *session->fabric;

  // Eight threads race for the same exclusive resources through the same
  // object. Exactly one reservation may be granted.
  const of::IntentSubmission submitted = submit_ready(fabric, session->fixture, "race",
                                                              session->token);
  constexpr int kThreads = 8;
  std::atomic<int> applied{0};
  std::atomic<int> refused{0};
  std::vector<std::thread> threads;
  std::vector<of::ReservationResult> results(kThreads);
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&, index]() {
      of::ReservationRequest request;
      request.attempt = of::generate_attempt_id();
      request.connectivity = submitted.connectivity;
      request.authority = session->token;
      request.ttl_ticks = 1000;
      results[static_cast<std::size_t>(index)] = fabric.reserve(request);
      if (results[static_cast<std::size_t>(index)].outcome == of::OutcomeCode::Applied) {
        applied.fetch_add(1);
      } else {
        refused.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  OF_REQUIRE_EQ(applied.load(), 1);
  OF_REQUIRE_EQ(refused.load(), kThreads - 1);
  OF_REQUIRE_EQ(fabric.accounting().reservations_live, 1u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);
}

OF_TEST(concurrency, parallel_activations_on_independent_lines_all_succeed) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  constexpr int kLines = 6;
  std::vector<of_test::LineFixture> fixtures;
  std::vector<of::AuthorityToken> tokens;
  std::vector<std::shared_ptr<of_test::SyntheticSource>> sources;
  for (int index = 0; index < kLines; ++index) {
    const std::string prefix = "parallel-" + std::to_string(index);
    fixtures.push_back(of_test::build_line(fabric, prefix));
    tokens.push_back(of_test::acquire_site_authority(fabric, "controller-" + std::to_string(index),
                                                     fixtures.back().site, 1000000));
    sources.push_back(of_test::attach_full_evidence(fabric, prefix + "-synthetic"));
    OF_REQUIRE(fabric
                   .refresh_from_source(sources.back()->describe().id,
                                        of_test::fixture_path_resources(fixtures.back()),
                                        of_test::fixture_evidence_kinds(), 100000)
                   .ok());
  }

  std::vector<of::ActivationResult> results(kLines);
  std::vector<std::thread> threads;
  for (int index = 0; index < kLines; ++index) {
    threads.emplace_back([&, index]() {
      results[static_cast<std::size_t>(index)] = of_test::activate_path(
          fabric, fixtures[static_cast<std::size_t>(index)], "parallel-path-" + std::to_string(index),
          tokens[static_cast<std::size_t>(index)]);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (const of::ActivationResult& result : results) {
    OF_REQUIRE_EQ(result.state, of::ConnectivityState::Active);
  }
  OF_REQUIRE_EQ(fabric.active_paths().authorized.size(), static_cast<std::size_t>(kLines));
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);
  OF_REQUIRE_EQ(fabric.claims().claims.size(), static_cast<std::size_t>(kLines) * 7u);
}

OF_TEST(concurrency, shutdown_stops_new_work_without_breaking_state) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "shutdown");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      submit_ready(fabric, session->fixture, "shutdown", session->token);

  std::atomic<int> refusals{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index) {
    threads.emplace_back([&]() {
      while (!stop.load()) {
        of::ValidateRequest validation;
        validation.attempt = of::generate_attempt_id();
        validation.connectivity = submitted.connectivity;
        validation.authority = session->token;
        const of::ValidationResult result = fabric.validate(validation);
        if (result.outcome == of::OutcomeCode::Refused &&
            result.refusal.code == of::RefusalCode::PersistenceFailure) {
          refusals.fetch_add(1);
        }
      }
    });
  }
  OF_REQUIRE(fabric.close().ok());
  // Deterministic proof that new work stops: the caller itself is refused, with
  // the typed refusal that says the runtime is closed.
  of::ValidateRequest after_close;
  after_close.attempt = of::generate_attempt_id();
  after_close.connectivity = submitted.connectivity;
  after_close.authority = session->token;
  const of::ValidationResult refused = fabric.validate(after_close);
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::PersistenceFailure);
  stop.store(true);
  for (std::thread& thread : threads) {
    thread.join();
  }
  OF_REQUIRE(fabric.closed());
  // In-flight workers may or may not have observed the close; whatever they saw
  // was counted, and the state stays consistent either way.
  (void)refusals;
  // Queries still answer, and the state is internally consistent.
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);
  OF_REQUIRE(fabric.close().ok());
  of::IntentSubmission after;
  after = fabric.submit_intent(of_test::make_intent(session->fixture, "after-close"));
  OF_REQUIRE_EQ(after.outcome, of::IntentOutcome::Refused);
}

OF_TEST(concurrency, racing_claimants_for_one_resource_never_both_win) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of_test::LineFixture fixture = of_test::build_line(fabric, "claim-race");
  const of::AuthorityToken token =
      of_test::acquire_site_authority(fabric, "controller-a", fixture.site, 1000000);
  auto source = of_test::attach_full_evidence(fabric, "claim-race-synthetic");
  OF_REQUIRE(fabric
                 .refresh_from_source(source->describe().id, of_test::fixture_path_resources(fixture),
                                      of_test::fixture_evidence_kinds(), 100000)
                 .ok());

  // Two distinct paths over the same exclusive resources: the forward one and
  // the reverse one. At most one may ever hold the claims.
  const of::IntentSubmission forward = submit_ready(fabric, fixture, "forward", token);
  of::ConnectivityIntent reversed = of_test::make_intent(fixture, "backward");
  reversed.source_port = fixture.port_b_far;
  reversed.destination_port = fixture.port_a_far;
  reversed.direction = of::Direction::Reverse;
  const of::IntentSubmission backward = fabric.submit_intent(reversed);
  OF_REQUIRE(backward.outcome == of::IntentOutcome::Accepted);
  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = backward.connectivity;
  validation.authority = token;
  OF_REQUIRE(fabric.validate(validation).outcome == of::OutcomeCode::Applied);

  std::atomic<int> applied{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 2; ++index) {
    threads.emplace_back([&, index]() {
      of::ReservationRequest request;
      request.attempt = of::generate_attempt_id();
      request.connectivity = index == 0 ? forward.connectivity : backward.connectivity;
      request.authority = token;
      request.ttl_ticks = 1000;
      if (fabric.reserve(request).outcome == of::OutcomeCode::Applied) {
        applied.fetch_add(1);
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  OF_REQUIRE_EQ(applied.load(), 1);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE_EQ(fabric.accounting().reservations_live, 1u);
}
