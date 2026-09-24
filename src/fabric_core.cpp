// Optical Fabric 1.0.0 - Summon Software Labs
// Runtime construction, recovery, persistence, registration and ingestion.
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "fabric_impl.hpp"
#include "state_codec.hpp"
#include "optical_fabric/version.hpp"

namespace optical_fabric {

namespace {

constexpr std::string_view kRuntimeItem = "runtime";
constexpr std::string_view kResourceItem = "resource";
constexpr std::string_view kEvidenceItem = "evidence";
constexpr std::string_view kConnectivityItem = "connectivity";
constexpr std::string_view kReservationItem = "reservation";
constexpr std::string_view kGrantItem = "grant";
constexpr std::string_view kAttemptItem = "attempt";
constexpr std::string_view kFenceItem = "fence";
constexpr std::string_view kResetItem = "reset";

[[nodiscard]] std::string encode_runtime_item(const detail::RuntimeState& state) {
  detail::TextWriter writer;
  detail::encode_runtime(writer, state);
  return writer.take();
}

[[nodiscard]] std::string encode_resource_item(const detail::ResourceState& state) {
  detail::TextWriter writer;
  detail::encode_resource(writer, state);
  return writer.take();
}

[[nodiscard]] std::string encode_evidence_item(const EvidenceRecord& record) {
  detail::TextWriter writer;
  detail::encode_evidence(writer, record);
  return writer.take();
}

[[nodiscard]] std::string encode_connectivity_item(const detail::ConnectivityRecord& record) {
  detail::TextWriter writer;
  detail::encode_connectivity(writer, record);
  return writer.take();
}

[[nodiscard]] std::string encode_attempt_item(const detail::AttemptRecord& record) {
  detail::TextWriter writer;
  detail::encode_attempt(writer, record);
  return writer.take();
}

[[nodiscard]] std::string encode_fence_item(const FenceRecord& record) {
  detail::TextWriter writer;
  detail::encode_fence(writer, record);
  return writer.take();
}

/// Structural equality of two registrations, used to make re-registration
/// idempotent without ever letting a changed definition reuse an identity.
[[nodiscard]] bool same_resource(const detail::ResourceState& lhs, const detail::ResourceState& rhs) {
  // Membership (the related list) is maintained by the runtime as children are
  // registered, so it is deliberately not part of the caller's definition.
  return lhs.resource == rhs.resource && lhs.name == rhs.name && lhs.sharing == rhs.sharing &&
         lhs.attributes == rhs.attributes && lhs.site == rhs.site &&
         lhs.node == rhs.node && lhs.endpoint_a == rhs.endpoint_a && lhs.endpoint_b == rhs.endpoint_b &&
         lhs.ingress == rhs.ingress && lhs.egress == rhs.egress && lhs.spans == rhs.spans &&
         lhs.endpoints == rhs.endpoints && lhs.role == rhs.role && lhs.region == rhs.region &&
         lhs.description == rhs.description && lhs.band == rhs.band &&
         lhs.channel_index == rhs.channel_index &&
         lhs.nominal_frequency_ghz == rhs.nominal_frequency_ghz &&
         lhs.declared_length_metres == rhs.declared_length_metres;
}

}  // namespace

detail::RuntimeState OpticalFabric::Impl::runtime_state() const {
  return runtime_state(generation, topology_generation);
}

detail::RuntimeState OpticalFabric::Impl::runtime_state(Generation next_generation,
                                                        Generation next_topology_generation) const {
  detail::RuntimeState state;
  state.epoch = epoch;
  state.tick = tick;
  state.generation = next_generation;
  state.topology_generation = next_topology_generation;
  state.boot_sequence = incarnation.boot_sequence;
  state.instance = incarnation.instance;
  state.host_label = incarnation.host_label;
  return state;
}

Digest128 OpticalFabric::Impl::digest_of(const std::string& canonical) {
  CanonicalHasher hasher;
  hasher.add_raw(canonical);
  return hasher.digest();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OpticalFabric::OpticalFabric(FabricOptions options) {
  const Status limits_status = options.limits.validate();
  if (!limits_status.ok()) {
    throw std::invalid_argument("optical fabric limits are invalid: " + limits_status.message);
  }
  const Limits limits = options.limits;
  impl_ = std::make_unique<Impl>(std::move(options), limits);
  const Status opened = impl_->open_and_recover();
  if (!opened.ok()) {
    // A refused store leaves the runtime closed rather than operating on state
    // it could not verify. The refusal is reported through recovery().
    impl_->closed = true;
  }
}

OpticalFabric::~OpticalFabric() {
  if (impl_) {
    impl_->store.close();
  }
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

Status OpticalFabric::Impl::open_and_recover() {
  const Status opened = store.open(recovery);
  if (!opened.ok()) {
    return opened;
  }
  const Status replayed = replay();
  if (!replayed.ok()) {
    recovery.status = RecoveryStatus::Rejected;
    recovery.detail = replayed.message;
    store.close();
    return replayed;
  }
  // A fresh store starts at boot sequence 1; a recovered store continues from
  // the persisted sequence, so a stale incarnation can always be recognised.
  // Zero is reserved for "never" across the runtime: a counter that has never
  // advanced still reads one, so "no generation" is never confused with "the
  // first generation".
  if (generation.is_nil()) {
    generation = Generation{1};
  }
  if (topology_generation.is_nil()) {
    topology_generation = Generation{1};
  }
  if (tick.value == 0) {
    tick = Tick{1};
  }
  const std::uint64_t previous = incarnation.boot_sequence;
  recovery.previous_boot_sequence = previous;
  incarnation.boot_sequence = previous + 1;
  fill_random_bytes(reinterpret_cast<std::uint8_t*>(&incarnation.instance.low), sizeof(std::uint64_t));
  fill_random_bytes(reinterpret_cast<std::uint8_t*>(&incarnation.instance.high), sizeof(std::uint64_t));
  incarnation.host_label = options.host_label;
  recovery.boot_sequence = incarnation.boot_sequence;
  const Status conservative = mark_recovery_conservative();
  if (!conservative.ok()) {
    recovery.status = RecoveryStatus::Rejected;
    recovery.detail = conservative.message;
    store.close();
    return conservative;
  }
  const Status checkpointed = write_checkpoint();
  if (!checkpointed.ok()) {
    return checkpointed;
  }
  return Status{};
}

Status OpticalFabric::Impl::replay() {
  for (const detail::StoreRecord& record : store.records()) {
    const Result<detail::DecodedCommit> decoded = detail::CommitBundle::decode(record.payload, limits);
    if (!decoded.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt,
                             "a persisted record could not be decoded: " + decoded.status.message);
    }
    const Status applied = apply_decoded(decoded.value);
    if (!applied.ok()) {
      return applied;
    }
    recovery.records_applied += 1;
    if (decoded.value.kind == RecordKind::Checkpoint) {
      recovery.checkpoints += 1;
    }
  }
  return Status{};
}

Status OpticalFabric::Impl::mark_recovery_conservative() {
  detail::CommitBundle bundle;
  const bool recovered = recovery.previous_boot_sequence != 0;

  // 1. Every grant from a previous incarnation is fenced explicitly, and the
  //    epoch advances so that a stale token fails on epoch as well as on
  //    incarnation.
  std::vector<GrantId> live;
  for (const auto& [id, grant] : grants) {
    if (!grant.view.fenced && !grant.view.released) {
      live.push_back(id);
    }
  }
  std::sort(live.begin(), live.end());
  if (recovered && !live.empty()) {
    epoch = epoch.next();
  }
  for (const GrantId id : live) {
    detail::GrantState& grant = grants[id];
    grant.view.fenced = true;
    grant.view.generation = grant.view.generation.next();
    grant.has_fence = true;
    grant.fence.grant = id;
    grant.fence.holder = grant.view.holder;
    grant.fence.fenced_epoch = grant.view.epoch;
    grant.fence.fencing_epoch = epoch;
    grant.fence.fenced_boot_sequence = grant.view.incarnation.boot_sequence;
    grant.fence.fencing_boot_sequence = incarnation.boot_sequence;
    grant.fence.scope = grant.view.scope;
    grant.fence.reason = recovered ? FenceReason::SupersededByNewerIncarnation : FenceReason::ExplicitOperatorFence;
    grant.fence.fenced_tick = tick;
    grant.fence.detail = "authority granted to a previous incarnation";
    fences.push_back(grant.fence);
    while (fences.size() > limits.max_fence_reasons_history) {
      fences.erase(fences.begin());
    }
    detail::TextWriter writer;
    detail::encode_grant(writer, grant);
    bundle.add(std::string(kGrantItem), writer.take());
    bundle.add(std::string(kFenceItem), encode_fence_item(grant.fence));
  }

  // 2. Objects that a previous incarnation left mid-flight are resolved
  //    conservatively: an uncommitted activation becomes FAILED, a withdrawal
  //    is completed, and live authority is unconfirmed until revalidated.
  for (auto& [id, record] : objects) {
    bool changed = false;
    if (record.state == ConnectivityState::Activating) {
      const ConnectivityState previous = record.state;
      record.state = ConnectivityState::Failed;
      record.generation = record.generation.next();
      // The pending activation died with the previous incarnation; leaving the
      // digest behind would let a stale commit message match it.
      record.activation_digest = Digest128{};
      record.last_refusal = RefusalCode::MissingEvidence;
      record.last_refusal_detail = "activation did not commit before the process ended";
      TransitionRecord transition;
      transition.from = previous;
      transition.to = record.state;
      transition.outcome = OutcomeCode::Applied;
      transition.refusal = RefusalCode::MissingEvidence;
      transition.generation = record.generation;
      transition.epoch = epoch;
      transition.tick = tick;
      transition.detail = record.last_refusal_detail;
      record.history.push_back(transition);
      transitions_total += 1;
      recovery.incomplete_activations += 1;
      changed = true;
    } else if (record.state == ConnectivityState::Withdrawing) {
      const ConnectivityState previous = record.state;
      record.state = ConnectivityState::Retired;
      record.generation = record.generation.next();
      record.retired_tick = tick;
      TransitionRecord transition;
      transition.from = previous;
      transition.to = record.state;
      transition.outcome = OutcomeCode::Applied;
      transition.generation = record.generation;
      transition.epoch = epoch;
      transition.tick = tick;
      transition.detail = "withdrawal resumed by recovery";
      record.history.push_back(transition);
      transitions_total += 1;
      recovery.resumed_withdrawals += 1;
      changed = true;
    } else if (is_committed_state(record.state) && record.authority_confirmed) {
      record.authority_confirmed = false;
      record.generation = record.generation.next();
      recovery.authority_unconfirmed += 1;
      changed = true;
    }
    if (changed) {
      detail::TextWriter writer;
      detail::encode_connectivity(writer, record);
      bundle.add(std::string(kConnectivityItem), writer.take());
    }
  }

  // 3. Every observation ingested by a previous incarnation stays in the store
  //    but is no longer fresh: the policy reports it as STALE until a current
  //    producer re-attests it.
  for (const auto& [id, record] : evidence) {
    if (record.provenance.ingested_incarnation.boot_sequence != incarnation.boot_sequence) {
      recovery.evidence_marked_stale += 1;
    }
    (void)id;
  }
  if (recovery.evidence_marked_stale > 0 || recovery.authority_unconfirmed > 0 ||
      recovery.incomplete_activations > 0 || recovery.resumed_withdrawals > 0) {
    recovery.records_applied += 1;
  }

  if (!recovered) {
    if (store.memory_only()) {
      recovery.status = RecoveryStatus::MemoryOnly;
      recovery.detail = "no store path was configured; state is in memory only";
    } else if (recovery.status != RecoveryStatus::TailDiscarded) {
      recovery.status = recovery.file_bytes == 0 ? RecoveryStatus::Created : RecoveryStatus::Clean;
      recovery.detail = recovery.file_bytes == 0 ? "the store was created" : "every record was intact";
    }
  }
  rebuild_indices();

  bundle.add(std::string(kRuntimeItem), encode_runtime_item(runtime_state()));
  if (bundle.size() > 0) {
    const Status committed = commit(bundle, RecordKind::RuntimeState);
    if (!committed.ok()) {
      return committed;
    }
  }
  return Status{};
}

void OpticalFabric::Impl::rebuild_indices() {
  claim_index.clear();
  reservation_index.clear();
  for (const auto& [id, record] : objects) {
    // A withdrawing object has already released its resources, and a retired or
    // refused one never held any; only a live committed path holds claims.
    if (!holds_committed_claims(record)) {
      continue;
    }
    for (const detail::ClaimEntry& claim : derive_claims_locked(record)) {
      claim_index[claim.resource].push_back(claim);
    }
    (void)id;
  }
  for (const auto& [id, reservation] : reservations) {
    if (reservation.view.released || reservation.view.consumed) {
      continue;
    }
    for (const ResourceRef resource : reservation.view.resources) {
      reservation_index[resource].push_back(id);
    }
  }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

Status OpticalFabric::Impl::commit(detail::CommitBundle& bundle, RecordKind kind) {
  if (persistence_failed) {
    return Status::failure(ErrorCode::StoreIo,
                           "the runtime refused a mutation after a persistence failure: " +
                               persistence_failure_detail);
  }
  const std::string payload = bundle.encode(kind);
  const Result<detail::DecodedCommit> decoded = detail::CommitBundle::decode(payload, limits);
  if (!decoded.ok()) {
    persistence_failed = true;
    persistence_failure_detail = "the runtime produced an undecodable commit: " + decoded.status.message;
    return Status::failure(ErrorCode::Internal, persistence_failure_detail);
  }
  const Status written = store.append(kind, payload);
  if (!written.ok()) {
    persistence_failed = true;
    persistence_failure_detail = written.message;
    note_diagnostic(Diagnostic{DiagnosticKind::InvariantViolation, RefusalCode::PersistenceFailure,
                               ResourceRef{}, ConnectivityId{}, ReservationId{}, GrantId{}, tick, epoch,
                               generation, generation, written.message});
    return written;
  }
  const Status applied = apply_decoded(decoded.value);
  if (!applied.ok()) {
    persistence_failed = true;
    persistence_failure_detail = applied.message;
    return applied;
  }
  if (should_compact()) {
    const Status compacted = write_checkpoint();
    if (!compacted.ok()) {
      return compacted;
    }
  }
  return Status{};
}

bool OpticalFabric::Impl::should_compact() const {
  return store.appended_bytes() >= static_cast<std::uint64_t>(limits.max_journal_bytes) ||
         store.appended_records() >= limits.max_journal_records;
}

/// Compaction is driven by growth since the last checkpoint, so a checkpoint
/// that is itself larger than the threshold does not cause a rewrite per commit.
Status OpticalFabric::Impl::write_checkpoint() {
  detail::CommitBundle bundle;
  bundle.add(std::string(kResetItem), std::string());
  bundle.add(std::string(kRuntimeItem), encode_runtime_item(runtime_state()));
  for (const auto& [id, state] : resources) {
    bundle.add(std::string(kResourceItem), encode_resource_item(state));
    (void)id;
  }
  for (const auto& [id, record] : evidence) {
    bundle.add(std::string(kEvidenceItem), encode_evidence_item(record));
    (void)id;
  }
  for (const auto& [id, record] : objects) {
    bundle.add(std::string(kConnectivityItem), encode_connectivity_item(record));
    (void)id;
  }
  for (const auto& [id, reservation] : reservations) {
    detail::TextWriter writer;
    detail::encode_reservation(writer, reservation);
    bundle.add(std::string(kReservationItem), writer.take());
    (void)id;
  }
  for (const auto& [id, grant] : grants) {
    detail::TextWriter writer;
    detail::encode_grant(writer, grant);
    bundle.add(std::string(kGrantItem), writer.take());
    (void)id;
  }
  for (const auto& [id, attempt] : attempts) {
    bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt));
    (void)id;
  }
  for (const FenceRecord& fence : fences) {
    bundle.add(std::string(kFenceItem), encode_fence_item(fence));
  }
  const std::string payload = bundle.encode(RecordKind::Checkpoint);
  const Status written = store.rewrite(RecordKind::Checkpoint, payload);
  if (!written.ok()) {
    persistence_failed = true;
    persistence_failure_detail = written.message;
    return written;
  }
  return Status{};
}

Status OpticalFabric::Impl::apply_decoded(const detail::DecodedCommit& decoded) {
  for (const detail::DecodedCommitItem& item : decoded.items) {
    detail::TextReader reader = item.fields;
    const Status applied = apply_item(item.type, reader);
    if (!applied.ok()) {
      return applied;
    }
    const Status finished = reader.finish();
    if (!finished.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, finished.message);
    }
  }
  return Status{};
}

