// Optical Fabric 1.0.0 - Summon Software Labs
// Evidence model.
//
// The runtime never infers a healthy optical path from the absence of bad news.
// Evidence is typed, generation-bound, incarnation-bound, time-bound and always
// carries provenance. Composition is a conservative join: combining two
// observations can never improve the answer beyond the weakest of them, and an
// observation that is missing is UNKNOWN (or UNSUPPORTED when no producer for
// that kind exists), never KNOWN.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"

namespace optical_fabric {

/// Quality of an observation. Listed in ascending order of severity, which is
/// also the composition order: join(a, b) is the more severe of the two, and
/// join is commutative, associative and idempotent.
enum class EvidenceState : std::uint8_t {
  /// A producer observed the subject and the observation is fresh.
  Known = 0,
  /// A producer answered, but the answer does not cover the whole subject.
  Incomplete,
  /// An observation exists but no longer describes the current generation,
  /// incarnation or validity window.
  Stale,
  /// No producer answered for this subject and kind.
  Unknown,
  /// No producer exists for this kind at this boundary; the runtime cannot
  /// ever know it, which is strictly worse than not knowing it right now.
  Unsupported,
  /// Producers disagree, or two observations of the same subject contradict.
  Conflicting,
  /// A producer answered with something malformed or self-contradictory.
  Invalid,
};

[[nodiscard]] std::string_view to_string(EvidenceState state) noexcept;
[[nodiscard]] bool evidence_state_from_string(std::string_view text, EvidenceState& out) noexcept;

/// Conservative join. Returns the more severe operand, so that UNKNOWN,
/// UNSUPPORTED, STALE, CONFLICTING and INCOMPLETE all survive composition with
/// KNOWN instead of being absorbed by it.
[[nodiscard]] EvidenceState compose(EvidenceState lhs, EvidenceState rhs) noexcept;

/// Composition of a sequence; an empty sequence is UNKNOWN, not KNOWN.
[[nodiscard]] EvidenceState compose_all(const std::vector<EvidenceState>& states) noexcept;

/// True only for an observation the runtime is willing to promote on.
[[nodiscard]] bool is_healthy(EvidenceState state) noexcept;

/// True when the state is the direct result of missing, stale or contradictory
/// information rather than a negative observation.
[[nodiscard]] bool is_indeterminate(EvidenceState state) noexcept;

enum class EvidenceKind : std::uint8_t {
  TopologyPresence = 0,
  PortCapability,
  ChannelCapability,
  LineSystemCapability,
  SpanCapability,
  CrossConnectCapability,
  PortOperationalState,
  SpanOperationalState,
  LineSystemOperationalState,
  CrossConnectOperationalState,
  ChannelAvailability,
  WavelengthAvailability,
  TransceiverCapability,
  CableAttachment,
  OpticalPowerTelemetry,
  AlignmentQuality,
  OperatorAttestation,
};

inline constexpr std::size_t kEvidenceKindCount = 17;

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] bool evidence_kind_from_string(std::string_view text, EvidenceKind& out) noexcept;

/// Kinds that describe the operational health of optical hardware. This
/// boundary has no hardware access, so these kinds are only ever populated by
/// an explicitly registered producer; with no producer they are UNSUPPORTED.
[[nodiscard]] bool is_hardware_telemetry_kind(EvidenceKind kind) noexcept;

