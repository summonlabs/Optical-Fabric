// Optical Fabric 1.0.0 - Summon Software Labs
// Durability: recovery, restart semantics, corruption and truncation.
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::Session;
using of_test::TempDirectory;

namespace {

of::FabricOptions store_options(const TempDirectory& directory, const char* name,
                                of::SalvagePolicy salvage = of::SalvagePolicy::DiscardTail) {
  of::FabricOptions options;
  options.store_path = directory.file(name);
  options.salvage = salvage;
  options.host_label = "unit-test";
  return options;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  OF_REQUIRE(stream.good());
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Builds and activates one path against a durable store.
of::Digest128 seed_store(const of::FabricOptions& options, const of::Digest128&, std::size_t* objects) {
  std::unique_ptr<Session> session = of_test::make_session(options, "persist");
  const of::ActivationResult activated =
      of_test::activate_path(*session->fabric, session->fixture, "persist-path", session->token);
  OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
  if (objects != nullptr) {
    *objects = session->fabric->connectivity_objects().size();
  }
  return session->fabric->snapshot().digest;
}

}  // namespace

OF_TEST(persistence, restart_creates_a_fresh_incarnation_and_unconfirms_authority) {
  TempDirectory directory;
  const of::FabricOptions options = store_options(directory, "fabric.store");
  std::size_t objects = 0;
  of::Digest128 topology_digest{};
  std::uint64_t first_boot = 0;
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "persist");
    const of::ActivationResult activated =
        of_test::activate_path(*session->fabric, session->fixture, "persist-path", session->token);
    OF_REQUIRE_EQ(activated.state, of::ConnectivityState::Active);
    OF_REQUIRE_EQ(session->fabric->active_paths().authorized.size(), 1u);
    objects = session->fabric->connectivity_objects().size();
    topology_digest = session->fabric->topology().digest;
    first_boot = session->fabric->incarnation().boot_sequence;
    OF_REQUIRE_EQ(session->fabric->recovery().status, of::RecoveryStatus::Created);
  }
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "persist");
    const of::RecoveryReport report = session->fabric->recovery();
    OF_REQUIRE(report.ok());
    OF_REQUIRE_EQ(report.previous_boot_sequence, first_boot);
    OF_REQUIRE_EQ(report.boot_sequence, first_boot + 1);
    OF_REQUIRE_EQ(session->fabric->incarnation().boot_sequence, first_boot + 1);
    OF_REQUIRE(report.authority_unconfirmed >= 1u);
    OF_REQUIRE(report.evidence_marked_stale >= 1u);
    // The topology is recovered exactly: identity and registration survive.
    OF_REQUIRE_EQ(session->fabric->topology().digest, topology_digest);
    OF_REQUIRE_EQ(session->fabric->connectivity_objects().size(), objects);
    // The path is still reported as committed, but it is not authorized until
    // its authority and evidence are reconfirmed.
    const of::ActivePathReport paths = session->fabric->active_paths();
    OF_REQUIRE_EQ(paths.active.size(), 1u);
    OF_REQUIRE_EQ(paths.active.front().state, of::ConnectivityState::Active);
    OF_REQUIRE(!paths.active.front().authority_confirmed);
    OF_REQUIRE(paths.authorized.empty());
    OF_REQUIRE(!paths.diagnostics.empty());
    OF_REQUIRE_EQ(paths.diagnostics.front().kind, of::DiagnosticKind::UnconfirmedAuthority);

    // Revalidation is the only way back to authorized, and it needs fresh
    // evidence from the current incarnation.
    const of::ConnectivityId id = paths.active.front().connectivity;
    of::RevalidationRequest revalidation;
    revalidation.attempt = of::generate_attempt_id();
    revalidation.connectivity = id;
    revalidation.authority = of_test::acquire_site_authority(*session->fabric, "controller-a",
                                                            session->fixture.site, 1000000);
    revalidation.withdraw_on_failure = false;
    const of::RevalidationResult rejected = session->fabric->revalidate(revalidation);
    OF_REQUIRE_EQ(rejected.outcome, of::OutcomeCode::Refused);
    OF_REQUIRE_EQ(rejected.refusal.code, of::RefusalCode::StaleEvidence);

    of_test::satisfy_evidence(*session);
    of::RevalidationRequest retry = revalidation;
    retry.attempt = of::generate_attempt_id();
    const of::RevalidationResult confirmed = session->fabric->revalidate(retry);
    OF_REQUIRE(confirmed.outcome == of::OutcomeCode::Applied);
    OF_REQUIRE(confirmed.authority_confirmed);
    OF_REQUIRE_EQ(session->fabric->active_paths().authorized.size(), 1u);
    OF_REQUIRE(session->fabric->verify_invariants().all_hold);
  }
}