Status OpticalFabric::Impl::apply_item(const std::string& type, detail::TextReader& reader) {
  if (type == kResetItem) {
    resources.clear();
    evidence.clear();
    objects.clear();
    reservations.clear();
    grants.clear();
    attempts.clear();
    attempt_order.clear();
    fences.clear();
    claim_index.clear();
    reservation_index.clear();
    transitions_total = 0;
    return Status{};
  }
  if (type == kRuntimeItem) {
    const Result<detail::RuntimeState> state = detail::decode_runtime(reader);
    if (!state.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, state.status.message);
    }
    epoch = state.value.epoch;
    tick = state.value.tick;
    generation = state.value.generation;
    topology_generation = state.value.topology_generation;
    incarnation.boot_sequence = state.value.boot_sequence;
    incarnation.instance = state.value.instance;
    incarnation.host_label = state.value.host_label;
    return Status{};
  }
  if (type == kResourceItem) {
    const Result<detail::ResourceState> state = detail::decode_resource(reader);
    if (!state.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, state.status.message);
    }
    resources[state.value.resource] = state.value;
    return Status{};
  }
  if (type == kEvidenceItem) {
    const Result<EvidenceRecord> record = detail::decode_evidence(reader);
    if (!record.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, record.status.message);
    }
    evidence[record.value.id] = record.value;
    return Status{};
  }
  if (type == kConnectivityItem) {
    const Result<detail::ConnectivityRecord> record = detail::decode_connectivity(reader, limits);
    if (!record.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, record.status.message);
    }
    detail::ConnectivityRecord stored = record.value;
    transitions_total += stored.history.size();
    objects[stored.id] = std::move(stored);
    rebuild_indices();
    return Status{};
  }
  if (type == kReservationItem) {
    const Result<detail::ReservationState> state = detail::decode_reservation(reader);
    if (!state.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, state.status.message);
    }
    reservations[state.value.view.id] = state.value;
    rebuild_indices();
    return Status{};
  }
  if (type == kGrantItem) {
    const Result<detail::GrantState> state = detail::decode_grant(reader);
    if (!state.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, state.status.message);
    }
    grants[state.value.view.id] = state.value;
    return Status{};
  }
  if (type == kAttemptItem) {
    const Result<detail::AttemptRecord> record = detail::decode_attempt(reader);
    if (!record.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, record.status.message);
    }
    const bool is_new = attempts.find(record.value.attempt) == attempts.end();
    attempts[record.value.attempt] = record.value;
    if (is_new) {
      attempt_order.push_back(record.value.attempt);
      while (attempt_order.size() > limits.max_attempt_history_total) {
        attempts.erase(attempt_order.front());
        attempt_order.pop_front();
      }
    }
    return Status{};
  }
  if (type == kFenceItem) {
    const Result<FenceRecord> record = detail::decode_fence(reader);
    if (!record.ok()) {
      return Status::failure(ErrorCode::StoreCorrupt, record.status.message);
    }
    fences.push_back(record.value);
    while (fences.size() > limits.max_fence_reasons_history) {
      fences.erase(fences.begin());
    }
    return Status{};
  }
  return Status::failure(ErrorCode::StoreCorrupt, "unknown commit item type: " + type);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const detail::ResourceState* OpticalFabric::Impl::find_resource(ResourceRef resource) const {
  const auto found = resources.find(resource);
  return found == resources.end() ? nullptr : &found->second;
}

