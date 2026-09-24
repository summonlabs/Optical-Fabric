// Optical Fabric 1.0.0 - Summon Software Labs
// Diagnostics: stale-generation, stale-authority and conflict reporting.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"

namespace optical_fabric {

enum class DiagnosticKind : std::uint8_t {
  StaleGeneration = 0,
  StaleEvidence,
  StaleAuthority,
  StaleIncarnation,
  UnsupportedEvidence,
  MissingEvidence,
  ConflictingEvidence,
  ResourceConflict,
  UnconfirmedAuthority,
  ReservationExpiry,
  IllegalTransitionRefused,
  InvariantViolation,
  CapacityPressure,
  UnsupportedBoundary,
};

inline constexpr std::size_t kDiagnosticKindCount = 14;

[[nodiscard]] std::string_view to_string(DiagnosticKind kind) noexcept;

struct Diagnostic {
  DiagnosticKind kind = DiagnosticKind::StaleGeneration;
  RefusalCode refusal = RefusalCode::None;
  ResourceRef subject{};
  ConnectivityId connectivity{};
  ReservationId reservation{};
  GrantId grant{};
  Tick observed_tick{};
  Epoch observed_epoch{};
  Generation observed_generation{};
  Generation current_generation{};
  std::string detail;
};

struct DiagnosticQuery {
  /// Restrict to one connectivity object when non-nil.
  ConnectivityId connectivity{};
  /// Restrict to one resource when non-nil.
  ResourceRef resource{};
  /// Restrict to one kind when set.
  bool filter_kind = false;
  DiagnosticKind kind = DiagnosticKind::StaleGeneration;
  std::size_t max_results = 256;
};

struct DiagnosticsReport {
  std::vector<Diagnostic> diagnostics;
  std::size_t total_available = 0;
  bool truncated = false;
  Digest128 digest{};

  [[nodiscard]] std::size_t count_of(DiagnosticKind kind) const noexcept;
};

}  // namespace optical_fabric
