// Optical Fabric 1.0.0 - Summon Software Labs
// Authority grants, renewal and fencing.
#include <algorithm>
#include <vector>

#include "fabric_impl.hpp"
#include "state_codec.hpp"

namespace optical_fabric {

namespace {

constexpr std::string_view kGrantItem = "grant";
constexpr std::string_view kFenceItem = "fence";
constexpr std::string_view kAttemptItem = "attempt";
constexpr std::string_view kRuntimeItem = "runtime";

[[nodiscard]] std::string encode_grant_item(const detail::GrantState& state) {
  detail::TextWriter writer;
  detail::encode_grant(writer, state);
  return writer.take();
}

[[nodiscard]] std::string encode_fence_item(const FenceRecord& record) {
  detail::TextWriter writer;
  detail::encode_fence(writer, record);
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

[[nodiscard]] GrantId derive_grant_id(ControllerId holder, const AuthorityScope& scope, Epoch epoch) {
  std::string canonical = holder.to_string();
  canonical.push_back('|');
  canonical.append(scope.to_string());
  canonical.push_back('|');
  canonical.append(epoch.to_string());
  return GrantId::from_value(Names::derive_composite("grant", canonical));
}

}  // namespace

bool OpticalFabric::Impl::has_overlapping_newer_epoch(const AuthorityScope& scope, Epoch candidate) const {
  for (const auto& [id, grant] : grants) {
    (void)id;
    if (grant.view.fenced || grant.view.released) {
      continue;
    }
    if (grant.view.scope.overlaps(scope) && grant.view.epoch.value > candidate.value) {
      return true;
    }
  }
  return false;
}

AuthorityCheck OpticalFabric::Impl::check_authority_locked(const AuthorityToken& token) const {
  AuthorityCheck check;
  check.current_epoch = epoch;
  check.current_boot_sequence = incarnation.boot_sequence;
  if (token.is_nil() || token.epoch.is_nil()) {
    check.currentness = AuthorityCurrentness::NoAuthority;
    check.detail = "no authority token was presented";
    return check;
  }
  const auto found = grants.find(token.grant);
  if (found == grants.end()) {
    check.currentness = AuthorityCurrentness::UnknownGrant;
    check.detail = "the presented grant does not exist in this runtime";
    return check;
  }
  const detail::GrantState& grant = found->second;
  if (grant.view.released) {
    check.currentness = AuthorityCurrentness::NoAuthority;
    check.detail = "the grant was released";
    return check;
  }
  if (grant.view.fenced) {
    check.currentness = AuthorityCurrentness::StaleEpoch;
    check.detail = std::string("the grant was fenced: ") + std::string(to_string(grant.fence.reason));
    return check;
  }
  if (!(token.holder == grant.view.holder)) {
    check.currentness = AuthorityCurrentness::HolderMismatch;
    check.detail = "the token holder is not the holder of the grant";
    return check;
  }
  if (!(token.epoch == grant.view.epoch)) {
    check.currentness = AuthorityCurrentness::StaleEpoch;
    check.detail = "the token epoch is not the epoch of the current grant";
    return check;
  }
  if (token.incarnation.boot_sequence != incarnation.boot_sequence) {
    check.currentness = AuthorityCurrentness::StaleIncarnation;
    check.detail = "the token was issued by an earlier process incarnation";
    return check;
  }
  if (!(token.scope == grant.view.scope)) {
    check.currentness = AuthorityCurrentness::ScopeNotCovered;
    check.detail = "the token scope is not the scope of the grant";
    return check;
  }
  if (tick.value >= grant.view.expiry_tick.value) {
    check.currentness = AuthorityCurrentness::Expired;
    check.detail = "the grant expired at tick " + grant.view.expiry_tick.to_string();
    return check;
  }
  if (has_overlapping_newer_epoch(grant.view.scope, grant.view.epoch)) {
    check.currentness = AuthorityCurrentness::StaleEpoch;
    check.detail = "a newer authority grant covers an overlapping scope";
    return check;
  }
  check.currentness = AuthorityCurrentness::Current;
  check.detail = "current";
  return check;
}

AuthorityCheck OpticalFabric::check_authority(const AuthorityToken& token) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->check_authority_locked(token);
}

namespace {

[[nodiscard]] RefusalCode map_currentness(AuthorityCurrentness currentness) noexcept {
  switch (currentness) {
    case AuthorityCurrentness::Current: return RefusalCode::None;
    case AuthorityCurrentness::NoAuthority: return RefusalCode::NotAuthoritative;
    case AuthorityCurrentness::UnknownGrant: return RefusalCode::NotAuthoritative;
    case AuthorityCurrentness::ScopeNotCovered: return RefusalCode::NotAuthoritative;
    case AuthorityCurrentness::StaleEpoch: return RefusalCode::StaleEpoch;
    case AuthorityCurrentness::StaleIncarnation: return RefusalCode::StaleIncarnation;
    case AuthorityCurrentness::Expired: return RefusalCode::StaleEpoch;
    case AuthorityCurrentness::HolderMismatch: return RefusalCode::NotAuthoritative;
  }
  return RefusalCode::NotAuthoritative;
}

}  // namespace

RefusalCode OpticalFabric::Impl::require_authority_locked(const AuthorityToken& token,
                                                          const AuthorityScope& required,
                                                          std::string& detail) const {
  const AuthorityCheck check = check_authority_locked(token);
  if (!check.current()) {
    detail = check.detail;
    return map_currentness(check.currentness);
  }
  if (!token.scope.covers(required)) {
    detail = "the token scope " + token.scope.to_string() + " does not cover " + required.to_string();
    return RefusalCode::NotAuthoritative;
  }
  return RefusalCode::None;
}

AuthorityScope OpticalFabric::Impl::object_scope_locked(const detail::ConnectivityRecord& record) const {
  for (const PathSegment& segment : record.path.segments) {
    const detail::ResourceState* state = find_resource(segment.resource);
    if (state != nullptr && !state->site.is_nil()) {
      return AuthorityScope::of_site(state->site);
    }
  }
  // No site could be established, so only a global grant may govern it.
  return AuthorityScope::none();
}

AuthorityResult OpticalFabric::acquire_authority(const AuthorityRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AuthorityResult result;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (request.holder.is_nil()) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument, "an authority request needs a holder");
    return result;
  }
  if (request.scope.is_none()) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument, "an authority request needs a scope");
    return result;
  }
  if (request.lease_ticks == 0 || request.lease_ticks > impl_->options.max_lease_ticks) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument,
                                 "the requested lease is outside the configured bound");
    return result;
  }
  if (impl_->grants.size() >= impl_->limits.max_authority_grants) {
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded,
                                 "the configured authority grant capacity is exhausted");
    return result;
  }
  const Epoch next_epoch = impl_->epoch.next();
  if (next_epoch.is_nil()) {
    result.refusal = Refusal::of(RefusalCode::CapacityExceeded, "the epoch counter is exhausted");
    return result;
  }
  const GrantId id = derive_grant_id(request.holder, request.scope, next_epoch);
  detail::GrantState grant;
  grant.view.id = id;
  grant.view.holder = request.holder;
  grant.view.scope = request.scope;
  grant.view.epoch = next_epoch;
  grant.view.incarnation = impl_->incarnation;
  grant.view.granted_tick = impl_->tick;
  grant.view.expiry_tick = impl_->tick.advanced_by(request.lease_ticks);
  grant.view.generation = Generation{1};

  detail::CommitBundle bundle;
  std::vector<FenceRecord> fences;
  for (auto& [existing_id, existing] : impl_->grants) {
    (void)existing_id;
    if (existing.view.fenced || existing.view.released) {
      continue;
    }
    if (!existing.view.scope.overlaps(request.scope)) {
      continue;
    }
    FenceRecord record;
    record.grant = existing.view.id;
    record.holder = existing.view.holder;
    record.fenced_epoch = existing.view.epoch;
    record.fencing_epoch = next_epoch;
    record.fenced_boot_sequence = existing.view.incarnation.boot_sequence;
    record.fencing_boot_sequence = impl_->incarnation.boot_sequence;
    record.scope = existing.view.scope;
    record.reason = FenceReason::SupersededByNewerEpoch;
    record.fenced_tick = impl_->tick;
    record.detail = request.reason.empty() ? std::string("superseded by a newer authority grant")
                                           : request.reason;
    existing.view.fenced = true;
    existing.view.generation = existing.view.generation.next();
    existing.has_fence = true;
    existing.fence = record;
    bundle.add(std::string(kGrantItem), encode_grant_item(existing));
    bundle.add(std::string(kFenceItem), encode_fence_item(record));
    fences.push_back(record);
  }
  bundle.add(std::string(kGrantItem), encode_grant_item(grant));
  detail::RuntimeState runtime = detail::RuntimeState{next_epoch, impl_->tick, impl_->generation.next(),
                                                      impl_->topology_generation,
                                                      impl_->incarnation.boot_sequence,
                                                      impl_->incarnation.instance,
                                                      impl_->incarnation.host_label};
  bundle.add(std::string(kRuntimeItem), encode_runtime_item(runtime));

  detail::AttemptRecord attempt;
  attempt.attempt = request.attempt;
  attempt.request_digest = digest_authority_request(request);
  attempt.operation = "acquire_authority";
  attempt.outcome = OutcomeCode::Applied;
  attempt.grant = id;
  attempt.tick = impl_->tick;
  attempt.generation = runtime.generation;
  attempt.summary = "authority granted";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt));

  const Status status = impl_->commit(bundle, RecordKind::AuthorityGranted);
  if (!status.ok()) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, status.message);
    return result;
  }
  result.outcome = OutcomeCode::Applied;
  result.grant = grant.view;
  result.token.grant = id;
  result.token.holder = request.holder;
  result.token.epoch = next_epoch;
  result.token.incarnation = impl_->incarnation;
  result.token.scope = request.scope;
  result.token.generation = grant.view.generation;
  (void)fences;
  return result;
}

