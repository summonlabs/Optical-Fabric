// Optical Fabric 1.0.0 - Summon Software Labs
#include "state_codec.hpp"

#include <array>

namespace optical_fabric::detail {

namespace {

[[nodiscard]] std::string digest_text(const Digest128& digest) { return digest.to_string(); }

Result<Digest128> read_digest(TextReader& reader, std::string_view key) {
  const Result<std::string> text = reader.require_string(key);
  if (!text.ok()) {
    return Result<Digest128>::failure(text.status.code, text.status.message);
  }
  Digest128 digest{};
  if (!Digest128::parse(text.value, digest)) {
    return Result<Digest128>::failure(ErrorCode::InvalidArgument,
                                      "a persisted digest is malformed: " + std::string(key));
  }
  return Result<Digest128>::success(digest);
}

Result<ResourceRef> read_resource(TextReader& reader, std::string_view key) {
  const Result<std::string> text = reader.require_string(key);
  if (!text.ok()) {
    return Result<ResourceRef>::failure(text.status.code, text.status.message);
  }
  const std::optional<ResourceRef> parsed = ResourceRef::parse(text.value);
  if (!parsed.has_value()) {
    return Result<ResourceRef>::failure(ErrorCode::InvalidArgument,
                                        "a persisted resource reference is malformed: " + std::string(key));
  }
  return Result<ResourceRef>::success(*parsed);
}

template <typename Id>
Result<Id> read_id(TextReader& reader, std::string_view key) {
  const Result<std::string> text = reader.require_string(key);
  if (!text.ok()) {
    return Result<Id>::failure(text.status.code, text.status.message);
  }
  const std::optional<Id> parsed = Id::parse(text.value);
  if (!parsed.has_value()) {
    return Result<Id>::failure(ErrorCode::InvalidArgument,
                               "a persisted identifier is malformed: " + std::string(key));
  }
  return Result<Id>::success(*parsed);
}

Result<Incarnation> read_incarnation(TextReader& reader, std::string_view prefix) {
  const std::string boot_key = std::string(prefix) + "boot";
  const std::string instance_key = std::string(prefix) + "instance";
  const std::string host_key = std::string(prefix) + "host";
  const Result<std::uint64_t> boot = reader.require_u64(boot_key);
  if (!boot.ok()) {
    return Result<Incarnation>::failure(boot.status.code, boot.status.message);
  }
  const Result<Digest128> instance = read_digest(reader, instance_key);
  if (!instance.ok()) {
    return Result<Incarnation>::failure(instance.status.code, instance.status.message);
  }
  const Result<std::string> host = reader.optional_string(host_key, std::string());
  if (!host.ok()) {
    return Result<Incarnation>::failure(host.status.code, host.status.message);
  }
  Incarnation incarnation;
  incarnation.boot_sequence = boot.value;
  incarnation.instance = instance.value;
  incarnation.host_label = host.value;
  return Result<Incarnation>::success(std::move(incarnation));
}

void write_incarnation(TextWriter& writer, std::string_view prefix, const Incarnation& incarnation) {
  writer.add(std::string(prefix) + "boot", incarnation.boot_sequence);
  writer.add(std::string(prefix) + "instance", digest_text(incarnation.instance));
  writer.add(std::string(prefix) + "host", incarnation.host_label);
}

Result<AuthorityScope> read_scope(TextReader& reader, std::string_view prefix) {
  const std::string kind_key = std::string(prefix) + "kind";
  const std::string site_key = std::string(prefix) + "site";
  const Result<std::string> kind_token = reader.require_string(kind_key);
  if (!kind_token.ok()) {
    return Result<AuthorityScope>::failure(kind_token.status.code, kind_token.status.message);
  }
  AuthorityScope scope;
  if (kind_token.value == "none") {
    scope.kind = ScopeKind::None;
  } else if (kind_token.value == "global") {
    scope.kind = ScopeKind::Global;
  } else if (kind_token.value == "site") {
    scope.kind = ScopeKind::Site;
    const Result<SiteId> site = read_id<SiteId>(reader, site_key);
    if (!site.ok()) {
      return Result<AuthorityScope>::failure(site.status.code, site.status.message);
    }
    scope.site = site.value;
  } else {
    return Result<AuthorityScope>::failure(ErrorCode::InvalidArgument,
                                           "a persisted authority scope kind is unknown");
  }
  if (scope.kind != ScopeKind::Site) {
    // A non-site scope carries no site; consume the key only when present so
    // that the strict reader reports an unexpected key otherwise.
    if (reader.has(site_key)) {
      const Result<std::string> ignored = reader.require_string(site_key);
      (void)ignored;
    }
  }
  return Result<AuthorityScope>::success(scope);
}

void write_scope(TextWriter& writer, std::string_view prefix, const AuthorityScope& scope) {
  switch (scope.kind) {
    case ScopeKind::None: writer.add(std::string(prefix) + "kind", "none"); break;
    case ScopeKind::Global: writer.add(std::string(prefix) + "kind", "global"); break;
    case ScopeKind::Site: writer.add(std::string(prefix) + "kind", "site"); break;
  }
  if (scope.kind == ScopeKind::Site) {
    writer.add(std::string(prefix) + "site", scope.site.to_string());
  }
}

Result<RefusalCode> read_refusal(TextReader& reader, std::string_view key) {
  const Result<std::string> token = reader.require_string(key);
  if (!token.ok()) {
    return Result<RefusalCode>::failure(token.status.code, token.status.message);
  }
  static constexpr std::array<RefusalCode, 31> codes = {
      RefusalCode::None, RefusalCode::InvalidArgument, RefusalCode::UnknownResource,
      RefusalCode::NameInvalid, RefusalCode::IdentityCollision, RefusalCode::UnsupportedCapability,
      RefusalCode::UnsupportedEvidenceKind, RefusalCode::MissingEvidence, RefusalCode::StaleEvidence,
      RefusalCode::ConflictingEvidence, RefusalCode::InvalidEvidence, RefusalCode::StaleGeneration,
      RefusalCode::StaleEpoch, RefusalCode::StaleIncarnation, RefusalCode::StaleReservation,
      RefusalCode::ReservationExpired, RefusalCode::ReservationNotHeld, RefusalCode::ReservationConflict,
      RefusalCode::ConflictingClaim, RefusalCode::DuplicateClaim, RefusalCode::IllegalTransition,
      RefusalCode::ObjectRetired, RefusalCode::ObjectActive, RefusalCode::ObjectNotActive,
      RefusalCode::NotAuthoritative, RefusalCode::AttemptConflict, RefusalCode::PlannerUnavailable,
      RefusalCode::RouteUnresolved, RefusalCode::CapacityExceeded, RefusalCode::InvariantViolation,
      RefusalCode::PersistenceFailure};
  for (const RefusalCode code : codes) {
    if (to_string(code) == token.value) {
      return Result<RefusalCode>::success(code);
    }
  }
  return Result<RefusalCode>::failure(ErrorCode::InvalidArgument, "a persisted refusal code is unknown");
}

Result<OutcomeCode> read_outcome(TextReader& reader, std::string_view key) {
  const Result<std::string> token = reader.require_string(key);
  if (!token.ok()) {
    return Result<OutcomeCode>::failure(token.status.code, token.status.message);
  }
  static constexpr std::array<OutcomeCode, 4> codes = {OutcomeCode::Applied, OutcomeCode::IdempotentReplay,
                                                       OutcomeCode::AlreadySatisfied, OutcomeCode::Refused};
  for (const OutcomeCode code : codes) {
    if (to_string(code) == token.value) {
      return Result<OutcomeCode>::success(code);
    }
  }
  return Result<OutcomeCode>::failure(ErrorCode::InvalidArgument, "a persisted outcome code is unknown");
}

Result<ConnectivityState> read_state(TextReader& reader, std::string_view key) {
  const Result<std::string> token = reader.require_string(key);
  if (!token.ok()) {
    return Result<ConnectivityState>::failure(token.status.code, token.status.message);
  }
  ConnectivityState state = ConnectivityState::Proposed;
  if (!connectivity_state_from_string(token.value, state)) {
    return Result<ConnectivityState>::failure(ErrorCode::InvalidArgument,
                                              "a persisted connectivity state is unknown");
  }
  return Result<ConnectivityState>::success(state);
}

Result<SharingMode> read_sharing(TextReader& reader, std::string_view key) {
  const Result<std::string> token = reader.require_string(key);
  if (!token.ok()) {
    return Result<SharingMode>::failure(token.status.code, token.status.message);
  }
  if (token.value == "exclusive") {
    return Result<SharingMode>::success(SharingMode::Exclusive);
  }
  if (token.value == "channelized") {
    return Result<SharingMode>::success(SharingMode::Channelized);
  }
  return Result<SharingMode>::failure(ErrorCode::InvalidArgument, "a persisted sharing mode is unknown");
}

}  // namespace

void encode_resource(TextWriter& writer, const ResourceState& state) {
  writer.add("resource", state.resource.to_string());
  writer.add("name", state.name);
  writer.add("sharing", to_string(state.sharing));
  writer.add("generation", state.generation.value);
  writer.add("registered_tick", state.registered_tick.value);
  writer.add("role", state.role);
  writer.add("region", state.region);
  writer.add("description", state.description);
  writer.add("band", state.band);
  writer.add("channel_index", state.channel_index);
  writer.add("nominal_frequency_ghz", state.nominal_frequency_ghz);
  writer.add("declared_length_metres", state.declared_length_metres);
  writer.add("site", state.site.to_string());
  writer.add("node", state.node.to_string());
  writer.add("endpoint_a", state.endpoint_a.to_string());
  writer.add("endpoint_b", state.endpoint_b.to_string());
  writer.add("ingress", state.ingress.to_string());
  writer.add("egress", state.egress.to_string());
  writer.add("related.count", static_cast<std::uint64_t>(state.related.size()));
  for (std::size_t index = 0; index < state.related.size(); ++index) {
    writer.add(indexed_key("related", index, ""), state.related[index].to_string());
  }
  writer.add("attributes.count", static_cast<std::uint64_t>(state.attributes.size()));
  for (std::size_t index = 0; index < state.attributes.size(); ++index) {
    writer.add(indexed_key("attributes", index, ""), state.attributes[index]);
  }
  writer.add("spans.count", static_cast<std::uint64_t>(state.spans.size()));
  for (std::size_t index = 0; index < state.spans.size(); ++index) {
    writer.add(indexed_key("spans", index, ""), state.spans[index].to_string());
  }
  writer.add("endpoints.count", static_cast<std::uint64_t>(state.endpoints.size()));
  for (std::size_t index = 0; index < state.endpoints.size(); ++index) {
    writer.add(indexed_key("endpoints", index, ""), state.endpoints[index].to_string());
  }
}

Result<ResourceState> decode_resource(TextReader& reader) {
  ResourceState state;
  const Result<ResourceRef> resource = read_resource(reader, "resource");
  if (!resource.ok()) {
    return Result<ResourceState>::failure(resource.status.code, resource.status.message);
  }
  state.resource = resource.value;
  const Result<std::string> name = reader.require_string("name");
  if (!name.ok()) {
    return Result<ResourceState>::failure(name.status.code, name.status.message);
  }
  state.name = name.value;
  const Result<SharingMode> sharing = read_sharing(reader, "sharing");
  if (!sharing.ok()) {
    return Result<ResourceState>::failure(sharing.status.code, sharing.status.message);
  }
  state.sharing = sharing.value;
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) {
    return Result<ResourceState>::failure(generation.status.code, generation.status.message);
  }
  state.generation = Generation{generation.value};
  const Result<std::uint64_t> registered = reader.require_u64("registered_tick");
  if (!registered.ok()) {
    return Result<ResourceState>::failure(registered.status.code, registered.status.message);
  }
  state.registered_tick = Tick{registered.value};
  const auto read_optional_text = [&](std::string_view key, std::string& target) -> Status {
    const Result<std::string> value = reader.require_string(key);
    if (!value.ok()) {
      return value.status;
    }
    target = value.value;
    return Status{};
  };
  Status status = read_optional_text("role", state.role);
  if (!status.ok()) return Result<ResourceState>::failure(status.code, status.message);
  status = read_optional_text("region", state.region);
  if (!status.ok()) return Result<ResourceState>::failure(status.code, status.message);
  status = read_optional_text("description", state.description);
  if (!status.ok()) return Result<ResourceState>::failure(status.code, status.message);
  status = read_optional_text("band", state.band);
  if (!status.ok()) return Result<ResourceState>::failure(status.code, status.message);
  const Result<std::uint64_t> channel_index = reader.require_u64("channel_index");
  if (!channel_index.ok()) {
    return Result<ResourceState>::failure(channel_index.status.code, channel_index.status.message);
  }
  state.channel_index = static_cast<std::uint32_t>(channel_index.value);
  const Result<std::uint64_t> frequency = reader.require_u64("nominal_frequency_ghz");
  if (!frequency.ok()) {
    return Result<ResourceState>::failure(frequency.status.code, frequency.status.message);
  }
  state.nominal_frequency_ghz = frequency.value;
  const Result<std::uint64_t> length = reader.require_u64("declared_length_metres");
  if (!length.ok()) {
    return Result<ResourceState>::failure(length.status.code, length.status.message);
  }
  state.declared_length_metres = length.value;

