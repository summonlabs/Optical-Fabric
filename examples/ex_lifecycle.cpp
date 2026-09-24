// Optical Fabric 1.0.0 - Summon Software Labs
// The full governed lifecycle of one optical path against a synthetic model.
#include <cstdio>
#include <string>

#include "ex_support.hpp"

int main() {
  of::FabricOptions options;
  of::OpticalFabric fabric(options);
  if (fabric.closed()) {
    std::fprintf(stderr, "the runtime refused to start: %s\n", fabric.recovery().detail.c_str());
    return 1;
  }
  const ExampleLine line = build_example_line(fabric, "example");
  auto source = std::make_shared<ExampleEvidenceSource>("example-1");
  fabric.register_evidence_source(source);
  const of::Result<std::size_t> refreshed = fabric.refresh_from_source(
      source->describe().id,
      example_path_resources(line), example_kinds(), 4096);
  if (!refreshed.ok()) {
    std::fprintf(stderr, "evidence refused: %s\n", refreshed.status.message.c_str());
    return 1;
  }

  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "example");
  authority.scope = of::AuthorityScope::of_site(line.site);
  authority.lease_ticks = 4096;
  const of::AuthorityResult granted = fabric.acquire_authority(authority);
  if (granted.outcome == of::OutcomeCode::Refused) {
    std::fprintf(stderr, "authority refused: %s\n", granted.refusal.detail.c_str());
    return 1;
  }

  of::ConnectivityIntent intent;
  intent.name = "example.client-a-to-client-b";
  intent.owner = "example";
  intent.source_port = line.client_a;
  intent.destination_port = line.client_b;
  intent.channel = line.channel;
  intent.reservation_ttl_ticks = 1024;
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  std::printf("intent           %s\n", std::string(of::to_string(submitted.outcome)).c_str());

  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = granted.token;
  const of::ValidationResult validated = fabric.validate(validation);
  std::printf("validation       %s evidence=%s\n",
              std::string(of::to_string(validated.outcome)).c_str(),
              std::string(of::to_string(validated.assessment.aggregate)).c_str());

  of::ReservationRequest reservation;
  reservation.attempt = of::generate_attempt_id();
  reservation.connectivity = submitted.connectivity;
  reservation.authority = granted.token;
  reservation.ttl_ticks = 1024;
  const of::ReservationResult reserved = fabric.reserve(reservation);
  std::printf("reservation      %s holding %llu resources\n",
              std::string(of::to_string(reserved.outcome)).c_str(),
              static_cast<unsigned long long>(reserved.reservation.resources.size()));

  of::ActivationRequest activation;
  activation.attempt = of::generate_attempt_id();
  activation.connectivity = submitted.connectivity;
  activation.authority = granted.token;
  const of::ActivationResult began = fabric.begin_activation(activation);
  of::CommitRequest commit;
  commit.attempt = of::generate_attempt_id();
  commit.connectivity = submitted.connectivity;
  commit.authority = granted.token;
  commit.activation_digest = began.activation_digest;
  const of::ActivationResult activated = fabric.commit_activation(commit);
  std::printf("activation       %s\n", std::string(of::to_string(activated.state)).c_str());

  const of::Result<of::PathExplanation> explanation = fabric.explain(submitted.connectivity);
  if (explanation.ok()) {
    const of::CanonicalPath canonical{explanation.value.canonical_path, explanation.value.identity,
                                      static_cast<std::uint32_t>(explanation.value.segments.size())};
    std::printf("path             %s\n", of::render_path_line(canonical).c_str());
    std::printf("authority        %s\n", explanation.value.authority_confirmed ? "confirmed" : "unconfirmed");
    for (const of::SegmentProvenance& segment : explanation.value.segments) {
      std::printf("  segment %-22s claim=%-16s evidence=%s\n", segment.resource_name.c_str(),
                  segment.claim.c_str(), std::string(of::to_string(segment.evidence_state)).c_str());
    }
  }
  std::printf("authorized       %llu\n",
              static_cast<unsigned long long>(fabric.active_paths().authorized.size()));
  std::printf("invariants       %s\n", fabric.verify_invariants().all_hold ? "hold" : "violated");
  return fabric.verify_invariants().all_hold ? 0 : 1;
}
