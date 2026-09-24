// Optical Fabric 1.0.0 - Summon Software Labs
// Queries: topology, active paths, provenance, diagnostics, accounting and
// invariant verification.
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fabric_impl.hpp"
#include "state_codec.hpp"
#include "optical_fabric/version.hpp"

namespace optical_fabric {

namespace {

[[nodiscard]] ResourceView to_view(const detail::ResourceState& state) {
  ResourceView view;
  view.resource = state.resource;
  view.name = state.name;
  view.sharing = state.sharing;
  view.generation = state.generation;
  view.registered_tick = state.registered_tick;
  view.related = state.related;
  view.attributes = state.attributes;
  return view;
}

[[nodiscard]] std::string encode_resource_canonical(const detail::ResourceState& state) {
  detail::TextWriter writer;
  detail::encode_resource(writer, state);
  return writer.take();
}

/// The snapshot digest covers authoritative state, not the provenance of the
/// requests that produced it. Caller-supplied attempt identifiers and per
/// process entropy must not change what "the same state" hashes to, so they are
/// deliberately excluded here; the persisted record keeps them in full.
void hash_connectivity_state(CanonicalHasher& hasher, const detail::ConnectivityRecord& record) {
  hasher.add_field("id", record.id.to_string());
  hasher.add_field("name", record.name);
  hasher.add_field("owner", record.owner);
  hasher.add_field("state", to_string(record.state));
  hasher.add_field("generation", record.generation.value);
  hasher.add_field("path", record.canonical.identity.to_string());
  hasher.add_field("direction", to_string(record.path.direction));
  hasher.add_field("channel", record.path.channel.to_string());
  hasher.add_field("route_origin", record.path.route_origin);
  hasher.add_field("authority_grant", record.authority.grant.to_string());
  hasher.add_field("authority_holder", record.authority.holder.to_string());
  hasher.add_field("authority_epoch", record.authority.epoch.value);
  hasher.add_field("authority_scope", record.authority.scope.to_string());
  hasher.add_field("authority_boot", record.authority.incarnation.boot_sequence);
  hasher.add_field("authority_confirmed", record.authority_confirmed ? 1u : 0u);
  hasher.add_field("reservation", record.reservation.to_string());
  hasher.add_field("has_reservation", record.has_reservation ? 1u : 0u);
  hasher.add_field("reservation_expiry", record.reservation_expiry.value);
  hasher.add_field("created", record.created_tick.value);
  hasher.add_field("updated", record.updated_tick.value);
  hasher.add_field("activated", record.activated_tick.value);
  hasher.add_field("retired", record.retired_tick.value);
  hasher.add_field("refusal", to_string(record.last_refusal));
  for (const PathSegment& segment : record.path.segments) {
    hasher.add_field("segment", std::string(to_string(segment.role)) + "|" +
                                    segment.resource.to_string() + "|" +
                                    segment.generation.to_string());
  }
}

void hash_reservation_state(CanonicalHasher& hasher, const detail::ReservationState& state) {
  hasher.add_field("reservation", state.view.id.to_string());
  hasher.add_field("reservation_connectivity", state.view.connectivity.to_string());
  hasher.add_field("reservation_holder", state.view.holder);
  hasher.add_field("reservation_epoch", state.view.epoch.value);
  hasher.add_field("reservation_boot", state.view.incarnation.boot_sequence);
  hasher.add_field("reservation_granted", state.view.granted_tick.value);
  hasher.add_field("reservation_expiry", state.view.expiry_tick.value);
  hasher.add_field("reservation_generation", state.view.generation.value);
  hasher.add_field("reservation_consumed", state.view.consumed ? 1u : 0u);
  hasher.add_field("reservation_released", state.view.released ? 1u : 0u);
  for (const ResourceRef resource : state.view.resources) {
    hasher.add_field("reservation_resource", resource.to_string());
  }
}

void hash_grant_state(CanonicalHasher& hasher, const detail::GrantState& state) {
  hasher.add_field("grant", state.view.id.to_string());
  hasher.add_field("grant_holder", state.view.holder.to_string());
  hasher.add_field("grant_scope", state.view.scope.to_string());
  hasher.add_field("grant_epoch", state.view.epoch.value);
  hasher.add_field("grant_boot", state.view.incarnation.boot_sequence);
  hasher.add_field("grant_granted", state.view.granted_tick.value);
  hasher.add_field("grant_expiry", state.view.expiry_tick.value);
  hasher.add_field("grant_fenced", state.view.fenced ? 1u : 0u);
  hasher.add_field("grant_released", state.view.released ? 1u : 0u);
}

[[nodiscard]] std::string encode_reservation_canonical(const detail::ReservationState& state) {
  detail::TextWriter writer;
  detail::encode_reservation(writer, state);
  return writer.take();
}

[[nodiscard]] std::string encode_grant_canonical(const detail::GrantState& state) {
  detail::TextWriter writer;
  detail::encode_grant(writer, state);
  return writer.take();
}

[[nodiscard]] DiagnosticKind kind_for_evidence(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Known: return DiagnosticKind::MissingEvidence;
    case EvidenceState::Incomplete: return DiagnosticKind::MissingEvidence;
    case EvidenceState::Stale: return DiagnosticKind::StaleEvidence;
    case EvidenceState::Unknown: return DiagnosticKind::MissingEvidence;
    case EvidenceState::Unsupported: return DiagnosticKind::UnsupportedEvidence;
    case EvidenceState::Conflicting: return DiagnosticKind::ConflictingEvidence;
    case EvidenceState::Invalid: return DiagnosticKind::ConflictingEvidence;
  }
  return DiagnosticKind::MissingEvidence;
}

}  // namespace

