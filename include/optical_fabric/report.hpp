// Optical Fabric 1.0.0 - Summon Software Labs
// Query results: what is authorized, what is active, what is claimed, and what
// invariants hold.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/authority.hpp"
#include "optical_fabric/diagnostics.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/lifecycle.hpp"

namespace optical_fabric {

/// Compact recovery facts carried in a snapshot so a caller does not need to
/// re-read the recovery report to notice that state was recovered.
struct RecoveryReportSummary {
  std::uint8_t status = 0;
  std::uint64_t discarded_bytes = 0;
  std::uint64_t boot_sequence = 0;
  std::size_t evidence_marked_stale = 0;
  std::size_t authority_unconfirmed = 0;
  std::size_t incomplete_activations = 0;

  friend bool operator==(const RecoveryReportSummary&, const RecoveryReportSummary&) = default;
};

/// One committed connection in the authoritative answer to "what is active".
struct ActivePathEntry {
  ConnectivityId connectivity{};
  std::string name;
  ConnectivityState state = ConnectivityState::Active;
  Digest128 identity{};
  std::string canonical_path;
  std::string owner;
  Tick activated_tick{};
  /// True when the authority recorded on the object was confirmed by the
  /// incarnation that is running now.
  bool authority_confirmed = false;
  /// Evidence assessment for the object as of the current tick.
  bool evidence_healthy = false;
  EvidenceState evidence_state = EvidenceState::Unknown;
  /// False for DEGRADED and for an object whose authority or evidence is not
  /// current. Only authorized entries appear in ActivePathReport::authorized.
  bool authorized = false;
  std::string unauthorized_reason;
};

struct ActivePathReport {
  /// Every object that asserts a committed optical path, including ones the
  /// runtime will not currently authorize. Reported, never hidden.
  std::vector<ActivePathEntry> active;
  /// Identities of the subset that is authorized right now: ACTIVE (not
  /// DEGRADED), current authority confirmed in this incarnation, and healthy
  /// evidence for every segment. After a restart this is empty until objects
  /// are revalidated.
  std::vector<ConnectivityId> authorized;
  /// The authority that must be fenced before the answer may change.
  std::vector<AuthorityView> authority;
  std::vector<Diagnostic> diagnostics;
  Tick observed_tick{};
  Epoch epoch{};
  Digest128 digest{};
};

/// A resource commitment. The commit index is what makes the
/// "no resource is simultaneously committed to incompatible authoritative
/// paths" invariant checkable rather than asserted.
struct ResourceClaim {
  ResourceRef resource{};
  ConnectivityId connectivity{};
  ChannelId channel{};
  bool exclusive = true;
  Tick committed_tick{};
};

struct ClaimReport {
  std::vector<ResourceClaim> claims;
  std::size_t exclusive_resources_claimed = 0;
  std::size_t channelized_resources_claimed = 0;
  Digest128 digest{};
};

struct AccountingReport {
  std::size_t resources_registered = 0;
  std::size_t claims = 0;
  std::size_t reservations_live = 0;
  std::size_t reservations_consumed = 0;
  std::size_t reservations_released = 0;
  std::size_t objects_total = 0;
  std::size_t objects_by_state[kConnectivityStateCount] = {};
  std::size_t grants_live = 0;
  std::size_t grants_fenced = 0;
  std::size_t grants_released = 0;
  std::size_t evidence_records = 0;
  std::size_t evidence_stale = 0;
  std::size_t attempt_records = 0;
  std::size_t diagnostics = 0;
  std::size_t transitions = 0;
  /// True when every count is internally consistent with the commit index.
  bool balanced = false;
  std::vector<std::string> drift;
};

struct InvariantCheck {
  std::string name;
  bool holds = true;
  std::string detail;
};

struct InvariantReport {
  std::vector<InvariantCheck> checks;
  bool all_hold = true;

  [[nodiscard]] std::size_t failures() const noexcept;
};

struct FabricSnapshot {
  Epoch epoch{};
  Incarnation incarnation{};
  Tick tick{};
  Generation generation{};
  Generation topology_generation{};
  std::size_t sites = 0;
  std::size_t optical_nodes = 0;
  std::size_t ports = 0;
  std::size_t spans = 0;
  std::size_t line_systems = 0;
  std::size_t cross_connects = 0;
  std::size_t channels = 0;
  std::size_t connectivity_objects = 0;
  std::size_t connectivity_active = 0;
  std::size_t reservations = 0;
  std::size_t claims = 0;
  std::size_t evidence_records = 0;
  std::size_t authority_grants = 0;
  RecoveryReportSummary recovery{};
  Digest128 digest{};
};

}  // namespace optical_fabric
