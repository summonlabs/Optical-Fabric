// Optical Fabric 1.0.0 - Summon Software Labs
// Connectivity intent submission and the governed lifecycle.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "fabric_impl.hpp"
#include "state_codec.hpp"

namespace optical_fabric {

namespace {

constexpr std::string_view kConnectivityItem = "connectivity";
constexpr std::string_view kReservationItem = "reservation";
constexpr std::string_view kAttemptItem = "attempt";
constexpr std::string_view kRuntimeItem = "runtime";

[[nodiscard]] std::string encode_connectivity_item(const detail::ConnectivityRecord& record) {
  detail::TextWriter writer;
  detail::encode_connectivity(writer, record);
  return writer.take();
}

[[nodiscard]] std::string encode_reservation_item(const detail::ReservationState& state) {
  detail::TextWriter writer;
  detail::encode_reservation(writer, state);
  return writer.take();
}

[[nodiscard]] std::string encode_attempt_item(const detail::AttemptRecord& record) {
  detail::TextWriter writer;
  detail::encode_attempt(writer, record);
  return writer.take();
}

[[nodiscard]] std::string encode_runtime_item(const detail::RuntimeState& state) {
  detail::TextWriter writer;
  detail::encode_runtime(writer, state);
  return writer.take();
}

[[nodiscard]] Digest128 activation_digest(ConnectivityId id, Generation generation, AttemptId attempt,
                                          Epoch epoch, const Digest128& path, EvidenceState aggregate) {
  CanonicalHasher hasher;
  hasher.add_field("op", "activation");
  hasher.add_field("connectivity", id.value());
  hasher.add_field("generation", generation.value);
  hasher.add_field("attempt", attempt.value());
  hasher.add_field("epoch", epoch.value);
  hasher.add_field("path", path.to_string());
  hasher.add_field("evidence", to_string(aggregate));
  return hasher.digest();
}

/// Reservation identity is derived from logical state (the object, its
/// generation and the tick it was granted at), never from caller entropy. The
/// attempt record is what makes a replay idempotent, so identity does not need
/// to carry the attempt.
[[nodiscard]] ReservationId derive_reservation_id(ConnectivityId connectivity, Generation generation,
                                                  Tick granted_tick) {
  std::string canonical = connectivity.to_string();
  canonical.push_back('|');
  canonical.append(generation.to_string());
  canonical.push_back('|');
  canonical.append(granted_tick.to_string());
  return ReservationId::from_value(Names::derive_composite("reservation", canonical));
}

}  // namespace

// ---------------------------------------------------------------------------
// Intent submission
// ---------------------------------------------------------------------------

Result<PathDescriptor> OpticalFabric::resolve_intent_path(const ConnectivityIntent& intent,
                                                          std::string& origin) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  Result<PathDescriptor> declared = impl_->resolve_declared_route_locked(intent, origin);
  if (declared.ok() || impl_->closed) {
    return declared;
  }
  std::shared_ptr<IPlannerPort> planner = impl_->planner;
  if (!planner) {
    return declared;
  }
  const Generation topology = impl_->topology_generation;
  // The planner is called without the runtime lock held. Its proposal is
  // validated against the registry afterwards, so a topology change during the
  // call cannot smuggle an unregistered resource into a path.
  RouteRequest request;
  request.source = intent.source_port;
  request.destination = intent.destination_port;
  request.channel = intent.channel;
  request.direction = intent.direction;
  request.topology_generation = topology;
  lock.unlock();
  const Result<RouteProposal> proposal = planner->propose_route(request);
  lock.lock();
  if (!proposal.ok()) {
    return Result<PathDescriptor>::failure(proposal.status.code,
                                           "the attached planner refused to propose a route: " +
                                               proposal.status.message);
  }
  std::string route_origin = proposal.value.origin.empty() ? std::string("planner") : proposal.value.origin;
  Result<PathDescriptor> described = impl_->describe_route_locked(proposal.value.resources,
                                                                  intent.channel, intent.direction,
                                                                  route_origin);
  if (!described.ok()) {
    return described;
  }
  if (!(described.value.segments.front().resource == as_ref(intent.source_port)) ||
      !(described.value.segments.back().resource == as_ref(intent.destination_port))) {
    return Result<PathDescriptor>::failure(
        ErrorCode::InvalidArgument, "the proposed route does not connect the requested endpoint ports");
  }
  origin = route_origin;
  return described;
}

Result<CanonicalPath> OpticalFabric::preview_intent(const ConnectivityIntent& intent) {
  std::string origin;
  const Result<PathDescriptor> path = resolve_intent_path(intent, origin);
  if (!path.ok()) {
    return Result<CanonicalPath>::failure(path.status.code, path.status.message);
  }
  return canonicalize_path(path.value, impl_->limits);
}