std::string OpticalFabric::Impl::resource_name(ResourceRef resource) const {
  const detail::ResourceState* state = find_resource(resource);
  return state == nullptr ? resource.to_string() : state->name;
}

void OpticalFabric::Impl::note_diagnostic(Diagnostic diagnostic) {
  diagnostics.push_back(std::move(diagnostic));
  while (diagnostics.size() > limits.max_diagnostics) {
    diagnostics.pop_front();
    diagnostics_dropped += 1;
  }
}

const detail::AttemptRecord* OpticalFabric::Impl::find_attempt_locked(AttemptId attempt) const {
  const auto found = attempts.find(attempt);
  return found == attempts.end() ? nullptr : &found->second;
}

void OpticalFabric::Impl::record_attempt_locked(const detail::AttemptRecord& record) {
  detail::CommitBundle bundle;
  bundle.add(std::string(kAttemptItem), encode_attempt_item(record));
  const Status status = commit(bundle, RecordKind::AttemptRecorded);
  (void)status;
}

Result<detail::ConnectivityRecord> OpticalFabric::Impl::copy_object_locked(ConnectivityId id) const {
  const auto found = objects.find(id);
  if (found == objects.end()) {
    return Result<detail::ConnectivityRecord>::failure(ErrorCode::NotFound,
                                                       "no connectivity object has that identity");
  }
  return Result<detail::ConnectivityRecord>::success(found->second);
}

