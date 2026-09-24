// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/evidence.hpp"

#include <array>

namespace optical_fabric {

namespace {

constexpr std::array<std::string_view, 7> kEvidenceStateTokens = {
    "known", "incomplete", "stale", "unknown", "unsupported", "conflicting", "invalid"};

constexpr std::array<std::string_view, kEvidenceKindCount> kEvidenceKindTokens = {
    "topology-presence",
    "port-capability",
    "channel-capability",
    "line-system-capability",
    "span-capability",
    "cross-connect-capability",
    "port-operational-state",
    "span-operational-state",
    "line-system-operational-state",
    "cross-connect-operational-state",
    "channel-availability",
    "wavelength-availability",
    "transceiver-capability",
    "cable-attachment",
    "optical-power-telemetry",
    "alignment-quality",
    "operator-attestation",
};

}  // namespace

std::string_view to_string(EvidenceState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= kEvidenceStateTokens.size()) {
    return "unknown";
  }
  return kEvidenceStateTokens[index];
}

bool evidence_state_from_string(std::string_view text, EvidenceState& out) noexcept {
  for (std::size_t index = 0; index < kEvidenceStateTokens.size(); ++index) {
    if (kEvidenceStateTokens[index] == text) {
      out = static_cast<EvidenceState>(index);
      return true;
    }
  }
  return false;
}

EvidenceState compose(EvidenceState lhs, EvidenceState rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) >= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

EvidenceState compose_all(const std::vector<EvidenceState>& states) noexcept {
  if (states.empty()) {
    // Nothing observed is not a healthy observation.
    return EvidenceState::Unknown;
  }
  EvidenceState result = EvidenceState::Known;
  for (const EvidenceState state : states) {
    result = compose(result, state);
  }
  return result;
}

bool is_healthy(EvidenceState state) noexcept { return state == EvidenceState::Known; }

bool is_indeterminate(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Known: return false;
    case EvidenceState::Incomplete:
    case EvidenceState::Stale:
    case EvidenceState::Unknown:
    case EvidenceState::Unsupported:
    case EvidenceState::Conflicting:
    case EvidenceState::Invalid:
      return true;
  }
  return true;
}

std::string_view to_string(EvidenceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kEvidenceKindTokens.size()) {
    return "unknown";
  }
  return kEvidenceKindTokens[index];
}

bool evidence_kind_from_string(std::string_view text, EvidenceKind& out) noexcept {
  for (std::size_t index = 0; index < kEvidenceKindTokens.size(); ++index) {
    if (kEvidenceKindTokens[index] == text) {
      out = static_cast<EvidenceKind>(index);
      return true;
    }
  }
  return false;
}

bool is_hardware_telemetry_kind(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::OpticalPowerTelemetry:
    case EvidenceKind::AlignmentQuality:
    case EvidenceKind::PortOperationalState:
    case EvidenceKind::SpanOperationalState:
    case EvidenceKind::LineSystemOperationalState:
    case EvidenceKind::CrossConnectOperationalState:
    case EvidenceKind::ChannelAvailability:
    case EvidenceKind::WavelengthAvailability:
      return true;
    default:
      return false;
  }
}

std::size_t EvidenceAssessment::count_of(EvidenceState state) const noexcept {
  std::size_t count = 0;
  for (const EvidenceEvaluation& evaluation : evaluations) {
    if (evaluation.state == state) {
      ++count;
    }
  }
  return count;
}

EvidenceState apply_policy(const EvidenceRecord& record, const EvidencePolicy& policy,
                           const Incarnation& current_incarnation, Generation current_generation,
                           Tick now) noexcept {
  // A state that is already indeterminate is never improved by the policy, and
  // a policy violation is reported as STALE rather than as a negative
  // observation: the runtime does not know the subject is unhealthy, it only
  // knows that what it holds no longer describes the subject.
  if (policy.enforce_validity_window &&
      record.provenance.valid_until_tick.value <= record.provenance.observed_tick.value) {
    // A window that ends at or before the observation is malformed, which is a
    // more precise diagnosis than "expired".
    return EvidenceState::Invalid;
  }
  if (policy.require_current_incarnation && !current_incarnation.is_nil() &&
      record.provenance.ingested_incarnation.boot_sequence != current_incarnation.boot_sequence) {
    return EvidenceState::Stale;
  }
  if (policy.require_current_generation && !current_generation.is_nil() &&
      record.provenance.observed_generation != current_generation) {
    return EvidenceState::Stale;
  }
  if (policy.enforce_validity_window && record.provenance.valid_until_tick.value <= now.value) {
    return EvidenceState::Stale;
  }
  return record.state;
}

}  // namespace optical_fabric
