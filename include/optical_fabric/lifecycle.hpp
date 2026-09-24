// Optical Fabric 1.0.0 - Summon Software Labs
// Connectivity object lifecycle.
//
// The lifecycle is explicit and total: every state has a defined set of legal
// successors and every illegal move is refused with a typed code. A proposal is
// never active merely because a planner proposed it: promotion to ACTIVE
// requires a validated reservation, fresh evidence for every segment, current
// authority, and an uncommitted resource set.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/path.hpp"

namespace optical_fabric {

enum class ConnectivityState : std::uint8_t {
  Proposed = 0,
  Validated,
  Reserved,
  Activating,
  Active,
  Degraded,
  Failed,
  Withdrawing,
  Retired,
  Refused,
};

inline constexpr std::size_t kConnectivityStateCount = 10;

[[nodiscard]] std::string_view to_string(ConnectivityState state) noexcept;
[[nodiscard]] bool connectivity_state_from_string(std::string_view text, ConnectivityState& out) noexcept;

/// True when the state asserts that the path is carrying traffic in the real
/// world. Only ACTIVE and DEGRADED do, and both of them additionally require a
/// confirmed authority before the runtime calls them authorized.
[[nodiscard]] bool is_committed_state(ConnectivityState state) noexcept;

/// True when the state may hold a resource claim.
[[nodiscard]] bool holds_claims(ConnectivityState state) noexcept;

/// Terminal states cannot be left.
[[nodiscard]] bool is_terminal_state(ConnectivityState state) noexcept;

/// Legal successor relation. Refusals are recorded as events and, where the
/// object cannot continue, as the REFUSED state itself.
[[nodiscard]] bool transition_allowed(ConnectivityState from, ConnectivityState to) noexcept;

/// One recorded lifecycle step. The full history is persisted so that a restart
/// cannot lose the reason an object is in its current state.
struct TransitionRecord {
  ConnectivityState from = ConnectivityState::Proposed;
  ConnectivityState to = ConnectivityState::Proposed;
  OutcomeCode outcome = OutcomeCode::Applied;
  RefusalCode refusal = RefusalCode::None;
  AttemptId attempt{};
  Generation generation{};
  Epoch epoch{};
  Tick tick{};
  std::string detail;
};

/// Read-only view of a connectivity object.
struct ConnectivityView {
  ConnectivityId id{};
  std::string name;
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  Digest128 path_identity{};
  std::string canonical_path;
  std::string owner;
  ReservationId reservation{};
  bool has_reservation = false;
  Tick reservation_expiry{};
  AuthorityToken authority{};
  /// True only when the authority currently recorded was confirmed by the
  /// process incarnation that is running now. After a restart this is false
  /// until the object is explicitly revalidated.
  bool authority_confirmed = false;
  Tick created_tick{};
  Tick updated_tick{};
  Tick activated_tick{};
  Tick retired_tick{};
  RefusalCode last_refusal = RefusalCode::None;
  std::string last_refusal_detail;
  std::vector<PathSegment> segments;
  Direction direction = Direction::Forward;
  ChannelId channel{};
};

}  // namespace optical_fabric