OF_TEST(persistence, incomplete_activation_and_withdrawal_are_resolved_conservatively) {
  TempDirectory directory;
  const of::FabricOptions options = store_options(directory, "fabric.store");
  of::ConnectivityId activating{};
  of::ConnectivityId withdrawing{};
  {
    // Two independent lines in one store, so both boundary objects exist at
    // the same time and neither shadows the other.
    std::unique_ptr<Session> activation_session = of_test::make_session(options, "boundary-a");
    of::OpticalFabric& fabric = *activation_session->fabric;
    // A second declared line in the same runtime, with its own site authority.
    const of_test::LineFixture second_line = of_test::build_line(fabric, "boundary-b");
    const of::AuthorityToken second_token = of_test::acquire_site_authority(
        fabric, "controller-b", second_line.site, 1000000);
    OF_REQUIRE(fabric
                   .refresh_from_source(activation_session->source->describe().id,
                                        of_test::fixture_path_resources(second_line),
                                        of_test::fixture_evidence_kinds(), 8192)
                   .ok());
    const of::IntentSubmission first = fabric.submit_intent(
        of_test::make_intent(activation_session->fixture, "activation-boundary"));
    OF_REQUIRE(first.outcome == of::IntentOutcome::Accepted);
    of::ValidateRequest validation;
    validation.attempt = of::generate_attempt_id();
    validation.connectivity = first.connectivity;
    validation.authority = activation_session->token;
    OF_REQUIRE(fabric.validate(validation).outcome == of::OutcomeCode::Applied);
    of::ReservationRequest reservation;
    reservation.attempt = of::generate_attempt_id();
    reservation.connectivity = first.connectivity;
    reservation.authority = activation_session->token;
    reservation.ttl_ticks = 5000;
    OF_REQUIRE(fabric.reserve(reservation).outcome == of::OutcomeCode::Applied);
    of::ActivationRequest activation;
    activation.attempt = of::generate_attempt_id();
    activation.connectivity = first.connectivity;
    activation.authority = activation_session->token;
    OF_REQUIRE(fabric.begin_activation(activation).outcome == of::OutcomeCode::Applied);
    activating = first.connectivity;

    // The second object reaches ACTIVE and is then left mid-withdrawal.
    const of::ActivationResult committed =
        of_test::activate_path(fabric, second_line, "withdrawal-boundary", second_token);
    OF_REQUIRE_EQ(committed.state, of::ConnectivityState::Active);
    const of::ConnectivityId second = fabric.connectivity_objects().back().id;
    of::WithdrawalRequest withdraw;
    withdraw.attempt = of::generate_attempt_id();
    withdraw.connectivity = second;
    withdraw.authority = second_token;
    withdraw.reason = "crash boundary";
    const of::WithdrawalResult started = fabric.withdraw(withdraw);
    OF_REQUIRE_EQ(started.state, of::ConnectivityState::Withdrawing);
    withdrawing = second;
    OF_REQUIRE(fabric.flush().ok());
  }
  {
    std::unique_ptr<Session> recovered = of_test::make_session(options, "boundary-a");
    const of::RecoveryReport report = recovered->fabric->recovery();
    OF_REQUIRE(report.incomplete_activations >= 1u);
    OF_REQUIRE(report.resumed_withdrawals >= 1u);
    const of::Result<of::ConnectivityView> activation =
        recovered->fabric->describe_connectivity(activating);
    OF_REQUIRE(activation.ok());
    // An activation that never crossed the commit boundary is not active.
    OF_REQUIRE_EQ(activation.value.state, of::ConnectivityState::Failed);
    OF_REQUIRE_EQ(activation.value.last_refusal, of::RefusalCode::MissingEvidence);
    const of::Result<of::ConnectivityView> withdrawal =
        recovered->fabric->describe_connectivity(withdrawing);
    OF_REQUIRE(withdrawal.ok());
    OF_REQUIRE_EQ(withdrawal.value.state, of::ConnectivityState::Retired);
    OF_REQUIRE(recovered->fabric->active_paths().authorized.empty());
    OF_REQUIRE_EQ(recovered->fabric->claims().claims.size(), 0u);
    OF_REQUIRE(recovered->fabric->verify_invariants().all_hold);
    OF_REQUIRE(recovered->fabric->accounting().balanced);
  }
}