IntentSubmission OpticalFabric::submit_intent(const ConnectivityIntent& intent) {
  IntentSubmission result;
  std::string name_reason;
  if (!Names::valid(intent.name, impl_->limits.max_name_length)) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::NameInvalid,
                                 std::string(Names::invalid_reason(intent.name, impl_->limits.max_name_length)));
    return result;
  }
  (void)name_reason;
  if (intent.source_port == intent.destination_port) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::InvalidArgument,
                                 "an intent needs two distinct endpoint ports");
    return result;
  }
  std::string origin;
  const Result<PathDescriptor> path = resolve_intent_path(intent, origin);
  if (!path.ok()) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(path.status.code == ErrorCode::NotFound ? RefusalCode::RouteUnresolved
                                                                        : RefusalCode::InvalidArgument,
                                 path.status.message);
    return result;
  }
  const Result<CanonicalPath> canonical = canonicalize_path(path.value, impl_->limits);
  if (!canonical.ok()) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::InvalidArgument, canonical.status.message);
    return result;
  }

  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->closed) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  // Identical input must yield the same logical answer: an existing object with
  // the same canonical path identity is reported, never duplicated.
  for (const auto& [id, existing] : impl_->objects) {
    if (existing.canonical.identity == canonical.value.identity &&
        existing.state != ConnectivityState::Retired && existing.state != ConnectivityState::Refused) {
      result.outcome = IntentOutcome::Duplicate;
      result.connectivity = id;
      result.path_identity = canonical.value.identity;
      result.state = existing.state;
      result.generation = existing.generation;
      result.created = false;
      return result;
    }
  }
  const ConnectivityId id = derive_named_id<ConnectivityId>("connectivity", intent.name);
  const auto named = impl_->objects.find(id);
  if (named != impl_->objects.end()) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::IdentityCollision,
                                 "a connectivity object with this name already exists with a different "
                                 "path identity");
    return result;
  }
  if (impl_->objects.size() >= impl_->limits.max_connectivity_objects) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded,
                                 "the configured connectivity capacity is exhausted");
    return result;
  }

  detail::ConnectivityRecord record;
  record.id = id;
  record.name = intent.name;
  record.owner = intent.owner;
  record.state = ConnectivityState::Proposed;
  record.generation = Generation{1};
  record.path = path.value;
  record.canonical = canonical.value;
  record.requirements = intent.requirements;
  record.created_tick = impl_->tick;
  record.updated_tick = impl_->tick;
  TransitionRecord created;
  created.from = ConnectivityState::Proposed;
  created.to = ConnectivityState::Proposed;
  created.outcome = OutcomeCode::Applied;
  created.generation = record.generation;
  created.epoch = impl_->epoch;
  created.tick = impl_->tick;
  created.detail = "intent accepted as a proposal; nothing is authorized yet";
  record.history.push_back(created);

  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(record));
  bundle.add(std::string(kRuntimeItem),
             encode_runtime_item(impl_->runtime_state(impl_->generation.next(),
                                                      impl_->topology_generation)));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    result.outcome = IntentOutcome::Refused;
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, status.message);
    return result;
  }
  result.outcome = IntentOutcome::Accepted;
  result.connectivity = id;
  result.path_identity = canonical.value.identity;
  result.state = ConnectivityState::Proposed;
  result.generation = record.generation;
  result.created = true;
  return result;
}

Result<ConnectivityView> OpticalFabric::describe_connectivity(ConnectivityId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->objects.find(id);
  if (found == impl_->objects.end()) {
    return Result<ConnectivityView>::failure(ErrorCode::NotFound, "no connectivity object has that identity");
  }
  const detail::ConnectivityRecord& record = found->second;
  ConnectivityView view;
  view.id = record.id;
  view.name = record.name;
  view.state = record.state;
  view.generation = record.generation;
  view.path_identity = record.canonical.identity;
  view.canonical_path = record.canonical.canonical_text;
  view.owner = record.owner;
  view.reservation = record.reservation;
  view.has_reservation = record.has_reservation;
  view.reservation_expiry = record.reservation_expiry;
  view.authority = record.authority;
  view.authority_confirmed = record.authority_confirmed &&
                             record.authority.incarnation.boot_sequence ==
                                 impl_->incarnation.boot_sequence;
  view.created_tick = record.created_tick;
  view.updated_tick = record.updated_tick;
  view.activated_tick = record.activated_tick;
  view.retired_tick = record.retired_tick;
  view.last_refusal = record.last_refusal;
  view.last_refusal_detail = record.last_refusal_detail;
  view.segments = record.path.segments;
  view.direction = record.path.direction;
  view.channel = record.path.channel;
  return Result<ConnectivityView>::success(std::move(view));
}