void OpticalFabric::Impl::push_transition_locked(detail::ConnectivityRecord& record, ConnectivityState to,
                                                 OutcomeCode outcome, RefusalCode refusal, AttemptId attempt,
                                                 const std::string& detail) {
  TransitionRecord transition;
  transition.from = record.state;
  transition.to = to;
  transition.outcome = outcome;
  transition.refusal = refusal;
  transition.attempt = attempt;
  transition.generation = record.generation.next();
  transition.epoch = epoch;
  transition.tick = tick;
  transition.detail = detail;
  record.history.push_back(transition);
  while (record.history.size() > limits.max_attempt_history_per_object) {
    record.history.erase(record.history.begin());
    record.transitions_dropped += 1;
  }
  transitions_total += 1;
}

// ---------------------------------------------------------------------------
// Runtime identity, clock and lifecycle
// ---------------------------------------------------------------------------

Incarnation OpticalFabric::incarnation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->incarnation;
}

Epoch OpticalFabric::current_epoch() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->epoch;
}

Tick OpticalFabric::current_tick() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->tick;
}

Generation OpticalFabric::generation() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->generation;
}

RecoveryReport OpticalFabric::recovery() const { return impl_->recovery; }

const Limits& OpticalFabric::limits() const { return impl_->limits; }

