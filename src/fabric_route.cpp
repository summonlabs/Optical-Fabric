// Optical Fabric 1.0.0 - Summon Software Labs
// Declared-route resolution, evidence requirements and resource claims.
#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "fabric_impl.hpp"
#include "state_codec.hpp"

namespace optical_fabric {

namespace {

/// Evidence kinds a segment of this role must be able to demonstrate.
[[nodiscard]] EvidenceKind capability_kind_for(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Port: return EvidenceKind::PortCapability;
    case ResourceKind::CrossConnect: return EvidenceKind::CrossConnectCapability;
    case ResourceKind::Span: return EvidenceKind::SpanCapability;
    case ResourceKind::LineSystem: return EvidenceKind::LineSystemCapability;
    case ResourceKind::Channel: return EvidenceKind::ChannelCapability;
    default: return EvidenceKind::TopologyPresence;
  }
}

[[nodiscard]] EvidenceKind operational_kind_for(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Port: return EvidenceKind::PortOperationalState;
    case ResourceKind::CrossConnect: return EvidenceKind::CrossConnectOperationalState;
    case ResourceKind::Span: return EvidenceKind::SpanOperationalState;
    case ResourceKind::LineSystem: return EvidenceKind::LineSystemOperationalState;
    case ResourceKind::Channel: return EvidenceKind::ChannelAvailability;
    default: return EvidenceKind::TopologyPresence;
  }
}

[[nodiscard]] SegmentRole role_for(ResourceKind kind) noexcept {
  switch (kind) {
    case ResourceKind::Port: return SegmentRole::Port;
    case ResourceKind::CrossConnect: return SegmentRole::CrossConnect;
    case ResourceKind::Span: return SegmentRole::Span;
    case ResourceKind::LineSystem: return SegmentRole::LineSystem;
    case ResourceKind::Channel: return SegmentRole::Channel;
    default: return SegmentRole::Port;
  }
}

[[nodiscard]] RefusalCode refusal_for(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Known: return RefusalCode::None;
    case EvidenceState::Incomplete: return RefusalCode::MissingEvidence;
    case EvidenceState::Stale: return RefusalCode::StaleEvidence;
    case EvidenceState::Unknown: return RefusalCode::UnsupportedEvidenceKind;
    case EvidenceState::Unsupported: return RefusalCode::UnsupportedEvidenceKind;
    case EvidenceState::Conflicting: return RefusalCode::ConflictingEvidence;
    case EvidenceState::Invalid: return RefusalCode::InvalidEvidence;
  }
  return RefusalCode::MissingEvidence;
}

}  // namespace

std::vector<EvidenceRequirement> OpticalFabric::Impl::requirements_for(
    const PathDescriptor& path, const IntentRequirements& requirements) const {
  std::map<std::pair<ResourceRef, EvidenceKind>, EvidenceRequirement> unique;
  const auto add = [&unique](ResourceRef subject, EvidenceKind kind, bool blocking, std::string reason) {
    const auto key = std::make_pair(subject, kind);
    if (unique.find(key) != unique.end()) {
      return;
    }
    EvidenceRequirement requirement;
    requirement.subject = subject;
    requirement.kind = kind;
    requirement.blocking = blocking;
    requirement.reason = std::move(reason);
    unique.emplace(key, std::move(requirement));
  };

  for (const PathSegment& segment : path.segments) {
    const detail::ResourceState* state = find_resource(segment.resource);
    const ResourceKind kind = state == nullptr ? segment.resource.kind : state->resource.kind;
    const std::string name = resource_name(segment.resource);
    add(segment.resource, EvidenceKind::TopologyPresence, true,
        "topology presence for segment resource " + name);
    const EvidenceKind capability = capability_kind_for(kind);
    if (capability != EvidenceKind::TopologyPresence) {
      add(segment.resource, capability, requirements.require_capability_evidence,
          "capability evidence for segment resource " + name);
    }
    const EvidenceKind operational = operational_kind_for(kind);
    if (operational != EvidenceKind::TopologyPresence) {
      add(segment.resource, operational, requirements.require_operational_evidence,
          "operational evidence for segment resource " + name);
    }
    if (requirements.require_attachment_evidence && kind == ResourceKind::Port) {
      add(segment.resource, EvidenceKind::CableAttachment, true,
          "attachment evidence for physical endpoint " + name);
    }
    for (const EvidenceKind extra : requirements.additional_required_kinds) {
      add(segment.resource, extra, true, "caller-required evidence for segment resource " + name);
    }
  }
  if (!path.channel.is_nil()) {
    add(as_ref(path.channel), EvidenceKind::ChannelAvailability, requirements.require_operational_evidence,
        "channel availability for the selected channel");
  }
  std::vector<EvidenceRequirement> ordered;
  ordered.reserve(unique.size());
  for (const auto& entry : unique) {
    ordered.push_back(entry.second);
  }
  return ordered;
}