std::vector<ConnectivityView> OpticalFabric::connectivity_objects() const {
  std::vector<ConnectivityView> views;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  views.reserve(impl_->objects.size());
  for (const auto& [id, record] : impl_->objects) {
    ConnectivityView view;
    view.id = record.id;
    view.name = record.name;
    view.state = record.state;
    view.generation = record.generation;
    view.path_identity = record.canonical.identity;
    view.canonical_path = record.canonical.canonical_text;
    view.owner = record.owner;
    view.reservation = record.reservation;
    view.has_reservation = record.has_reservation;
    view.reservation_expiry = record.reservation_expiry;
    view.authority = record.authority;
    view.authority_confirmed = record.authority_confirmed &&
                               record.authority.incarnation.boot_sequence ==
                                   impl_->incarnation.boot_sequence;
    view.created_tick = record.created_tick;
    view.updated_tick = record.updated_tick;
    view.activated_tick = record.activated_tick;
    view.retired_tick = record.retired_tick;
    view.last_refusal = record.last_refusal;
    view.last_refusal_detail = record.last_refusal_detail;
    view.segments = record.path.segments;
    view.direction = record.path.direction;
    view.channel = record.path.channel;
    views.push_back(std::move(view));
    (void)id;
  }
  return views;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

ValidationResult OpticalFabric::validate(const ValidateRequest& request) {
  ValidationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_validate_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "validate";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "validate") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const Result<detail::ConnectivityRecord> existing =
        impl_->copy_object_locked(request.connectivity);
    if (existing.ok()) {
      result.state = existing.value.state;
      result.generation = existing.value.generation;
      result.assessment = impl_->assess_locked(existing.value.path, existing.value.requirements);
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;

  if (record.state == ConnectivityState::Retired) {
    return refuse(RefusalCode::ObjectRetired, "the connectivity object is retired");
  }
  if (record.state == ConnectivityState::Refused) {
    return refuse(record.last_refusal == RefusalCode::None ? RefusalCode::ObjectRetired : record.last_refusal,
                  "the connectivity object was refused: " + record.last_refusal_detail);
  }
  if (record.state != ConnectivityState::Proposed) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    result.state = record.state;
    return result;
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  detail::ConnectivityRecord updated = record;
  const EvidenceAssessment assessment = impl_->assess_locked(updated.path, updated.requirements);
  result.assessment = assessment;
  if (!assessment.healthy()) {
    std::string detail;
    const RefusalCode code = impl_->refusal_for_assessment(assessment, detail);
    updated.last_refusal = code;
    updated.last_refusal_detail = detail;
    bool terminal = false;
    for (const EvidenceEvaluation& evaluation : assessment.evaluations) {
      if (evaluation.requirement.blocking && evaluation.state == EvidenceState::Stale &&
          evaluation.requirement.kind == EvidenceKind::TopologyPresence) {
        terminal = true;
      }
    }
    if (terminal) {
      // The recorded path no longer describes the registered topology, so this
      // connectivity object can never be promoted as it stands.
      impl_->push_transition_locked(updated, ConnectivityState::Refused, OutcomeCode::Applied, code,
                                    request.attempt, detail);
      updated.state = ConnectivityState::Refused;
      updated.generation = updated.generation.next();
    }
    updated.updated_tick = impl_->tick;
    detail::CommitBundle bundle;
    bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
    detail::AttemptRecord attempt;
    attempt.attempt = request.attempt;
    attempt.request_digest = digest;
    attempt.operation = "validate";
    attempt.outcome = OutcomeCode::Refused;
    attempt.refusal = code;
    attempt.connectivity = request.connectivity;
    attempt.tick = impl_->tick;
    attempt.generation = impl_->generation;
    attempt.summary = detail;
    bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt));
    const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
    if (!status.ok()) {
      return refuse(RefusalCode::PersistenceFailure, status.message);
    }
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    result.state = updated.state;
    result.generation = updated.generation;
    return result;
  }

  impl_->push_transition_locked(updated, ConnectivityState::Validated, OutcomeCode::Applied,
                                RefusalCode::None, request.attempt,
                                "preconditions and evidence validated; nothing is committed");
  updated.state = ConnectivityState::Validated;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  updated.last_refusal = RefusalCode::None;
  updated.last_refusal_detail.clear();

  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  detail::AttemptRecord attempt;
  attempt.attempt = request.attempt;
  attempt.request_digest = digest;
  attempt.operation = "validate";
  attempt.outcome = OutcomeCode::Applied;
  attempt.connectivity = request.connectivity;
  attempt.tick = impl_->tick;
  attempt.generation = impl_->generation;
  attempt.summary = "validated";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;
  result.generation = updated.generation;
  return result;
}

// ---------------------------------------------------------------------------
// Reservation
// ---------------------------------------------------------------------------

