// Optical Fabric 1.0.0 - Summon Software Labs
// The runtime: governed optical connectivity and optical path state as an
// explicit control-plane boundary.
//
// Hardware posture: this runtime does not drive photonic hardware. It has no
// ROADM, coherent-optics, transceiver-programming, wavelength-switching or
// optical-power-telemetry integration, and it does not claim one. Every optical
// device and signal it reasons about is SYNTHETIC unless a deployment attaches
// a real producer through IEvidenceSource and labels it accordingly.
//
// Locking discipline (enforced by construction, audited by the test suite):
//   * one mutex guards all authoritative state; it is never held across a call
//     into caller code, a producer, a planner or a transport;
//   * producers and planners are invoked outside the lock, and their results
//     are re-validated under the lock before they are applied;
//   * the persistence layer has its own mutex which is always acquired after
//     the state mutex and never calls back into the runtime.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "optical_fabric/activation.hpp"
#include "optical_fabric/authority.hpp"
#include "optical_fabric/diagnostics.hpp"
#include "optical_fabric/error.hpp"
#include "optical_fabric/evidence.hpp"
#include "optical_fabric/explanation.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/intent.hpp"
#include "optical_fabric/interfaces.hpp"
#include "optical_fabric/limits.hpp"
#include "optical_fabric/persistence.hpp"
#include "optical_fabric/report.hpp"
#include "optical_fabric/reservation.hpp"
#include "optical_fabric/topology.hpp"

namespace optical_fabric {

struct FabricOptions {
  /// Empty means memory-only: no durability, no recovery, no cross-process
  /// exclusion. Durable operation requires a path.
  std::filesystem::path store_path;
  SalvagePolicy salvage = SalvagePolicy::DiscardTail;
  bool durable_writes = true;
  bool exclusive_lock = true;
  /// Diagnostics only; never identity, never authority.
  std::string host_label;
  Limits limits{};
  EvidencePolicy evidence_policy{};
  /// First tick when a store is created. A recovered store continues from the
  /// persisted tick so that lease expiries keep their absolute meaning.
  Tick initial_tick{1};
  /// Maximum leases a single authority grant may request.
  std::uint64_t max_lease_ticks = 1u << 20;
};

class OpticalFabric {
 public:
  explicit OpticalFabric(FabricOptions options);
  ~OpticalFabric();

  OpticalFabric(const OpticalFabric&) = delete;
  OpticalFabric& operator=(const OpticalFabric&) = delete;
  OpticalFabric(OpticalFabric&&) = delete;
  OpticalFabric& operator=(OpticalFabric&&) = delete;

  // ---- runtime identity and time -----------------------------------------

  [[nodiscard]] Incarnation incarnation() const;
  [[nodiscard]] Epoch current_epoch() const;
  [[nodiscard]] Tick current_tick() const;
  [[nodiscard]] Generation generation() const;
  [[nodiscard]] RecoveryReport recovery() const;
  [[nodiscard]] const Limits& limits() const;
  [[nodiscard]] bool memory_only() const;
  [[nodiscard]] bool closed() const;

  /// Advances the logical clock. Ticks never move backwards; an attempt to do
  /// so is refused.
  [[nodiscard]] Result<Tick> advance_tick(std::uint64_t delta);
  /// Commits every pending durable record and flushes to stable storage.
  [[nodiscard]] Status flush();
  /// Stops accepting new work, flushes, and releases the writer lock. Requests
  /// after close are refused with ErrorCode::Closed.
  [[nodiscard]] Status close();

  // ---- registration ------------------------------------------------------

  [[nodiscard]] RegistrationResult register_site(const SiteRegistration& registration);
  [[nodiscard]] RegistrationResult register_optical_node(const OpticalNodeRegistration& registration);
  [[nodiscard]] RegistrationResult register_port(const PortRegistration& registration);
  [[nodiscard]] RegistrationResult register_span(const SpanRegistration& registration);
  [[nodiscard]] RegistrationResult register_line_system(const LineSystemRegistration& registration);
  [[nodiscard]] RegistrationResult register_cross_connect(const CrossConnectRegistration& registration);
  [[nodiscard]] RegistrationResult register_channel(const ChannelRegistration& registration);

  [[nodiscard]] TopologySnapshot topology() const;
  [[nodiscard]] Result<ResourceView> describe_resource(ResourceRef resource) const;