AuthorityResult OpticalFabric::renew_authority(const AuthorityRequest& request) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  AuthorityResult result;
  if (impl_->closed) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, "the runtime is closed");
    return result;
  }
  if (request.lease_ticks == 0 || request.lease_ticks > impl_->options.max_lease_ticks) {
    result.refusal = Refusal::of(RefusalCode::InvalidArgument,
                                 "the requested lease is outside the configured bound");
    return result;
  }
  GrantId target{};
  for (const auto& [id, grant] : impl_->grants) {
    if (grant.view.fenced || grant.view.released) {
      continue;
    }
    if (!(grant.view.holder == request.holder) || !(grant.view.scope == request.scope)) {
      continue;
    }
    if (impl_->has_overlapping_newer_epoch(grant.view.scope, grant.view.epoch)) {
      continue;
    }
    if (grant.view.incarnation.boot_sequence != impl_->incarnation.boot_sequence) {
      continue;
    }
    target = id;
    break;
  }
  if (target.is_nil()) {
    result.refusal = Refusal::of(RefusalCode::NotAuthoritative,
                                 "no current grant exists for this holder and scope");
    return result;
  }
  detail::GrantState updated = impl_->grants[target];
  updated.view.expiry_tick = impl_->tick.advanced_by(request.lease_ticks);
  updated.view.generation = updated.view.generation.next();
  detail::CommitBundle bundle;
  bundle.add(std::string(kGrantItem), encode_grant_item(updated));
  detail::AttemptRecord attempt;
  attempt.attempt = request.attempt;
  attempt.request_digest = digest_authority_request(request);
  attempt.operation = "renew_authority";
  attempt.outcome = OutcomeCode::Applied;
  attempt.grant = target;
  attempt.tick = impl_->tick;
  attempt.generation = impl_->generation;
  attempt.summary = "authority renewed";
  bundle.add(std::string(kAttemptItem), encode_attempt_item(attempt));
  const Status status = impl_->commit(bundle, RecordKind::AuthorityGranted);
  if (!status.ok()) {
    result.refusal = Refusal::of(RefusalCode::PersistenceFailure, status.message);
    return result;
  }
  const detail::GrantState& stored = impl_->grants[target];
  result.outcome = OutcomeCode::Applied;
  result.grant = stored.view;
  result.token.grant = target;
  result.token.holder = stored.view.holder;
  result.token.epoch = stored.view.epoch;
  result.token.incarnation = stored.view.incarnation;
  result.token.scope = stored.view.scope;
  result.token.generation = stored.view.generation;
  return result;
}

