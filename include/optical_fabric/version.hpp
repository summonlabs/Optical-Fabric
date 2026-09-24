// Optical Fabric 1.0.0 - Summon Software Labs
// Version identity for the runtime and its persisted artifacts.
#pragma once

#include <cstdint>
#include <string_view>

namespace optical_fabric {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Textual form of the library version ("1.0.0").
[[nodiscard]] std::string_view version_string() noexcept;

/// Persisted-state format version. A store written by a different format
/// version is refused rather than interpreted optimistically.
inline constexpr std::uint32_t kStateFormatVersion = 1;

/// Canonical domain separator prefix used by every derived identity and every
/// canonical serialization. Bumping it invalidates all previously derived
/// identities on purpose.
inline constexpr std::string_view kCanonicalDomain = "ofab/1";

}  // namespace optical_fabric