  const auto read_site = [&](std::string_view key, SiteId& target) -> Status {
    const Result<SiteId> value = read_id<SiteId>(reader, key);
    if (!value.ok()) {
      return value.status;
    }
    target = value.value;
    return Status{};
  };
  status = read_site("site", state.site);
  if (!status.ok()) return Result<ResourceState>::failure(status.code, status.message);
  const Result<OpticalNodeId> node = read_id<OpticalNodeId>(reader, "node");
  if (!node.ok()) return Result<ResourceState>::failure(node.status.code, node.status.message);
  state.node = node.value;
  const Result<PortId> endpoint_a = read_id<PortId>(reader, "endpoint_a");
  if (!endpoint_a.ok()) return Result<ResourceState>::failure(endpoint_a.status.code, endpoint_a.status.message);
  state.endpoint_a = endpoint_a.value;
  const Result<PortId> endpoint_b = read_id<PortId>(reader, "endpoint_b");
  if (!endpoint_b.ok()) return Result<ResourceState>::failure(endpoint_b.status.code, endpoint_b.status.message);
  state.endpoint_b = endpoint_b.value;
  const Result<PortId> ingress = read_id<PortId>(reader, "ingress");
  if (!ingress.ok()) return Result<ResourceState>::failure(ingress.status.code, ingress.status.message);
  state.ingress = ingress.value;
  const Result<PortId> egress = read_id<PortId>(reader, "egress");
  if (!egress.ok()) return Result<ResourceState>::failure(egress.status.code, egress.status.message);
  state.egress = egress.value;

