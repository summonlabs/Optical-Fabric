// Optical Fabric 1.0.0 - Summon Software Labs
// Why a proposal is not authority: missing evidence and stale authority are
// refused with typed codes, and nothing becomes active by accident.
#include <cstdio>
#include <string>

#include "ex_support.hpp"

int main() {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const ExampleLine line = build_example_line(fabric, "refusal");
  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "example");
  authority.scope = of::AuthorityScope::of_site(line.site);
  authority.lease_ticks = 4096;
  const of::AuthorityResult granted = fabric.acquire_authority(authority);

  of::ConnectivityIntent intent;
  intent.name = "refusal.path";
  intent.source_port = line.client_a;
  intent.destination_port = line.client_b;
  intent.channel = line.channel;
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  std::printf("intent           %s\n", std::string(of::to_string(submitted.outcome)).c_str());

  // No producer is registered at this boundary, so the runtime cannot know the
  // health of the path and refuses to promote it.
  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = granted.token;
  const of::ValidationResult refused = fabric.validate(validation);
  std::printf("without evidence %s / %s\n",
              std::string(of::to_string(refused.outcome)).c_str(),
              std::string(of::to_string(refused.refusal.code)).c_str());
  std::printf("reason           %s\n", refused.refusal.detail.c_str());

  // A token that names another site cannot govern this path either.
  auto source = std::make_shared<ExampleEvidenceSource>("example-refusal");
  fabric.register_evidence_source(source);
  (void)fabric.refresh_from_source(source->describe().id, example_path_resources(line),
                                   example_kinds(), 4096);
  of::AuthorityRequest other;
  other.attempt = of::generate_attempt_id();
  other.holder = granted.token.holder;
  other.scope = of::AuthorityScope::global();
  other.lease_ticks = 4096;
  const of::AuthorityResult taken_over = fabric.acquire_authority(other);
  of::ValidateRequest stale;
  stale.attempt = of::generate_attempt_id();
  stale.connectivity = submitted.connectivity;
  stale.authority = granted.token;
  const of::ValidationResult stale_result = fabric.validate(stale);
  std::printf("stale authority  %s / %s\n",
              std::string(of::to_string(stale_result.outcome)).c_str(),
              std::string(of::to_string(stale_result.refusal.code)).c_str());

  of::ValidateRequest current = stale;
  current.attempt = of::generate_attempt_id();
  current.authority = taken_over.token;
  std::printf("current authority %s\n",
              std::string(of::to_string(fabric.validate(current).outcome)).c_str());
  std::printf("active paths     %llu\n",
              static_cast<unsigned long long>(fabric.active_paths().active.size()));
  return refused.outcome == of::OutcomeCode::Refused ? 0 : 1;
}
