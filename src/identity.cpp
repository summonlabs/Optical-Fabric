// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/identity.hpp"

#include "optical_fabric/version.hpp"

#include <array>
#include <cstring>

namespace optical_fabric {

namespace {

constexpr std::array<std::string_view, 9> kResourceKindTokens = {
    "unknown", "site", "optical-node", "port", "line-system", "span", "cross-connect", "channel",
    "wavelength-reference"};

constexpr std::array<std::string_view, 2> kSharingTokens = {"exclusive", "channelized"};

[[nodiscard]] bool is_name_byte(char value) noexcept {
  const bool alpha = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
  const bool digit = value >= '0' && value <= '9';
  const bool punctuation = value == '.' || value == '_' || value == ':' || value == '-';
  return alpha || digit || punctuation;
}

}  // namespace

std::string_view to_string(ResourceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kResourceKindTokens.size()) {
    return "unknown";
  }
  return kResourceKindTokens[index];
}

ResourceKind resource_kind_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kResourceKindTokens.size(); ++index) {
    if (kResourceKindTokens[index] == text) {
      return static_cast<ResourceKind>(index);
    }
  }
  return ResourceKind::Unknown;
}

std::string_view to_string(SharingMode mode) noexcept {
  const auto index = static_cast<std::size_t>(mode);
  if (index >= kSharingTokens.size()) {
    return "exclusive";
  }
  return kSharingTokens[index];
}

bool parse_hex_u64(std::string_view text, std::uint64_t& out) noexcept {
  std::string_view digits = text;
  if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    digits.remove_prefix(2);
  }
  if (digits.empty() || digits.size() > 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char digit : digits) {
    std::uint64_t nibble = 0;
    if (digit >= '0' && digit <= '9') {
      nibble = static_cast<std::uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      nibble = static_cast<std::uint64_t>(digit - 'a' + 10);
    } else if (digit >= 'A' && digit <= 'F') {
      nibble = static_cast<std::uint64_t>(digit - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | nibble;
  }
  out = value;
  return true;
}

std::string ResourceRef::to_string() const {
  std::string out;
  out.append(optical_fabric::to_string(kind));
  out.push_back(':');
  out.append(hex_encode(id));
  return out;
}

std::optional<ResourceRef> ResourceRef::parse(std::string_view text) noexcept {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return std::nullopt;
  }
  const ResourceKind kind = resource_kind_from_string(text.substr(0, separator));
  if (kind == ResourceKind::Unknown) {
    return std::nullopt;
  }
  std::uint64_t id = 0;
  if (!parse_hex_u64(text.substr(separator + 1), id)) {
    return std::nullopt;
  }
  return ResourceRef{kind, id};
}

bool Names::valid(std::string_view name, std::size_t max_length) noexcept {
  if (name.empty() || name.size() > max_length) {
    return false;
  }
  if (name.front() == '.' || name.front() == '-' || name.front() == ':' || name.front() == '_') {
    return false;
  }
  if (name.back() == '.' || name.back() == '-' || name.back() == ':' || name.back() == '_') {
    return false;
  }
  for (const char value : name) {
    if (!is_name_byte(value)) {
      return false;
    }
  }
  return true;
}

std::string_view Names::invalid_reason(std::string_view name, std::size_t max_length) noexcept {
  if (name.empty()) {
    return "name is empty";
  }
  if (name.size() > max_length) {
    return "name exceeds the configured maximum length";
  }
  if (name.front() == '.' || name.front() == '-' || name.front() == ':' || name.front() == '_') {
    return "name begins with a separator";
  }
  if (name.back() == '.' || name.back() == '-' || name.back() == ':' || name.back() == '_') {
    return "name ends with a separator";
  }
  for (const char value : name) {
    if (!is_name_byte(value)) {
      return "name contains a character outside [A-Za-z0-9._:-]";
    }
  }
  return "name is invalid";
}

std::uint64_t Names::derive_resource(ResourceKind kind, std::string_view name) noexcept {
  Fnv1a64 hasher;
  hasher.update(kCanonicalDomain);
  hasher.update_byte(static_cast<std::uint8_t>(0x1F));
  hasher.update(to_string(kind));
  hasher.update_byte(static_cast<std::uint8_t>(0x1F));
  hasher.update(name);
  std::uint64_t value = hasher.value();
  if (value == 0) {
    // A nil identity is reserved for "no resource"; never hand it out.
    value = 0x9E3779B97F4A7C15ull;
  }
  return value;
}

std::uint64_t Names::derive_named(std::string_view domain, std::string_view name) noexcept {
  Fnv1a64 hasher;
  hasher.update(kCanonicalDomain);
  hasher.update_byte(static_cast<std::uint8_t>(0x1E));
  hasher.update(domain);
  hasher.update_byte(static_cast<std::uint8_t>(0x1F));
  hasher.update(name);
  std::uint64_t value = hasher.value();
  if (value == 0) {
    value = 0x9E3779B97F4A7C15ull;
  }
  return value;
}

std::uint64_t Names::derive_composite(std::string_view domain, const std::string& canonical) noexcept {
  Fnv1a64 hasher;
  hasher.update(kCanonicalDomain);
  hasher.update_byte(static_cast<std::uint8_t>(0x1D));
  hasher.update(domain);
  hasher.update_byte(static_cast<std::uint8_t>(0x1F));
  hasher.update(canonical);
  std::uint64_t value = hasher.value();
  if (value == 0) {
    value = 0x9E3779B97F4A7C15ull;
  }
  return value;
}

AttemptId generate_attempt_id() noexcept {
  std::uint8_t bytes[8] = {};
  fill_random_bytes(bytes, sizeof(bytes));
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(bytes[index]);
  }
  if (value == 0) {
    value = 0xA5A5A5A5A5A5A5A5ull;
  }
  return AttemptId::from_value(value);
}

GrantId generate_grant_id() noexcept {
  std::uint8_t bytes[8] = {};
  fill_random_bytes(bytes, sizeof(bytes));
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value = (value << 8) | static_cast<std::uint64_t>(bytes[index]);
  }
  if (value == 0) {
    value = 0x5A5A5A5A5A5A5A5Aull;
  }
  return GrantId::from_value(value);
}

}  // namespace optical_fabric
