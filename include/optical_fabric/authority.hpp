// Optical Fabric 1.0.0 - Summon Software Labs
// Authority grants and fencing.
//
// Only one grant may be current for an overlapping scope. Acquiring authority
// advances the epoch, which immediately fences every token issued under an
// older epoch and every token issued by an older process incarnation.
#pragma once

#include <cstdint>
#include <string>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/limits.hpp"

namespace optical_fabric {

struct AuthorityRequest {
  AttemptId attempt{};
  ControllerId holder{};
  /// Diagnostics only; never identity.
  std::string holder_label;
  AuthorityScope scope{};
  std::uint64_t lease_ticks = 256;
  std::string reason;
};

struct AuthorityGrantView {
  GrantId id{};
  ControllerId holder{};
  AuthorityScope scope{};
  Epoch epoch{};
  Incarnation incarnation{};
  Tick granted_tick{};
  Tick expiry_tick{};
  Generation generation{};
  bool fenced = false;
  bool released = false;
};

struct AuthorityResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  AuthorityGrantView grant{};
  AuthorityToken token{};
};

/// Acquiring a fence is acquiring authority over a scope with an explicit
/// reason. The fenced grant is reported back so the caller can prove that the
/// previous holder can no longer mutate anything.
struct FenceRequest {
  AttemptId attempt{};
  ControllerId holder{};
  std::string holder_label;
  AuthorityScope scope{};
  std::uint64_t lease_ticks = 256;
  FenceReason reason = FenceReason::ExplicitOperatorFence;
  std::string detail;
};

struct FenceResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  FenceRecord fenced{};
  AuthorityToken token{};
  bool fenced_anything = false;
};

struct AuthorityView {
  GrantId id{};
  ControllerId holder{};
  AuthorityScope scope{};
  Epoch epoch{};
  Incarnation incarnation{};
  Tick granted_tick{};
  Tick expiry_tick{};
  bool fenced = false;
  bool released = false;
  bool current = false;
};

}  // namespace optical_fabric