OF_TEST(persistence, truncated_tail_is_recovered_and_mid_file_corruption_is_refused) {
  TempDirectory directory;
  const std::filesystem::path path = directory.file("fabric.store");
  const of::FabricOptions options = store_options(directory, "fabric.store");
  const of::Digest128 seeded = seed_store(options, of::Digest128{}, nullptr);
  OF_REQUIRE(!seeded.is_nil());
  const std::string intact = read_file(path);
  OF_REQUIRE(intact.size() > 64u);

  // 1. A torn tail: the last bytes never formed a complete record.
  {
    write_file(path, intact.substr(0, intact.size() - 7));
    std::unique_ptr<Session> session =
        of_test::make_session(store_options(directory, "fabric.store"), "persist");
    const of::RecoveryReport report = session->fabric->recovery();
    OF_REQUIRE_EQ(report.status, of::RecoveryStatus::TailDiscarded);
    OF_REQUIRE(report.discarded_bytes > 0u);
    OF_REQUIRE_EQ(session->fabric->connectivity_objects().size(), 1u);
    const of::InvariantReport invariants = session->fabric->verify_invariants();
    std::string failures;
    for (const of::InvariantCheck& check : invariants.checks) {
      if (!check.holds) {
        failures.append(check.name).append(": ").append(check.detail).append("; ");
      }
    }
    OF_REQUIRE_MSG(invariants.all_hold, failures);
  }

  // 2. The same torn tail under the reject policy must be refused untouched.
  {
    write_file(path, intact.substr(0, intact.size() - 7));
    const std::string before = read_file(path);
    of::OpticalFabric fabric(store_options(directory, "fabric.store", of::SalvagePolicy::Reject));
    OF_REQUIRE(fabric.closed());
    OF_REQUIRE_EQ(fabric.recovery().status, of::RecoveryStatus::Rejected);
    OF_REQUIRE_EQ(read_file(path), before);
  }

  // 3. Mid-file corruption with intact records after it is never truncated.
  {
    std::string corrupted = intact;
    const std::size_t victim = 64;
    corrupted[victim] = static_cast<char>(corrupted[victim] ^ 0x5A);
    write_file(path, corrupted);
    const std::string before = read_file(path);
    of::OpticalFabric fabric(store_options(directory, "fabric.store"));
    OF_REQUIRE(fabric.closed());
    OF_REQUIRE_EQ(fabric.recovery().status, of::RecoveryStatus::Rejected);
    OF_REQUIRE_EQ(read_file(path), before);
  }

  // 4. A wrong magic or an unsupported version is refused, never guessed at.
  {
    std::string wrong_magic = intact;
    wrong_magic[0] = 'X';
    write_file(path, wrong_magic);
    of::OpticalFabric fabric(store_options(directory, "fabric.store"));
    OF_REQUIRE(fabric.closed());
    OF_REQUIRE_EQ(fabric.recovery().status, of::RecoveryStatus::Rejected);

    std::string wrong_version = intact;
    wrong_version[4] = static_cast<char>(9);
    write_file(path, wrong_version);
    of::OpticalFabric versioned(store_options(directory, "fabric.store"));
    OF_REQUIRE(versioned.closed());
    OF_REQUIRE_EQ(versioned.recovery().status, of::RecoveryStatus::Rejected);
  }

  // 5. The intact store still opens after all of that.
  {
    write_file(path, intact);
    std::unique_ptr<Session> session = of_test::make_session(
        store_options(directory, "fabric.store"), "persist");
    OF_REQUIRE(session->fabric->recovery().ok());
    OF_REQUIRE_EQ(session->fabric->connectivity_objects().size(), 1u);
  }
}