EvidenceEvaluation OpticalFabric::Impl::evaluate_requirement_locked(
    const EvidenceRequirement& requirement) const {
  EvidenceEvaluation evaluation;
  evaluation.requirement = requirement;

  if (requirement.kind == EvidenceKind::TopologyPresence) {
    const detail::ResourceState* state = find_resource(requirement.subject);
    if (state == nullptr) {
      evaluation.state = EvidenceState::Unknown;
      evaluation.detail = "the resource is not registered";
      return evaluation;
    }
    evaluation.state = EvidenceState::Known;
    evaluation.detail = "registered as '" + state->name + "' at generation " +
                        state->generation.to_string();
    return evaluation;
  }

  // Group observations by producer and keep the newest attestation from each,
  // so that a re-attestation replaces an older one instead of colliding with it.
  std::map<SourceId, const EvidenceRecord*> newest;
  for (const auto& [id, record] : evidence) {
    (void)id;
    if (!(record.subject == requirement.subject) || record.kind != requirement.kind) {
      continue;
    }
    const auto found = newest.find(record.provenance.source_id);
    if (found == newest.end()) {
      newest.emplace(record.provenance.source_id, &record);
      continue;
    }
    const EvidenceRecord* current = found->second;
    const bool newer = record.provenance.source_sequence > current->provenance.source_sequence ||
                       (record.provenance.source_sequence == current->provenance.source_sequence &&
                        record.provenance.observed_tick.value > current->provenance.observed_tick.value);
    if (newer) {
      found->second = &record;
    }
  }

  bool produced = false;
  bool declared_unsupported = false;
  for (const auto& [id, registration] : sources) {
    (void)id;
    for (const EvidenceKind kind : registration.descriptor.produced) {
      if (kind == requirement.kind) {
        produced = true;
      }
    }
    for (const EvidenceKind kind : registration.descriptor.unsupported) {
      if (kind == requirement.kind) {
        declared_unsupported = true;
      }
    }
  }

  if (newest.empty()) {
    const detail::ResourceState* state = find_resource(requirement.subject);
    const Generation subject_generation = state == nullptr ? Generation{} : state->generation;
    evaluation.state = declared_unsupported ? EvidenceState::Unsupported : EvidenceState::Unknown;
    if (!produced && !declared_unsupported) {
      evaluation.state = EvidenceState::Unsupported;
      evaluation.detail = "no producer for " + std::string(to_string(requirement.kind)) +
                          " is registered at this boundary";
    } else {
      evaluation.detail = "no producer answered for " + std::string(to_string(requirement.kind));
    }
    evaluation.current_generation = subject_generation;
    return evaluation;
  }

  // A record ingested by a previous incarnation, or observed at an older
  // generation, is an outdated attestation rather than a negative observation:
  // a live producer that re-attests the same fact supersedes it. If nothing
  // live remains, the answer is STALE - never KNOWN - so persisted evidence can
  // still never become fresh by being loaded.
  std::vector<EvidenceState> states;
  bool saw_outdated = false;
  const EvidenceRecord* worst = nullptr;
  for (const auto& [source, record] : newest) {
    (void)source;
    const detail::ResourceState* state = find_resource(requirement.subject);
    const Generation subject_generation =
        state == nullptr ? record->provenance.observed_generation : state->generation;
    const bool outdated =
        (policy.require_current_incarnation && !incarnation.is_nil() &&
         record->provenance.ingested_incarnation.boot_sequence != incarnation.boot_sequence) ||
        (policy.require_current_generation && !subject_generation.is_nil() &&
         record->provenance.observed_generation != subject_generation);
    if (outdated) {
      saw_outdated = true;
      continue;
    }
    const EvidenceState state_now = apply_policy(*record, policy, incarnation, subject_generation, tick);
    states.push_back(state_now);
    if (worst == nullptr ||
        static_cast<std::uint8_t>(state_now) > static_cast<std::uint8_t>(apply_policy(
                                             *worst, policy, incarnation, subject_generation, tick))) {
      worst = record;
    }
  }
  if (states.empty() && saw_outdated) {
    evaluation.state = EvidenceState::Stale;
    evaluation.detail = "every attestation for this subject came from an earlier incarnation";
    evaluation.current_generation = find_resource(requirement.subject) == nullptr
                                        ? Generation{}
                                        : find_resource(requirement.subject)->generation;
    return evaluation;
  }
  EvidenceState aggregate = compose_all(states);

  // Two independent producers that both claim to know the same fact must agree.
  if (aggregate == EvidenceState::Known && newest.size() > 1) {
    const Digest128* reference = nullptr;
    for (const auto& [source, record] : newest) {
      (void)source;
      const detail::ResourceState* state = find_resource(requirement.subject);
      const Generation subject_generation =
          state == nullptr ? record->provenance.observed_generation : state->generation;
      if (apply_policy(*record, policy, incarnation, subject_generation, tick) != EvidenceState::Known) {
        continue;
      }
      if (reference == nullptr) {
        reference = &record->provenance.content_digest;
        continue;
      }
      if (!(*reference == record->provenance.content_digest)) {
        aggregate = EvidenceState::Conflicting;
        break;
      }
    }
  }

  evaluation.state = aggregate;
  if (worst != nullptr) {
    evaluation.record = worst->id;
    evaluation.detail = worst->detail.empty()
                            ? std::string("reported ") + std::string(to_string(aggregate))
                            : worst->detail;
  }
  evaluation.current_generation = find_resource(requirement.subject) == nullptr
                                      ? Generation{}
                                      : find_resource(requirement.subject)->generation;
  return evaluation;
}