FenceResult OpticalFabric::fence(const FenceRequest& request) {
  AuthorityRequest acquisition;
  acquisition.attempt = request.attempt;
  acquisition.holder = request.holder;
  acquisition.holder_label = request.holder_label;
  acquisition.scope = request.scope;
  acquisition.lease_ticks = request.lease_ticks;
  acquisition.reason = request.detail;
  const AuthorityResult granted = acquire_authority(acquisition);
  FenceResult result;
  result.outcome = granted.outcome;
  result.refusal = granted.refusal;
  result.token = granted.token;
  if (granted.outcome == OutcomeCode::Refused) {
    return result;
  }
  std::lock_guard<std::mutex> guard(impl_->mutex);
  // Report the most recent grant that this acquisition fenced.
  const FenceRecord* latest = nullptr;
  for (const auto& [id, grant] : impl_->grants) {
    (void)id;
    if (!grant.has_fence || grant.view.id == granted.grant.id) {
      continue;
    }
    if (grant.fence.fencing_epoch == granted.grant.epoch) {
      if (latest == nullptr || grant.fence.fenced_tick.value >= latest->fenced_tick.value) {
        latest = &grant.fence;
      }
    }
  }
  if (latest != nullptr) {
    result.fenced = *latest;
    result.fenced.reason = request.reason;
    result.fenced_anything = true;
  }
  return result;
}

