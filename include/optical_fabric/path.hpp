// Optical Fabric 1.0.0 - Summon Software Labs
// Optical path identity.
//
// A path is a canonical, ordered sequence of segments over registered resources
// with the generation each segment was validated against, plus the selected
// channel and the direction. The path identity is a 128-bit digest of the
// canonical serialization of exactly that information, so:
//   * repeated equivalent input yields the same identity, in any process;
//   * reordering the segments changes the identity;
//   * a re-registered resource with a new generation changes the identity;
//   * nothing process-local (addresses, iteration order, wall clock) is hashed.
// The canonical text is part of the identity: it is what gets persisted, what
// gets transported, and what a caller can diff.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "optical_fabric/error.hpp"
#include "optical_fabric/generation.hpp"
#include "optical_fabric/identity.hpp"
#include "optical_fabric/limits.hpp"

namespace optical_fabric {

enum class SegmentRole : std::uint8_t {
  Port = 0,
  CrossConnect,
  Span,
  LineSystem,
  Channel,
};

[[nodiscard]] std::string_view to_string(SegmentRole role) noexcept;
[[nodiscard]] bool segment_role_from_string(std::string_view text, SegmentRole& out) noexcept;

/// Direction of the requested connectivity relative to the canonical ordering
/// established at intent submission. Direction participates in identity: a
/// reversed request is a different optical path.
enum class Direction : std::uint8_t { Forward = 0, Reverse, Bidirectional };

[[nodiscard]] std::string_view to_string(Direction direction) noexcept;
[[nodiscard]] bool direction_from_string(std::string_view text, Direction& out) noexcept;

struct PathSegment {
  SegmentRole role = SegmentRole::Port;
  ResourceRef resource{};
  /// Generation of the resource when the segment entered the path. A path
  /// whose segment generation no longer matches the registered resource is
  /// stale and cannot be promoted.
  Generation generation{};

  friend bool operator==(const PathSegment&, const PathSegment&) noexcept = default;
  friend auto operator<=>(const PathSegment&, const PathSegment&) noexcept = default;
};

struct PathDescriptor {
  /// Canonical order: source endpoint first, destination endpoint last.
  std::vector<PathSegment> segments;
  ChannelId channel{};
  Generation channel_generation{};
  Direction direction = Direction::Forward;
  /// How the route was obtained. Recorded so a reader can tell a planner's
  /// proposal apart from declared-route resolution.
  std::string route_origin;
};

struct CanonicalPath {
  std::string canonical_text;
  Digest128 identity{};
  std::uint32_t segment_count = 0;
};

/// Validates structure (non-empty, bounded, no duplicate resources, endpoints
/// are ports) and produces the canonical text and identity.
[[nodiscard]] Result<CanonicalPath> canonicalize_path(const PathDescriptor& path, const Limits& limits);

/// Recomputes the identity of an already-canonical text. Used when loading
/// persisted state: a stored identity that does not match its own text is
/// corruption and is refused.
[[nodiscard]] bool canonical_path_matches(std::string_view canonical_text, const Digest128& identity) noexcept;

/// Structural validation without canonicalization.
[[nodiscard]] Status validate_path_structure(const PathDescriptor& path, const Limits& limits);

/// Reverses a descriptor's segment order and direction. Forward becomes
/// Reverse and vice versa; Bidirectional is unchanged.
[[nodiscard]] PathDescriptor reversed_path(const PathDescriptor& path);

}  // namespace optical_fabric