ReservationResult OpticalFabric::reserve(const ReservationRequest& request) {
  ReservationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_reserve_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "reserve";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (request.ttl_ticks == 0 || request.ttl_ticks > impl_->options.max_lease_ticks) {
    return refuse(RefusalCode::InvalidArgument, "the requested reservation lifetime is out of range");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "reserve") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const auto existing = impl_->reservations.find(prior->reservation);
    if (existing != impl_->reservations.end()) {
      result.reservation = existing->second.view;
    }
    const Result<detail::ConnectivityRecord> existing_object =
        impl_->copy_object_locked(request.connectivity);
    if (existing_object.ok()) {
      result.generation = existing_object.value.generation;
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  if (record.state == ConnectivityState::Reserved && record.has_reservation) {
    const auto existing = impl_->reservations.find(record.reservation);
    if (existing != impl_->reservations.end() &&
        existing->second.view.live_at(impl_->tick) &&
        existing->second.view.holder == request.authority.holder.to_string()) {
      result.outcome = OutcomeCode::AlreadySatisfied;
      result.reservation = existing->second.view;
      result.generation = record.generation;
      return result;
    }
  }
  if (record.state != ConnectivityState::Validated) {
    return refuse(RefusalCode::IllegalTransition,
                  "a reservation requires a validated connectivity object, but the object is " +
                      std::string(to_string(record.state)));
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
  if (!assessment.healthy()) {
    std::string detail;
    const RefusalCode code = impl_->refusal_for_assessment(assessment, detail);
    return refuse(code, detail);
  }
  std::vector<ResourceRef> resources;
  resources.reserve(record.path.segments.size());
  for (const PathSegment& segment : record.path.segments) {
    resources.push_back(segment.resource);
  }
  const OpticalFabric::Impl::Conflict conflict =
      impl_->check_claims_locked(resources, record.path.channel, record.id, ReservationId{});
  if (conflict.conflicting) {
    return refuse(RefusalCode::ConflictingClaim, conflict.detail);
  }
  if (impl_->reservations.size() >= impl_->limits.max_reservations) {
    return refuse(RefusalCode::CapacityExceeded, "the configured reservation capacity is exhausted");
  }

  detail::ReservationState reservation;
  reservation.view.id = derive_reservation_id(record.id, record.generation.next(), impl_->tick);
  reservation.view.connectivity = record.id;
  reservation.view.holder = request.authority.holder.to_string();
  reservation.view.epoch = impl_->epoch;
  reservation.view.incarnation = impl_->incarnation;
  reservation.view.granted_tick = impl_->tick;
  reservation.view.expiry_tick = impl_->tick.advanced_by(request.ttl_ticks);
  reservation.view.generation = Generation{1};
  reservation.view.resources = resources;

  detail::ConnectivityRecord updated = record;
  impl_->push_transition_locked(updated, ConnectivityState::Reserved, OutcomeCode::Applied, RefusalCode::None,
                                request.attempt, "resources reserved for a bounded lease");
  updated.state = ConnectivityState::Reserved;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  updated.reservation = reservation.view.id;
  updated.has_reservation = true;
  updated.reservation_expiry = reservation.view.expiry_tick;
  updated.authority = request.authority;

  detail::CommitBundle bundle;
  bundle.add(std::string(kReservationItem), encode_reservation_item(reservation));
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "reserve";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = request.connectivity;
  attempt_record.reservation = reservation.view.id;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "reserved";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ReservationChanged);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.reservation = reservation.view;
  result.claimed = resources;
  result.generation = updated.generation;
  return result;
}

ReservationResult OpticalFabric::renew_reservation(const ReservationRenewal& request) {
  ReservationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_renew_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "renew_reservation";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.reservation = request.reservation;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (request.extend_ticks == 0 || request.extend_ticks > impl_->options.max_lease_ticks) {
    return refuse(RefusalCode::InvalidArgument, "the requested extension is out of range");
  }
  const auto found = impl_->reservations.find(request.reservation);
  if (found == impl_->reservations.end()) {
    return refuse(RefusalCode::ReservationNotHeld, "no reservation has that identity");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "renew_reservation") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    result.reservation = found->second.view;
    return result;
  }
  if (found->second.view.released) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation was released");
  }
  if (found->second.view.consumed) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation was consumed by an activation");
  }
  if (found->second.view.holder != request.authority.holder.to_string()) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation is held by a different controller");
  }
  if (!found->second.view.live_at(impl_->tick)) {
    return refuse(RefusalCode::ReservationExpired, "the reservation expired at tick " +
                                                       found->second.view.expiry_tick.to_string());
  }
  // Renewal is governed by the scope of the object the reservation belongs to,
  // which is the same scope that authorized the reservation in the first place.
  const auto reserved_object = impl_->objects.find(found->second.view.connectivity);
  if (reserved_object == impl_->objects.end()) {
    return refuse(RefusalCode::ReservationNotHeld,
                  "the connectivity object this reservation belongs to no longer exists");
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(reserved_object->second), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  detail::ReservationState updated = found->second;
  updated.view.expiry_tick = impl_->tick.advanced_by(request.extend_ticks);
  updated.view.generation = updated.view.generation.next();
  detail::ConnectivityRecord* object = nullptr;
  const auto object_found = impl_->objects.find(updated.view.connectivity);
  if (object_found != impl_->objects.end()) {
    object = &object_found->second;
    object->reservation_expiry = updated.view.expiry_tick;
    object->generation = object->generation.next();
    object->updated_tick = impl_->tick;
  }
  detail::CommitBundle bundle;
  bundle.add(std::string(kReservationItem), encode_reservation_item(updated));
  if (object != nullptr) {
    bundle.add(std::string(kConnectivityItem), encode_connectivity_item(*object));
  }
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "renew_reservation";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.reservation = request.reservation;
  attempt_record.connectivity = updated.view.connectivity;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "reservation renewed";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ReservationChanged);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.reservation = updated.view;
  return result;
}

