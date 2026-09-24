// Optical Fabric 1.0.0 - Summon Software Labs
// A durable store survives a restart, but authority does not: the new
// incarnation must reconfirm the path before it is authorized again.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "ex_support.hpp"

namespace {

constexpr const char* kStoreName = "optical-fabric-example.store";

of::AuthorityToken acquire(of::OpticalFabric& fabric, of::SiteId site) {
  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "example");
  authority.scope = of::AuthorityScope::of_site(site);
  authority.lease_ticks = 1000000;
  return fabric.acquire_authority(authority).token;
}

}  // namespace

int main() {
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "optical-fabric-example";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  of::FabricOptions options;
  options.store_path = directory / kStoreName;
  options.host_label = "example";

  of::ConnectivityId connectivity{};
  of::SiteId site{};
  {
    of::OpticalFabric fabric(options);
    const ExampleLine line = build_example_line(fabric, "restart");
    site = line.site;
    auto source = std::make_shared<ExampleEvidenceSource>("example-restart");
    fabric.register_evidence_source(source);
    (void)fabric.refresh_from_source(source->describe().id,
                                     example_path_resources(line),
                                     example_kinds(), 4096);
    const of::AuthorityToken token = acquire(fabric, line.site);
    of::ConnectivityIntent intent;
    intent.name = "restart.path";
    intent.source_port = line.client_a;
    intent.destination_port = line.client_b;
    intent.channel = line.channel;
    intent.reservation_ttl_ticks = 4096;
    connectivity = fabric.submit_intent(intent).connectivity;
    of::ValidateRequest validation;
    validation.attempt = of::generate_attempt_id();
    validation.connectivity = connectivity;
    validation.authority = token;
    (void)fabric.validate(validation);
    of::ReservationRequest reservation;
    reservation.attempt = of::generate_attempt_id();
    reservation.connectivity = connectivity;
    reservation.authority = token;
    reservation.ttl_ticks = 4096;
    (void)fabric.reserve(reservation);
    of::ActivationRequest activation;
    activation.attempt = of::generate_attempt_id();
    activation.connectivity = connectivity;
    activation.authority = token;
    const of::ActivationResult began = fabric.begin_activation(activation);
    of::CommitRequest commit;
    commit.attempt = of::generate_attempt_id();
    commit.connectivity = connectivity;
    commit.authority = token;
    commit.activation_digest = began.activation_digest;
    const of::ActivationResult activated = fabric.commit_activation(commit);
    std::printf("before restart   state=%s authorized=%llu\n",
                std::string(of::to_string(activated.state)).c_str(),
                static_cast<unsigned long long>(fabric.active_paths().authorized.size()));
  }
  {
    of::OpticalFabric fabric(options);
    const of::RecoveryReport report = fabric.recovery();
    std::printf("recovery         %s boot=%llu (was %llu)\n",
                std::string(of::to_string(report.status)).c_str(),
                static_cast<unsigned long long>(report.boot_sequence),
                static_cast<unsigned long long>(report.previous_boot_sequence));
    const of::ActivePathReport paths = fabric.active_paths();
    std::printf("after restart    active=%llu authorized=%llu\n",
                static_cast<unsigned long long>(paths.active.size()),
                static_cast<unsigned long long>(paths.authorized.size()));
    std::printf("reason           %s\n",
                paths.active.empty() ? "" : paths.active.front().unauthorized_reason.c_str());

    auto source = std::make_shared<ExampleEvidenceSource>("example-restart-2");
    fabric.register_evidence_source(source);
    const ExampleLine line = build_example_line(fabric, "restart");
    const of::Result<std::size_t> refreshed = fabric.refresh_from_source(
        source->describe().id, example_path_resources(line), example_kinds(), 1000000);
    std::printf("evidence refresh %s (%zu records)\n",
                refreshed.ok() ? "ok" : refreshed.status.message.c_str(),
                refreshed.ok() ? refreshed.value : 0u);
    of::RevalidationRequest revalidation;
    revalidation.attempt = of::generate_attempt_id();
    revalidation.connectivity = connectivity;
    revalidation.authority = acquire(fabric, site);
    const of::RevalidationResult confirmed = fabric.revalidate(revalidation);
    std::printf("revalidation     %s confirmed=%s evidence=%s\n",
                std::string(of::to_string(confirmed.outcome)).c_str(),
                confirmed.authority_confirmed ? "yes" : "no",
                std::string(of::to_string(confirmed.assessment.aggregate)).c_str());
    for (const of::EvidenceEvaluation& evaluation : confirmed.assessment.evaluations) {
      if (evaluation.state == of::EvidenceState::Known) {
        continue;
      }
      const of::Result<of::ResourceView> subject =
          fabric.describe_resource(evaluation.requirement.subject);
      std::printf("  not satisfied  %-32s %-14s %s\n", std::string(of::to_string(evaluation.requirement.kind)).c_str(),
                  std::string(of::to_string(evaluation.state)).c_str(),
                  evaluation.detail.c_str());
      (void)subject;
    }
    std::printf("authorized       %llu\n",
                static_cast<unsigned long long>(fabric.active_paths().authorized.size()));
  }
  std::filesystem::remove_all(directory, error);
  return 0;
}
