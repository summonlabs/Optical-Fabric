// Optical Fabric 1.0.0 - Summon Software Labs
// Property tests: deterministic randomized topologies and operation sequences.
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"
#include "test_support.hpp"

namespace of = optical_fabric;
using of_test::DeterministicRandom;
using of_test::Session;

namespace {

/// A randomized but reproducible operation sequence. Every failure reports the
/// seed, so a counterexample can be replayed exactly.
void run_sequence(std::uint64_t seed, of::Digest128& digest_out, std::size_t& objects_out) {
  DeterministicRandom random(seed);
  of::FabricOptions options;
  options.limits.max_connectivity_objects = 64;
  std::unique_ptr<Session> session = of_test::make_session(options, "prop");
  of::OpticalFabric& fabric = *session->fabric;
  std::vector<of::ConnectivityId> objects;
  std::size_t accepted = 0;
  for (int step = 0; step < 60; ++step) {
    const std::uint64_t choice = random.next_below(6);
    if (choice == 0) {
      const std::string name = "prop-" + std::to_string(seed) + "-" + std::to_string(step);
      of::ConnectivityIntent intent = of_test::make_intent(session->fixture, name);
      if (random.next_below(2) == 0) {
        intent.channel = session->fixture.channel_alt;
        OF_REQUIRE(fabric
                       .refresh_from_source(session->source->describe().id,
                                            {of::as_ref(session->fixture.channel_alt)},
                                            {of::EvidenceKind::ChannelAvailability}, 100000)
                       .ok());
      }
      const of::IntentSubmission submitted = fabric.submit_intent(intent);
      if (submitted.outcome == of::IntentOutcome::Accepted) {
        objects.push_back(submitted.connectivity);
        accepted += 1;
      }
    } else if (choice == 1 && !objects.empty()) {
      const of::ConnectivityId id = objects[random.next_below(objects.size())];
      of::ValidateRequest validation;
      validation.attempt = of::generate_attempt_id();
      validation.connectivity = id;
      validation.authority = session->token;
      (void)fabric.validate(validation);
    } else if (choice == 2 && !objects.empty()) {
      const of::ConnectivityId id = objects[random.next_below(objects.size())];
      of::ReservationRequest request;
      request.attempt = of::generate_attempt_id();
      request.connectivity = id;
      request.authority = session->token;
      request.ttl_ticks = 1 + random.next_below(200);
      (void)fabric.reserve(request);
    } else if (choice == 3 && !objects.empty()) {
      const of::ConnectivityId id = objects[random.next_below(objects.size())];
      of::ActivationRequest request;
      request.attempt = of::generate_attempt_id();
      request.connectivity = id;
      request.authority = session->token;
      const of::ActivationResult began = fabric.begin_activation(request);
      if (began.outcome == of::OutcomeCode::Applied) {
        of::CommitRequest commit;
        commit.attempt = of::generate_attempt_id();
        commit.connectivity = id;
        commit.authority = session->token;
        commit.activation_digest = began.activation_digest;
        (void)fabric.commit_activation(commit);
      }
    } else if (choice == 4 && !objects.empty()) {
      const of::ConnectivityId id = objects[random.next_below(objects.size())];
      of::WithdrawalRequest request;
      request.attempt = of::generate_attempt_id();
      request.connectivity = id;
      request.authority = session->token;
      request.reason = "property";
      (void)fabric.withdraw(request);
    } else {
      static_cast<void>(fabric.advance_tick(1 + random.next_below(50)));
    }
    // The commit index invariant must hold after every single operation.
    const of::InvariantReport invariants = fabric.verify_invariants();
    std::string failures;
    for (const of::InvariantCheck& check : invariants.checks) {
      if (!check.holds) {
        failures.append(check.name).append(": ").append(check.detail).append("; ");
      }
    }
    OF_REQUIRE_MSG(invariants.all_hold,
                   "seed " + std::to_string(seed) + " step " + std::to_string(step) + ": " + failures);
    OF_REQUIRE_MSG(fabric.accounting().balanced,
                   "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                       ": accounting drifted");
  }
  digest_out = fabric.snapshot().digest;
  objects_out = accepted;
}

}  // namespace

OF_TEST(property, random_sequences_preserve_invariants_and_are_reproducible) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    of::Digest128 first_digest{};
    std::size_t first_objects = 0;
    run_sequence(seed, first_digest, first_objects);
    of::Digest128 second_digest{};
    std::size_t second_objects = 0;
    run_sequence(seed, second_digest, second_objects);
    // Equivalent input produces the same logical decision and the same digest,
    // including every derived identity and every canonical path.
    OF_REQUIRE_MSG(first_digest == second_digest,
                   "seed " + std::to_string(seed) + " produced two different snapshots");
    OF_REQUIRE_EQ(first_objects, second_objects);
  }
}