ReleaseResult OpticalFabric::release_reservation(const ReleaseRequest& request) {
  ReleaseResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_release_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "release_reservation";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.reservation = request.reservation;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  const auto found = impl_->reservations.find(request.reservation);
  if (found == impl_->reservations.end()) {
    return refuse(RefusalCode::ReservationNotHeld, "no reservation has that identity");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "release_reservation") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    result.reservation = request.reservation;
    return result;
  }
  if (found->second.view.released) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    result.reservation = request.reservation;
    return result;
  }
  if (found->second.view.consumed) {
    return refuse(RefusalCode::ReservationNotHeld,
                  "the reservation was consumed by an activation; withdraw the path instead");
  }
  if (found->second.view.holder != request.authority.holder.to_string()) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation is held by a different controller");
  }
  const auto reserved_object = impl_->objects.find(found->second.view.connectivity);
  if (reserved_object == impl_->objects.end()) {
    return refuse(RefusalCode::ReservationNotHeld,
                  "the connectivity object this reservation belongs to no longer exists");
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(reserved_object->second), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  detail::ReservationState updated = found->second;
  updated.view.released = true;
  updated.view.generation = updated.view.generation.next();

  detail::CommitBundle bundle;
  bundle.add(std::string(kReservationItem), encode_reservation_item(updated));
  const auto object_found = impl_->objects.find(updated.view.connectivity);
  if (object_found != impl_->objects.end() && object_found->second.state == ConnectivityState::Reserved) {
    detail::ConnectivityRecord object = object_found->second;
    impl_->push_transition_locked(object, ConnectivityState::Validated, OutcomeCode::Applied,
                                  RefusalCode::None, request.attempt,
                                  request.reason.empty() ? "reservation released" : request.reason);
    object.state = ConnectivityState::Validated;
    object.generation = object.generation.next();
    object.updated_tick = impl_->tick;
    object.has_reservation = false;
    object.reservation = ReservationId{};
    object.reservation_expiry = Tick{};
    bundle.add(std::string(kConnectivityItem), encode_connectivity_item(object));
  }
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "release_reservation";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.reservation = request.reservation;
  attempt_record.connectivity = updated.view.connectivity;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "reservation released";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ReservationChanged);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.reservation = request.reservation;
  result.released = updated.view.resources;
  result.released_tick = impl_->tick;
  return result;
}

// ---------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------

ActivationResult OpticalFabric::begin_activation(const ActivationRequest& request) {
  ActivationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_activation_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "begin_activation";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "begin_activation") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const Result<detail::ConnectivityRecord> existing =
        impl_->copy_object_locked(request.connectivity);
    if (existing.ok()) {
      result.state = existing.value.state;
      result.generation = existing.value.generation;
      result.activation_digest = existing.value.activation_digest;
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;
  if (record.state != ConnectivityState::Reserved) {
    return refuse(RefusalCode::IllegalTransition,
                  "activation requires a reserved connectivity object, but the object is " +
                      std::string(to_string(record.state)));
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  if (!record.has_reservation) {
    return refuse(RefusalCode::ReservationNotHeld, "the object holds no reservation");
  }
  const auto reservation = impl_->reservations.find(record.reservation);
  if (reservation == impl_->reservations.end()) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation no longer exists");
  }
  if (reservation->second.view.released || reservation->second.view.consumed) {
    return refuse(RefusalCode::ReservationNotHeld, "the reservation is no longer live");
  }
  if (!reservation->second.view.live_at(impl_->tick)) {
    return refuse(RefusalCode::ReservationExpired,
                  "the reservation expired at tick " + reservation->second.view.expiry_tick.to_string());
  }
  const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
  result.assessment = assessment;
  if (!assessment.healthy()) {
    std::string detail;
    const RefusalCode code = impl_->refusal_for_assessment(assessment, detail);
    return refuse(code, detail);
  }
  std::vector<ResourceRef> resources;
  resources.reserve(record.path.segments.size());
  for (const PathSegment& segment : record.path.segments) {
    resources.push_back(segment.resource);
  }
  const OpticalFabric::Impl::Conflict conflict =
      impl_->check_claims_locked(resources, record.path.channel, record.id, record.reservation);
  if (conflict.conflicting) {
    return refuse(RefusalCode::ConflictingClaim, conflict.detail);
  }

  detail::ConnectivityRecord updated = record;
  impl_->push_transition_locked(updated, ConnectivityState::Activating, OutcomeCode::Applied,
                                RefusalCode::None, request.attempt,
                                "preconditions re-checked; awaiting the commit boundary");
  updated.state = ConnectivityState::Activating;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  updated.authority = request.authority;
  updated.activation_attempt = request.attempt;
  updated.activation_digest = activation_digest(record.id, updated.generation, request.attempt,
                                                impl_->epoch, record.canonical.identity,
                                                assessment.aggregate);

  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "begin_activation";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = request.connectivity;
  attempt_record.reservation = record.reservation;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "activating";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;
  result.generation = updated.generation;
  result.activation_digest = updated.activation_digest;
  return result;
}