  const Result<std::uint64_t> related_count = reader.group_count("related");
  if (!related_count.ok()) {
    return Result<ResourceState>::failure(related_count.status.code, related_count.status.message);
  }
  for (std::uint64_t index = 0; index < related_count.value; ++index) {
    const Result<ResourceRef> item = read_resource(reader, indexed_key("related", index, ""));
    if (!item.ok()) return Result<ResourceState>::failure(item.status.code, item.status.message);
    state.related.push_back(item.value);
  }
  const Result<std::uint64_t> attribute_count = reader.group_count("attributes");
  if (!attribute_count.ok()) {
    return Result<ResourceState>::failure(attribute_count.status.code, attribute_count.status.message);
  }
  for (std::uint64_t index = 0; index < attribute_count.value; ++index) {
    const Result<std::string> item = reader.require_string(indexed_key("attributes", index, ""));
    if (!item.ok()) return Result<ResourceState>::failure(item.status.code, item.status.message);
    state.attributes.push_back(item.value);
  }
  const Result<std::uint64_t> span_count = reader.group_count("spans");
  if (!span_count.ok()) {
    return Result<ResourceState>::failure(span_count.status.code, span_count.status.message);
  }
  for (std::uint64_t index = 0; index < span_count.value; ++index) {
    const Result<SpanId> item = read_id<SpanId>(reader, indexed_key("spans", index, ""));
    if (!item.ok()) return Result<ResourceState>::failure(item.status.code, item.status.message);
    state.spans.push_back(item.value);
  }
  const Result<std::uint64_t> endpoint_count = reader.group_count("endpoints");
  if (!endpoint_count.ok()) {
    return Result<ResourceState>::failure(endpoint_count.status.code, endpoint_count.status.message);
  }
  for (std::uint64_t index = 0; index < endpoint_count.value; ++index) {
    const Result<PortId> item = read_id<PortId>(reader, indexed_key("endpoints", index, ""));
    if (!item.ok()) return Result<ResourceState>::failure(item.status.code, item.status.message);
    state.endpoints.push_back(item.value);
  }
  return Result<ResourceState>::success(std::move(state));
}

void encode_authority(TextWriter& writer, const AuthorityToken& token) {
  writer.add("grant", token.grant.to_string());
  writer.add("holder", token.holder.to_string());
  writer.add("epoch", token.epoch.value);
  writer.add("generation", token.generation.value);
  write_scope(writer, "scope_", token.scope);
  write_incarnation(writer, "incarnation_", token.incarnation);
}

Result<AuthorityToken> decode_authority(TextReader& reader) {
  AuthorityToken token;
  const Result<GrantId> grant = read_id<GrantId>(reader, "grant");
  if (!grant.ok()) return Result<AuthorityToken>::failure(grant.status.code, grant.status.message);
  token.grant = grant.value;
  const Result<ControllerId> holder = read_id<ControllerId>(reader, "holder");
  if (!holder.ok()) return Result<AuthorityToken>::failure(holder.status.code, holder.status.message);
  token.holder = holder.value;
  const Result<std::uint64_t> epoch = reader.require_u64("epoch");
  if (!epoch.ok()) return Result<AuthorityToken>::failure(epoch.status.code, epoch.status.message);
  token.epoch = Epoch{epoch.value};
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<AuthorityToken>::failure(generation.status.code, generation.status.message);
  token.generation = Generation{generation.value};
  const Result<AuthorityScope> scope = read_scope(reader, "scope_");
  if (!scope.ok()) return Result<AuthorityToken>::failure(scope.status.code, scope.status.message);
  token.scope = scope.value;
  const Result<Incarnation> incarnation = read_incarnation(reader, "incarnation_");
  if (!incarnation.ok()) {
    return Result<AuthorityToken>::failure(incarnation.status.code, incarnation.status.message);
  }
  token.incarnation = incarnation.value;
  return Result<AuthorityToken>::success(std::move(token));
}

void encode_path(TextWriter& writer, const PathDescriptor& path) {
  writer.add("seg.count", static_cast<std::uint64_t>(path.segments.size()));
  for (std::size_t index = 0; index < path.segments.size(); ++index) {
    const PathSegment& segment = path.segments[index];
    writer.add(indexed_key("seg", index, "role"), to_string(segment.role));
    writer.add(indexed_key("seg", index, "resource"), segment.resource.to_string());
    writer.add(indexed_key("seg", index, "generation"), segment.generation.value);
  }
  writer.add("channel", path.channel.to_string());
  writer.add("channel_generation", path.channel_generation.value);
  writer.add("direction", to_string(path.direction));
  writer.add("route_origin", path.route_origin);
}

Result<PathDescriptor> decode_path(TextReader& reader) {
  PathDescriptor path;
  const Result<std::uint64_t> count = reader.group_count("seg");
  if (!count.ok()) return Result<PathDescriptor>::failure(count.status.code, count.status.message);
  for (std::uint64_t index = 0; index < count.value; ++index) {
    PathSegment segment;
    const Result<std::string> role_token = reader.require_string(indexed_key("seg", index, "role"));
    if (!role_token.ok()) return Result<PathDescriptor>::failure(role_token.status.code, role_token.status.message);
    if (!segment_role_from_string(role_token.value, segment.role)) {
      return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument, "a persisted segment role is unknown");
    }
    const Result<ResourceRef> resource = read_resource(reader, indexed_key("seg", index, "resource"));
    if (!resource.ok()) return Result<PathDescriptor>::failure(resource.status.code, resource.status.message);
    segment.resource = resource.value;
    const Result<std::uint64_t> generation = reader.require_u64(indexed_key("seg", index, "generation"));
    if (!generation.ok()) return Result<PathDescriptor>::failure(generation.status.code, generation.status.message);
    segment.generation = Generation{generation.value};
    path.segments.push_back(segment);
  }
  const Result<ChannelId> channel = read_id<ChannelId>(reader, "channel");
  if (!channel.ok()) return Result<PathDescriptor>::failure(channel.status.code, channel.status.message);
  path.channel = channel.value;
  const Result<std::uint64_t> channel_generation = reader.require_u64("channel_generation");
  if (!channel_generation.ok()) {
    return Result<PathDescriptor>::failure(channel_generation.status.code, channel_generation.status.message);
  }
  path.channel_generation = Generation{channel_generation.value};
  const Result<std::string> direction = reader.require_string("direction");
  if (!direction.ok()) return Result<PathDescriptor>::failure(direction.status.code, direction.status.message);
  if (!direction_from_string(direction.value, path.direction)) {
    return Result<PathDescriptor>::failure(ErrorCode::InvalidArgument, "a persisted direction is unknown");
  }
  const Result<std::string> origin = reader.require_string("route_origin");
  if (!origin.ok()) return Result<PathDescriptor>::failure(origin.status.code, origin.status.message);
  path.route_origin = origin.value;
  return Result<PathDescriptor>::success(std::move(path));
}

void encode_canonical(TextWriter& writer, const CanonicalPath& canonical) {
  writer.add("identity", digest_text(canonical.identity));
  writer.add("segment_count", static_cast<std::uint64_t>(canonical.segment_count));
}

Result<CanonicalPath> decode_canonical(TextReader& reader) {
  CanonicalPath canonical;
  const Result<Digest128> identity = read_digest(reader, "identity");
  if (!identity.ok()) return Result<CanonicalPath>::failure(identity.status.code, identity.status.message);
  canonical.identity = identity.value;
  const Result<std::uint64_t> count = reader.require_u64("segment_count");
  if (!count.ok()) return Result<CanonicalPath>::failure(count.status.code, count.status.message);
  canonical.segment_count = static_cast<std::uint32_t>(count.value);
  return Result<CanonicalPath>::success(std::move(canonical));
}

void encode_transition(TextWriter& writer, const TransitionRecord& record) {
  writer.add("from", to_string(record.from));
  writer.add("to", to_string(record.to));
  writer.add("outcome", to_string(record.outcome));
  writer.add("refusal", to_string(record.refusal));
  writer.add("attempt", record.attempt.to_string());
  writer.add("generation", record.generation.value);
  writer.add("epoch", record.epoch.value);
  writer.add("tick", record.tick.value);
  writer.add("detail", record.detail);
}

