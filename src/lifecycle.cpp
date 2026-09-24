// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/lifecycle.hpp"

#include "optical_fabric/intent.hpp"

#include <array>

namespace optical_fabric {

namespace {

constexpr std::array<std::string_view, kConnectivityStateCount> kStateTokens = {
    "proposed", "validated", "reserved", "activating", "active",
    "degraded", "failed",    "withdrawing", "retired", "refused"};

// Explicit successor relation. Everything not listed here is refused, so an
// illegal move can never be reached by accident.
constexpr std::array<std::array<bool, kConnectivityStateCount>, kConnectivityStateCount>
    kTransitions = {{
        // proposed -> validated, refused, retired
        {{false, true, false, false, false, false, false, true, true, true}},
        // validated -> reserved, refused, withdrawing, retired
        {{false, false, true, false, false, false, false, true, true, true}},
        // reserved -> activating, refused, withdrawing
        {{false, false, false, true, false, false, false, true, false, true}},
        // activating -> active, failed, withdrawing
        {{false, false, false, false, true, false, true, true, false, false}},
        // active -> degraded, failed, withdrawing
        {{false, false, false, false, false, true, true, true, false, false}},
        // degraded -> active, failed, withdrawing
        {{false, false, false, false, true, false, true, true, false, false}},
        // failed -> withdrawing, refused, retired
        {{false, false, false, false, false, false, false, true, true, true}},
        // withdrawing -> retired, failed
        {{false, false, false, false, false, false, true, false, true, false}},
        // retired -> (terminal)
        {{false, false, false, false, false, false, false, false, false, false}},
        // refused -> withdrawing, retired
        {{false, false, false, false, false, false, false, true, true, false}},
    }};

}  // namespace

std::string_view to_string(ConnectivityState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= kStateTokens.size()) {
    return "unknown";
  }
  return kStateTokens[index];
}

bool connectivity_state_from_string(std::string_view text, ConnectivityState& out) noexcept {
  for (std::size_t index = 0; index < kStateTokens.size(); ++index) {
    if (kStateTokens[index] == text) {
      out = static_cast<ConnectivityState>(index);
      return true;
    }
  }
  return false;
}

bool is_committed_state(ConnectivityState state) noexcept {
  return state == ConnectivityState::Active || state == ConnectivityState::Degraded;
}

bool holds_claims(ConnectivityState state) noexcept {
  switch (state) {
    case ConnectivityState::Reserved:
    case ConnectivityState::Activating:
    case ConnectivityState::Active:
    case ConnectivityState::Degraded:
    case ConnectivityState::Failed:
    case ConnectivityState::Withdrawing:
      return true;
    default:
      return false;
  }
}

bool is_terminal_state(ConnectivityState state) noexcept {
  return state == ConnectivityState::Retired;
}

std::string_view to_string(IntentOutcome outcome) noexcept {
  switch (outcome) {
    case IntentOutcome::Accepted: return "accepted";
    case IntentOutcome::Duplicate: return "duplicate";
    case IntentOutcome::Refused: return "refused";
  }
  return "unknown";
}

bool transition_allowed(ConnectivityState from, ConnectivityState to) noexcept {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= kConnectivityStateCount || to_index >= kConnectivityStateCount) {
    return false;
  }
  return kTransitions[from_index][to_index];
}

}  // namespace optical_fabric