bool OpticalFabric::memory_only() const { return impl_->store.memory_only(); }

bool OpticalFabric::closed() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->closed;
}

Result<Tick> OpticalFabric::advance_tick(std::uint64_t delta) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Result<Tick>::failure(ErrorCode::Closed, "the runtime is closed");
  }
  if (delta == 0) {
    return Result<Tick>::success(impl_->tick);
  }
  impl_->tick = impl_->tick.advanced_by(delta);
  impl_->generation = impl_->generation.next();
  detail::CommitBundle bundle;
  detail::TextWriter writer;
  detail::encode_runtime(writer, detail::RuntimeState{impl_->epoch, impl_->tick, impl_->generation,
                                                      impl_->topology_generation,
                                                      impl_->incarnation.boot_sequence,
                                                      impl_->incarnation.instance,
                                                      impl_->incarnation.host_label});
  bundle.add("runtime", writer.take());
  const Status status = impl_->commit(bundle, RecordKind::RuntimeState);
  if (!status.ok()) {
    return Result<Tick>::failure(status.code, status.message);
  }
  return Result<Tick>::success(impl_->tick);
}

Status OpticalFabric::flush() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->store.flush();
}

Status OpticalFabric::close() {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Status{};
  }
  const Status flushed = impl_->store.flush();
  impl_->store.close();
  impl_->closed = true;
  return flushed;
}

// ---------------------------------------------------------------------------
// Topology registration
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] bool valid_name(const Limits& limits, std::string_view name, std::string& reason) {
  if (!Names::valid(name, limits.max_name_length)) {
    reason = std::string(Names::invalid_reason(name, limits.max_name_length));
    return false;
  }
  return true;
}

[[nodiscard]] std::string attribute(std::string_view key, std::string_view value) {
  std::string out;
  out.append(key);
  out.push_back('=');
  out.append(value);
  return out;
}

}  // namespace

RegistrationResult OpticalFabric::Impl::register_resource_locked(detail::ResourceState state,
                                                                std::size_t existing_of_kind,
                                                                std::size_t kind_limit,
                                                                ResourceRef owner) {
  RegistrationResult result;
  const auto found = resources.find(state.resource);
  if (found != resources.end()) {
    if (same_resource(found->second, state)) {
      result.outcome = TopologyOutcome::AlreadyRegistered;
      result.resource = state.resource;
      result.generation = found->second.generation;
      return result;
    }
    result.outcome = TopologyOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::IdentityCollision,
                                 "a different resource is already registered under the name '" +
                                     state.name + "'");
    return result;
  }
  if (existing_of_kind >= kind_limit) {
    result.outcome = TopologyOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded,
                                 "the configured capacity for this resource kind is exhausted");
    return result;
  }
  if (topology_generation.value + 1 == 0 || generation.value + 1 == 0) {
    result.outcome = TopologyOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded, "the generation counter is exhausted");
    return result;
  }
  state.generation = Generation{1};
  state.registered_tick = tick;

  const Generation next_topology = topology_generation.next();
  const Generation next_runtime = generation.next();
  detail::TextWriter resource_writer;
  detail::encode_resource(resource_writer, state);
  const detail::RuntimeState runtime = runtime_state(next_runtime, next_topology);

  detail::CommitBundle bundle;
  bundle.add(std::string(kResourceItem), resource_writer.take());
  // The containing resource's member list is part of the same commit, so a torn
  // write can never leave a registered member out of its owner.
  const auto owner_state = resources.find(owner);
  if (!owner.is_nil() && owner_state != resources.end() &&
      std::find(owner_state->second.related.begin(), owner_state->second.related.end(), state.resource) ==
          owner_state->second.related.end()) {
    detail::ResourceState updated = owner_state->second;
    updated.related.push_back(state.resource);
    // Membership is structural metadata, not a definition change: bumping the
    // owner generation here would stale every path that traverses it.
    detail::TextWriter owner_writer;
    detail::encode_resource(owner_writer, updated);
    bundle.add(std::string(kResourceItem), owner_writer.take());
  }
  bundle.add(std::string(kRuntimeItem), encode_runtime_item(runtime));
  const Status status = commit(bundle, RecordKind::TopologyRegistered);
  if (!status.ok()) {
    result.outcome = TopologyOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, status.message);
    return result;
  }
  result.outcome = TopologyOutcome::Registered;
  result.resource = state.resource;
  result.generation = state.generation;
  return result;
}