Result<TransitionRecord> decode_transition(TextReader& reader) {
  TransitionRecord record;
  const Result<ConnectivityState> from = read_state(reader, "from");
  if (!from.ok()) return Result<TransitionRecord>::failure(from.status.code, from.status.message);
  record.from = from.value;
  const Result<ConnectivityState> to = read_state(reader, "to");
  if (!to.ok()) return Result<TransitionRecord>::failure(to.status.code, to.status.message);
  record.to = to.value;
  const Result<OutcomeCode> outcome = read_outcome(reader, "outcome");
  if (!outcome.ok()) return Result<TransitionRecord>::failure(outcome.status.code, outcome.status.message);
  record.outcome = outcome.value;
  const Result<RefusalCode> refusal = read_refusal(reader, "refusal");
  if (!refusal.ok()) return Result<TransitionRecord>::failure(refusal.status.code, refusal.status.message);
  record.refusal = refusal.value;
  const Result<AttemptId> attempt = read_id<AttemptId>(reader, "attempt");
  if (!attempt.ok()) return Result<TransitionRecord>::failure(attempt.status.code, attempt.status.message);
  record.attempt = attempt.value;
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<TransitionRecord>::failure(generation.status.code, generation.status.message);
  record.generation = Generation{generation.value};
  const Result<std::uint64_t> epoch = reader.require_u64("epoch");
  if (!epoch.ok()) return Result<TransitionRecord>::failure(epoch.status.code, epoch.status.message);
  record.epoch = Epoch{epoch.value};
  const Result<std::uint64_t> tick = reader.require_u64("tick");
  if (!tick.ok()) return Result<TransitionRecord>::failure(tick.status.code, tick.status.message);
  record.tick = Tick{tick.value};
  const Result<std::string> detail = reader.require_string("detail");
  if (!detail.ok()) return Result<TransitionRecord>::failure(detail.status.code, detail.status.message);
  record.detail = detail.value;
  return Result<TransitionRecord>::success(std::move(record));
}


void encode_connectivity(TextWriter& writer, const ConnectivityRecord& record) {
  writer.add("id", record.id.to_string());
  writer.add("name", record.name);
  writer.add("owner", record.owner);
  writer.add("state", to_string(record.state));
  writer.add("generation", record.generation.value);
  writer.add("created_tick", record.created_tick.value);
  writer.add("updated_tick", record.updated_tick.value);
  writer.add("activated_tick", record.activated_tick.value);
  writer.add("retired_tick", record.retired_tick.value);
  writer.add("last_refusal", to_string(record.last_refusal));
  writer.add("last_refusal_detail", record.last_refusal_detail);
  writer.add("identity", digest_text(record.canonical.identity));
  encode_path(writer, record.path);
  writer.push_prefix("authority_");
  encode_authority(writer, record.authority);
  writer.pop_prefix();
  writer.add("authority_confirmed", record.authority_confirmed ? 1u : 0u);
  writer.add("reservation", record.reservation.to_string());
  writer.add("has_reservation", record.has_reservation ? 1u : 0u);
  writer.add("reservation_expiry", record.reservation_expiry.value);
  writer.add("activation_digest", digest_text(record.activation_digest));
  writer.add("activation_attempt", record.activation_attempt.to_string());
  writer.add("transitions_dropped", static_cast<std::uint64_t>(record.transitions_dropped));
  writer.add("history.count", static_cast<std::uint64_t>(record.history.size()));
  for (std::size_t index = 0; index < record.history.size(); ++index) {
    std::string prefix = indexed_key("history", index, "");
    prefix.push_back('.');
    writer.push_prefix(prefix);
    encode_transition(writer, record.history[index]);
    writer.pop_prefix();
  }
  writer.add("attempt_log.count", static_cast<std::uint64_t>(record.attempt_log.size()));
  for (std::size_t index = 0; index < record.attempt_log.size(); ++index) {
    writer.add(indexed_key("attempt_log", index, ""), record.attempt_log[index].to_string());
  }
  writer.add("require_capability", record.requirements.require_capability_evidence ? 1u : 0u);
  writer.add("require_operational", record.requirements.require_operational_evidence ? 1u : 0u);
  writer.add("require_attachment", record.requirements.require_attachment_evidence ? 1u : 0u);
  writer.add("allow_unsupported_advisory", record.requirements.allow_unsupported_advisory ? 1u : 0u);
  writer.add("additional.count", static_cast<std::uint64_t>(record.requirements.additional_required_kinds.size()));
  for (std::size_t index = 0; index < record.requirements.additional_required_kinds.size(); ++index) {
    writer.add(indexed_key("additional", index, ""),
               to_string(record.requirements.additional_required_kinds[index]));
  }
}