ActivationResult OpticalFabric::commit_activation(const CommitRequest& request) {
  ActivationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_commit_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "commit_activation";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "commit_activation") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;
  if (record.state == ConnectivityState::Active || record.state == ConnectivityState::Degraded) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    result.state = record.state;
    return result;
  }
  if (record.state != ConnectivityState::Activating) {
    return refuse(RefusalCode::IllegalTransition,
                  "there is no pending activation for this object; the object is " +
                      std::string(to_string(record.state)));
  }
  if (!(request.activation_digest == record.activation_digest) || record.activation_digest.is_nil()) {
    return refuse(RefusalCode::AttemptConflict,
                  "the presented activation digest is not the pending activation of this object");
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
  result.assessment = assessment;
  if (!assessment.healthy()) {
    std::string detail;
    return refuse(impl_->refusal_for_assessment(assessment, detail), detail);
  }
  std::vector<ResourceRef> resources;
  resources.reserve(record.path.segments.size());
  for (const PathSegment& segment : record.path.segments) {
    resources.push_back(segment.resource);
  }
  const OpticalFabric::Impl::Conflict conflict =
      impl_->check_claims_locked(resources, record.path.channel, record.id, record.reservation);
  if (conflict.conflicting) {
    return refuse(RefusalCode::ConflictingClaim, conflict.detail);
  }

  detail::ConnectivityRecord updated = record;
  impl_->push_transition_locked(updated, ConnectivityState::Active, OutcomeCode::Applied, RefusalCode::None,
                                request.attempt, "activation committed; the path is authoritative");
  updated.state = ConnectivityState::Active;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  updated.activated_tick = impl_->tick;
  updated.authority = request.authority;
  updated.authority_confirmed = true;
  updated.activation_digest = Digest128{};

  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  if (updated.has_reservation) {
    const auto reservation = impl_->reservations.find(updated.reservation);
    if (reservation != impl_->reservations.end()) {
      detail::ReservationState consumed = reservation->second;
      consumed.view.consumed = true;
      consumed.view.generation = consumed.view.generation.next();
      bundle.add(std::string(kReservationItem), encode_reservation_item(consumed));
    }
  }
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "commit_activation";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = request.connectivity;
  attempt_record.reservation = updated.reservation;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "active";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;
  result.generation = updated.generation;
  result.activated_tick = updated.activated_tick;
  result.committed = resources;
  return result;
}

// ---------------------------------------------------------------------------
// Withdrawal
// ---------------------------------------------------------------------------

WithdrawalResult OpticalFabric::withdraw(const WithdrawalRequest& request) {
  WithdrawalResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_withdrawal_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "withdraw";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "withdraw") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const Result<detail::ConnectivityRecord> existing =
        impl_->copy_object_locked(request.connectivity);
    if (existing.ok()) {
      result.state = existing.value.state;
      result.generation = existing.value.generation;
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;
  if (record.state == ConnectivityState::Retired) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    result.state = record.state;
    result.terminal = true;
    return result;
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  detail::ConnectivityRecord updated = record;
  std::vector<ResourceRef> released;
  if (updated.state == ConnectivityState::Withdrawing) {
    impl_->push_transition_locked(updated, ConnectivityState::Retired, OutcomeCode::Applied,
                                  RefusalCode::None, request.attempt,
                                  request.reason.empty() ? "withdrawal completed" : request.reason);
    updated.state = ConnectivityState::Retired;
    updated.generation = updated.generation.next();
    updated.updated_tick = impl_->tick;
    updated.retired_tick = impl_->tick;
    updated.authority_confirmed = false;
    result.terminal = true;
  } else {
    for (const PathSegment& segment : updated.path.segments) {
      released.push_back(segment.resource);
    }
    impl_->push_transition_locked(updated, ConnectivityState::Withdrawing, OutcomeCode::Applied,
                                  RefusalCode::None, request.attempt,
                                  request.reason.empty() ? "withdrawal started; resources released"
                                                         : request.reason);
    updated.state = ConnectivityState::Withdrawing;
    updated.generation = updated.generation.next();
    updated.updated_tick = impl_->tick;
    updated.authority_confirmed = false;
    updated.activation_digest = Digest128{};
    result.terminal = false;
  }
  auto reservation_release = std::optional<detail::ReservationState>{};
  if (updated.has_reservation) {
    const auto reservation = impl_->reservations.find(updated.reservation);
    // A consumed reservation is already finished: withdrawal releases the
    // committed claims, and consumption stays terminal for the lease.
    if (reservation != impl_->reservations.end() && !reservation->second.view.released &&
        !reservation->second.view.consumed) {
      detail::ReservationState updated_reservation = reservation->second;
      updated_reservation.view.released = true;
      updated_reservation.view.generation = updated_reservation.view.generation.next();
      reservation_release = updated_reservation;
    }
  }
  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  if (reservation_release.has_value()) {
    bundle.add(std::string(kReservationItem), encode_reservation_item(*reservation_release));
  }
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "withdraw";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = request.connectivity;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = result.terminal ? "retired" : "withdrawing";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;
  result.generation = updated.generation;
  result.released = released;
  result.retired_tick = updated.retired_tick;
  return result;
}

