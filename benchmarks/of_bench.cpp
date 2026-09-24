// Optical Fabric 1.0.0 - Summon Software Labs
// Benchmarks of completed useful work.
//
// Every number below counts an operation that finished, including its durable
// commit where one applies. Nothing is measured at submission time, and no
// result describes optical hardware: the work is a synthetic control-plane
// model and the timings say nothing about photonic devices.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ex_support.hpp"

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const char* name, double seconds, std::size_t completed) {
  const double per_second = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
  std::printf("%-34s %10zu completed in %8.3f s  %12.0f /s\n", name, completed, seconds, per_second);
}

/// Canonical path identity: the work is a fully canonicalised path.
void bench_canonical_identity() {
  of::Limits limits;
  of::PathDescriptor descriptor;
  descriptor.segments = {
      of::PathSegment{of::SegmentRole::Port, of::as_ref(of::derive_id<of::PortId>("bench.p1")),
                       of::Generation{1}},
      of::PathSegment{of::SegmentRole::Span, of::as_ref(of::derive_id<of::SpanId>("bench.s1")),
                       of::Generation{1}},
      of::PathSegment{of::SegmentRole::Port, of::as_ref(of::derive_id<of::PortId>("bench.p2")),
                       of::Generation{1}}};
  descriptor.channel = of::derive_id<of::ChannelId>("bench.c0");
  descriptor.channel_generation = of::Generation{1};
  constexpr std::size_t kIterations = 200000;
  const Clock::time_point start = Clock::now();
  std::size_t completed = 0;
  for (std::size_t index = 0; index < kIterations; ++index) {
    const of::Result<of::CanonicalPath> path = of::canonicalize_path(descriptor, limits);
    if (path.ok()) {
      completed += 1;
    }
  }
  report("canonical path identity", seconds_since(start), completed);
}

