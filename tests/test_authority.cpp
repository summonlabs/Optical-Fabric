// Optical Fabric 1.0.0 - Summon Software Labs
// Authority: epochs, incarnations, scopes, renewal, expiry and fencing.
#include <memory>
#include <string>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::Session;

namespace {

of::AuthorityResult acquire(of::OpticalFabric& fabric, const std::string& holder, of::AuthorityScope scope,
                            std::uint64_t lease) {
  of::AuthorityRequest request;
  request.attempt = of::generate_attempt_id();
  request.holder = of::derive_named_id<of::ControllerId>("controller", holder);
  request.scope = scope;
  request.lease_ticks = lease;
  request.reason = "test";
  return fabric.acquire_authority(request);
}

of::ValidateRequest validate_with(of::ConnectivityId id, const of::AuthorityToken& token) {
  of::ValidateRequest request;
  request.attempt = of::generate_attempt_id();
  request.connectivity = id;
  request.authority = token;
  return request;
}

}  // namespace

OF_TEST(authority, acquisition_fences_the_previous_holder) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "auth-fence");
  of::OpticalFabric& fabric = *session->fabric;
  const of::AuthorityCheck current = fabric.check_authority(session->token);
  OF_REQUIRE(current.current());
  OF_REQUIRE_EQ(current.current_epoch, fabric.current_epoch());

  const of::AuthorityResult second = acquire(fabric, "controller-b",
                                             of::AuthorityScope::of_site(session->fixture.site), 1000);
  OF_REQUIRE(second.outcome == of::OutcomeCode::Applied);
  OF_REQUIRE(second.token.epoch.value > session->token.epoch.value);

  const of::AuthorityCheck fenced = fabric.check_authority(session->token);
  OF_REQUIRE(!fenced.current());
  OF_REQUIRE_EQ(fenced.currentness, of::AuthorityCurrentness::StaleEpoch);
  OF_REQUIRE(fabric.check_authority(second.token).current());

  // The fenced controller cannot mutate anything, even its own object.
  const of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(session->fixture, "fenced"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  const of::ValidationResult refused = fabric.validate(validate_with(submitted.connectivity, session->token));
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::StaleEpoch);
  const of::ValidationResult allowed = fabric.validate(validate_with(submitted.connectivity, second.token));
  OF_REQUIRE(allowed.outcome == of::OutcomeCode::Applied);

  // An unknown grant identifier is refused rather than treated as authority.
  of::AuthorityToken forged = second.token;
  forged.grant = of::GrantId::from_value(0xDEADBEEF);
  OF_REQUIRE_EQ(fabric.check_authority(forged).currentness, of::AuthorityCurrentness::UnknownGrant);
  forged = second.token;
  forged.epoch = of::Epoch{second.token.epoch.value + 5};
  OF_REQUIRE_EQ(fabric.check_authority(forged).currentness, of::AuthorityCurrentness::StaleEpoch);
}

OF_TEST(authority, scope_is_enforced_and_global_covers_sites) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of_test::LineFixture first = of_test::build_line(fabric, "auth-a");
  const of::SiteId other_site = of_test::build_site(fabric, "auth-b");
  const of::AuthorityToken token =
      of_test::acquire_site_authority(fabric, "controller-a", first.site, 4096);

  of::AuthorityRequest cross;
  cross.attempt = of::generate_attempt_id();
  cross.holder = token.holder;
  cross.scope = of::AuthorityScope::of_site(other_site);
  cross.lease_ticks = 4096;
  const of::AuthorityResult ignored = fabric.acquire_authority(cross);
  OF_REQUIRE(ignored.outcome == of::OutcomeCode::Applied);
  // The first token still governs its own site: scopes that do not overlap do
  // not fence each other.
  OF_REQUIRE(fabric.check_authority(token).current());

  of::IntentSubmission submitted =
      fabric.submit_intent(of_test::make_intent(first, "scope-check"));
  OF_REQUIRE(submitted.outcome == of::IntentOutcome::Accepted);
  of::AuthorityToken wrong_scope = ignored.token;
  wrong_scope.scope = of::AuthorityScope::of_site(other_site);
  const of::ValidationResult refused = fabric.validate(validate_with(submitted.connectivity, wrong_scope));
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);

  // A global grant covers every site, so it fences both site grants.
  const of::AuthorityResult global = acquire(fabric, "controller-root", of::AuthorityScope::global(), 4096);
  OF_REQUIRE(global.outcome == of::OutcomeCode::Applied);
  OF_REQUIRE(global.token.scope.covers(of::AuthorityScope::of_site(first.site)));
  OF_REQUIRE_EQ(fabric.check_authority(token).currentness, of::AuthorityCurrentness::StaleEpoch);
  OF_REQUIRE_EQ(fabric.check_authority(ignored.token).currentness, of::AuthorityCurrentness::StaleEpoch);
  OF_REQUIRE(fabric.check_authority(global.token).current());
}