std::string render_path_line(const CanonicalPath& path) {
  std::string out;
  out.reserve(path.canonical_text.size() + 40);
  out.append(path.identity.to_string());
  out.push_back(' ');
  for (const char character : path.canonical_text) {
    out.push_back(character == '\n' ? ' ' : character);
  }
  return out;
}

TopologySnapshot OpticalFabric::topology() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  TopologySnapshot snapshot;
  CanonicalHasher hasher;
  snapshot.topology_generation = impl_->topology_generation;
  snapshot.resources.reserve(impl_->resources.size());
  for (const auto& [ref, state] : impl_->resources) {
    (void)ref;
    snapshot.resources.push_back(to_view(state));
    hasher.add_raw(encode_resource_canonical(state));
  }
  snapshot.digest = hasher.digest();
  return snapshot;
}

Result<ResourceView> OpticalFabric::describe_resource(ResourceRef resource) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const detail::ResourceState* state = impl_->find_resource(resource);
  if (state == nullptr) {
    return Result<ResourceView>::failure(ErrorCode::NotFound, "the resource is not registered");
  }
  return Result<ResourceView>::success(to_view(*state));
}

ActivePathReport OpticalFabric::active_paths() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  ActivePathReport report;
  report.observed_tick = impl_->tick;
  report.epoch = impl_->epoch;
  CanonicalHasher hasher;
  for (const auto& [id, record] : impl_->objects) {
    if (!is_committed_state(record.state)) {
      continue;
    }
    ActivePathEntry entry;
    entry.connectivity = id;
    entry.name = record.name;
    entry.state = record.state;
    entry.identity = record.canonical.identity;
    entry.canonical_path = record.canonical.canonical_text;
    entry.owner = record.owner;
    entry.activated_tick = record.activated_tick;
    entry.authority_confirmed = record.authority_confirmed &&
                                record.authority.incarnation.boot_sequence ==
                                    impl_->incarnation.boot_sequence;
    const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
    entry.evidence_healthy = assessment.healthy();
    entry.evidence_state = assessment.aggregate;
    if (record.state != ConnectivityState::Active) {
      entry.unauthorized_reason = "the path is " + std::string(to_string(record.state));
    } else if (!entry.authority_confirmed) {
      entry.unauthorized_reason =
          "authority has not been reconfirmed by the current process incarnation";
    } else if (!entry.evidence_healthy) {
      entry.unauthorized_reason = "evidence for the path is " +
                                  std::string(to_string(assessment.aggregate));
    }
    entry.authorized = entry.unauthorized_reason.empty();
    if (entry.authorized) {
      report.authorized.push_back(id);
    } else {
      Diagnostic diagnostic;
      diagnostic.kind = !entry.authority_confirmed ? DiagnosticKind::UnconfirmedAuthority
                                                   : kind_for_evidence(assessment.aggregate);
      diagnostic.subject = record.path.segments.empty() ? ResourceRef{}
                                                         : record.path.segments.front().resource;
      diagnostic.connectivity = id;
      diagnostic.observed_tick = impl_->tick;
      diagnostic.observed_epoch = impl_->epoch;
      diagnostic.observed_generation = record.generation;
      diagnostic.current_generation = impl_->generation;
      diagnostic.detail = entry.unauthorized_reason;
      report.diagnostics.push_back(std::move(diagnostic));
    }
    hasher.add_raw(entry.identity.to_string());
    hasher.add_field("state", to_string(record.state));
    hasher.add_field("authorized", entry.authorized ? 1u : 0u);
    report.active.push_back(std::move(entry));
  }
  for (const auto& [id, grant] : impl_->grants) {
    (void)id;
    if (grant.view.fenced || grant.view.released) {
      continue;
    }
    AuthorityView view;
    view.id = grant.view.id;
    view.holder = grant.view.holder;
    view.scope = grant.view.scope;
    view.epoch = grant.view.epoch;
    view.incarnation = grant.view.incarnation;
    view.granted_tick = grant.view.granted_tick;
    view.expiry_tick = grant.view.expiry_tick;
    view.fenced = grant.view.fenced;
    view.released = grant.view.released;
    view.current = !impl_->has_overlapping_newer_epoch(grant.view.scope, grant.view.epoch) &&
                   grant.view.incarnation.boot_sequence == impl_->incarnation.boot_sequence &&
                   impl_->tick.value < grant.view.expiry_tick.value;
    report.authority.push_back(view);
  }
  report.digest = hasher.digest();
  return report;
}

