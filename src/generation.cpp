// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/generation.hpp"

namespace optical_fabric {

namespace {

[[nodiscard]] std::string digest_suffix(const Digest128& digest) {
  if (digest.is_nil()) {
    return "nil";
  }
  return digest.to_string();
}

}  // namespace

std::string Incarnation::to_string() const {
  std::string out = "boot-";
  out.append(std::to_string(boot_sequence));
  out.push_back('/');
  out.append(digest_suffix(instance));
  return out;
}

std::string_view to_string(ScopeKind kind) noexcept {
  switch (kind) {
    case ScopeKind::None: return "none";
    case ScopeKind::Site: return "site";
    case ScopeKind::Global: return "global";
  }
  return "none";
}

bool AuthorityScope::covers(const AuthorityScope& other) const noexcept {
  if (kind == ScopeKind::None) {
    return false;
  }
  if (kind == ScopeKind::Global) {
    return true;
  }
  return other.kind == ScopeKind::Site && other.site == site;
}

bool AuthorityScope::overlaps(const AuthorityScope& other) const noexcept {
  if (kind == ScopeKind::None || other.kind == ScopeKind::None) {
    return false;
  }
  if (kind == ScopeKind::Global || other.kind == ScopeKind::Global) {
    return true;
  }
  return site == other.site;
}

std::string AuthorityScope::to_string() const {
  switch (kind) {
    case ScopeKind::None: return "none";
    case ScopeKind::Global: return "global";
    case ScopeKind::Site: return "site:" + site.to_string();
  }
  return "none";
}

std::string AuthorityToken::to_string() const {
  std::string out = "grant=";
  out.append(grant.to_string());
  out.append(" holder=");
  out.append(holder.to_string());
  out.append(" epoch=");
  out.append(epoch.to_string());
  out.append(" gen=");
  out.append(generation.to_string());
  out.append(" scope=");
  out.append(scope.to_string());
  out.append(" boot=");
  out.append(std::to_string(incarnation.boot_sequence));
  return out;
}

std::string_view to_string(AuthorityCurrentness state) noexcept {
  switch (state) {
    case AuthorityCurrentness::Current: return "current";
    case AuthorityCurrentness::NoAuthority: return "no_authority";
    case AuthorityCurrentness::UnknownGrant: return "unknown_grant";
    case AuthorityCurrentness::ScopeNotCovered: return "scope_not_covered";
    case AuthorityCurrentness::StaleEpoch: return "stale_epoch";
    case AuthorityCurrentness::StaleIncarnation: return "stale_incarnation";
    case AuthorityCurrentness::Expired: return "expired";
    case AuthorityCurrentness::HolderMismatch: return "holder_mismatch";
  }
  return "unknown";
}

std::string_view to_string(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::ExplicitOperatorFence: return "explicit_operator_fence";
    case FenceReason::SupersededByNewerEpoch: return "superseded_by_newer_epoch";
    case FenceReason::SupersededByNewerIncarnation: return "superseded_by_newer_incarnation";
    case FenceReason::StoreTakeover: return "store_takeover";
    case FenceReason::AuthorityReleased: return "authority_released";
    case FenceReason::ScopeRevoked: return "scope_revoked";
  }
  return "unknown";
}

}  // namespace optical_fabric