EvidenceAssessment OpticalFabric::Impl::assess_locked(const PathDescriptor& path,
                                                      const IntentRequirements& requirements) const {
  EvidenceAssessment assessment;
  std::vector<EvidenceState> blocking;
  std::vector<EvidenceState> advisory;

  // Structural checks come first: a path segment whose recorded generation no
  // longer matches the registry is stale before any producer is consulted.
  for (const PathSegment& segment : path.segments) {
    EvidenceRequirement requirement;
    requirement.subject = segment.resource;
    requirement.kind = EvidenceKind::TopologyPresence;
    requirement.blocking = true;
    requirement.reason = "segment generation matches the registered resource";
    EvidenceEvaluation evaluation;
    evaluation.requirement = requirement;
    const detail::ResourceState* state = find_resource(segment.resource);
    if (state == nullptr) {
      evaluation.state = EvidenceState::Unknown;
      evaluation.detail = "the segment resource is not registered";
    } else if (!(state->generation == segment.generation)) {
      evaluation.state = EvidenceState::Stale;
      evaluation.detail = "the segment was validated against generation " +
                          segment.generation.to_string() + " but the resource is at generation " +
                          state->generation.to_string();
    } else {
      evaluation.state = EvidenceState::Known;
      evaluation.detail = "generation " + state->generation.to_string();
    }
    blocking.push_back(evaluation.state);
    assessment.evaluations.push_back(std::move(evaluation));
  }

  for (const EvidenceRequirement& requirement : requirements_for(path, requirements)) {
    const EvidenceEvaluation evaluation = evaluate_requirement_locked(requirement);
    if (requirement.blocking) {
      blocking.push_back(evaluation.state);
    } else {
      advisory.push_back(evaluation.state);
    }
    assessment.evaluations.push_back(evaluation);
  }

  assessment.aggregate = compose_all(blocking);
  assessment.advisory_aggregate = advisory.empty() ? EvidenceState::Known : compose_all(advisory);
  return assessment;
}