Result<PathExplanation> OpticalFabric::explain(ConnectivityId id) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->objects.find(id);
  if (found == impl_->objects.end()) {
    return Result<PathExplanation>::failure(ErrorCode::NotFound,
                                            "no connectivity object has that identity");
  }
  const detail::ConnectivityRecord& record = found->second;
  PathExplanation explanation;
  explanation.connectivity = record.id;
  explanation.name = record.name;
  explanation.state = record.state;
  explanation.generation = record.generation;
  explanation.identity = record.canonical.identity;
  explanation.canonical_path = record.canonical.canonical_text;
  explanation.direction = record.path.direction;
  explanation.channel = record.path.channel;
  explanation.route_origin = record.path.route_origin;
  explanation.authority = record.authority;
  explanation.authority_confirmed = record.authority_confirmed &&
                                    record.authority.incarnation.boot_sequence ==
                                        impl_->incarnation.boot_sequence;
  explanation.reservation = record.reservation;
  explanation.has_reservation = record.has_reservation;
  explanation.reservation_expiry = record.reservation_expiry;
  explanation.last_refusal = record.last_refusal;
  explanation.last_refusal_detail = record.last_refusal_detail;
  explanation.history = record.history;

  const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
  explanation.evidence_aggregate = assessment.aggregate;
  explanation.evidence_healthy = assessment.healthy();

  for (const PathSegment& segment : record.path.segments) {
    SegmentProvenance provenance;
    provenance.segment = segment;
    const detail::ResourceState* state = impl_->find_resource(segment.resource);
    if (state == nullptr) {
      provenance.resource_name = segment.resource.to_string();
      provenance.evidence_state = EvidenceState::Unknown;
      provenance.generation_current = false;
      explanation.segments.push_back(std::move(provenance));
      continue;
    }
    provenance.resource_name = state->name;
    provenance.sharing = state->sharing;
    provenance.registered_generation = state->generation;
    provenance.generation_current = state->generation == segment.generation;
    if (state->sharing == SharingMode::Exclusive) {
      provenance.claim = "exclusive";
    } else {
      provenance.claim = record.path.channel.is_nil()
                             ? "channelized:any"
                             : "channelized:" + record.path.channel.to_string();
    }
    std::vector<EvidenceState> states;
    for (const EvidenceEvaluation& evaluation : assessment.evaluations) {
      if (!(evaluation.requirement.subject == segment.resource)) {
        continue;
      }
      provenance.evidence.push_back(evaluation);
      if (evaluation.requirement.blocking) {
        states.push_back(evaluation.state);
      }
    }
    provenance.evidence_state = compose_all(states);
    explanation.segments.push_back(std::move(provenance));
  }

  if (record.path.route_origin == "declared-route") {
    explanation.notes.push_back(
        "the route was resolved from registered cross-connect opportunities; no planner was consulted");
  } else {
    explanation.notes.push_back("the route was proposed by " + record.path.route_origin +
                                " and validated against the registry");
  }
  if (assessment.advisory_aggregate != EvidenceState::Known) {
    explanation.notes.push_back("advisory observations are " +
                                std::string(to_string(assessment.advisory_aggregate)) +
                                "; they are recorded but never gate promotion");
  }
  for (const EvidenceEvaluation& evaluation : assessment.evaluations) {
    if (evaluation.requirement.blocking && evaluation.state == EvidenceState::Unsupported) {
      explanation.notes.push_back("no producer exists at this boundary for " +
                                  std::string(to_string(evaluation.requirement.kind)) + " on " +
                                  impl_->resource_name(evaluation.requirement.subject));
    }
  }
  if (!explanation.authority_confirmed && is_committed_state(record.state)) {
    explanation.notes.push_back(
        "the path is committed but its authority has not been reconfirmed since the process started");
  }
  return Result<PathExplanation>::success(std::move(explanation));
}