std::size_t OpticalFabric::Impl::count_of_kind(ResourceKind kind) const {
  std::size_t count = 0;
  for (const auto& [ref, state] : resources) {
    if (ref.kind == kind) {
      ++count;
    }
    (void)state;
  }
  return count;
}

RegistrationResult OpticalFabric::register_site(const SiteRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<SiteId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.region = registration.region;
  state.description = registration.description;
  if (!registration.region.empty()) {
    state.attributes.push_back(attribute("region", registration.region));
  }
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::Site),
                                         impl_->limits.max_sites, ResourceRef{});
}

RegistrationResult OpticalFabric::register_optical_node(const OpticalNodeRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  if (impl_->find_resource(as_ref(registration.site)) == nullptr) {
    result.refusal = Refusal::of(RefusalCode::UnknownResource, "the owning site is not registered");
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<OpticalNodeId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.site = registration.site;
  state.role = registration.role;
  state.related.push_back(as_ref(registration.site));
  if (!registration.role.empty()) {
    state.attributes.push_back(attribute("role", registration.role));
  }
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::OpticalNode),
                                         impl_->limits.max_optical_nodes, as_ref(registration.site));
}

RegistrationResult OpticalFabric::register_port(const PortRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  const detail::ResourceState* node = impl_->find_resource(as_ref(registration.node));
  if (node == nullptr) {
    result.refusal = Refusal::of(RefusalCode::UnknownResource, "the owning optical node is not registered");
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<PortId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.node = registration.node;
  state.site = node->site;
  state.role = registration.role;
  state.related.push_back(as_ref(registration.node));
  if (!registration.role.empty()) {
    state.attributes.push_back(attribute("role", registration.role));
  }
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::Port),
                                         impl_->limits.max_ports, as_ref(registration.node));
}

RegistrationResult OpticalFabric::register_span(const SpanRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  if (registration.endpoint_a == registration.endpoint_b) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument, "a span needs two distinct endpoints");
    return result;
  }
  const detail::ResourceState* endpoint_a = impl_->find_resource(as_ref(registration.endpoint_a));
  const detail::ResourceState* endpoint_b = impl_->find_resource(as_ref(registration.endpoint_b));
  if (endpoint_a == nullptr || endpoint_b == nullptr) {
    result.refusal = Refusal::of(RefusalCode::UnknownResource, "a span endpoint is not registered");
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<SpanId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.endpoint_a = registration.endpoint_a;
  state.endpoint_b = registration.endpoint_b;
  state.declared_length_metres = registration.declared_length_metres;
  state.related.push_back(as_ref(registration.endpoint_a));
  state.related.push_back(as_ref(registration.endpoint_b));
  state.attributes.push_back(attribute("declared_length_metres",
                                       std::to_string(registration.declared_length_metres)));
  // A span is listed as a member of both endpoints; the owner link below makes
  // the second endpoint's list complete in the same commit.
  RegistrationResult registered = impl_->register_resource_locked(
      state, impl_->count_of_kind(ResourceKind::Span), impl_->limits.max_spans,
      as_ref(registration.endpoint_a));
  if (registered.outcome == TopologyOutcome::Registered) {
    impl_->link_related_locked(as_ref(registration.endpoint_b), registered.resource);
  }
  return registered;
}

RegistrationResult OpticalFabric::register_line_system(const LineSystemRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  if (registration.spans.empty()) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument, "a line system needs at least one span");
    return result;
  }
  if (registration.spans.size() > impl_->limits.max_spans) {
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded, "the line system declares too many spans");
    return result;
  }
  for (const SpanId span : registration.spans) {
    const detail::ResourceState* state = impl_->find_resource(as_ref(span));
    if (state == nullptr || state->resource.kind != ResourceKind::Span) {
      result.refusal = Refusal::of(RefusalCode::UnknownResource, "a line-system span is not registered");
      return result;
    }
  }
  for (const PortId endpoint : registration.endpoints) {
    const detail::ResourceState* state = impl_->find_resource(as_ref(endpoint));
    if (state == nullptr || state->resource.kind != ResourceKind::Port) {
      result.refusal = Refusal::of(RefusalCode::UnknownResource, "a line-system endpoint is not a port");
      return result;
    }
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<LineSystemId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.spans = registration.spans;
  state.endpoints = registration.endpoints;
  for (const SpanId span : registration.spans) {
    state.related.push_back(as_ref(span));
  }
  for (const PortId endpoint : registration.endpoints) {
    state.related.push_back(as_ref(endpoint));
  }
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::LineSystem),
                                         impl_->limits.max_line_systems, ResourceRef{});
}