Result<AuthorityGrantView> OpticalFabric::describe_grant(GrantId grant) const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const auto found = impl_->grants.find(grant);
  if (found == impl_->grants.end()) {
    return Result<AuthorityGrantView>::failure(ErrorCode::NotFound, "no grant has that identity");
  }
  return Result<AuthorityGrantView>::success(found->second.view);
}

std::vector<AuthorityView> OpticalFabric::authority_grants() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  std::vector<AuthorityView> views;
  views.reserve(impl_->grants.size());
  for (const auto& [id, grant] : impl_->grants) {
    (void)id;
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
    view.current = !grant.view.fenced && !grant.view.released &&
                   !impl_->has_overlapping_newer_epoch(grant.view.scope, grant.view.epoch) &&
                   grant.view.incarnation.boot_sequence == impl_->incarnation.boot_sequence &&
                   impl_->tick.value < grant.view.expiry_tick.value;
    views.push_back(view);
  }
  return views;
}

Digest128 digest_authority_request(const AuthorityRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "authority");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("holder", request.holder.value());
  hasher.add_field("scope", request.scope.to_string());
  hasher.add_field("lease", request.lease_ticks);
  hasher.add_field("reason", request.reason);
  return hasher.digest();
}

Digest128 digest_fence_request(const FenceRequest& request) {
  CanonicalHasher hasher;
  hasher.add_field("op", "fence");
  hasher.add_field("attempt", request.attempt.value());
  hasher.add_field("holder", request.holder.value());
  hasher.add_field("scope", request.scope.to_string());
  hasher.add_field("lease", request.lease_ticks);
  hasher.add_field("reason", static_cast<std::uint64_t>(request.reason));
  hasher.add_field("detail", request.detail);
  return hasher.digest();
}

}  // namespace optical_fabric