RefusalCode OpticalFabric::Impl::refusal_for_assessment(const EvidenceAssessment& assessment,
                                                        std::string& detail) const {
  if (is_healthy(assessment.aggregate)) {
    return RefusalCode::None;
  }
  for (const EvidenceEvaluation& evaluation : assessment.evaluations) {
    if (!evaluation.requirement.blocking) {
      continue;
    }
    if (evaluation.state == assessment.aggregate) {
      detail = std::string(to_string(evaluation.requirement.kind)) + " for " +
               resource_name(evaluation.requirement.subject) + ": " +
               std::string(to_string(evaluation.state));
      if (!evaluation.detail.empty()) {
        detail.append(" (").append(evaluation.detail).append(")");
      }
      return refusal_for(evaluation.state);
    }
  }
  detail = std::string("evidence assessment is ") + std::string(to_string(assessment.aggregate));
  return refusal_for(assessment.aggregate);
}

EvidenceEvaluation OpticalFabric::evaluate_requirement(const EvidenceRequirement& requirement) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->evaluate_requirement_locked(requirement);
}

bool OpticalFabric::Impl::holds_committed_claims(const detail::ConnectivityRecord& record) noexcept {
  return record.activated_tick.value != 0 && record.state != ConnectivityState::Retired &&
         record.state != ConnectivityState::Refused && record.state != ConnectivityState::Withdrawing;
}

std::vector<detail::ClaimEntry> OpticalFabric::Impl::derive_claims_locked(
    const detail::ConnectivityRecord& record) const {
  std::vector<detail::ClaimEntry> claims;
  if (record.activated_tick.value == 0) {
    return claims;
  }
  for (const PathSegment& segment : record.path.segments) {
    const detail::ResourceState* state = find_resource(segment.resource);
    detail::ClaimEntry claim;
    claim.resource = segment.resource;
    claim.connectivity = record.id;
    claim.channel = record.path.channel;
    claim.exclusive = state == nullptr || state->sharing == SharingMode::Exclusive;
    claim.committed_tick = record.activated_tick;
    claims.push_back(claim);
  }
  return claims;
}

OpticalFabric::Impl::Conflict OpticalFabric::Impl::check_claims_locked(
    const std::vector<ResourceRef>& candidate_resources, ChannelId channel, ConnectivityId self,
    ReservationId own_reservation) const {
  Conflict conflict;
  for (const ResourceRef resource : candidate_resources) {
    const detail::ResourceState* state = find_resource(resource);
    const bool exclusive = state == nullptr || state->sharing == SharingMode::Exclusive;
    const auto committed = claim_index.find(resource);
    if (committed != claim_index.end()) {
      for (const detail::ClaimEntry& claim : committed->second) {
        if (claim.connectivity == self) {
          continue;
        }
        if (exclusive || claim.channel == channel) {
          conflict.conflicting = true;
          conflict.other_object = claim.connectivity;
          conflict.resource = resource;
          conflict.detail = "resource " + resource_name(resource) +
                            " is committed to another authoritative path";
          return conflict;
        }
      }
    }
    const auto held = reservation_index.find(resource);
    if (held != reservation_index.end()) {
      for (const ReservationId id : held->second) {
        if (id == own_reservation) {
          continue;
        }
        const auto reservation = reservations.find(id);
        if (reservation == reservations.end()) {
          continue;
        }
        ChannelId other_channel{};
        bool known = false;
        const auto object = objects.find(reservation->second.view.connectivity);
        if (object != objects.end()) {
          other_channel = object->second.path.channel;
          known = true;
        }
        // An unknown channel on a channelized resource is treated as
        // incompatible: the runtime does not assume two claims can coexist.
        if (exclusive || !known || other_channel == channel) {
          conflict.conflicting = true;
          conflict.other_reservation = id;
          conflict.resource = resource;
          conflict.detail = "resource " + resource_name(resource) +
                            " is reserved by another connectivity object";
          return conflict;
        }
      }
    }
  }
  return conflict;
}