Result<ConnectivityRecord> decode_connectivity(TextReader& reader, const Limits& limits) {
  ConnectivityRecord record;
  const Result<ConnectivityId> id = read_id<ConnectivityId>(reader, "id");
  if (!id.ok()) return Result<ConnectivityRecord>::failure(id.status.code, id.status.message);
  record.id = id.value;
  const Result<std::string> name = reader.require_string("name");
  if (!name.ok()) return Result<ConnectivityRecord>::failure(name.status.code, name.status.message);
  record.name = name.value;
  const Result<std::string> owner = reader.require_string("owner");
  if (!owner.ok()) return Result<ConnectivityRecord>::failure(owner.status.code, owner.status.message);
  record.owner = owner.value;
  const Result<ConnectivityState> state = read_state(reader, "state");
  if (!state.ok()) return Result<ConnectivityRecord>::failure(state.status.code, state.status.message);
  record.state = state.value;
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<ConnectivityRecord>::failure(generation.status.code, generation.status.message);
  record.generation = Generation{generation.value};
  const auto read_tick = [&](std::string_view key, Tick& target) -> Status {
    const Result<std::uint64_t> value = reader.require_u64(key);
    if (!value.ok()) {
      return value.status;
    }
    target = Tick{value.value};
    return Status{};
  };
  Status status = read_tick("created_tick", record.created_tick);
  if (!status.ok()) return Result<ConnectivityRecord>::failure(status.code, status.message);
  status = read_tick("updated_tick", record.updated_tick);
  if (!status.ok()) return Result<ConnectivityRecord>::failure(status.code, status.message);
  status = read_tick("activated_tick", record.activated_tick);
  if (!status.ok()) return Result<ConnectivityRecord>::failure(status.code, status.message);
  status = read_tick("retired_tick", record.retired_tick);
  if (!status.ok()) return Result<ConnectivityRecord>::failure(status.code, status.message);
  const Result<RefusalCode> refusal = read_refusal(reader, "last_refusal");
  if (!refusal.ok()) return Result<ConnectivityRecord>::failure(refusal.status.code, refusal.status.message);
  record.last_refusal = refusal.value;
  const Result<std::string> refusal_detail = reader.require_string("last_refusal_detail");
  if (!refusal_detail.ok()) {
    return Result<ConnectivityRecord>::failure(refusal_detail.status.code, refusal_detail.status.message);
  }
  record.last_refusal_detail = refusal_detail.value;
  const Result<Digest128> identity = read_digest(reader, "identity");
  if (!identity.ok()) return Result<ConnectivityRecord>::failure(identity.status.code, identity.status.message);
  const Result<PathDescriptor> path = decode_path(reader);
  if (!path.ok()) return Result<ConnectivityRecord>::failure(path.status.code, path.status.message);
  record.path = path.value;

  const Result<CanonicalPath> canonical = canonicalize_path(record.path, limits);
  if (!canonical.ok()) {
    return Result<ConnectivityRecord>::failure(canonical.status.code, canonical.status.message);
  }
  if (!(canonical.value.identity == identity.value)) {
    return Result<ConnectivityRecord>::failure(
        ErrorCode::StoreCorrupt, "a persisted path does not match its recorded identity");
  }
  record.canonical = canonical.value;

  reader.push_prefix_scope("authority_");
  const Result<AuthorityToken> authority = decode_authority(reader);
  reader.pop_prefix_scope();
  if (!authority.ok()) return Result<ConnectivityRecord>::failure(authority.status.code, authority.status.message);
  record.authority = authority.value;
  const Result<bool> confirmed = reader.require_bool("authority_confirmed");
  if (!confirmed.ok()) return Result<ConnectivityRecord>::failure(confirmed.status.code, confirmed.status.message);
  record.authority_confirmed = confirmed.value;
  const Result<ReservationId> reservation = read_id<ReservationId>(reader, "reservation");
  if (!reservation.ok()) return Result<ConnectivityRecord>::failure(reservation.status.code, reservation.status.message);
  record.reservation = reservation.value;
  const Result<bool> has_reservation = reader.require_bool("has_reservation");
  if (!has_reservation.ok()) {
    return Result<ConnectivityRecord>::failure(has_reservation.status.code, has_reservation.status.message);
  }
  record.has_reservation = has_reservation.value;
  status = read_tick("reservation_expiry", record.reservation_expiry);
  if (!status.ok()) return Result<ConnectivityRecord>::failure(status.code, status.message);
  const Result<Digest128> activation_digest = read_digest(reader, "activation_digest");
  if (!activation_digest.ok()) {
    return Result<ConnectivityRecord>::failure(activation_digest.status.code, activation_digest.status.message);
  }
  record.activation_digest = activation_digest.value;
  const Result<AttemptId> activation_attempt = read_id<AttemptId>(reader, "activation_attempt");
  if (!activation_attempt.ok()) {
    return Result<ConnectivityRecord>::failure(activation_attempt.status.code, activation_attempt.status.message);
  }
  record.activation_attempt = activation_attempt.value;
  const Result<std::uint64_t> dropped = reader.require_u64("transitions_dropped");
  if (!dropped.ok()) return Result<ConnectivityRecord>::failure(dropped.status.code, dropped.status.message);
  record.transitions_dropped = static_cast<std::size_t>(dropped.value);
  const Result<std::uint64_t> history_count = reader.group_count("history");
  if (!history_count.ok()) {
    return Result<ConnectivityRecord>::failure(history_count.status.code, history_count.status.message);
  }
  if (history_count.value > limits.max_attempt_history_per_object) {
    return Result<ConnectivityRecord>::failure(ErrorCode::CapacityExceeded,
                                               "a persisted transition history exceeds the bound");
  }
  for (std::uint64_t index = 0; index < history_count.value; ++index) {
    const Result<TextReader> group = reader.group("history", index);
    if (!group.ok()) return Result<ConnectivityRecord>::failure(group.status.code, group.status.message);
    TextReader sub = group.value;
    const Result<TransitionRecord> transition = decode_transition(sub);
    if (!transition.ok()) return Result<ConnectivityRecord>::failure(transition.status.code, transition.status.message);
    const Status finished = sub.finish();
    if (!finished.ok()) return Result<ConnectivityRecord>::failure(finished.code, finished.message);
    record.history.push_back(transition.value);
  }
  const Result<std::uint64_t> attempt_count = reader.group_count("attempt_log");
  if (!attempt_count.ok()) {
    return Result<ConnectivityRecord>::failure(attempt_count.status.code, attempt_count.status.message);
  }
  if (attempt_count.value > limits.max_attempt_history_per_object) {
    return Result<ConnectivityRecord>::failure(ErrorCode::CapacityExceeded,
                                               "a persisted attempt log exceeds the bound");
  }
  for (std::uint64_t index = 0; index < attempt_count.value; ++index) {
    const Result<AttemptId> item = read_id<AttemptId>(reader, indexed_key("attempt_log", index, ""));
    if (!item.ok()) return Result<ConnectivityRecord>::failure(item.status.code, item.status.message);
    record.attempt_log.push_back(item.value);
  }
  const Result<bool> require_capability = reader.require_bool("require_capability");
  if (!require_capability.ok()) {
    return Result<ConnectivityRecord>::failure(require_capability.status.code, require_capability.status.message);
  }
  record.requirements.require_capability_evidence = require_capability.value;
  const Result<bool> require_operational = reader.require_bool("require_operational");
  if (!require_operational.ok()) {
    return Result<ConnectivityRecord>::failure(require_operational.status.code, require_operational.status.message);
  }
  record.requirements.require_operational_evidence = require_operational.value;
  const Result<bool> require_attachment = reader.require_bool("require_attachment");
  if (!require_attachment.ok()) {
    return Result<ConnectivityRecord>::failure(require_attachment.status.code, require_attachment.status.message);
  }
  record.requirements.require_attachment_evidence = require_attachment.value;
  const Result<bool> allow_advisory = reader.require_bool("allow_unsupported_advisory");
  if (!allow_advisory.ok()) {
    return Result<ConnectivityRecord>::failure(allow_advisory.status.code, allow_advisory.status.message);
  }
  record.requirements.allow_unsupported_advisory = allow_advisory.value;
  const Result<std::uint64_t> additional_count = reader.group_count("additional");
  if (!additional_count.ok()) {
    return Result<ConnectivityRecord>::failure(additional_count.status.code, additional_count.status.message);
  }
  if (additional_count.value > kEvidenceKindCount) {
    return Result<ConnectivityRecord>::failure(ErrorCode::CapacityExceeded,
                                               "a persisted requirement list exceeds the bound");
  }
  for (std::uint64_t index = 0; index < additional_count.value; ++index) {
    const Result<std::string> token = reader.require_string(indexed_key("additional", index, ""));
    if (!token.ok()) return Result<ConnectivityRecord>::failure(token.status.code, token.status.message);
    EvidenceKind kind = EvidenceKind::TopologyPresence;
    if (!evidence_kind_from_string(token.value, kind)) {
      return Result<ConnectivityRecord>::failure(ErrorCode::InvalidArgument,
                                                 "a persisted evidence requirement is unknown");
    }
    record.requirements.additional_required_kinds.push_back(kind);
  }
  return Result<ConnectivityRecord>::success(std::move(record));
}

void encode_evidence(TextWriter& writer, const EvidenceRecord& record) {
  writer.add("id", record.id.to_string());
  writer.add("subject", record.subject.to_string());
  writer.add("kind", to_string(record.kind));
  writer.add("state", to_string(record.state));
  writer.add("advisory", record.advisory ? 1u : 0u);
  writer.add("detail", record.detail);
  writer.add("source_runtime", record.provenance.source_runtime);
  writer.add("source_instance", record.provenance.source_instance);
  writer.add("source_id", record.provenance.source_id.to_string());
  writer.add("source_sequence", record.provenance.source_sequence);
  writer.add("source_epoch", record.provenance.source_epoch.value);
  write_incarnation(writer, "ingested_", record.provenance.ingested_incarnation);
  writer.add("observed_generation", record.provenance.observed_generation.value);
  writer.add("observed_tick", record.provenance.observed_tick.value);
  writer.add("valid_until_tick", record.provenance.valid_until_tick.value);
  writer.add("content_digest", digest_text(record.provenance.content_digest));
}

