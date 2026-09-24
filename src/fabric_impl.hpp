// Optical Fabric 1.0.0 - Summon Software Labs
// Runtime internals.
//
// Locking: exactly one mutex guards every field below. It is never held across
// a call into caller-supplied code (evidence producers, planners), never held
// across a socket operation, and never acquired twice. The store has its own
// mutex, always acquired after this one and never calling back into the runtime.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "optical_fabric/fabric.hpp"
#include "state.hpp"
#include "store.hpp"

namespace optical_fabric {

struct OpticalFabric::Impl {
  Impl(FabricOptions fabric_options, const Limits& validated_limits)
      : options(std::move(fabric_options)),
        limits(validated_limits),
        policy(options.evidence_policy),
        store(options.store_path, options.salvage, options.durable_writes, options.exclusive_lock,
              validated_limits) {
    // Tick zero means "never": it is the sentinel for an object that has not
    // been activated and for a reservation that was never granted. The logical
    // clock therefore starts at one, and a caller cannot configure it to zero.
    tick = options.initial_tick.value == 0 ? Tick{1} : options.initial_tick;
  }

  FabricOptions options;
  Limits limits;
  EvidencePolicy policy;

  mutable std::mutex mutex;

  bool closed = false;
  bool persistence_failed = false;
  std::string persistence_failure_detail;
  RecoveryReport recovery;

  Incarnation incarnation;
  Epoch epoch;
  Tick tick;
  Generation generation;
  Generation topology_generation;

  detail::Store store;

  std::map<ResourceRef, detail::ResourceState> resources;
  std::map<EvidenceId, EvidenceRecord> evidence;
  std::map<ConnectivityId, detail::ConnectivityRecord> objects;
  std::map<ReservationId, detail::ReservationState> reservations;
  std::map<GrantId, detail::GrantState> grants;
  std::map<AttemptId, detail::AttemptRecord> attempts;
  std::deque<AttemptId> attempt_order;
  std::vector<FenceRecord> fences;
  std::deque<Diagnostic> diagnostics;
  std::size_t diagnostics_dropped = 0;
  std::map<SourceId, detail::SourceRegistration> sources;
  std::shared_ptr<IPlannerPort> planner;
  std::map<ControllerId, std::string> controllers;

  /// Derived indices. They are always rebuilt from the authoritative records on
  /// apply, so they can never drift from them.
  std::map<ResourceRef, std::vector<detail::ClaimEntry>> claim_index;
  std::map<ResourceRef, std::vector<ReservationId>> reservation_index;
  std::size_t transitions_total = 0;

  // ---- persistence -------------------------------------------------------

  /// Persists and applies one commit. On a write failure the runtime stops
  /// accepting mutations rather than diverging from its durable state.
  Status commit(detail::CommitBundle& bundle, RecordKind kind);
  RegistrationResult register_resource_locked(detail::ResourceState state, std::size_t existing_of_kind,
                                              std::size_t kind_limit, ResourceRef owner);
  [[nodiscard]] std::size_t count_of_kind(ResourceKind kind) const;
  void link_related_locked(ResourceRef owner, ResourceRef member);
  [[nodiscard]] EvidenceId derive_evidence_id(const EvidenceRecord& record) const;
  Result<EvidenceId> ingest_evidence_locked(EvidenceRecord record);
  Status apply_decoded(const detail::DecodedCommit& commit);
  Status apply_item(const std::string& type, detail::TextReader& reader);
  void rebuild_indices();
  Status write_checkpoint();
  [[nodiscard]] bool should_compact() const;

  // ---- recovery ----------------------------------------------------------

  Status open_and_recover();
  Status replay();
  Status mark_recovery_conservative();

  // ---- helpers -----------------------------------------------------------

  [[nodiscard]] const detail::ResourceState* find_resource(ResourceRef resource) const;
  [[nodiscard]] std::string resource_name(ResourceRef resource) const;
  void note_diagnostic(Diagnostic diagnostic);
  [[nodiscard]] Generation next_generation() { return generation = generation.next(); }