ClaimReport OpticalFabric::claims() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  ClaimReport report;
  CanonicalHasher hasher;
  for (const auto& [resource, entries] : impl_->claim_index) {
    for (const detail::ClaimEntry& entry : entries) {
      ResourceClaim claim;
      claim.resource = resource;
      claim.connectivity = entry.connectivity;
      claim.channel = entry.channel;
      claim.exclusive = entry.exclusive;
      claim.committed_tick = entry.committed_tick;
      if (entry.exclusive) {
        report.exclusive_resources_claimed += 1;
      } else {
        report.channelized_resources_claimed += 1;
      }
      hasher.add_field("resource", resource.to_string());
      hasher.add_field("connectivity", entry.connectivity.to_string());
      hasher.add_field("channel", entry.channel.to_string());
      report.claims.push_back(std::move(claim));
    }
  }
  report.digest = hasher.digest();
  return report;
}

DiagnosticsReport OpticalFabric::diagnose(const DiagnosticQuery& query) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  DiagnosticsReport report;
  std::vector<Diagnostic> collected;
  const auto accepts = [&query](const Diagnostic& diagnostic) {
    if (!query.connectivity.is_nil() && !(diagnostic.connectivity == query.connectivity)) {
      return false;
    }
    if (!query.resource.is_nil() && !(diagnostic.subject == query.resource)) {
      return false;
    }
    if (query.filter_kind && diagnostic.kind != query.kind) {
      return false;
    }
    return true;
  };

  for (const Diagnostic& diagnostic : impl_->diagnostics) {
    if (accepts(diagnostic)) {
      collected.push_back(diagnostic);
    }
  }
  for (const auto& [id, record] : impl_->objects) {
    for (const PathSegment& segment : record.path.segments) {
      const detail::ResourceState* state = impl_->find_resource(segment.resource);
      if (state == nullptr) {
        Diagnostic diagnostic;
        diagnostic.kind = DiagnosticKind::StaleGeneration;
        diagnostic.refusal = RefusalCode::UnknownResource;
        diagnostic.subject = segment.resource;
        diagnostic.connectivity = id;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.observed_generation = segment.generation;
        diagnostic.detail = "a path segment names a resource that is no longer registered";
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
        continue;
      }
      if (!(state->generation == segment.generation)) {
        Diagnostic diagnostic;
        diagnostic.kind = DiagnosticKind::StaleGeneration;
        diagnostic.refusal = RefusalCode::StaleGeneration;
        diagnostic.subject = segment.resource;
        diagnostic.connectivity = id;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.observed_generation = segment.generation;
        diagnostic.current_generation = state->generation;
        diagnostic.detail = "the path was validated against generation " +
                            segment.generation.to_string() + " but the resource is at generation " +
                            state->generation.to_string();
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
      }
    }
    if (is_committed_state(record.state)) {
      const bool confirmed = record.authority_confirmed &&
                             record.authority.incarnation.boot_sequence ==
                                 impl_->incarnation.boot_sequence;
      if (!confirmed) {
        Diagnostic diagnostic;
        diagnostic.kind = DiagnosticKind::UnconfirmedAuthority;
        diagnostic.refusal = RefusalCode::StaleIncarnation;
        diagnostic.connectivity = id;
        diagnostic.grant = record.authority.grant;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.observed_epoch = record.authority.epoch;
        diagnostic.observed_generation = record.generation;
        diagnostic.detail = "the committed path is not authorized until its authority is reconfirmed";
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
      }
      const EvidenceAssessment assessment = impl_->assess_locked(record.path, record.requirements);
      for (const EvidenceEvaluation& evaluation : assessment.evaluations) {
        if (!evaluation.requirement.blocking || evaluation.state == EvidenceState::Known) {
          continue;
        }
        Diagnostic diagnostic;
        diagnostic.kind = kind_for_evidence(evaluation.state);
        diagnostic.refusal = RefusalCode::MissingEvidence;
        diagnostic.subject = evaluation.requirement.subject;
        diagnostic.connectivity = id;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.observed_generation = evaluation.current_generation;
        diagnostic.detail = std::string(to_string(evaluation.requirement.kind)) + ": " +
                            std::string(to_string(evaluation.state));
        if (!evaluation.detail.empty()) {
          diagnostic.detail.append(" (").append(evaluation.detail).append(")");
        }
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
      }
    }
    if (record.has_reservation && is_committed_state(record.state) &&
        record.reservation_expiry.value <= impl_->tick.value) {
      Diagnostic diagnostic;
      diagnostic.kind = DiagnosticKind::ReservationExpiry;
      diagnostic.refusal = RefusalCode::ReservationExpired;
      diagnostic.connectivity = id;
      diagnostic.reservation = record.reservation;
      diagnostic.observed_tick = impl_->tick;
      diagnostic.detail = "the reservation backing this path expired at tick " +
                          record.reservation_expiry.to_string();
      if (accepts(diagnostic)) {
        collected.push_back(std::move(diagnostic));
      }
    }
    if (!record.authority.is_nil()) {
      const AuthorityCheck check = impl_->check_authority_locked(record.authority);
      if (!check.current()) {
        Diagnostic diagnostic;
        diagnostic.kind = check.currentness == AuthorityCurrentness::StaleIncarnation
                              ? DiagnosticKind::StaleIncarnation
                              : DiagnosticKind::StaleAuthority;
        diagnostic.refusal = check.currentness == AuthorityCurrentness::StaleIncarnation
                                 ? RefusalCode::StaleIncarnation
                                 : RefusalCode::StaleEpoch;
        diagnostic.connectivity = id;
        diagnostic.grant = record.authority.grant;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.observed_epoch = record.authority.epoch;
        diagnostic.detail = check.detail;
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
      }
    }
  }
  // Conflicting claims on an exclusive resource are reported even though the
  // runtime refuses to create them: the check is what makes the refusal
  // verifiable rather than asserted.
  for (const auto& [resource, entries] : impl_->claim_index) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
      for (std::size_t other = index + 1; other < entries.size(); ++other) {
        const bool conflict = entries[index].exclusive || entries[other].exclusive ||
                              entries[index].channel == entries[other].channel;
        if (!conflict) {
          continue;
        }
        Diagnostic diagnostic;
        diagnostic.kind = DiagnosticKind::ResourceConflict;
        diagnostic.refusal = RefusalCode::ConflictingClaim;
        diagnostic.subject = resource;
        diagnostic.connectivity = entries[index].connectivity;
        diagnostic.observed_tick = impl_->tick;
        diagnostic.detail = "resource " + impl_->resource_name(resource) +
                            " is committed to more than one authoritative path";
        if (accepts(diagnostic)) {
          collected.push_back(std::move(diagnostic));
        }
      }
    }
  }
  const auto pressure = [&](std::size_t current, std::size_t limit, const char* label) {
    if (limit == 0 || current * 10 < limit * 9) {
      return;
    }
    Diagnostic diagnostic;
    diagnostic.kind = DiagnosticKind::CapacityPressure;
    diagnostic.refusal = RefusalCode::CapacityExceeded;
    diagnostic.observed_tick = impl_->tick;
    diagnostic.detail = std::string(label) + " is at " + std::to_string(current) + " of " +
                        std::to_string(limit);
    if (accepts(diagnostic)) {
      collected.push_back(std::move(diagnostic));
    }
  };
  pressure(impl_->resources.size(), impl_->limits.max_ports + impl_->limits.max_spans, "resources");
  pressure(impl_->objects.size(), impl_->limits.max_connectivity_objects, "connectivity objects");
  pressure(impl_->evidence.size(), impl_->limits.max_evidence_records, "evidence records");
  pressure(impl_->reservations.size(), impl_->limits.max_reservations, "reservations");
  pressure(impl_->attempts.size(), impl_->limits.max_attempt_history_total, "attempt records");

  report.total_available = collected.size();
  if (collected.size() > query.max_results) {
    collected.resize(query.max_results);
    report.truncated = true;
  }
  CanonicalHasher hasher;
  for (const Diagnostic& diagnostic : collected) {
    hasher.add_field("kind", to_string(diagnostic.kind));
    hasher.add_field("subject", diagnostic.subject.to_string());
    hasher.add_field("connectivity", diagnostic.connectivity.to_string());
    hasher.add_field("detail", diagnostic.detail);
  }
  report.diagnostics = std::move(collected);
  report.digest = hasher.digest();
  return report;
}