// ---------------------------------------------------------------------------
// Health reporting and post-restart revalidation
// ---------------------------------------------------------------------------

HealthResult OpticalFabric::apply_health(const HealthReport& report, ConnectivityState target,
                                         const char* operation) {
  HealthResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_health_request(report);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = report.attempt;
    record.request_digest = digest;
    record.operation = operation;
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = report.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(report.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != operation) {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const Result<detail::ConnectivityRecord> existing =
        impl_->copy_object_locked(report.connectivity);
    if (existing.ok()) {
      result.state = existing.value.state;
      result.generation = existing.value.generation;
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(report.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;
  if (record.state == target) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    return result;
  }
  if (!is_committed_state(record.state)) {
    return refuse(RefusalCode::ObjectNotActive,
                  "health reporting applies to a committed path, but the object is " +
                      std::string(to_string(record.state)));
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      report.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  detail::ConnectivityRecord updated = record;
  std::string detail = report.detail;
  if (detail.empty()) {
    detail = std::string("health reported as ") + std::string(to_string(report.observed));
  }
  impl_->push_transition_locked(updated, target, OutcomeCode::Applied, RefusalCode::None, report.attempt,
                                detail);
  updated.state = target;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = report.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = operation;
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = report.connectivity;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = detail;
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;  result.generation = updated.generation;
  return result;
}

HealthResult OpticalFabric::report_degraded(const HealthReport& report) {
  return apply_health(report, ConnectivityState::Degraded, "report_degraded");
}

HealthResult OpticalFabric::report_failed(const HealthReport& report) {
  return apply_health(report, ConnectivityState::Failed, "report_failed");
}

RevalidationResult OpticalFabric::revalidate(const RevalidationRequest& request) {
  RevalidationResult result;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const Digest128 digest = digest_revalidation_request(request);
  const auto refuse = [&](RefusalCode code, const std::string& detail) {
    result.outcome = OutcomeCode::Refused;
    result.refusal = Refusal::of(code, detail);
    detail::AttemptRecord record;
    record.attempt = request.attempt;
    record.request_digest = digest;
    record.operation = "revalidate";
    record.outcome = OutcomeCode::Refused;
    record.refusal = code;
    record.connectivity = request.connectivity;
    record.tick = impl_->tick;
    record.generation = impl_->generation;
    record.summary = detail;
    impl_->record_attempt_locked(record);
    return result;
  };
  if (impl_->closed) {
    return refuse(RefusalCode::PersistenceFailure, "the runtime is closed");
  }
  if (const detail::AttemptRecord* prior = impl_->find_attempt_locked(request.attempt)) {
    if (!(prior->request_digest == digest) || prior->operation != "revalidate") {
      return refuse(RefusalCode::AttemptConflict,
                    "this attempt identifier was already used for a different request");
    }
    result.outcome = prior->outcome == OutcomeCode::Refused ? OutcomeCode::Refused
                                                            : OutcomeCode::IdempotentReplay;
    result.refusal = Refusal::of(prior->refusal, prior->summary);
    const Result<detail::ConnectivityRecord> existing =
        impl_->copy_object_locked(request.connectivity);
    if (existing.ok()) {
      result.state = existing.value.state;
      result.generation = existing.value.generation;
      result.authority_confirmed = existing.value.authority_confirmed;
    }
    return result;
  }
  const Result<detail::ConnectivityRecord> found = impl_->copy_object_locked(request.connectivity);
  if (!found.ok()) {
    return refuse(RefusalCode::UnknownResource, found.status.message);
  }
  const detail::ConnectivityRecord& record = found.value;
  result.state = record.state;
  result.generation = record.generation;
  if (!is_committed_state(record.state)) {
    return refuse(RefusalCode::ObjectNotActive,
                  "revalidation applies to a committed path, but the object is " +
                      std::string(to_string(record.state)));
  }
  std::string authority_detail;
  const RefusalCode authority = impl_->require_authority_locked(
      request.authority, impl_->object_scope_locked(record), authority_detail);
  if (authority != RefusalCode::None) {
    return refuse(authority, authority_detail);
  }
  const bool confirmed_here =
      record.authority_confirmed &&
      record.authority.incarnation.boot_sequence == impl_->incarnation.boot_sequence;
  if (confirmed_here) {
    result.outcome = OutcomeCode::AlreadySatisfied;
    result.authority_confirmed = true;
    result.assessment = impl_->assess_locked(record.path, record.requirements);
    return result;
  }
  const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
  result.assessment = assessment;
  detail::ConnectivityRecord updated = record;
  if (!assessment.healthy()) {
    std::string detail;
    const RefusalCode code = impl_->refusal_for_assessment(assessment, detail);
    if (!request.withdraw_on_failure) {
      return refuse(code, "revalidation failed: " + detail);
    }
    impl_->push_transition_locked(updated, ConnectivityState::Withdrawing, OutcomeCode::Applied, code,
                                  request.attempt, "revalidation failed: " + detail);
    updated.state = ConnectivityState::Withdrawing;
    updated.generation = updated.generation.next();
    updated.updated_tick = impl_->tick;
    updated.authority_confirmed = false;
    updated.last_refusal = code;
    updated.last_refusal_detail = detail;
    detail::CommitBundle bundle;
    bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
    detail::AttemptRecord attempt_record;
    attempt_record.attempt = request.attempt;
    attempt_record.request_digest = digest;
    attempt_record.operation = "revalidate";
    attempt_record.outcome = OutcomeCode::Applied;
    attempt_record.refusal = code;
    attempt_record.connectivity = request.connectivity;
    attempt_record.tick = impl_->tick;
    attempt_record.generation = impl_->generation;
    attempt_record.summary = "revalidation failed; withdrawal started";
    bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
    const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
    if (!status.ok()) {
      return refuse(RefusalCode::PersistenceFailure, status.message);
    }
    result.outcome = OutcomeCode::Applied;
    result.state = updated.state;
    result.generation = updated.generation;
    result.authority_confirmed = false;
    return result;
  }
  updated.authority = request.authority;
  updated.authority_confirmed = true;
  updated.generation = updated.generation.next();
  updated.updated_tick = impl_->tick;
  updated.last_refusal = RefusalCode::None;
  updated.last_refusal_detail.clear();
  detail::CommitBundle bundle;
  bundle.add(std::string(kConnectivityItem), encode_connectivity_item(updated));
  detail::AttemptRecord attempt_record;
  attempt_record.attempt = request.attempt;
  attempt_record.request_digest = digest;
  attempt_record.operation = "revalidate";
  attempt_record.outcome = OutcomeCode::Applied;
  attempt_record.connectivity = request.connectivity;
  attempt_record.tick = impl_->tick;
  attempt_record.generation = impl_->generation;
  attempt_record.summary = "authority reconfirmed after restart";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt_record));
  const Status status = impl_->commit(bundle, RecordKind::ConnectivityTransition);
  if (!status.ok()) {
    return refuse(RefusalCode::PersistenceFailure, status.message);
  }
  result.outcome = OutcomeCode::Applied;
  result.state = updated.state;
  result.generation = updated.generation;
  result.authority_confirmed = true;
  return result;
}

// ---------------------------------------------------------------------------
// Request digests
// ---------------------------------------------------------------------------

Digest128 digest_validate_request(const ValidateRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "validate");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  return hasher.digest();
}

