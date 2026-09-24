// Optical Fabric 1.0.0 - Summon Software Labs
// Activation, withdrawal, health and revalidation requests.
//
// Activation is two-phase. begin_activation re-checks every precondition and
// moves the object to ACTIVATING, returning a digest bound to that exact
// attempt and generation. commit_activation presents the digest and is the only
// path to ACTIVE. A process that dies between the two leaves an ACTIVATING
// object that recovery refuses to treat as active.
#pragma once

#include "optical_fabric/error.hpp"
#include "optical_fabric/evidence.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/lifecycle.hpp"

namespace optical_fabric {

struct ValidateRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
};

struct ValidationResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  EvidenceAssessment assessment{};
};

struct ActivationRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
};

struct CommitRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
  /// Digest returned by begin_activation. A commit that does not present the
  /// current activation digest is refused, which makes a replayed or reordered
  /// commit inert.
  Digest128 activation_digest{};
};

struct ActivationResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  Digest128 activation_digest{};
  Tick activated_tick{};
  EvidenceAssessment assessment{};
  std::vector<ResourceRef> committed;
};

struct WithdrawalRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
  std::string reason;
};

struct WithdrawalResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  std::vector<ResourceRef> released;
  bool terminal = false;
  Tick retired_tick{};
};

struct HealthReport {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
  EvidenceState observed = EvidenceState::Unknown;
  std::string detail;
};

struct HealthResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
};

/// Re-attests a live object after a restart. This is the only way an object
/// that survived a restart can become authorized again: the operator (or the
/// recovering controller) must re-supply fresh evidence under current authority.
struct RevalidationRequest {
  AttemptId attempt{};
  ConnectivityId connectivity{};
  AuthorityToken authority{};
  /// When true, an object that fails revalidation is moved to WITHDRAWING
  /// instead of being left unconfirmed.
  bool withdraw_on_failure = true;
};

struct RevalidationResult {
  OutcomeCode outcome = OutcomeCode::Refused;
  Refusal refusal{};
  ConnectivityState state = ConnectivityState::Proposed;
  Generation generation{};
  EvidenceAssessment assessment{};
  bool authority_confirmed = false;
};

}  // namespace optical_fabric