Result<EvidenceRecord> decode_evidence(TextReader& reader) {
  EvidenceRecord record;
  const Result<EvidenceId> id = read_id<EvidenceId>(reader, "id");
  if (!id.ok()) return Result<EvidenceRecord>::failure(id.status.code, id.status.message);
  record.id = id.value;
  const Result<ResourceRef> subject = read_resource(reader, "subject");
  if (!subject.ok()) return Result<EvidenceRecord>::failure(subject.status.code, subject.status.message);
  record.subject = subject.value;
  const Result<std::string> kind_token = reader.require_string("kind");
  if (!kind_token.ok()) return Result<EvidenceRecord>::failure(kind_token.status.code, kind_token.status.message);
  if (!evidence_kind_from_string(kind_token.value, record.kind)) {
    return Result<EvidenceRecord>::failure(ErrorCode::InvalidArgument, "a persisted evidence kind is unknown");
  }
  const Result<std::string> state_token = reader.require_string("state");
  if (!state_token.ok()) return Result<EvidenceRecord>::failure(state_token.status.code, state_token.status.message);
  if (!evidence_state_from_string(state_token.value, record.state)) {
    return Result<EvidenceRecord>::failure(ErrorCode::InvalidArgument, "a persisted evidence state is unknown");
  }
  const Result<bool> advisory = reader.require_bool("advisory");
  if (!advisory.ok()) return Result<EvidenceRecord>::failure(advisory.status.code, advisory.status.message);
  record.advisory = advisory.value;
  const Result<std::string> detail = reader.require_string("detail");
  if (!detail.ok()) return Result<EvidenceRecord>::failure(detail.status.code, detail.status.message);
  record.detail = detail.value;
  const Result<std::string> runtime = reader.require_string("source_runtime");
  if (!runtime.ok()) return Result<EvidenceRecord>::failure(runtime.status.code, runtime.status.message);
  record.provenance.source_runtime = runtime.value;
  const Result<std::string> instance = reader.require_string("source_instance");
  if (!instance.ok()) return Result<EvidenceRecord>::failure(instance.status.code, instance.status.message);
  record.provenance.source_instance = instance.value;
  const Result<SourceId> source_id = read_id<SourceId>(reader, "source_id");
  if (!source_id.ok()) return Result<EvidenceRecord>::failure(source_id.status.code, source_id.status.message);
  record.provenance.source_id = source_id.value;
  const Result<std::uint64_t> sequence = reader.require_u64("source_sequence");
  if (!sequence.ok()) return Result<EvidenceRecord>::failure(sequence.status.code, sequence.status.message);
  record.provenance.source_sequence = sequence.value;
  const Result<std::uint64_t> source_epoch = reader.require_u64("source_epoch");
  if (!source_epoch.ok()) return Result<EvidenceRecord>::failure(source_epoch.status.code, source_epoch.status.message);
  record.provenance.source_epoch = Epoch{source_epoch.value};
  const Result<Incarnation> ingested = read_incarnation(reader, "ingested_");
  if (!ingested.ok()) return Result<EvidenceRecord>::failure(ingested.status.code, ingested.status.message);
  record.provenance.ingested_incarnation = ingested.value;
  const Result<std::uint64_t> observed_generation = reader.require_u64("observed_generation");
  if (!observed_generation.ok()) {
    return Result<EvidenceRecord>::failure(observed_generation.status.code, observed_generation.status.message);
  }
  record.provenance.observed_generation = Generation{observed_generation.value};
  const Result<std::uint64_t> observed_tick = reader.require_u64("observed_tick");
  if (!observed_tick.ok()) {
    return Result<EvidenceRecord>::failure(observed_tick.status.code, observed_tick.status.message);
  }
  record.provenance.observed_tick = Tick{observed_tick.value};
  const Result<std::uint64_t> valid_until = reader.require_u64("valid_until_tick");
  if (!valid_until.ok()) {
    return Result<EvidenceRecord>::failure(valid_until.status.code, valid_until.status.message);
  }
  record.provenance.valid_until_tick = Tick{valid_until.value};
  const Result<Digest128> content = read_digest(reader, "content_digest");
  if (!content.ok()) return Result<EvidenceRecord>::failure(content.status.code, content.status.message);
  record.provenance.content_digest = content.value;
  return Result<EvidenceRecord>::success(std::move(record));
}

void encode_reservation(TextWriter& writer, const ReservationState& state) {
  const ReservationView& view = state.view;
  writer.add("id", view.id.to_string());
  writer.add("connectivity", view.connectivity.to_string());
  writer.add("holder", view.holder);
  writer.add("epoch", view.epoch.value);
  write_incarnation(writer, "incarnation_", view.incarnation);
  writer.add("granted_tick", view.granted_tick.value);
  writer.add("expiry_tick", view.expiry_tick.value);
  writer.add("generation", view.generation.value);
  writer.add("consumed", view.consumed ? 1u : 0u);
  writer.add("released", view.released ? 1u : 0u);
  writer.add("resources.count", static_cast<std::uint64_t>(view.resources.size()));
  for (std::size_t index = 0; index < view.resources.size(); ++index) {
    writer.add(indexed_key("resources", index, ""), view.resources[index].to_string());
  }
}

Result<ReservationState> decode_reservation(TextReader& reader) {
  ReservationState state;
  const Result<ReservationId> id = read_id<ReservationId>(reader, "id");
  if (!id.ok()) return Result<ReservationState>::failure(id.status.code, id.status.message);
  state.view.id = id.value;
  const Result<ConnectivityId> connectivity = read_id<ConnectivityId>(reader, "connectivity");
  if (!connectivity.ok()) return Result<ReservationState>::failure(connectivity.status.code, connectivity.status.message);
  state.view.connectivity = connectivity.value;
  const Result<std::string> holder = reader.require_string("holder");
  if (!holder.ok()) return Result<ReservationState>::failure(holder.status.code, holder.status.message);
  state.view.holder = holder.value;
  const Result<std::uint64_t> epoch = reader.require_u64("epoch");
  if (!epoch.ok()) return Result<ReservationState>::failure(epoch.status.code, epoch.status.message);
  state.view.epoch = Epoch{epoch.value};
  const Result<Incarnation> incarnation = read_incarnation(reader, "incarnation_");
  if (!incarnation.ok()) return Result<ReservationState>::failure(incarnation.status.code, incarnation.status.message);
  state.view.incarnation = incarnation.value;
  const Result<std::uint64_t> granted = reader.require_u64("granted_tick");
  if (!granted.ok()) return Result<ReservationState>::failure(granted.status.code, granted.status.message);
  state.view.granted_tick = Tick{granted.value};
  const Result<std::uint64_t> expiry = reader.require_u64("expiry_tick");
  if (!expiry.ok()) return Result<ReservationState>::failure(expiry.status.code, expiry.status.message);
  state.view.expiry_tick = Tick{expiry.value};
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<ReservationState>::failure(generation.status.code, generation.status.message);
  state.view.generation = Generation{generation.value};
  const Result<bool> consumed = reader.require_bool("consumed");
  if (!consumed.ok()) return Result<ReservationState>::failure(consumed.status.code, consumed.status.message);
  state.view.consumed = consumed.value;
  const Result<bool> released = reader.require_bool("released");
  if (!released.ok()) return Result<ReservationState>::failure(released.status.code, released.status.message);
  state.view.released = released.value;
  const Result<std::uint64_t> count = reader.group_count("resources");
  if (!count.ok()) return Result<ReservationState>::failure(count.status.code, count.status.message);
  for (std::uint64_t index = 0; index < count.value; ++index) {
    const Result<ResourceRef> item = read_resource(reader, indexed_key("resources", index, ""));
    if (!item.ok()) return Result<ReservationState>::failure(item.status.code, item.status.message);
    state.view.resources.push_back(item.value);
  }
  return Result<ReservationState>::success(std::move(state));
}

