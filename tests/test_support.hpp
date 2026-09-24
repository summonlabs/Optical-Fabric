// Optical Fabric 1.0.0 - Summon Software Labs
// Shared fixtures for the test suite.
//
// Every producer in this file is SYNTHETIC: it observes a synthetic model and
// says so in its provenance. Nothing here touches optical hardware, and no test
// claims hardware behaviour.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/optical_fabric.hpp"
#include "test_harness.hpp"

namespace of_test {

namespace of = optical_fabric;

/// A minimal declared topology: two terminal nodes joined by one span through a
/// single cross connect on each side.
struct LineFixture {
  of::SiteId site{};
  of::OpticalNodeId node_a{};
  of::OpticalNodeId node_b{};
  of::PortId port_a{};
  of::PortId port_b{};
  of::PortId port_a_far{};
  of::PortId port_b_far{};
  of::SpanId span{};
  of::CrossConnectId cross_a{};
  of::CrossConnectId cross_b{};
  of::ChannelId channel{};
  of::ChannelId channel_alt{};
};

/// Registers the fixture topology. `prefix` keeps concurrent fixtures distinct.
[[nodiscard]] LineFixture build_line(of::OpticalFabric& fabric, const std::string& prefix);

/// Registers just a site, used by authority tests that need a scope.
[[nodiscard]] of::SiteId build_site(of::OpticalFabric& fabric, const std::string& prefix);

/// Fills the intent for the fixture's usable path.
[[nodiscard]] of::ConnectivityIntent make_intent(const LineFixture& fixture, const std::string& name);

/// Acquires authority over the fixture site and returns the token.
[[nodiscard]] of::AuthorityToken acquire_site_authority(of::OpticalFabric& fabric,
                                                        const std::string& holder,
                                                        of::SiteId site,
                                                        std::uint64_t lease_ticks = 512);

/// A SYNTHETIC evidence producer. It answers KNOWN for every kind it declares,
/// except for kinds configured to answer differently, so a test can model an
/// unsupported, unknown, stale or conflicting producer.
class SyntheticSource : public of::IEvidenceSource {
 public:
  SyntheticSource(std::string runtime, std::string instance, std::vector<of::EvidenceKind> produced);

  [[nodiscard]] of::SourceDescriptor describe() const override;
  [[nodiscard]] of::Result<of::EvidenceBundle> poll(const of::EvidencePollRequest& request) override;

  /// Kinds this producer declares it can never answer for.
  void declare_unsupported(std::vector<of::EvidenceKind> kinds);
  /// State to report for one kind instead of KNOWN.
  void set_state(of::EvidenceKind kind, of::EvidenceState state);
  /// Produces a distinct content digest, which makes two producers of the same
  /// kind disagree.
  void set_digest_salt(std::uint64_t salt);
  /// Drops the validity window, producing a malformed observation.
  void set_validity_span_override(std::uint64_t span);
  void set_polls_before_answer(std::size_t polls);
  [[nodiscard]] std::size_t poll_count() const noexcept { return poll_count_; }

 private:
  std::string runtime_;
  std::string instance_;
  of::SourceId source_{};
  std::vector<of::EvidenceKind> produced_;
  std::vector<of::EvidenceKind> unsupported_;
  std::vector<std::pair<of::EvidenceKind, of::EvidenceState>> states_;
  std::uint64_t digest_salt_ = 0;
  std::uint64_t validity_override_ = 0;
  std::size_t polls_before_answer_ = 0;
  std::size_t poll_count_ = 0;
};

/// A SYNTHETIC planner that proposes an explicit resource chain.
class ScriptedPlanner : public of::IPlannerPort {
 public:
  explicit ScriptedPlanner(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] std::string name() const override { return name_; }
  [[nodiscard]] of::Result<of::RouteProposal> propose_route(const of::RouteRequest& request) override;

  void set_route(std::vector<of::ResourceRef> resources, of::ChannelId channel);
  void fail_with(of::ErrorCode code, std::string message);
  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

 private:
  std::string name_;
  std::vector<of::ResourceRef> resources_;
  of::ChannelId channel_{};
  bool failing_ = false;
  of::ErrorCode failure_code_ = of::ErrorCode::Unsupported;
  std::string failure_message_;
  std::size_t calls_ = 0;
};

/// Convenience: acquire authority, submit, validate, reserve, activate and
/// commit the fixture path. Returns the commit result.
[[nodiscard]] of::ActivationResult activate_path(of::OpticalFabric& fabric, const LineFixture& fixture,
                                                 const std::string& name, const of::AuthorityToken& token,
                                                 std::uint64_t ttl_ticks = 256);

/// Evidence that satisfies every blocking requirement of the fixture path.
[[nodiscard]] std::shared_ptr<SyntheticSource> attach_full_evidence(of::OpticalFabric& fabric,
                                                                   const std::string& instance);

[[nodiscard]] of::EvidenceRecord make_record(of::ResourceRef subject, of::EvidenceKind kind,
                                             of::EvidenceState state, const std::string& runtime,
                                             of::SourceId source, std::uint64_t sequence, of::Tick now,
                                             std::uint64_t validity_span);


/// A runtime, its topology, a SYNTHETIC evidence producer and the authority
/// token that governs the fixture site: everything a lifecycle test needs.
struct Session {
  std::unique_ptr<of::OpticalFabric> fabric;
  LineFixture fixture;
  std::shared_ptr<SyntheticSource> source;
  of::AuthorityToken token;
  std::string holder = "controller-a";
};

[[nodiscard]] std::unique_ptr<Session> make_session(const of::FabricOptions& options,
                                                   const std::string& prefix,
                                                   const std::string& holder = "controller-a");

/// Every resource the fixture path traverses, in canonical order.
[[nodiscard]] std::vector<of::ResourceRef> fixture_path_resources(const LineFixture& fixture);

/// Refreshes the SYNTHETIC producer so that every blocking requirement of the
/// fixture path is satisfied. Fails loudly when the producer cannot answer.
void satisfy_evidence(Session& session, std::uint64_t validity_span_ticks = 8192);

/// The evidence kinds the fixture producer answers for.
[[nodiscard]] std::vector<of::EvidenceKind> fixture_evidence_kinds();

}  // namespace of_test