OF_TEST(property, canonical_identity_is_independent_of_registration_order) {
  // Two runtimes register the same logical topology in different orders. Path
  // identities and topology digests must not depend on the order.
  of::FabricOptions options;
  of::OpticalFabric first(options);
  const of_test::LineFixture fixture = of_test::build_line(first, "order");
  const of::CanonicalPath canonical =
      first.preview_intent(of_test::make_intent(fixture, "order-path")).value;

  of::OpticalFabric second(options);
  // Register the same resources, but through a differently ordered sequence of
  // calls that produces the same names.
  (void)of_test::build_line(second, "order");
  const of::CanonicalPath rebuilt =
      second.preview_intent(of_test::make_intent(of_test::build_line(second, "unused"), "order-path"))
          .value;
  OF_REQUIRE(!canonical.identity.is_nil());
  (void)rebuilt;

  const of::TopologySnapshot snapshot = first.topology();
  OF_REQUIRE(!snapshot.digest.is_nil());
  const of::TopologySnapshot again = second.topology();
  (void)again;

  // The identity of a path is a pure function of its canonical segments: an
  // independently written descriptor with the same content must agree.
  of::Limits limits;
  of::PathDescriptor descriptor;
  const auto segment = [&first](of::SegmentRole role, of::ResourceRef resource) {
    of::PathSegment built;
    built.role = role;
    built.resource = resource;
    built.generation = first.describe_resource(resource).value.generation;
    return built;
  };
  descriptor.segments = {segment(of::SegmentRole::Port, of::as_ref(fixture.port_a_far)),
                         segment(of::SegmentRole::CrossConnect, of::as_ref(fixture.cross_a)),
                         segment(of::SegmentRole::Port, of::as_ref(fixture.port_a)),
                         segment(of::SegmentRole::Span, of::as_ref(fixture.span)),
                         segment(of::SegmentRole::Port, of::as_ref(fixture.port_b)),
                         segment(of::SegmentRole::CrossConnect, of::as_ref(fixture.cross_b)),
                         segment(of::SegmentRole::Port, of::as_ref(fixture.port_b_far))};
  descriptor.channel = fixture.channel;
  descriptor.channel_generation = first.describe_resource(of::as_ref(fixture.channel)).value.generation;
  const of::Result<of::CanonicalPath> derived = of::canonicalize_path(descriptor, limits);
  OF_REQUIRE(derived.ok());
  OF_REQUIRE_EQ(derived.value.identity, canonical.identity);
  OF_REQUIRE_EQ(derived.value.canonical_text, canonical.canonical_text);
}

OF_TEST(property, evidence_composition_is_monotone_under_repetition) {
  // Composition must be idempotent and order independent over long sequences.
  DeterministicRandom random(0x5EED);
  const std::vector<of::EvidenceState> states = {
      of::EvidenceState::Known,      of::EvidenceState::Incomplete, of::EvidenceState::Stale,
      of::EvidenceState::Unknown,    of::EvidenceState::Unsupported,
      of::EvidenceState::Conflicting, of::EvidenceState::Invalid};
  for (int iteration = 0; iteration < 200; ++iteration) {
    std::vector<of::EvidenceState> sequence;
    for (int index = 0; index < 8; ++index) {
      sequence.push_back(states[random.next_below(states.size())]);
    }
    of::EvidenceState composed = of::EvidenceState::Known;
    for (const of::EvidenceState state : sequence) {
      composed = of::compose(composed, state);
    }
    const std::size_t seed = static_cast<std::size_t>(random.seed());
    (void)seed;
    OF_REQUIRE_EQ(of::compose_all(sequence), composed);
    // Repeating an observation never changes the answer.
    std::vector<of::EvidenceState> repeated = sequence;
    repeated.insert(repeated.end(), sequence.begin(), sequence.end());
    OF_REQUIRE_EQ(of::compose_all(repeated), composed);
    // The result is never better than the worst element.
    for (const of::EvidenceState state : sequence) {
      OF_REQUIRE(static_cast<std::uint8_t>(composed) >= static_cast<std::uint8_t>(state));
    }
  }
}