RegistrationResult OpticalFabric::register_cross_connect(const CrossConnectRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  if (registration.ingress == registration.egress) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument,
                                 "a cross connect needs two distinct ports");
    return result;
  }
  const detail::ResourceState* node = impl_->find_resource(as_ref(registration.node));
  if (node == nullptr || node->resource.kind != ResourceKind::OpticalNode) {
    result.refusal = Refusal::of(RefusalCode::UnknownResource, "the cross-connect node is not registered");
    return result;
  }
  const detail::ResourceState* ingress = impl_->find_resource(as_ref(registration.ingress));
  const detail::ResourceState* egress = impl_->find_resource(as_ref(registration.egress));
  if (ingress == nullptr || egress == nullptr) {
    result.refusal = Refusal::of(RefusalCode::UnknownResource, "a cross-connect port is not registered");
    return result;
  }
  if (!(ingress->node == registration.node) || !(egress->node == registration.node)) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument,
                                 "both cross-connect ports must belong to the named optical node");
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<CrossConnectId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.node = registration.node;
  state.ingress = registration.ingress;
  state.egress = registration.egress;
  state.related.push_back(as_ref(registration.node));
  state.related.push_back(as_ref(registration.ingress));
  state.related.push_back(as_ref(registration.egress));
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::CrossConnect),
                                         impl_->limits.max_cross_connects, as_ref(registration.node));
}

RegistrationResult OpticalFabric::register_channel(const ChannelRegistration& registration) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  RegistrationResult result;
  std::string reason;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (!valid_name(impl_->limits, registration.name, reason)) {
    result.refusal = Refusal::of(RefusalCode::NameInvalid, reason);
    return result;
  }
  detail::ResourceState state;
  state.resource = as_ref(derive_id<ChannelId>(registration.name));
  state.name = registration.name;
  state.sharing = registration.sharing;
  state.channel_index = registration.channel_index;
  state.nominal_frequency_ghz = registration.nominal_frequency_ghz;
  state.band = registration.band;
  state.attributes.push_back(attribute("index", std::to_string(registration.channel_index)));
  state.attributes.push_back(attribute("nominal_frequency_ghz",
                                       std::to_string(registration.nominal_frequency_ghz)));
  if (!registration.band.empty()) {
    state.attributes.push_back(attribute("band", registration.band));
  }
  return impl_->register_resource_locked(state, impl_->count_of_kind(ResourceKind::Channel),
                                         impl_->limits.max_channels, ResourceRef{});
}

void OpticalFabric::Impl::link_related_locked(ResourceRef owner, ResourceRef member) {
  const auto found = resources.find(owner);
  if (found == resources.end()) {
    return;
  }
  if (std::find(found->second.related.begin(), found->second.related.end(), member) !=
      found->second.related.end()) {
    return;
  }
  found->second.related.push_back(member);
  detail::TextWriter writer;
  detail::encode_resource(writer, found->second);
  detail::CommitBundle bundle;
  bundle.add(std::string(kResourceItem), writer.take());
  const Status status = commit(bundle, RecordKind::TopologyRegistered);
  (void)status;
}

// ---------------------------------------------------------------------------
// Evidence ingestion
// ---------------------------------------------------------------------------

EvidenceId OpticalFabric::Impl::derive_evidence_id(const EvidenceRecord& record) const {
  std::string canonical;
  canonical.append(record.subject.to_string());
  canonical.push_back('|');
  canonical.append(to_string(record.kind));
  canonical.push_back('|');
  canonical.append(record.provenance.source_id.to_string());
  canonical.push_back('|');
  canonical.append(std::to_string(record.provenance.source_sequence));
  return EvidenceId::from_value(Names::derive_composite("evidence", canonical));
}

Result<EvidenceId> OpticalFabric::Impl::ingest_evidence_locked(EvidenceRecord record) {
  if (record.subject.is_nil()) {
    return Result<EvidenceId>::failure(ErrorCode::InvalidArgument, "an observation needs a subject");
  }
  if (find_resource(record.subject) == nullptr) {
    return Result<EvidenceId>::failure(ErrorCode::NotFound,
                                       "the observation subject is not a registered resource");
  }
  if (record.provenance.source_runtime.empty()) {
    return Result<EvidenceId>::failure(ErrorCode::InvalidArgument,
                                       "an observation must name the runtime that produced it");
  }
  if (record.detail.size() > limits.max_evidence_detail_length) {
    return Result<EvidenceId>::failure(ErrorCode::CapacityExceeded,
                                       "the observation detail exceeds the configured bound");
  }
  if (record.provenance.valid_until_tick.value <= record.provenance.observed_tick.value) {
    return Result<EvidenceId>::failure(ErrorCode::InvalidArgument,
                                       "an observation must carry a validity window that ends after it");
  }
  const detail::ResourceState* subject = find_resource(record.subject);
  if (record.provenance.observed_generation.is_nil()) {
    record.provenance.observed_generation = subject->generation;
  }
  record.provenance.ingested_incarnation = incarnation;
  record.id = derive_evidence_id(record);

  const auto existing = evidence.find(record.id);
  if (existing != evidence.end()) {
    if (existing->second.provenance.content_digest == record.provenance.content_digest) {
      return Result<EvidenceId>::success(record.id);
    }
    return Result<EvidenceId>::failure(
        ErrorCode::ConflictingEvidence,
        "the same producer sequence was re-used with different content for this subject and kind");
  }
  // A producer that goes backwards is refused: an older observation must never
  // replace a newer one.
  for (const auto& [id, stored] : evidence) {
    (void)id;
    if (stored.subject == record.subject && stored.kind == record.kind &&
        stored.provenance.source_id == record.provenance.source_id &&
        stored.provenance.source_sequence > record.provenance.source_sequence) {
      return Result<EvidenceId>::failure(
          ErrorCode::StaleEvidence,
          "a newer observation from this producer is already recorded for this subject and kind");
    }
  }
  if (evidence.size() >= limits.max_evidence_records) {
    return Result<EvidenceId>::failure(ErrorCode::CapacityExceeded,
                                       "the configured evidence capacity is exhausted");
  }
  detail::TextWriter writer;
  detail::encode_evidence(writer, record);
  detail::CommitBundle bundle;
  bundle.add(std::string(kEvidenceItem), writer.take());
  bundle.add(std::string(kRuntimeItem),
             encode_runtime_item(runtime_state(generation.next(), topology_generation)));
  const Status status = commit(bundle, RecordKind::EvidenceIngested);
  if (!status.ok()) {
    return Result<EvidenceId>::failure(status.code, status.message);
  }
  return Result<EvidenceId>::success(record.id);
}