// ---------------------------------------------------------------------------
// Route resolution
// ---------------------------------------------------------------------------

Result<PathDescriptor> OpticalFabric::Impl::resolve_declared_route_locked(const ConnectivityIntent& intent,
                                                                         std::string& origin) const {
  const detail::ResourceState* source = find_resource(as_ref(intent.source_port));
  const detail::ResourceState* destination = find_resource(as_ref(intent.destination_port));
  if (source == nullptr || source->resource.kind != ResourceKind::Port) {
    return Result<PathDescriptor>::failure(ErrorCode::NotFound, "the source port is not registered");
  }
  if (destination == nullptr || destination->resource.kind != ResourceKind::Port) {
    return Result<PathDescriptor>::failure(ErrorCode::NotFound, "the destination port is not registered");
  }
  if (intent.source_port == intent.destination_port) {
    return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument,
                                           "a path needs two distinct endpoint ports");
  }

  // Adjacency over declared opportunities only. Nothing is inferred, ranked or
  // optimized: the walk follows ports, registered cross connects and registered
  // spans, and it stops at the first shortest chain it finds.
  std::map<PortId, std::vector<CrossConnectId>> cross_connects;
  std::map<PortId, std::vector<SpanId>> spans;
  for (const auto& [ref, state] : resources) {
    if (ref.kind == ResourceKind::CrossConnect) {
      cross_connects[state.ingress].push_back(CrossConnectId::from_value(ref.id));
      cross_connects[state.egress].push_back(CrossConnectId::from_value(ref.id));
    } else if (ref.kind == ResourceKind::Span) {
      spans[state.endpoint_a].push_back(SpanId::from_value(ref.id));
      spans[state.endpoint_b].push_back(SpanId::from_value(ref.id));
    }
  }

  struct Step {
    PortId port{};
    std::vector<PathSegment> segments;
  };
  std::vector<Step> frontier;
  std::set<PortId> visited;
  Step initial;
  initial.port = intent.source_port;
  const detail::ResourceState* source_state = find_resource(as_ref(intent.source_port));
  initial.segments.push_back(PathSegment{SegmentRole::Port, as_ref(intent.source_port),
                                        source_state->generation});
  frontier.push_back(std::move(initial));
  visited.insert(intent.source_port);

  std::vector<PathSegment> found;
  std::size_t expansions = 0;
  while (!frontier.empty() && found.empty()) {
    std::vector<Step> next;
    for (const Step& step : frontier) {
      if (++expansions > limits.max_path_segments * 64u) {
        return Result<PathDescriptor>::failure(ErrorCode::CapacityExceeded,
                                               "declared-route resolution exceeded its search bound");
      }
      const auto cross = cross_connects.find(step.port);
      if (cross != cross_connects.end()) {
        for (const CrossConnectId id : cross->second) {
          const detail::ResourceState* state = find_resource(as_ref(id));
          if (state == nullptr) {
            continue;
          }
          const PortId far = state->ingress == step.port ? state->egress : state->ingress;
          const detail::ResourceState* far_state = find_resource(as_ref(far));
          if (far_state == nullptr) {
            continue;
          }
          Step advanced = step;
          advanced.segments.push_back(PathSegment{SegmentRole::CrossConnect, as_ref(id), state->generation});
          advanced.segments.push_back(PathSegment{SegmentRole::Port, as_ref(far), far_state->generation});
          advanced.port = far;
          if (far == intent.destination_port) {
            found = advanced.segments;
            break;
          }
          if (visited.insert(far).second) {
            next.push_back(std::move(advanced));
          }
        }
      }
      if (!found.empty()) {
        break;
      }
      const auto links = spans.find(step.port);
      if (links != spans.end()) {
        for (const SpanId id : links->second) {
          const detail::ResourceState* state = find_resource(as_ref(id));
          if (state == nullptr) {
            continue;
          }
          const PortId far = state->endpoint_a == step.port ? state->endpoint_b : state->endpoint_a;
          const detail::ResourceState* far_state = find_resource(as_ref(far));
          if (far_state == nullptr) {
            continue;
          }
          Step advanced = step;
          advanced.segments.push_back(PathSegment{SegmentRole::Span, as_ref(id), state->generation});
          advanced.segments.push_back(PathSegment{SegmentRole::Port, as_ref(far), far_state->generation});
          advanced.port = far;
          if (far == intent.destination_port) {
            found = advanced.segments;
            break;
          }
          if (visited.insert(far).second) {
            next.push_back(std::move(advanced));
          }
        }
      }
      if (!found.empty()) {
        break;
      }
    }
    frontier = std::move(next);
  }

  if (found.empty()) {
    return Result<PathDescriptor>::failure(
        ErrorCode::NotFound, "no chain of declared cross connects and spans connects these ports");
  }
  if (found.size() > limits.max_path_segments) {
    return Result<PathDescriptor>::failure(ErrorCode::CapacityExceeded,
                                           "the resolved route exceeds the configured segment bound");
  }

  PathDescriptor path;
  path.segments = std::move(found);
  // The chain is always resolved from source to destination; a reverse request
  // is the same physical resources in the opposite orientation, which is what
  // reversed_path produces (and why the direction is set here and not before).
  path.direction = Direction::Forward;
  path.route_origin = "declared-route";
  if (!intent.channel.is_nil()) {
    const detail::ResourceState* channel = find_resource(as_ref(intent.channel));
    if (channel == nullptr || channel->resource.kind != ResourceKind::Channel) {
      return Result<PathDescriptor>::failure(ErrorCode::NotFound, "the requested channel is not registered");
    }
    path.channel = intent.channel;
    path.channel_generation = channel->generation;
  }
  if (intent.direction == Direction::Reverse) {
    path = reversed_path(path);
  }
  origin = path.route_origin;
  return Result<PathDescriptor>::success(std::move(path));
}

