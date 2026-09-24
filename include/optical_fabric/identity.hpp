// Optical Fabric 1.0.0 - Summon Software Labs
// Strongly typed identities.
//
// Every identity in the runtime is either a named identity derived from a
// validated canonical name or a value identity carried by a record. Named
// identities are derived with a deterministic hash of the canonical name, so
// the same name yields the same identity in every process, on every machine,
// and in every recovery. Collisions are detected and refused rather than
// accepted silently.
#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "optical_fabric/digest.hpp"

namespace optical_fabric {

/// Resource kinds are physical/structural identities. They are deliberately
/// separate from operational state and from authority.
enum class ResourceKind : std::uint8_t {
  Unknown = 0,
  Site,
  OpticalNode,
  Port,
  LineSystem,
  Span,
  CrossConnect,
  Channel,
  WavelengthReference,
};

[[nodiscard]] std::string_view to_string(ResourceKind kind) noexcept;
[[nodiscard]] ResourceKind resource_kind_from_string(std::string_view text) noexcept;

/// How a resource may be shared by authoritative paths.
enum class SharingMode : std::uint8_t {
  /// At most one authoritative path may commit the resource.
  Exclusive = 0,
  /// Multiple authoritative paths may commit the resource when their selected
  /// channel (wavelength) differs. The same channel on the same resource is a
  /// conflict, never a merge.
  Channelized,
};

[[nodiscard]] std::string_view to_string(SharingMode mode) noexcept;

/// Parses 1..16 hexadecimal digits, with an optional "0x"/"0X" prefix.
[[nodiscard]] bool parse_hex_u64(std::string_view text, std::uint64_t& out) noexcept;
[[nodiscard]] std::string hex_encode(std::uint64_t value);

template <typename Tag>
class StrongId {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr StrongId() noexcept = default;

  [[nodiscard]] static constexpr StrongId from_value(std::uint64_t value) noexcept {
    StrongId id;
    id.value_ = value;
    return id;
  }

  [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept {
    std::uint64_t value = 0;
    if (!parse_hex_u64(text, value)) {
      return std::nullopt;
    }
    return StrongId::from_value(value);
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }

  [[nodiscard]] std::string to_string() const { return hex_encode(value_); }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  std::uint64_t value_ = 0;
};

struct SiteTag {};
struct OpticalNodeTag {};
struct PortTag {};
struct LineSystemTag {};
struct SpanTag {};
struct CrossConnectTag {};
struct ChannelTag {};
struct WavelengthReferenceTag {};
struct IntentTag {};
struct ConnectivityTag {};
struct ReservationTag {};
struct GrantTag {};
struct EvidenceTag {};
struct AttemptTag {};
struct SourceTag {};
struct ControllerTag {};
struct OperationTag {};

/// Associates a strongly typed identity with its resource kind so that typed
/// identities can be widened into a ResourceRef without stringly-typed code.
template <typename Tag>
struct IdKind;

#define OF_DECLARE_RESOURCE_ID(alias, tag, kind_value)      \
  using alias = StrongId<tag>;                              \
  template <>                                               \
  struct IdKind<tag> {                                      \
    static constexpr ResourceKind value = ResourceKind::kind_value; \
  }

OF_DECLARE_RESOURCE_ID(SiteId, SiteTag, Site);
OF_DECLARE_RESOURCE_ID(OpticalNodeId, OpticalNodeTag, OpticalNode);
OF_DECLARE_RESOURCE_ID(PortId, PortTag, Port);
OF_DECLARE_RESOURCE_ID(LineSystemId, LineSystemTag, LineSystem);
OF_DECLARE_RESOURCE_ID(SpanId, SpanTag, Span);
OF_DECLARE_RESOURCE_ID(CrossConnectId, CrossConnectTag, CrossConnect);
OF_DECLARE_RESOURCE_ID(ChannelId, ChannelTag, Channel);
OF_DECLARE_RESOURCE_ID(WavelengthReferenceId, WavelengthReferenceTag, WavelengthReference);

#undef OF_DECLARE_RESOURCE_ID

using IntentId = StrongId<IntentTag>;
using ConnectivityId = StrongId<ConnectivityTag>;
using ReservationId = StrongId<ReservationTag>;
using GrantId = StrongId<GrantTag>;
using EvidenceId = StrongId<EvidenceTag>;
using AttemptId = StrongId<AttemptTag>;
using SourceId = StrongId<SourceTag>;
using ControllerId = StrongId<ControllerTag>;
using OperationId = StrongId<OperationTag>;

/// A resource reference is the erased form used on the wire, in canonical
/// identity, and in the commit index. Typed identities widen into it.
struct ResourceRef {
  ResourceKind kind = ResourceKind::Unknown;
  std::uint64_t id = 0;