AccountingReport OpticalFabric::accounting() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AccountingReport report;
  report.resources_registered = impl_->resources.size();
  std::size_t claim_count = 0;
  for (const auto& [resource, entries] : impl_->claim_index) {
    (void)resource;
    claim_count += entries.size();
  }
  std::size_t derived_claims = 0;
  for (const auto& [id, record] : impl_->objects) {
    (void)id;
    if (!impl_->holds_committed_claims(record)) {
      continue;
    }
    derived_claims += impl_->derive_claims_locked(record).size();
  }
  report.claims = claim_count;
  if (claim_count != derived_claims) {
    report.drift.push_back("the commit index holds " + std::to_string(claim_count) +
                           " claims while the objects derive " + std::to_string(derived_claims));
  }
  for (const auto& [id, reservation] : impl_->reservations) {
    (void)id;
    if (reservation.view.released) {
      report.reservations_released += 1;
    } else if (reservation.view.consumed) {
      report.reservations_consumed += 1;
    } else {
      report.reservations_live += 1;
    }
  }
  report.objects_total = impl_->objects.size();
  for (const auto& [id, record] : impl_->objects) {
    (void)id;
    report.objects_by_state[static_cast<std::size_t>(record.state)] += 1;
    report.transitions += record.history.size();
  }
  for (const auto& [id, grant] : impl_->grants) {
    (void)id;
    if (grant.view.released) {
      report.grants_released += 1;
    } else if (grant.view.fenced) {
      report.grants_fenced += 1;
    } else {
      report.grants_live += 1;
    }
  }
  report.evidence_records = impl_->evidence.size();
  for (const auto& [id, record] : impl_->evidence) {
    (void)id;
    if (record.provenance.ingested_incarnation.boot_sequence != impl_->incarnation.boot_sequence) {
      report.evidence_stale += 1;
    }
  }
  report.attempt_records = impl_->attempts.size();
  report.diagnostics = impl_->diagnostics.size() + impl_->diagnostics_dropped;
  report.balanced = report.drift.empty();
  return report;
}