OF_TEST(authority, renewal_extends_only_the_holders_own_grant) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of::SiteId site = of_test::build_site(fabric, "auth-renew");
  const of::AuthorityToken token = of_test::acquire_site_authority(fabric, "controller-a", site, 100);
  OF_REQUIRE(fabric.advance_tick(50).ok());

  of::AuthorityRequest renewal;
  renewal.attempt = of::generate_attempt_id();
  renewal.holder = token.holder;
  renewal.scope = token.scope;
  renewal.lease_ticks = 500;
  const of::AuthorityResult renewed = fabric.renew_authority(renewal);
  OF_REQUIRE(renewed.outcome == of::OutcomeCode::Applied);
  OF_REQUIRE_EQ(renewed.token.epoch, token.epoch);
  OF_REQUIRE(renewed.grant.expiry_tick.value > token.epoch.value);
  OF_REQUIRE(fabric.check_authority(renewed.token).current());

  // A different holder cannot renew someone else's grant.
  of::AuthorityRequest stolen = renewal;
  stolen.attempt = of::generate_attempt_id();
  stolen.holder = of::derive_named_id<of::ControllerId>("controller", "controller-b");
  const of::AuthorityResult refused = fabric.renew_authority(stolen);
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::NotAuthoritative);

  // Once the lease runs out the token is expired, not current.
  const of::AuthorityResult takeover = acquire(fabric, "controller-b", token.scope, 1000);
  OF_REQUIRE(takeover.outcome == of::OutcomeCode::Applied);
  OF_REQUIRE_EQ(fabric.check_authority(renewed.token).currentness, of::AuthorityCurrentness::StaleEpoch);
  OF_REQUIRE(fabric.advance_tick(2000).ok());
  OF_REQUIRE_EQ(fabric.check_authority(takeover.token).currentness, of::AuthorityCurrentness::Expired);
}

OF_TEST(authority, fence_reports_the_grant_it_fenced) {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const of::SiteId site = of_test::build_site(fabric, "auth-explicit");
  const of::AuthorityToken token = of_test::acquire_site_authority(fabric, "controller-a", site, 1000);

  of::FenceRequest fence;
  fence.attempt = of::generate_attempt_id();
  fence.holder = of::derive_named_id<of::ControllerId>("controller", "operator");
  fence.scope = of::AuthorityScope::of_site(site);
  fence.lease_ticks = 1000;
  fence.reason = of::FenceReason::ExplicitOperatorFence;
  fence.detail = "operator fence";
  const of::FenceResult result = fabric.fence(fence);
  OF_REQUIRE(result.outcome == of::OutcomeCode::Applied);
  OF_REQUIRE(result.fenced_anything);
  OF_REQUIRE_EQ(result.fenced.grant, token.grant);
  OF_REQUIRE_EQ(result.fenced.fenced_boot_sequence, token.incarnation.boot_sequence);
  OF_REQUIRE_EQ(fabric.check_authority(token).currentness, of::AuthorityCurrentness::StaleEpoch);

  // The fence is durable state, not a transient answer: the fenced grant stays
  // fenced and the operator holds the only current authority.
  const std::vector<of::AuthorityView> grants = fabric.authority_grants();
  std::size_t current_grants = 0;
  std::size_t fenced_grants = 0;
  for (const of::AuthorityView& grant : grants) {
    if (grant.current) {
      ++current_grants;
    }
    if (grant.fenced) {
      ++fenced_grants;
    }
  }
  OF_REQUIRE_EQ(current_grants, 1u);
  OF_REQUIRE_EQ(fenced_grants, 1u);
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(authority, a_stale_controller_cannot_withdraw_a_live_path) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "auth-stale");
  of::OpticalFabric& fabric = *session->fabric;
  const of::ActivationResult activated =
      of_test::activate_path(fabric, session->fixture, "stale-owner", session->token);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  const of::ConnectivityId id = fabric.connectivity_objects().front().id;

  // A newer controller takes the scope.
  const of::AuthorityResult second = acquire(fabric, "controller-b", session->token.scope, 1000000);
  OF_REQUIRE(second.outcome == of::OutcomeCode::Applied);

  of::WithdrawalRequest stale;
  stale.attempt = of::generate_attempt_id();
  stale.connectivity = id;
  stale.authority = session->token;
  stale.reason = "stale controller";
  const of::WithdrawalResult refused = fabric.withdraw(stale);
  OF_REQUIRE_EQ(refused.outcome, of::OutcomeCode::Refused);
  OF_REQUIRE_EQ(refused.refusal.code, of::RefusalCode::StaleEpoch);
  OF_REQUIRE_EQ(fabric.active_paths().authorized.size(), 1u);

  of::WithdrawalRequest current = stale;
  current.attempt = of::generate_attempt_id();
  current.authority = second.token;
  OF_REQUIRE(fabric.withdraw(current).outcome == of::OutcomeCode::Applied);
  OF_REQUIRE(fabric.active_paths().authorized.empty());
  OF_REQUIRE(fabric.verify_invariants().all_hold);
}

OF_TEST(authority, an_incarnation_from_another_process_cannot_be_forged) {
  std::unique_ptr<Session> session = of_test::make_session(of::FabricOptions{}, "auth-incarnation");
  of::OpticalFabric& fabric = *session->fabric;
  of::AuthorityToken forged = session->token;
  forged.incarnation.boot_sequence = session->token.incarnation.boot_sequence + 1;
  OF_REQUIRE_EQ(fabric.check_authority(forged).currentness, of::AuthorityCurrentness::StaleIncarnation);
  forged = session->token;
  forged.holder = of::derive_named_id<of::ControllerId>("controller", "someone-else");
  OF_REQUIRE_EQ(fabric.check_authority(forged).currentness, of::AuthorityCurrentness::HolderMismatch);
  OF_REQUIRE(fabric.check_authority(session->token).current());
}
