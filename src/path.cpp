// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/path.hpp"

#include <algorithm>
#include <array>

#include "optical_fabric/version.hpp"

namespace optical_fabric {

namespace {

constexpr std::array<std::string_view, 5> kSegmentRoleTokens = {"port", "cross-connect", "span",
                                                                "line-system", "channel"};
constexpr std::array<std::string_view, 3> kDirectionTokens = {"forward", "reverse", "bidirectional"};

[[nodiscard]] std::string_view role_token(SegmentRole role) noexcept {
  const auto index = static_cast<std::size_t>(role);
  if (index >= kSegmentRoleTokens.size()) {
    return "unknown";
  }
  return kSegmentRoleTokens[index];
}

[[nodiscard]] std::string_view direction_token(Direction direction) noexcept {
  const auto index = static_cast<std::size_t>(direction);
  if (index >= kDirectionTokens.size()) {
    return "forward";
  }
  return kDirectionTokens[index];
}

/// The single place where a path identity is computed. Canonicalization and
/// verification must never diverge, so both call this.
[[nodiscard]] Digest128 path_digest(std::string_view canonical_text) noexcept {
  CanonicalHasher hasher;
  hasher.add_raw(canonical_text);
  return hasher.digest();
}

}  // namespace

std::string_view to_string(SegmentRole role) noexcept { return role_token(role); }

bool segment_role_from_string(std::string_view text, SegmentRole& out) noexcept {
  for (std::size_t index = 0; index < kSegmentRoleTokens.size(); ++index) {
    if (kSegmentRoleTokens[index] == text) {
      out = static_cast<SegmentRole>(index);
      return true;
    }
  }
  return false;
}

std::string_view to_string(Direction direction) noexcept {
  const auto index = static_cast<std::size_t>(direction);
  if (index >= kDirectionTokens.size()) {
    return "forward";
  }
  return kDirectionTokens[index];
}

bool direction_from_string(std::string_view text, Direction& out) noexcept {
  for (std::size_t index = 0; index < kDirectionTokens.size(); ++index) {
    if (kDirectionTokens[index] == text) {
      out = static_cast<Direction>(index);
      return true;
    }
  }
  return false;
}

Status validate_path_structure(const PathDescriptor& path, const Limits& limits) {
  if (path.segments.size() < 2) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a path needs at least a source and a destination segment");
  }
  if (path.segments.size() > limits.max_path_segments) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a path exceeds the configured maximum segment count");
  }
  for (const PathSegment& segment : path.segments) {
    if (segment.resource.is_nil()) {
      return Status::failure(ErrorCode::InvalidArgument, "a path contains a nil resource reference");
    }
    if (segment.generation.is_nil()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "a path segment carries no resource generation");
    }
  }
  if (path.segments.front().role != SegmentRole::Port ||
      path.segments.back().role != SegmentRole::Port) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a path must begin and end at a port segment");
  }
  if (path.channel.is_nil() && !path.channel_generation.is_nil()) {
    return Status::failure(ErrorCode::InvalidArgument, "a channel generation was supplied without a channel");
  }
  if (!path.channel.is_nil() && path.channel_generation.is_nil()) {
    return Status::failure(ErrorCode::InvalidArgument, "a channel was supplied without its generation");
  }
  std::vector<ResourceRef> seen;
  seen.reserve(path.segments.size());
  for (const PathSegment& segment : path.segments) {
    if (std::find(seen.begin(), seen.end(), segment.resource) != seen.end()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "a path references the same resource twice: " + segment.resource.to_string());
    }
    seen.push_back(segment.resource);
  }
  return Status{};
}

Result<CanonicalPath> canonicalize_path(const PathDescriptor& path, const Limits& limits) {
  const Status structure = validate_path_structure(path, limits);
  if (!structure.ok()) {
    return Result<CanonicalPath>::failure(structure.code, structure.message);
  }

  std::string text;
  text.reserve(96 * path.segments.size() + 64);
  text.append(kCanonicalDomain);
  text.append("|path\n");
  for (const PathSegment& segment : path.segments) {
    text.append("seg|");
    text.append(role_token(segment.role));
    text.push_back('|');
    text.append(to_string(segment.resource.kind));
    text.push_back('|');
    text.append(hex_encode(segment.resource.id));
    text.push_back('|');
    text.append(segment.generation.to_string());
    text.push_back('\n');
  }
  text.append("chan|");
  text.append(path.channel.is_nil() ? std::string("nil") : path.channel.to_string());
  text.push_back('|');
  text.append(path.channel_generation.is_nil() ? std::string("nil") : path.channel_generation.to_string());
  text.push_back('\n');
  text.append("dir|");
  text.append(direction_token(path.direction));
  text.push_back('\n');

  CanonicalPath canonical;
  canonical.canonical_text = std::move(text);
  canonical.identity = path_digest(canonical.canonical_text);
  canonical.segment_count = static_cast<std::uint32_t>(path.segments.size());
  return Result<CanonicalPath>::success(std::move(canonical));
}

bool canonical_path_matches(std::string_view canonical_text, const Digest128& identity) noexcept {
  return path_digest(canonical_text) == identity;
}

PathDescriptor reversed_path(const PathDescriptor& path) {
  PathDescriptor result = path;
  std::reverse(result.segments.begin(), result.segments.end());
  if (path.direction == Direction::Forward) {
    result.direction = Direction::Reverse;
  } else if (path.direction == Direction::Reverse) {
    result.direction = Direction::Forward;
  }
  return result;
}

}  // namespace optical_fabric
