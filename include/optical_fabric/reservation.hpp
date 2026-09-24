// Optical Fabric 1.0.0 - Summon Software Labs
// Reservations: the exclusive, expiring, generation-bound claim on the
// resources of a path.
//
// A reservation is the only way a connectivity object may hold resources, and
// it is always granted as a whole: a partial claim is never created.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"

namespace optical_fabric {

struct ReservationRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
  /// Lifetime in ticks from the current tick.
  std::uint64_t ttl_ticks = 64;
};

struct ReservationRenewal {
  AttemptId attempt{};
  ReservationId reservation{};
  AuthorityToken authority{};
  std::uint64_t extend_ticks = 64;
};

struct ReleaseRequest {
  AttemptId attempt{};
  ReservationId reservation{};
  AuthorityToken authority{};
  std::string reason;
};

struct ReservationView {
  ReservationId id{};
  ConnectivityId connectivity{};
  std::string holder;
  Epoch epoch{};
  Incarnation incarnation{};
  Tick granted_tick{};
  Tick expiry_tick{};
  Generation generation{};
  std::vector<ResourceRef> resources;
  bool consumed = false;
  bool released = false;

  [[nodiscard]] bool live_at(Tick now) const noexcept {
    return !released && now.value < expiry_tick.value;
  }
};

struct ReservationResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ReservationView reservation{};
  /// Resources newly claimed by this request, in canonical order.
  std::vector<ResourceRef> claimed;
  Generation generation{};
};

struct ReleaseResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ReservationId reservation{};
  std::vector<ResourceRef> released;
  Tick released_tick{};
};

}  // namespace optical_fabric