  // ---- evidence ----------------------------------------------------------

  void register_evidence_source(std::shared_ptr<IEvidenceSource> source);
  void attach_planner(std::shared_ptr<IPlannerPort> planner);

  [[nodiscard]] Result<EvidenceId> ingest_evidence(const EvidenceRecord& record);
  [[nodiscard]] Result<std::size_t> ingest_bundle(const EvidenceBundle& bundle);

  /// Polls a registered producer outside the lock, validates the answer and
  /// ingests it. Never invokes the producer while holding runtime state.
  [[nodiscard]] Result<std::size_t> refresh_from_source(SourceId source, const std::vector<ResourceRef>& subjects,
                                                        const std::vector<EvidenceKind>& kinds,
                                                        std::uint64_t validity_span_ticks);

  [[nodiscard]] EvidenceEvaluation evaluate_requirement(const EvidenceRequirement& requirement) const;
  [[nodiscard]] Result<EvidenceRecord> describe_evidence(EvidenceId id) const;

  // ---- authority ---------------------------------------------------------

  [[nodiscard]] AuthorityResult acquire_authority(const AuthorityRequest& request);
  [[nodiscard]] AuthorityResult renew_authority(const AuthorityRequest& request);
  [[nodiscard]] FenceResult fence(const FenceRequest& request);
  [[nodiscard]] AuthorityCheck check_authority(const AuthorityToken& token) const;
  [[nodiscard]] Result<AuthorityGrantView> describe_grant(GrantId grant) const;
  [[nodiscard]] std::vector<AuthorityView> authority_grants() const;

  // ---- connectivity ------------------------------------------------------

  [[nodiscard]] IntentSubmission submit_intent(const ConnectivityIntent& intent);
  /// Resolves and canonicalizes an intent without creating or changing
  /// anything. Used by callers and by the inspection utility.
  [[nodiscard]] Result<CanonicalPath> preview_intent(const ConnectivityIntent& intent);
  [[nodiscard]] Result<ConnectivityView> describe_connectivity(ConnectivityId id) const;

  [[nodiscard]] ValidationResult validate(const ValidateRequest& request);
  [[nodiscard]] ReservationResult reserve(const ReservationRequest& request);
  [[nodiscard]] ReservationResult renew_reservation(const ReservationRenewal& request);
  [[nodiscard]] ReleaseResult release_reservation(const ReleaseRequest& request);
  [[nodiscard]] ActivationResult begin_activation(const ActivationRequest& request);
  [[nodiscard]] ActivationResult commit_activation(const CommitRequest& request);
  [[nodiscard]] WithdrawalResult withdraw(const WithdrawalRequest& request);
  [[nodiscard]] HealthResult report_degraded(const HealthReport& report);
  [[nodiscard]] HealthResult report_failed(const HealthReport& report);
  [[nodiscard]] RevalidationResult revalidate(const RevalidationRequest& request);

  // ---- queries -----------------------------------------------------------

  [[nodiscard]] ActivePathReport active_paths() const;
  [[nodiscard]] Result<PathExplanation> explain(ConnectivityId id) const;
  [[nodiscard]] DiagnosticsReport diagnose(const DiagnosticQuery& query) const;
  [[nodiscard]] ClaimReport claims() const;
  [[nodiscard]] AccountingReport accounting() const;
  [[nodiscard]] InvariantReport verify_invariants() const;
  [[nodiscard]] FabricSnapshot snapshot() const;
  [[nodiscard]] std::vector<ConnectivityView> connectivity_objects() const;

 private:
  struct Impl;

  /// Resolves an intent to a path: declared-route resolution first, then the
  /// attached planner if one is present. The planner is called without the
  /// runtime lock held and its proposal is validated against the registry.
  [[nodiscard]] Result<PathDescriptor> resolve_intent_path(const ConnectivityIntent& intent,
                                                          std::string& origin);
  [[nodiscard]] HealthResult apply_health(const HealthReport& report, ConnectivityState target,
                                          const char* operation);

  std::unique_ptr<Impl> impl_;
};

/// Canonical one-line rendering of a connectivity identity and its path, used
/// by the CLI and by tests. Stable across processes.
[[nodiscard]] std::string render_path_line(const CanonicalPath& path);

}  // namespace optical_fabric
