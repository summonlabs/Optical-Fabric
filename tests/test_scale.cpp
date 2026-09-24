// Optical Fabric 1.0.0 - Summon Software Labs
// Scale: many resources and objects, bounded histories, accounting closure.
#include <memory>
#include <string>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::Session;

OF_TEST(scale, many_lines_activate_and_close_their_accounting) {
  of::FabricOptions options;
  options.limits.max_connectivity_objects = 512;
  of::OpticalFabric fabric(options);
  constexpr int kLines = 40;
  for (int index = 0; index < kLines; ++index) {
    const std::string prefix = "scale-" + std::to_string(index);
    const of_test::LineFixture fixture = of_test::build_line(fabric, prefix);
    const of::AuthorityToken token = of_test::acquire_site_authority(
        fabric, "controller-" + std::to_string(index), fixture.site, 1000000);
    auto source = of_test::attach_full_evidence(fabric, prefix + "-synthetic");
    OF_REQUIRE(fabric
                   .refresh_from_source(source->describe().id,
                                        of_test::fixture_path_resources(fixture),
                                        of_test::fixture_evidence_kinds(), 1000000)
                   .ok());
    const of::ActivationResult activated = of_test::activate_path(
        fabric, fixture, "scale-path-" + std::to_string(index), token);
    OF_REQUIRE_MSG(activated.state == of::ConnectivityState::Active, activated.refusal.detail);
  }
  const of::ClaimReport claims = fabric.claims();
  OF_REQUIRE_EQ(claims.claims.size(), static_cast<std::size_t>(kLines) * 7u);
  OF_REQUIRE_EQ(fabric.active_paths().authorized.size(), static_cast<std::size_t>(kLines));
  OF_REQUIRE_EQ(fabric.topology().resources.size(), static_cast<std::size_t>(kLines) * 12u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);

  // Retire every path through its own holder and prove the accounting closes
  // exactly: no claims, no live reservations, every object retired.
  for (int index = 0; index < kLines; ++index) {
    const std::string prefix = "scale-" + std::to_string(index);
    const std::string name = "scale-path-" + std::to_string(index);
    const of::SiteId site = of::derive_id<of::SiteId>(prefix + ".site");
    const of::AuthorityToken token =
        of_test::acquire_site_authority(fabric, "closer-" + std::to_string(index), site, 1000000);
    const of::ConnectivityId id = of::derive_named_id<of::ConnectivityId>("connectivity", name);
    of::WithdrawalRequest first;
    first.attempt = of::generate_attempt_id();
    first.connectivity = id;
    first.authority = token;
    first.reason = "scale cleanup";
    OF_REQUIRE_EQ(fabric.withdraw(first).state, of::ConnectivityState::Withdrawing);
    of::WithdrawalRequest second = first;
    second.attempt = of::generate_attempt_id();
    OF_REQUIRE_EQ(fabric.withdraw(second).state, of::ConnectivityState::Retired);
  }
  const of::AccountingReport accounting = fabric.accounting();
  OF_REQUIRE_EQ(accounting.claims, 0u);
  OF_REQUIRE_EQ(accounting.reservations_live, 0u);
  OF_REQUIRE_EQ(accounting.objects_by_state[static_cast<std::size_t>(of::ConnectivityState::Retired)],
                static_cast<std::size_t>(kLines));
  OF_REQUIRE(accounting.balanced);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(scale, histories_stay_bounded_under_long_churn) {
  of::FabricOptions options;
  options.limits.max_attempt_history_per_object = 8;
  options.limits.max_attempt_history_total = 64;
  options.limits.max_diagnostics = 32;
  std::unique_ptr<Session> session = of_test::make_session(options, "churn");
  of::OpticalFabric& fabric = *session->fabric;
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "churn-path"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  for (int index = 0; index < 200; ++index) {
    of::ValidateRequest validation;
    validation.attempt = of::generate_attempt_id();
    validation.connectivity = submitted.connectivity;
    validation.authority = session->token;
    (void)fabric.validate(validation);
  }
  const of::Result<of::ConnectivityView> view = fabric.describe_connectivity(submitted.connectivity);
  OF_REQUIRE(view.ok());
  const of::Result<of::PathExplanation> explanation = fabric.explain(submitted.connectivity);
  OF_REQUIRE(explanation.ok());
  OF_REQUIRE(explanation.value.history.size() <= options.limits.max_attempt_history_per_object);
  OF_REQUIRE(fabric.snapshot().connectivity_objects <= options.limits.max_connectivity_objects);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
  OF_REQUIRE(fabric.accounting().balanced);
}