void encode_fence(TextWriter& writer, const FenceRecord& record) {
  writer.add("grant", record.grant.to_string());
  writer.add("holder", record.holder.to_string());
  writer.add("fenced_epoch", record.fenced_epoch.value);
  writer.add("fencing_epoch", record.fencing_epoch.value);
  writer.add("fenced_boot", record.fenced_boot_sequence);
  writer.add("fencing_boot", record.fencing_boot_sequence);
  write_scope(writer, "scope_", record.scope);
  writer.add("reason", to_string(record.reason));
  writer.add("fenced_tick", record.fenced_tick.value);
  writer.add("detail", record.detail);
}

Result<FenceRecord> decode_fence(TextReader& reader) {
  FenceRecord record;
  const Result<GrantId> grant = read_id<GrantId>(reader, "grant");
  if (!grant.ok()) return Result<FenceRecord>::failure(grant.status.code, grant.status.message);
  record.grant = grant.value;
  const Result<ControllerId> holder = read_id<ControllerId>(reader, "holder");
  if (!holder.ok()) return Result<FenceRecord>::failure(holder.status.code, holder.status.message);
  record.holder = holder.value;
  const Result<std::uint64_t> fenced_epoch = reader.require_u64("fenced_epoch");
  if (!fenced_epoch.ok()) return Result<FenceRecord>::failure(fenced_epoch.status.code, fenced_epoch.status.message);
  record.fenced_epoch = Epoch{fenced_epoch.value};
  const Result<std::uint64_t> fencing_epoch = reader.require_u64("fencing_epoch");
  if (!fencing_epoch.ok()) return Result<FenceRecord>::failure(fencing_epoch.status.code, fencing_epoch.status.message);
  record.fencing_epoch = Epoch{fencing_epoch.value};
  const Result<std::uint64_t> fenced_boot = reader.require_u64("fenced_boot");
  if (!fenced_boot.ok()) return Result<FenceRecord>::failure(fenced_boot.status.code, fenced_boot.status.message);
  record.fenced_boot_sequence = fenced_boot.value;
  const Result<std::uint64_t> fencing_boot = reader.require_u64("fencing_boot");
  if (!fencing_boot.ok()) return Result<FenceRecord>::failure(fencing_boot.status.code, fencing_boot.status.message);
  record.fencing_boot_sequence = fencing_boot.value;
  const Result<AuthorityScope> scope = read_scope(reader, "scope_");
  if (!scope.ok()) return Result<FenceRecord>::failure(scope.status.code, scope.status.message);
  record.scope = scope.value;
  const Result<std::string> reason = reader.require_string("reason");
  if (!reason.ok()) return Result<FenceRecord>::failure(reason.status.code, reason.status.message);
  static constexpr std::array<FenceReason, 6> reasons = {
      FenceReason::ExplicitOperatorFence, FenceReason::SupersededByNewerEpoch,
      FenceReason::SupersededByNewerIncarnation, FenceReason::StoreTakeover,
      FenceReason::AuthorityReleased, FenceReason::ScopeRevoked};
  bool matched = false;
  for (const FenceReason candidate : reasons) {
    if (to_string(candidate) == reason.value) {
      record.reason = candidate;
      matched = true;
      break;
    }
  }
  if (!matched) {
    return Result<FenceRecord>::failure(ErrorCode::InvalidArgument, "a persisted fence reason is unknown");
  }
  const Result<std::uint64_t> fenced_tick = reader.require_u64("fenced_tick");
  if (!fenced_tick.ok()) return Result<FenceRecord>::failure(fenced_tick.status.code, fenced_tick.status.message);
  record.fenced_tick = Tick{fenced_tick.value};
  const Result<std::string> detail = reader.require_string("detail");
  if (!detail.ok()) return Result<FenceRecord>::failure(detail.status.code, detail.status.message);
  record.detail = detail.value;
  return Result<FenceRecord>::success(std::move(record));
}

void encode_grant(TextWriter& writer, const GrantState& state) {
  const AuthorityGrantView& view = state.view;
  writer.add("id", view.id.to_string());
  writer.add("holder", view.holder.to_string());
  write_scope(writer, "scope_", view.scope);
  writer.add("epoch", view.epoch.value);
  write_incarnation(writer, "incarnation_", view.incarnation);
  writer.add("granted_tick", view.granted_tick.value);
  writer.add("expiry_tick", view.expiry_tick.value);
  writer.add("generation", view.generation.value);
  writer.add("fenced", view.fenced ? 1u : 0u);
  writer.add("released", view.released ? 1u : 0u);
  writer.add("has_fence", state.has_fence ? 1u : 0u);
  if (state.has_fence) {
    writer.push_prefix("fence_");
    encode_fence(writer, state.fence);
    writer.pop_prefix();
  }
}

Result<GrantState> decode_grant(TextReader& reader) {
  GrantState state;
  const Result<GrantId> id = read_id<GrantId>(reader, "id");
  if (!id.ok()) return Result<GrantState>::failure(id.status.code, id.status.message);
  state.view.id = id.value;
  const Result<ControllerId> holder = read_id<ControllerId>(reader, "holder");
  if (!holder.ok()) return Result<GrantState>::failure(holder.status.code, holder.status.message);
  state.view.holder = holder.value;
  const Result<AuthorityScope> scope = read_scope(reader, "scope_");
  if (!scope.ok()) return Result<GrantState>::failure(scope.status.code, scope.status.message);
  state.view.scope = scope.value;
  const Result<std::uint64_t> epoch = reader.require_u64("epoch");
  if (!epoch.ok()) return Result<GrantState>::failure(epoch.status.code, epoch.status.message);
  state.view.epoch = Epoch{epoch.value};
  const Result<Incarnation> incarnation = read_incarnation(reader, "incarnation_");
  if (!incarnation.ok()) return Result<GrantState>::failure(incarnation.status.code, incarnation.status.message);
  state.view.incarnation = incarnation.value;
  const Result<std::uint64_t> granted = reader.require_u64("granted_tick");
  if (!granted.ok()) return Result<GrantState>::failure(granted.status.code, granted.status.message);
  state.view.granted_tick = Tick{granted.value};
  const Result<std::uint64_t> expiry = reader.require_u64("expiry_tick");
  if (!expiry.ok()) return Result<GrantState>::failure(expiry.status.code, expiry.status.message);
  state.view.expiry_tick = Tick{expiry.value};
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<GrantState>::failure(generation.status.code, generation.status.message);
  state.view.generation = Generation{generation.value};
  const Result<bool> fenced = reader.require_bool("fenced");
  if (!fenced.ok()) return Result<GrantState>::failure(fenced.status.code, fenced.status.message);
  state.view.fenced = fenced.value;
  const Result<bool> released = reader.require_bool("released");
  if (!released.ok()) return Result<GrantState>::failure(released.status.code, released.status.message);
  state.view.released = released.value;
  const Result<bool> has_fence = reader.require_bool("has_fence");
  if (!has_fence.ok()) return Result<GrantState>::failure(has_fence.status.code, has_fence.status.message);
  state.has_fence = has_fence.value;
  if (state.has_fence) {
    reader.push_prefix_scope("fence_");
    const Result<FenceRecord> fence = decode_fence(reader);
    reader.pop_prefix_scope();
    if (!fence.ok()) return Result<GrantState>::failure(fence.status.code, fence.status.message);
    state.fence = fence.value;
  }
  return Result<GrantState>::success(std::move(state));
}