InvariantReport OpticalFabric::verify_invariants() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  InvariantReport report;
  const auto record = [&report](std::string name, bool holds, std::string detail) {
    InvariantCheck check;
    check.name = std::move(name);
    check.holds = holds;
    check.detail = std::move(detail);
    if (!holds) {
      report.all_hold = false;
    }
    report.checks.push_back(std::move(check));
  };

  // 1. No resource is committed to incompatible authoritative paths.
  bool compatible = true;
  std::string incompatible_detail;
  for (const auto& [resource, entries] : impl_->claim_index) {
    for (std::size_t index = 0; index < entries.size() && compatible; ++index) {
      for (std::size_t other = index + 1; other < entries.size(); ++other) {
        const bool conflict = entries[index].exclusive || entries[other].exclusive ||
                              entries[index].channel == entries[other].channel;
        if (conflict) {
          compatible = false;
          incompatible_detail = "resource " + impl_->resource_name(resource) + " is claimed by " +
                                entries[index].connectivity.to_string() + " and " +
                                entries[other].connectivity.to_string();
          break;
        }
      }
    }
  }
  record("no_resource_committed_to_incompatible_paths", compatible, incompatible_detail);

  // 2. The commit index agrees with the objects that hold claims.
  std::size_t derived = 0;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (!impl_->holds_committed_claims(object)) {
      continue;
    }
    derived += impl_->derive_claims_locked(object).size();
  }
  std::size_t indexed = 0;
  for (const auto& [resource, entries] : impl_->claim_index) {
    (void)resource;
    indexed += entries.size();
  }
  record("commit_index_matches_objects", derived == indexed,
         "objects derive " + std::to_string(derived) + " claims, the index holds " +
             std::to_string(indexed));

  // 3. Retired and refused objects hold no claims.
  bool retired_clean = true;
  std::string retired_detail;
  for (const auto& [id, object] : impl_->objects) {
    if (object.state != ConnectivityState::Retired && object.state != ConnectivityState::Refused) {
      continue;
    }
    for (const auto& [resource, entries] : impl_->claim_index) {
      for (const detail::ClaimEntry& entry : entries) {
        if (entry.connectivity == id) {
          retired_clean = false;
          retired_detail = "a retired object still holds " + resource.to_string();
        }
      }
    }
  }
  record("retired_objects_hold_no_claims", retired_clean, retired_detail);

  // 4. Live reservations are indexed, and indexed reservations are live.
  bool reservation_index_consistent = true;
  std::string reservation_detail;
  for (const auto& [id, reservation] : impl_->reservations) {
    const bool should_be_indexed = !reservation.view.released && !reservation.view.consumed;
    for (const ResourceRef resource : reservation.view.resources) {
      const auto found = impl_->reservation_index.find(resource);
      bool present = false;
      if (found != impl_->reservation_index.end()) {
        present = std::find(found->second.begin(), found->second.end(), id) != found->second.end();
      }
      if (present != should_be_indexed) {
        reservation_index_consistent = false;
        reservation_detail = "reservation " + id.to_string() + " index state disagrees with its record";
      }
    }
  }
  record("reservation_index_matches_records", reservation_index_consistent, reservation_detail);

  // 5. Every committed object names registered resources and a live reservation
  //    or a consumed one.
  bool committed_well_formed = true;
  std::string committed_detail;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (!is_committed_state(object.state) && object.state != ConnectivityState::Withdrawing) {
      continue;
    }
    for (const PathSegment& segment : object.path.segments) {
      if (impl_->find_resource(segment.resource) == nullptr) {
        committed_well_formed = false;
        committed_detail = "a committed path names an unregistered resource";
      }
    }
    if (object.has_reservation && impl_->reservations.find(object.reservation) ==
                                      impl_->reservations.end()) {
      committed_well_formed = false;
      committed_detail = "a committed path names a reservation that does not exist";
    }
  }
  record("committed_paths_are_well_formed", committed_well_formed, committed_detail);

  // 6. At most one current grant per overlapping scope.
  bool grants_unique = true;
  std::string grants_detail;
  for (const auto& [id, grant] : impl_->grants) {
    if (grant.view.fenced || grant.view.released) {
      continue;
    }
    for (const auto& [other_id, other] : impl_->grants) {
      if (other_id == id || other.view.fenced || other.view.released) {
        continue;
      }
      if (grant.view.scope.overlaps(other.view.scope) && grant.view.epoch.value != other.view.epoch.value) {
        const Epoch higher = grant.view.epoch.value > other.view.epoch.value ? grant.view.epoch
                                                                            : other.view.epoch;
        const Epoch lower = grant.view.epoch.value > other.view.epoch.value ? other.view.epoch
                                                                           : grant.view.epoch;
        if (higher.value != lower.value) {
          grants_unique = false;
          grants_detail = "two live grants overlap at epochs " + lower.to_string() + " and " +
                          higher.to_string();
        }
      }
    }
  }
  record("at_most_one_current_grant_per_scope", grants_unique, grants_detail);

  // 7. Authority confirmation can only have been given by this incarnation.
  bool confirmation_sound = true;
  std::string confirmation_detail;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (object.authority_confirmed && !object.authority.is_nil() &&
        object.authority.incarnation.boot_sequence != impl_->incarnation.boot_sequence) {
      confirmation_sound = false;
      confirmation_detail = "an object claims authority confirmed by an earlier incarnation";
    }
  }
  record("authority_confirmation_is_incarnation_bound", confirmation_sound, confirmation_detail);

  // 8. Evidence identities match their derivation and their subjects exist.
  bool evidence_sound = true;
  std::string evidence_detail;
  for (const auto& [id, record_] : impl_->evidence) {
    if (!(impl_->derive_evidence_id(record_) == id)) {
      evidence_sound = false;
      evidence_detail = "an observation identity does not match its derivation";
    }
    if (impl_->find_resource(record_.subject) == nullptr) {
      evidence_sound = false;
      evidence_detail = "an observation names an unregistered subject";
    }
  }
  record("evidence_identities_are_derived", evidence_sound, evidence_detail);

  // 9. Counters are monotone and bounded.
  bool counters_sane = impl_->generation.value >= impl_->topology_generation.value &&
                       impl_->generation.value > 0 && impl_->epoch.value >= 0;
  record("counters_are_consistent", counters_sane,
         "generation=" + impl_->generation.to_string() +
             " topology=" + impl_->topology_generation.to_string());

  // 10. A pending activation always carries a digest, and no other state does.
  bool activation_sound = true;
  std::string activation_detail;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (object.state == ConnectivityState::Activating && object.activation_digest.is_nil()) {
      activation_sound = false;
      activation_detail = "an activating object carries no activation digest";
    }
    if (object.state != ConnectivityState::Activating && !object.activation_digest.is_nil()) {
      activation_sound = false;
      activation_detail = "a non-activating object carries a pending activation digest";
    }
  }
  record("pending_activations_are_bound_to_a_digest", activation_sound, activation_detail);

  // 11. Reservation windows are well formed.
  bool reservations_sane = true;
  std::string reservation_window_detail;
  for (const auto& [id, reservation] : impl_->reservations) {
    (void)id;
    if (reservation.view.expiry_tick.value <= reservation.view.granted_tick.value) {
      reservations_sane = false;
      reservation_window_detail = "a reservation expires at or before it was granted";
    }
    if (reservation.view.resources.empty()) {
      reservations_sane = false;
      reservation_window_detail = "a reservation holds no resources";
    }
  }
  record("reservation_windows_are_well_formed", reservations_sane, reservation_window_detail);

  // 12. Bounded histories stay bounded.
  bool histories_bounded = impl_->attempts.size() <= impl_->limits.max_attempt_history_total &&
                           impl_->diagnostics.size() <= impl_->limits.max_diagnostics &&
                           impl_->fences.size() <= impl_->limits.max_fence_reasons_history;
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (object.history.size() > impl_->limits.max_attempt_history_per_object ||
        object.attempt_log.size() > impl_->limits.max_attempt_history_per_object) {
      histories_bounded = false;
    }
  }
  record("bounded_histories_stay_bounded", histories_bounded, "");

  return report;
}

