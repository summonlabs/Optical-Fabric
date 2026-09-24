// Optical Fabric 1.0.0 - Summon Software Labs
// Time, generations, epochs and incarnations.
//
// Authority in this runtime is bound to three independent things and is void
// when any of them moves:
//   * generation  - a version counter of the object or resource being mutated;
//   * epoch       - a monotonically increasing control-plane term;
//   * incarnation - one process lifetime, identified by a boot sequence that is
//                   persisted and never reused.
// A stale controller fails at least one of these checks.
#pragma once

#include <compare>
#include <cstdint>
#include <string>

#include "optical_fabric/digest.hpp"
#include "optical_fabric/identity.hpp"

namespace optical_fabric {

/// Logical time. All lease and evidence validity is expressed in ticks of the
/// runtime's own clock, which is persisted across restarts, so an expiry never
/// depends on a wall clock jumping or on process uptime.
struct Tick {
  std::uint64_t value = 0;

  friend bool operator==(Tick, Tick) noexcept = default;
  friend auto operator<=>(Tick, Tick) noexcept = default;

  [[nodiscard]] Tick advanced_by(std::uint64_t delta) const noexcept { return Tick{value + delta}; }
  [[nodiscard]] std::string to_string() const { return std::to_string(value); }
};

/// Control-plane term. Every authority acquisition advances the epoch, which
/// fences every token issued under an older one.
struct Epoch {
  std::uint64_t value = 0;

  friend bool operator==(Epoch, Epoch) noexcept = default;
  friend auto operator<=>(Epoch, Epoch) noexcept = default;

  [[nodiscard]] bool is_nil() const noexcept { return value == 0; }
  [[nodiscard]] Epoch next() const noexcept { return Epoch{value + 1}; }
  [[nodiscard]] std::string to_string() const { return std::to_string(value); }
};

/// Version counter of a single object, resource or record. Every accepted
/// mutation advances the generation of exactly the objects it changed.
struct Generation {
  std::uint64_t value = 0;

  friend bool operator==(Generation, Generation) noexcept = default;
  friend auto operator<=>(Generation, Generation) noexcept = default;

  [[nodiscard]] bool is_nil() const noexcept { return value == 0; }
  [[nodiscard]] Generation next() const noexcept { return Generation{value + 1}; }
  [[nodiscard]] std::string to_string() const { return std::to_string(value); }
};

/// One process lifetime of the runtime.
struct Incarnation {
  /// Monotonic, persisted, never reused. This is the fencing key.
  std::uint64_t boot_sequence = 0;
  /// Entropy-based instance identity, used for diagnostics and for detecting a
  /// torn or foreign store. Never used alone as an authority check.
  Digest128 instance{};
  /// Free-form label of the process/machine that produced the record. It is
  /// explicitly NOT identity and never affects a decision.
  std::string host_label;

  [[nodiscard]] bool is_nil() const noexcept { return boot_sequence == 0; }
  [[nodiscard]] bool same_boot(const Incarnation& other) const noexcept {
    return boot_sequence == other.boot_sequence;
  }
  friend bool operator==(const Incarnation&, const Incarnation&) noexcept = default;
  [[nodiscard]] std::string to_string() const;
};

enum class ScopeKind : std::uint8_t { None = 0, Site, Global };

[[nodiscard]] std::string_view to_string(ScopeKind kind) noexcept;

/// Authority is granted over a scope. Two scopes overlap when they name the same
/// site, or when either is global; overlapping grants fence each other.
struct AuthorityScope {
  ScopeKind kind = ScopeKind::None;
  SiteId site{};

  [[nodiscard]] static AuthorityScope none() noexcept { return AuthorityScope{}; }
  [[nodiscard]] static AuthorityScope global() noexcept { return AuthorityScope{ScopeKind::Global, SiteId{}}; }
  [[nodiscard]] static AuthorityScope of_site(SiteId site) noexcept {
    return AuthorityScope{ScopeKind::Site, site};
  }

  [[nodiscard]] bool is_none() const noexcept { return kind == ScopeKind::None; }
  [[nodiscard]] bool covers(const AuthorityScope& other) const noexcept;
  [[nodiscard]] bool overlaps(const AuthorityScope& other) const noexcept;
  friend bool operator==(const AuthorityScope&, const AuthorityScope&) noexcept = default;
  [[nodiscard]] std::string to_string() const;
};

/// The capability to mutate authoritative state. A token is only meaningful
/// together with the current epoch and the current incarnation.
struct AuthorityToken {
  GrantId grant{};
  ControllerId holder{};
  Epoch epoch{};
  Incarnation incarnation{};
  AuthorityScope scope{};
  Generation generation{};

  [[nodiscard]] bool is_nil() const noexcept { return grant.is_nil() || epoch.is_nil(); }
  friend bool operator==(const AuthorityToken&, const AuthorityToken&) noexcept = default;
  [[nodiscard]] std::string to_string() const;
};

enum class AuthorityCurrentness : std::uint8_t {
  Current = 0,
  NoAuthority,
  UnknownGrant,
  ScopeNotCovered,
  StaleEpoch,
  StaleIncarnation,
  Expired,
  HolderMismatch,
};

[[nodiscard]] std::string_view to_string(AuthorityCurrentness state) noexcept;

struct AuthorityCheck {
  AuthorityCurrentness currentness = AuthorityCurrentness::NoAuthority;
  Epoch current_epoch{};
  std::uint64_t current_boot_sequence = 0;
  std::string detail;

  [[nodiscard]] bool current() const noexcept { return currentness == AuthorityCurrentness::Current; }
};

enum class FenceReason : std::uint8_t {
  ExplicitOperatorFence = 0,
  SupersededByNewerEpoch,
  SupersededByNewerIncarnation,
  StoreTakeover,
  AuthorityReleased,
  ScopeRevoked,
};

[[nodiscard]] std::string_view to_string(FenceReason reason) noexcept;

struct FenceRecord {
  GrantId grant{};
  ControllerId holder{};
  Epoch fenced_epoch{};
  Epoch fencing_epoch{};
  std::uint64_t fenced_boot_sequence = 0;
  std::uint64_t fencing_boot_sequence = 0;
  AuthorityScope scope{};
  FenceReason reason = FenceReason::ExplicitOperatorFence;
  Tick fenced_tick{};
  std::string detail;
};

}  // namespace optical_fabric