Digest128 digest_reserve_request(const ReservationRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "reserve");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("ttl", request.ttl_ticks);
  return hasher.digest();
}

Digest128 digest_renew_request(const ReservationRenewal& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "renew_reservation");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("reservation", request.reservation.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("extend", request.extend_ticks);
  return hasher.digest();
}

Digest128 digest_release_request(const ReleaseRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "release_reservation");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("reservation", request.reservation.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("reason", request.reason);
  return hasher.digest();
}

Digest128 digest_activation_request(const ActivationRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "begin_activation");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  return hasher.digest();
}

Digest128 digest_commit_request(const CommitRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "commit_activation");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("activation", request.activation_digest.to_string());
  return hasher.digest();
}

Digest128 digest_withdrawal_request(const WithdrawalRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "withdraw");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("reason", request.reason);
  return hasher.digest();
}

Digest128 digest_health_request(const HealthReport& report) {
  CanonicalHasher hasher;
  hasher.add_field("op", "health");
  hasher.add_field("attempt", report.attempt.value());
  hasher.add_field("connectivity", report.connectivity.value());
  hasher.add_field("authority", report.authority.to_string());
  hasher.add_field("observed", to_string(report.observed));
  hasher.add_field("detail", report.detail);
  return hasher.digest();
}

Digest128 digest_revalidation_request(const RevalidationRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "revalidate");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("connectivity", request.connectivity.value());
  hasher.add_field("authority", request.authority.to_string());
  hasher.add_field("withdraw_on_failure", request.withdraw_on_failure ? 1u : 0u);
  return hasher.digest();
}

}  // namespace optical_fabric