void encode_attempt(TextWriter& writer, const AttemptRecord& record) {
  writer.add("attempt", record.attempt.to_string());
  writer.add("digest", digest_text(record.request_digest));
  writer.add("operation", record.operation);
  writer.add("outcome", to_string(record.outcome));
  writer.add("refusal", to_string(record.refusal));
  writer.add("connectivity", record.connectivity.to_string());
  writer.add("reservation", record.reservation.to_string());
  writer.add("grant", record.grant.to_string());
  writer.add("generation", record.generation.value);
  writer.add("tick", record.tick.value);
  writer.add("summary", record.summary);
}

Result<AttemptRecord> decode_attempt(TextReader& reader) {
  AttemptRecord record;
  const Result<AttemptId> attempt = read_id<AttemptId>(reader, "attempt");
  if (!attempt.ok()) return Result<AttemptRecord>::failure(attempt.status.code, attempt.status.message);
  record.attempt = attempt.value;
  const Result<Digest128> digest = read_digest(reader, "digest");
  if (!digest.ok()) return Result<AttemptRecord>::failure(digest.status.code, digest.status.message);
  record.request_digest = digest.value;
  const Result<std::string> operation = reader.require_string("operation");
  if (!operation.ok()) return Result<AttemptRecord>::failure(operation.status.code, operation.status.message);
  record.operation = operation.value;
  const Result<OutcomeCode> outcome = read_outcome(reader, "outcome");
  if (!outcome.ok()) return Result<AttemptRecord>::failure(outcome.status.code, outcome.status.message);
  record.outcome = outcome.value;
  const Result<RefusalCode> refusal = read_refusal(reader, "refusal");
  if (!refusal.ok()) return Result<AttemptRecord>::failure(refusal.status.code, refusal.status.message);
  record.refusal = refusal.value;
  const Result<ConnectivityId> connectivity = read_id<ConnectivityId>(reader, "connectivity");
  if (!connectivity.ok()) return Result<AttemptRecord>::failure(connectivity.status.code, connectivity.status.message);
  record.connectivity = connectivity.value;
  const Result<ReservationId> reservation = read_id<ReservationId>(reader, "reservation");
  if (!reservation.ok()) return Result<AttemptRecord>::failure(reservation.status.code, reservation.status.message);
  record.reservation = reservation.value;
  const Result<GrantId> grant = read_id<GrantId>(reader, "grant");
  if (!grant.ok()) return Result<AttemptRecord>::failure(grant.status.code, grant.status.message);
  record.grant = grant.value;
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<AttemptRecord>::failure(generation.status.code, generation.status.message);
  record.generation = Generation{generation.value};
  const Result<std::uint64_t> tick = reader.require_u64("tick");
  if (!tick.ok()) return Result<AttemptRecord>::failure(tick.status.code, tick.status.message);
  record.tick = Tick{tick.value};
  const Result<std::string> summary = reader.require_string("summary");
  if (!summary.ok()) return Result<AttemptRecord>::failure(summary.status.code, summary.status.message);
  record.summary = summary.value;
  return Result<AttemptRecord>::success(std::move(record));
}

void encode_runtime(TextWriter& writer, const RuntimeState& state) {
  writer.add("epoch", state.epoch.value);
  writer.add("tick", state.tick.value);
  writer.add("generation", state.generation.value);
  writer.add("topology_generation", state.topology_generation.value);
  writer.add("boot_sequence", state.boot_sequence);
  writer.add("instance", digest_text(state.instance));
  writer.add("host_label", state.host_label);
}

Result<RuntimeState> decode_runtime(TextReader& reader) {
  RuntimeState state;
  const Result<std::uint64_t> epoch = reader.require_u64("epoch");
  if (!epoch.ok()) return Result<RuntimeState>::failure(epoch.status.code, epoch.status.message);
  state.epoch = Epoch{epoch.value};
  const Result<std::uint64_t> tick = reader.require_u64("tick");
  if (!tick.ok()) return Result<RuntimeState>::failure(tick.status.code, tick.status.message);
  state.tick = Tick{tick.value};
  const Result<std::uint64_t> generation = reader.require_u64("generation");
  if (!generation.ok()) return Result<RuntimeState>::failure(generation.status.code, generation.status.message);
  state.generation = Generation{generation.value};
  const Result<std::uint64_t> topology = reader.require_u64("topology_generation");
  if (!topology.ok()) return Result<RuntimeState>::failure(topology.status.code, topology.status.message);
  state.topology_generation = Generation{topology.value};
  const Result<std::uint64_t> boot = reader.require_u64("boot_sequence");
  if (!boot.ok()) return Result<RuntimeState>::failure(boot.status.code, boot.status.message);
  state.boot_sequence = boot.value;
  const Result<Digest128> instance = read_digest(reader, "instance");
  if (!instance.ok()) return Result<RuntimeState>::failure(instance.status.code, instance.status.message);
  state.instance = instance.value;
  const Result<std::string> host = reader.require_string("host_label");
  if (!host.ok()) return Result<RuntimeState>::failure(host.status.code, host.status.message);
  state.host_label = host.value;
  return Result<RuntimeState>::success(std::move(state));
}

void CommitBundle::add(std::string type, std::string payload) {
  items_.push_back(CommitItem{std::move(type), std::move(payload)});
}

std::string CommitBundle::encode(RecordKind kind) const {
  TextWriter writer;
  writer.token(to_string(kind));
  writer.add("commit.count", static_cast<std::uint64_t>(items_.size()));
  for (std::size_t index = 0; index < items_.size(); ++index) {
    // The group prefix always ends with the separator so that "commit.0." is
    // the namespace of the first item, which is what the reader looks for.
    std::string prefix = indexed_key("commit", index, "");
    prefix.push_back('.');
    writer.add(prefix + "type", items_[index].type);
    writer.append_block(prefix, items_[index].payload);
  }
  return writer.take();
}

Result<DecodedCommit> CommitBundle::decode(std::string_view text, const Limits& limits) {
  const Result<TextReader> parsed = TextReader::parse(text, limits);
  if (!parsed.ok()) {
    return Result<DecodedCommit>::failure(parsed.status.code, parsed.status.message);
  }
  TextReader reader = parsed.value;
  RecordKind kind = RecordKind::Unknown;
  for (std::size_t index = 0; index < kRecordKindCount; ++index) {
    const auto candidate = static_cast<RecordKind>(index);
    if (to_string(candidate) == reader.token()) {
      kind = candidate;
      break;
    }
  }
  if (kind == RecordKind::Unknown) {
    return Result<DecodedCommit>::failure(ErrorCode::InvalidArgument, "a persisted record kind is unknown");
  }
  const Result<std::uint64_t> count = reader.group_count("commit");
  if (!count.ok()) {
    return Result<DecodedCommit>::failure(count.status.code, count.status.message);
  }
  if (count.value > limits.max_journal_records) {
    return Result<DecodedCommit>::failure(ErrorCode::CapacityExceeded,
                                          "a persisted commit declares more items than the bound");
  }
  DecodedCommit decoded;
  decoded.kind = kind;
  for (std::uint64_t index = 0; index < count.value; ++index) {
    Result<TextReader> group = reader.group("commit", index);
    if (!group.ok()) {
      return Result<DecodedCommit>::failure(group.status.code, group.status.message);
    }
    const Result<std::string> type = group.value.require_string("type");
    if (!type.ok()) {
      return Result<DecodedCommit>::failure(type.status.code, type.status.message);
    }
    decoded.items.push_back(DecodedCommitItem{type.value, std::move(group.value)});
  }
  const Status finished = reader.finish();
  if (!finished.ok()) {
    return Result<DecodedCommit>::failure(finished.code, finished.message);
  }
  return Result<DecodedCommit>::success(std::move(decoded));
}

}  // namespace optical_fabric::detail