Result<EvidenceId> OpticalFabric::ingest_evidence(const EvidenceRecord& record) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Result<EvidenceId>::failure(ErrorCode::Closed, "the runtime is closed");
  }
  return impl_->ingest_evidence_locked(record);
}

Result<std::size_t> OpticalFabric::ingest_bundle(const EvidenceBundle& bundle) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    return Result<std::size_t>::failure(ErrorCode::Closed, "the runtime is closed");
  }
  if (bundle.records.empty()) {
    return Result<std::size_t>::success(0);
  }
  std::size_t ingested = 0;
  for (const EvidenceRecord& incoming : bundle.records) {
    EvidenceRecord record = incoming;
    if (record.provenance.source_id.is_nil() && !bundle.source_id.is_nil()) {
      record.provenance.source_id = bundle.source_id;
    }
    if (record.provenance.source_runtime.empty()) {
      record.provenance.source_runtime = bundle.source_runtime;
    }
    if (record.provenance.source_instance.empty()) {
      record.provenance.source_instance = bundle.source_instance;
    }
    const Result<EvidenceId> result = impl_->ingest_evidence_locked(record);
    if (!result.ok()) {
      return Result<std::size_t>::failure(result.status.code,
                                          "observation " + std::to_string(ingested) + " of the bundle: " +
                                              result.status.message);
    }
    ingested += 1;
  }
  return Result<std::size_t>::success(ingested);
}

Result<EvidenceRecord> OpticalFabric::describe_evidence(EvidenceId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->evidence.find(id);
  if (found == impl_->evidence.end()) {
    return Result<EvidenceRecord>::failure(ErrorCode::NotFound, "no observation has that identity");
  }
  return Result<EvidenceRecord>::success(found->second);
}

void OpticalFabric::register_evidence_source(std::shared_ptr<IEvidenceSource> source) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (!source) {
    return;
  }
  detail::SourceRegistration registration;
  registration.descriptor = source->describe();
  registration.source = std::move(source);
  if (impl_->sources.size() >= impl_->limits.max_evidence_sources) {
    impl_->note_diagnostic(Diagnostic{DiagnosticKind::CapacityPressure, RefusalCode::CapacityExceeded,
                                      ResourceRef{}, ConnectivityId{}, ReservationId{}, GrantId{},
                                      impl_->tick, impl_->epoch, impl_->generation, impl_->generation,
                                      "the evidence source registry is full"});
    return;
  }
  impl_->sources[registration.descriptor.id] = std::move(registration);
}

void OpticalFabric::attach_planner(std::shared_ptr<IPlannerPort> planner) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->planner = std::move(planner);
}

Result<std::size_t> OpticalFabric::refresh_from_source(SourceId source, const std::vector<ResourceRef>& subjects,
                                                       const std::vector<EvidenceKind>& kinds,
                                                       std::uint64_t validity_span_ticks) {
  std::shared_ptr<IEvidenceSource> producer;
  Tick now;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->closed) {
      return Result<std::size_t>::failure(ErrorCode::Closed, "the runtime is closed");
    }
    const auto found = impl_->sources.find(source);
    if (found == impl_->sources.end()) {
      return Result<std::size_t>::failure(ErrorCode::NotFound, "no evidence source is registered");
    }
    producer = found->second.source;
    now = impl_->tick;
  }
  if (!producer) {
    return Result<std::size_t>::failure(ErrorCode::NotFound, "the evidence source has no implementation");
  }
  // The producer runs outside the lock: it may take as long as it likes and may
  // not re-enter the runtime.
  EvidencePollRequest request;
  request.now = now;
  request.validity_span = Tick{validity_span_ticks};
  request.subjects = subjects;
  request.kinds = kinds;
  const Result<EvidenceBundle> bundle = producer->poll(request);
  if (!bundle.ok()) {
    return Result<std::size_t>::failure(bundle.status.code, bundle.status.message);
  }
  if (bundle.value.records.size() > impl_->limits.max_evidence_records) {
    return Result<std::size_t>::failure(ErrorCode::CapacityExceeded,
                                        "the producer returned more observations than the bound allows");
  }
  EvidenceBundle filled = bundle.value;
  if (filled.source_id.is_nil()) {
    filled.source_id = source;
  }
  const SourceDescriptor descriptor = producer->describe();
  if (filled.source_runtime.empty()) {
    filled.source_runtime = descriptor.runtime;
  }
  if (filled.source_instance.empty()) {
    filled.source_instance = descriptor.instance;
  }
  return ingest_bundle(filled);
}

}  // namespace optical_fabric