/// Full lifecycle on one line: validate, reserve, activate, commit, withdraw.
void bench_lifecycle(std::size_t rounds, bool durable) {
  of::FabricOptions options;
  std::filesystem::path directory;
  if (durable) {
    directory = std::filesystem::temp_directory_path() / "optical-fabric-bench";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    options.store_path = directory / "bench.store";
  }
  of::OpticalFabric fabric(options);
  of::AuthorityToken token{};
  std::size_t completed = 0;
  const Clock::time_point start = Clock::now();
  for (std::size_t round = 0; round < rounds; ++round) {
    const std::string prefix = "bench-" + std::to_string(round);
    const ExampleLine line = build_example_line(fabric, prefix);
    auto source = std::make_shared<ExampleEvidenceSource>(prefix);
    fabric.register_evidence_source(source);
    (void)fabric.refresh_from_source(source->describe().id, example_path_resources(line),
                                     example_kinds(), 1000000);
    of::AuthorityRequest authority;
    authority.attempt = of::generate_attempt_id();
    authority.holder = of::derive_named_id<of::ControllerId>("controller", prefix);
    authority.scope = of::AuthorityScope::of_site(line.site);
    authority.lease_ticks = 1000000;
    token = fabric.acquire_authority(authority).token;
    of::ConnectivityIntent intent;
    intent.name = prefix + ".path";
    intent.source_port = line.client_a;
    intent.destination_port = line.client_b;
    intent.channel = line.channel;
    intent.reservation_ttl_ticks = 1000000;
    const of::IntentSubmission submitted = fabric.submit_intent(intent);
    of::ValidateRequest validation;
    validation.attempt = of::generate_attempt_id();
    validation.connectivity = submitted.connectivity;
    validation.authority = token;
    const of::ValidationResult validated = fabric.validate(validation);
    of::ReservationRequest reservation;
    reservation.attempt = of::generate_attempt_id();
    reservation.connectivity = submitted.connectivity;
    reservation.authority = token;
    reservation.ttl_ticks = 1000000;
    const of::ReservationResult reserved = fabric.reserve(reservation);
    of::ActivationRequest activation;
    activation.attempt = of::generate_attempt_id();
    activation.connectivity = submitted.connectivity;
    activation.authority = token;
    const of::ActivationResult began = fabric.begin_activation(activation);
    of::CommitRequest commit;
    commit.attempt = of::generate_attempt_id();
    commit.connectivity = submitted.connectivity;
    commit.authority = token;
    commit.activation_digest = began.activation_digest;
    const of::ActivationResult activated = fabric.commit_activation(commit);
    of::WithdrawalRequest withdraw;
    withdraw.attempt = of::generate_attempt_id();
    withdraw.connectivity = submitted.connectivity;
    withdraw.authority = token;
    const of::WithdrawalResult withdrawn = fabric.withdraw(withdraw);
    of::WithdrawalRequest finish = withdraw;
    finish.attempt = of::generate_attempt_id();
    const of::WithdrawalResult retired = fabric.withdraw(finish);
    if (validated.outcome != of::OutcomeCode::Refused &&
        reserved.outcome != of::OutcomeCode::Refused &&
        activated.state == of::ConnectivityState::Active &&
        withdrawn.state == of::ConnectivityState::Withdrawing &&
        retired.state == of::ConnectivityState::Retired) {
      completed += 1;
    }
  }
  report(durable ? "lifecycle (durable store)" : "lifecycle (memory only)", seconds_since(start),
         completed);
  if (durable) {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
}

/// Concurrent reservations for one object: completed grants, and refused ones.
void bench_concurrent_reservations() {
  of::OpticalFabric fabric{of::FabricOptions{}};
  const ExampleLine line = build_example_line(fabric, "bench-race");
  auto source = std::make_shared<ExampleEvidenceSource>("bench-race");
  fabric.register_evidence_source(source);
  (void)fabric.refresh_from_source(source->describe().id, example_path_resources(line),
                                   example_kinds(), 1000000);
  of::AuthorityRequest authority;
  authority.attempt = of::generate_attempt_id();
  authority.holder = of::derive_named_id<of::ControllerId>("controller", "bench");
  authority.scope = of::AuthorityScope::of_site(line.site);
  authority.lease_ticks = 1000000;
  const of::AuthorityToken token = fabric.acquire_authority(authority).token;
  of::ConnectivityIntent intent;
  intent.name = "bench-race.path";
  intent.source_port = line.client_a;
  intent.destination_port = line.client_b;
  intent.channel = line.channel;
  const of::IntentSubmission submitted = fabric.submit_intent(intent);
  of::ValidateRequest validation;
  validation.attempt = of::generate_attempt_id();
  validation.connectivity = submitted.connectivity;
  validation.authority = token;
  (void)fabric.validate(validation);

  constexpr std::size_t kWorkers = 8;
  constexpr std::size_t kEach = 200;
  std::atomic<std::size_t> granted{0};
  std::vector<std::thread> workers;
  const Clock::time_point start = Clock::now();
  for (std::size_t worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&]() {
      for (std::size_t index = 0; index < kEach; ++index) {
        of::ReservationRequest request;
        request.attempt = of::generate_attempt_id();
        request.connectivity = submitted.connectivity;
        request.authority = token;
        request.ttl_ticks = 1000000;
        const of::ReservationResult result = fabric.reserve(request);
        if (result.outcome == of::OutcomeCode::Applied) {
          granted.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  report("concurrent reservation attempts", seconds_since(start), kWorkers * kEach);
  std::printf("%-34s %10zu granted (expected exactly one)\n", "  reservations completed",
              granted.load());
}

}  // namespace

int main() {
  std::printf("Optical Fabric %s benchmarks\n", std::string(of::version_string()).c_str());
  std::printf("SYNTHETIC control-plane model: no optical hardware is involved and no\n");
  std::printf("number below describes photonic device behaviour.\n\n");
  bench_canonical_identity();
  bench_lifecycle(64, false);
  bench_lifecycle(16, true);
  bench_concurrent_reservations();
  return 0;
}