/// Every observation carries where it came from and what it was about.
struct EvidenceProvenance {
  /// Name of the producing runtime, for example "transceiver-registry".
  std::string source_runtime;
  /// Label of the producing instance (a process, a node name). Diagnostics only.
  std::string source_instance;
  SourceId source_id{};
  /// Producer-local monotonically increasing sequence number.
  std::uint64_t source_sequence = 0;
  /// Epoch the producer believed it was operating under, when it has one.
  Epoch source_epoch{};
  /// Incarnation of the runtime that ingested the observation. An observation
  /// ingested by a previous incarnation is STALE until a new incarnation
  /// re-attests it, even if its validity window has not expired.
  Incarnation ingested_incarnation{};
  /// Generation of the subject the observation describes. An observation of an
  /// older generation never describes the current one.
  Generation observed_generation{};
  Tick observed_tick{};
  /// Exclusive upper bound of validity. observed_tick with a zero span is not a
  /// valid observation.
  Tick valid_until_tick{};
  /// Digest of the producer's payload, so an identical re-attestation is
  /// detectable and a changed payload at the same sequence is a conflict.
  Digest128 content_digest{};

  [[nodiscard]] bool has_producer() const noexcept { return !source_runtime.empty(); }
};

struct EvidenceRecord {
  EvidenceId id{};
  ResourceRef subject{};
  EvidenceKind kind = EvidenceKind::TopologyPresence;
  EvidenceState state = EvidenceState::Unknown;
  EvidenceProvenance provenance{};
  std::string detail;
  /// Advisory observations are recorded and reported but never block promotion.
  /// Hardware telemetry that this boundary cannot obtain is advisory by
  /// construction; capability and topology facts are not.
  bool advisory = false;

  [[nodiscard]] bool is_advisory() const noexcept { return advisory; }
};

struct EvidenceBundle {
  std::string source_runtime;
  std::string source_instance;
  SourceId source_id{};
  std::vector<EvidenceRecord> records;
};

/// A requirement derived by the runtime from an actual path. Requirements are
/// computed, never supplied by the caller, so a caller cannot lower the bar.
struct EvidenceRequirement {
  ResourceRef subject{};
  EvidenceKind kind = EvidenceKind::TopologyPresence;
  /// Blocking requirements gate promotion. Advisory requirements are recorded
  /// and reported but never gate it.
  bool blocking = true;
  std::string reason;

  friend bool operator==(const EvidenceRequirement&, const EvidenceRequirement&) noexcept = default;
  friend auto operator<=>(const EvidenceRequirement&, const EvidenceRequirement&) noexcept = default;
};

/// Result of evaluating one requirement.
struct EvidenceEvaluation {
  EvidenceRequirement requirement{};
  EvidenceState state = EvidenceState::Unknown;
  EvidenceId record{};
  /// Generation the subject was at when the observation was evaluated.
  Generation current_generation{};
  std::string detail;
};

/// Result of evaluating every requirement of a path.
struct EvidenceAssessment {
  /// Composition of every blocking requirement. Promotion requires Known.
  EvidenceState aggregate = EvidenceState::Unknown;
  /// Composition of the advisory requirements only. Reported, never blocking.
  EvidenceState advisory_aggregate = EvidenceState::Known;
  std::vector<EvidenceEvaluation> evaluations;

  [[nodiscard]] bool healthy() const noexcept { return is_healthy(aggregate); }
  [[nodiscard]] std::size_t count_of(EvidenceState state) const noexcept;
};

/// Freshness and provenance rules, expressed once and used by every caller.
struct EvidencePolicy {
  /// When true (the default), an observation ingested by another incarnation is
  /// STALE even if its validity window has not expired. This is what stops
  /// persisted dynamic evidence from silently becoming fresh after a restart.
  bool require_current_incarnation = true;
  /// When true, an observation of an older subject generation is STALE.
  bool require_current_generation = true;
  /// When true, an observation whose validity window has passed is STALE.
  bool enforce_validity_window = true;

  [[nodiscard]] static EvidencePolicy conservative() noexcept { return EvidencePolicy{}; }
};

/// Applies the policy to a stored record. Never upgrades a state.
[[nodiscard]] EvidenceState apply_policy(const EvidenceRecord& record, const EvidencePolicy& policy,
                                         const Incarnation& current_incarnation,
                                         Generation current_generation, Tick now) noexcept;

}  // namespace optical_fabric