OF_TEST(persistence, inspection_is_read_only_and_reports_integrity) {
  TempDirectory directory;
  const std::filesystem::path path = directory.file("fabric.store");
  const of::FabricOptions options = store_options(directory, "fabric.store");
  of::Limits limits;
  const of::Result<of::PersistenceInspection> missing = of::inspect_persistence(path, limits);
  OF_REQUIRE(missing.ok());
  OF_REQUIRE(!missing.value.exists);

  const of::Digest128 seeded = seed_store(options, of::Digest128{}, nullptr);
  OF_REQUIRE(!seeded.is_nil());
  const std::string intact = read_file(path);
  const of::Result<of::PersistenceInspection> inspection = of::inspect_persistence(path, limits);
  OF_REQUIRE(inspection.ok());
  OF_REQUIRE(inspection.value.exists);
  OF_REQUIRE(inspection.value.header_valid);
  OF_REQUIRE_EQ(inspection.value.format_version, of::kStateFormatVersion);
  OF_REQUIRE(inspection.value.records > 0u);
  OF_REQUIRE(!inspection.value.tail_torn);
  OF_REQUIRE_EQ(inspection.value.valid_bytes, intact.size());
  OF_REQUIRE(inspection.value.last_epoch > 0u);
  OF_REQUIRE_EQ(read_file(path), intact);

  std::string torn = intact.substr(0, intact.size() - 5);
  write_file(path, torn);
  const of::Result<of::PersistenceInspection> damaged = of::inspect_persistence(path, limits);
  OF_REQUIRE(damaged.ok());
  OF_REQUIRE(damaged.value.tail_torn);
  OF_REQUIRE(damaged.value.trailing_bytes > 0u);
  OF_REQUIRE(!damaged.value.detail.empty());
}

OF_TEST(persistence, a_second_writer_is_refused) {
  TempDirectory directory;
  const of::FabricOptions options = store_options(directory, "fabric.store");
  std::unique_ptr<Session> session = of_test::make_session(options, "lock");
  of::OpticalFabric second(options);
  OF_REQUIRE(second.closed());
  OF_REQUIRE_EQ(second.recovery().status, of::RecoveryStatus::Rejected);
  // The lock is a sidecar file, so the store itself is untouched by the refusal.
  OF_REQUIRE(std::filesystem::exists(directory.file("fabric.store")));
  OF_REQUIRE(session->fabric->verify_invariants().all_hold);
  OF_REQUIRE(session->fabric->close().ok());
  of::OpticalFabric third(options);
  OF_REQUIRE(!third.closed());
}

OF_TEST(persistence, compaction_bounds_growth_and_preserves_state) {
  TempDirectory directory;
  of::FabricOptions options = store_options(directory, "fabric.store");
  options.limits.max_journal_bytes = 8192;
  const std::filesystem::path path = directory.file("fabric.store");
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "compact");
    of::OpticalFabric& fabric = *session->fabric;
    for (int index = 0; index < 20; ++index) {
      OF_REQUIRE(fabric.advance_tick(1).ok());
      of_test::satisfy_evidence(*session);
    }
    OF_REQUIRE(fabric.active_paths().active.empty());
  }
  of::Limits limits;
  const of::Result<of::PersistenceInspection> inspection = of::inspect_persistence(path, limits);
  OF_REQUIRE(inspection.ok());
  // Compaction collapses the journal into one checkpoint plus the commits that
  // followed it, so the record count stays bounded no matter how many commits
  // have happened.
  OF_REQUIRE_MSG(inspection.value.records <= 8u,
                 "the journal holds " + std::to_string(inspection.value.records) + " records");
  const std::uintmax_t after_evidence = std::filesystem::file_size(path);
  // Further commits that add no state must not grow the store: the journal is
  // rewritten by compaction, not appended to forever.
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "compact");
    for (int index = 0; index < 60; ++index) {
      OF_REQUIRE(session->fabric->advance_tick(1).ok());
    }
  }
  const std::uintmax_t after_commits = std::filesystem::file_size(path);
  OF_REQUIRE_MSG(after_commits < after_evidence * 2u,
                 "the store grew from " + std::to_string(after_evidence) + " to " +
                     std::to_string(after_commits) + " bytes without new state");
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "compact");
    OF_REQUIRE(session->fabric->recovery().ok());
    OF_REQUIRE(session->fabric->topology().resources.size() == 12u);
    OF_REQUIRE(session->fabric->verify_invariants().all_hold);
    OF_REQUIRE(session->fabric->accounting().balanced);
  }
}

OF_TEST(persistence, state_digest_is_stable_across_a_clean_reopen) {
  TempDirectory directory;
  const of::FabricOptions options = store_options(directory, "fabric.store");
  of::Digest128 topology{};
  std::uint64_t epoch = 0;
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "stable");
    topology = session->fabric->topology().digest;
    epoch = session->fabric->current_epoch().value;
    OF_REQUIRE(session->fabric->flush().ok());
  }
  {
    std::unique_ptr<Session> session = of_test::make_session(options, "stable");
    OF_REQUIRE_EQ(session->fabric->topology().digest, topology);
    OF_REQUIRE(session->fabric->current_epoch().value >= epoch);
    OF_REQUIRE_EQ(session->fabric->topology().resources.size(), 12u);
  }
}