FabricSnapshot OpticalFabric::snapshot() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  FabricSnapshot snapshot;
  snapshot.epoch = impl_->epoch;
  snapshot.incarnation = impl_->incarnation;
  snapshot.tick = impl_->tick;
  snapshot.generation = impl_->generation;
  snapshot.topology_generation = impl_->topology_generation;
  for (const auto& [ref, state] : impl_->resources) {
    switch (ref.kind) {
      case ResourceKind::Site: snapshot.sites += 1; break;
      case ResourceKind::OpticalNode: snapshot.optical_nodes += 1; break;
      case ResourceKind::Port: snapshot.ports += 1; break;
      case ResourceKind::Span: snapshot.spans += 1; break;
      case ResourceKind::LineSystem: snapshot.line_systems += 1; break;
      case ResourceKind::CrossConnect: snapshot.cross_connects += 1; break;
      case ResourceKind::Channel: snapshot.channels += 1; break;
      default: break;
    }
    (void)state;
  }
  snapshot.connectivity_objects = impl_->objects.size();
  for (const auto& [id, object] : impl_->objects) {
    (void)id;
    if (is_committed_state(object.state)) {
      snapshot.connectivity_active += 1;
    }
  }
  snapshot.reservations = impl_->reservations.size();
  for (const auto& [resource, entries] : impl_->claim_index) {
    (void)resource;
    snapshot.claims += entries.size();
  }
  snapshot.evidence_records = impl_->evidence.size();
  snapshot.authority_grants = impl_->grants.size();
  snapshot.recovery.status = static_cast<std::uint8_t>(impl_->recovery.status);
  snapshot.recovery.discarded_bytes = impl_->recovery.discarded_bytes;
  snapshot.recovery.boot_sequence = impl_->recovery.boot_sequence;
  snapshot.recovery.evidence_marked_stale = impl_->recovery.evidence_marked_stale;
  snapshot.recovery.authority_unconfirmed = impl_->recovery.authority_unconfirmed;
  snapshot.recovery.incomplete_activations = impl_->recovery.incomplete_activations;

  CanonicalHasher hasher;
  hasher.add_field("epoch", impl_->epoch.value);
  hasher.add_field("tick", impl_->tick.value);
  hasher.add_field("generation", impl_->generation.value);
  hasher.add_field("topology_generation", impl_->topology_generation.value);
  for (const auto& [ref, state] : impl_->resources) {
    hasher.add_raw(encode_resource_canonical(state));
    (void)ref;
  }
  for (const auto& [id, object] : impl_->objects) {
    hash_connectivity_state(hasher, object);
    (void)id;
  }
  for (const auto& [id, reservation] : impl_->reservations) {
    hash_reservation_state(hasher, reservation);
    (void)id;
  }
  for (const auto& [id, grant] : impl_->grants) {
    hash_grant_state(hasher, grant);
    (void)id;
  }
  snapshot.digest = hasher.digest();
  return snapshot;
}

std::size_t InvariantReport::failures() const noexcept {
  std::size_t count = 0;
  for (const InvariantCheck& check : checks) {
    if (!check.holds) {
      ++count;
    }
  }
  return count;
}

}  // namespace optical_fabric
