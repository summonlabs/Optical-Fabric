// Optical Fabric 1.0.0 - Summon Software Labs
// Deterministic digests and integrity checksums.
//
// Two independent 64-bit hashes are carried together so that canonical identity
// is a 128-bit value. The construction is deterministic across processes,
// machines, and build configurations: it never hashes pointers, addresses,
// iteration order of unordered containers, or wall-clock time.
#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace optical_fabric {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xCBF29CE484222325ull;
inline constexpr std::uint64_t kFnvPrime = 0x00000100000001B3ull;

/// Streaming FNV-1a-64. Used for derived identities.
class Fnv1a64 {
 public:
  Fnv1a64() = default;
  explicit Fnv1a64(std::uint64_t seed) : state_(seed) {}

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u64(std::uint64_t value) noexcept;
  void update_i64(std::int64_t value) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_ = kFnvOffsetBasis;
};

/// Second, independent 64-bit hash (a 64-bit variant of the Jenkins one-at-a-time
/// mixing used as a stream hash). Carried alongside FNV so identity is 128 bits.
class Mix64 {
 public:
  Mix64() = default;
  explicit Mix64(std::uint64_t seed) : state_(seed) {}

  void update(const void* data, std::size_t size) noexcept;
  void update(std::string_view text) noexcept { update(text.data(), text.size()); }
  void update_u64(std::uint64_t value) noexcept;
  void update_i64(std::int64_t value) noexcept;

  [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_ = 0x9E3779B97F4A7C15ull;
  std::uint64_t length_ = 0;
};

/// CRC-32C (Castagnoli), used for persisted-record and transport-frame
/// integrity. Not a security primitive: it detects torn and corrupt writes.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] inline std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(text.data(), text.size());
}

struct Digest128 {
  std::uint64_t high = 0;
  std::uint64_t low = 0;

  friend bool operator==(const Digest128&, const Digest128&) noexcept = default;
  friend auto operator<=>(const Digest128&, const Digest128&) noexcept = default;

  [[nodiscard]] bool is_nil() const noexcept { return high == 0 && low == 0; }
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static Digest128 nil() noexcept { return Digest128{}; }
  [[nodiscard]] static bool parse(std::string_view text, Digest128& out) noexcept;
};

/// Canonical digest of a completed canonical byte stream.
class CanonicalHasher {
 public:
  CanonicalHasher() = default;

  void add_field(std::string_view name, std::string_view value) noexcept;
  void add_field(std::string_view name, std::uint64_t value) noexcept;
  void add_field(std::string_view name, std::int64_t value) noexcept;
  /// Any other integral type is widened explicitly, so int, unsigned, size_t
  /// and bool arguments are never ambiguous.
  template <typename T>
    requires std::is_integral_v<T>
  void add_field(std::string_view name, T value) noexcept {
    if constexpr (std::is_signed_v<T>) {
      add_field(name, static_cast<std::int64_t>(value));
    } else {
      add_field(name, static_cast<std::uint64_t>(value));
    }
  }
  void add_bytes(std::string_view raw) noexcept;
  void add_raw(std::string_view text) noexcept;

  [[nodiscard]] Digest128 digest() const noexcept;

 private:
  Fnv1a64 fnv_;
  Mix64 mix_;
};

/// Canonical encoding helpers shared by identity, persistence and transport.
/// Every string is length-prefixed so that ("ab","c") and ("a","bc") differ.
[[nodiscard]] std::string canonical_field(std::string_view name, std::string_view value);
[[nodiscard]] std::string canonical_field(std::string_view name, std::uint64_t value);
[[nodiscard]] std::string canonical_field(std::string_view name, std::int64_t value);
[[nodiscard]] std::string canonical_u64(std::uint64_t value);
[[nodiscard]] std::string canonical_hex(std::uint64_t value);
[[nodiscard]] std::string hex_encode(std::uint64_t value);

/// Timing-independent byte-source used for incarnation identity. This is used
/// for identity, never for authority: authority is fenced by boot sequence.
void fill_random_bytes(std::uint8_t* out, std::size_t size) noexcept;

}  // namespace optical_fabric