Result<PathDescriptor> OpticalFabric::Impl::describe_route_locked(
    const std::vector<ResourceRef>& proposed_resources, ChannelId channel, Direction direction,
    std::string origin) const {
  if (proposed_resources.size() < 2) {
    return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument,
                                           "a proposed route needs at least two resources");
  }
  if (proposed_resources.size() > limits.max_path_segments) {
    return Result<PathDescriptor>::failure(ErrorCode::CapacityExceeded,
                                           "the proposed route exceeds the configured segment bound");
  }
  PathDescriptor path;
  path.route_origin = std::move(origin);
  path.direction = direction;
  std::set<ResourceRef> seen;
  for (const ResourceRef resource : proposed_resources) {
    const detail::ResourceState* state = find_resource(resource);
    if (state == nullptr) {
      return Result<PathDescriptor>::failure(ErrorCode::NotFound,
                                             "the proposed route names a resource that is not registered: " +
                                                 resource.to_string());
    }
    if (!seen.insert(resource).second) {
      return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument,
                                             "the proposed route names the same resource twice");
    }
    PathSegment segment;
    segment.role = role_for(state->resource.kind);
    segment.resource = resource;
    segment.generation = state->generation;
    path.segments.push_back(segment);
  }
  if (path.segments.front().role != SegmentRole::Port || path.segments.back().role != SegmentRole::Port) {
    return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument,
                                           "a route must begin and end at a port");
  }
  if (!channel.is_nil()) {
    const detail::ResourceState* state = find_resource(as_ref(channel));
    if (state == nullptr || state->resource.kind != ResourceKind::Channel) {
      return Result<PathDescriptor>::failure(ErrorCode::NotFound, "the requested channel is not registered");
    }
    path.channel = channel;
    path.channel_generation = state->generation;
  }
  return Result<PathDescriptor>::success(std::move(path));
}

}  // namespace optical_fabric