  /// The runtime record describing this process as it is now, optionally with
  /// the generations a commit is about to install.
  [[nodiscard]] detail::RuntimeState runtime_state() const;
  [[nodiscard]] detail::RuntimeState runtime_state(Generation next_generation,
                                                   Generation next_topology_generation) const;

  [[nodiscard]] AuthorityCheck check_authority_locked(const AuthorityToken& token) const;
  [[nodiscard]] RefusalCode require_authority_locked(const AuthorityToken& token,
                                                     const AuthorityScope& required,
                                                     std::string& detail) const;
  [[nodiscard]] AuthorityScope object_scope_locked(const detail::ConnectivityRecord& record) const;
  [[nodiscard]] bool has_overlapping_newer_epoch(const AuthorityScope& scope, Epoch epoch) const;

  [[nodiscard]] std::vector<EvidenceRequirement> requirements_for(const PathDescriptor& path,
                                                                 const IntentRequirements& requirements) const;
  [[nodiscard]] EvidenceEvaluation evaluate_requirement_locked(const EvidenceRequirement& requirement) const;
  [[nodiscard]] EvidenceAssessment assess_locked(const PathDescriptor& path,
                                                const IntentRequirements& requirements) const;
  [[nodiscard]] RefusalCode refusal_for_assessment(const EvidenceAssessment& assessment,
                                                   std::string& detail) const;

  [[nodiscard]] Result<PathDescriptor> resolve_declared_route_locked(const ConnectivityIntent& intent,
                                                                    std::string& origin) const;
  [[nodiscard]] Result<PathDescriptor> describe_route_locked(const std::vector<ResourceRef>& resources,
                                                            ChannelId channel, Direction direction,
                                                            std::string origin) const;

  struct Conflict {
    bool conflicting = false;
    ConnectivityId other_object{};
    ReservationId other_reservation{};
    ResourceRef resource{};
    std::string detail;
  };
  /// Checks a candidate claim set against live reservations, committed claims
  /// and the object's own existing claims.
  [[nodiscard]] Conflict check_claims_locked(const std::vector<ResourceRef>& resources, ChannelId channel,
                                             ConnectivityId self, ReservationId own_reservation) const;
  [[nodiscard]] std::vector<detail::ClaimEntry> derive_claims_locked(
      const detail::ConnectivityRecord& record) const;
  /// The single definition of "this object currently holds committed claims".
  [[nodiscard]] static bool holds_committed_claims(const detail::ConnectivityRecord& record) noexcept;

  void record_attempt_locked(const detail::AttemptRecord& record);
  [[nodiscard]] const detail::AttemptRecord* find_attempt_locked(AttemptId attempt) const;

  [[nodiscard]] Result<detail::ConnectivityRecord> copy_object_locked(ConnectivityId id) const;
  void push_transition_locked(detail::ConnectivityRecord& record, ConnectivityState to, OutcomeCode outcome,
                              RefusalCode refusal, AttemptId attempt, const std::string& detail);

  [[nodiscard]] static Digest128 digest_of(const std::string& canonical);
};

/// Canonical digests of the mutating requests. They are computed from the
/// request content only, so an attempt identifier reused with different content
/// is detectable.
[[nodiscard]] Digest128 digest_validate_request(const ValidateRequest& request);
[[nodiscard]] Digest128 digest_reserve_request(const ReservationRequest& request);
[[nodiscard]] Digest128 digest_renew_request(const ReservationRenewal& request);
[[nodiscard]] Digest128 digest_release_request(const ReleaseRequest& request);
[[nodiscard]] Digest128 digest_activation_request(const ActivationRequest& request);
[[nodiscard]] Digest128 digest_commit_request(const CommitRequest& request);
[[nodiscard]] Digest128 digest_withdrawal_request(const WithdrawalRequest& request);
[[nodiscard]] Digest128 digest_health_request(const HealthReport& report);
[[nodiscard]] Digest128 digest_revalidation_request(const RevalidationRequest& request);
[[nodiscard]] Digest128 digest_authority_request(const AuthorityRequest& request);
[[nodiscard]] Digest128 digest_fence_request(const FenceRequest& request);

}  // namespace optical_fabric