  friend bool operator==(const ResourceRef&, const ResourceRef&) noexcept = default;
  friend auto operator<=>(const ResourceRef&, const ResourceRef&) noexcept = default;

  [[nodiscard]] bool is_nil() const noexcept { return kind == ResourceKind::Unknown || id == 0; }
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static std::optional<ResourceRef> parse(std::string_view text) noexcept;
  [[nodiscard]] static ResourceRef make(ResourceKind kind, std::uint64_t id) noexcept {
    return ResourceRef{kind, id};
  }
};

/// Widens a typed resource identity into its erased reference form.
template <typename Id>
[[nodiscard]] ResourceRef as_ref(Id id) {
  return ResourceRef{IdKind<typename Id::tag_type>::value, id.value()};
}

/// Canonical-name derivation and validation.
struct Names {
  /// A name is 1..max_length bytes from [A-Za-z0-9._:-] and must not begin or
  /// end with a separator. Names are case-sensitive.
  [[nodiscard]] static bool valid(std::string_view name, std::size_t max_length) noexcept;
  [[nodiscard]] static std::string_view invalid_reason(std::string_view name, std::size_t max_length) noexcept;

  /// Deterministic identity of a named resource. Domain-separated by kind.
  [[nodiscard]] static std::uint64_t derive_resource(ResourceKind kind, std::string_view name) noexcept;

  /// Deterministic identity of a named non-resource object (connectivity
  /// object, controller, evidence source, operation).
  [[nodiscard]] static std::uint64_t derive_named(std::string_view domain, std::string_view name) noexcept;

  /// Composite identities are derived from canonical ordered components.
  [[nodiscard]] static std::uint64_t derive_composite(std::string_view domain,
                                                      const std::string& canonical) noexcept;
};

template <typename Id>
[[nodiscard]] Id derive_id(std::string_view name) {
  return Id::from_value(Names::derive_resource(IdKind<typename Id::tag_type>::value, name));
}

template <typename Id>
[[nodiscard]] Id derive_named_id(std::string_view domain, std::string_view name) {
  return Id::from_value(Names::derive_named(domain, name));
}

/// Attempts are caller-supplied and must be unique per request. Generation uses
/// operating-system entropy; determinism is never required of an attempt id.
[[nodiscard]] AttemptId generate_attempt_id() noexcept;
[[nodiscard]] GrantId generate_grant_id() noexcept;

}  // namespace optical_fabric

namespace std {
template <typename Tag>
struct hash<optical_fabric::StrongId<Tag>> {
  [[nodiscard]] size_t operator()(const optical_fabric::StrongId<Tag>& id) const noexcept {
    return static_cast<size_t>(id.value());
  }
};

template <>
struct hash<optical_fabric::ResourceRef> {
  [[nodiscard]] size_t operator()(const optical_fabric::ResourceRef& ref) const noexcept {
    return static_cast<size_t>(ref.id ^ (static_cast<std::uint64_t>(ref.kind) * 0x9E3779B97F4A7C15ull));
  }
};
}  // namespace std
