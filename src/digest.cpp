// Optical Fabric 1.0.0 - Summon Software Labs
#include "optical_fabric/digest.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "crc_stream.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace optical_fabric {

void Fnv1a64::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t state = state_;
  for (std::size_t index = 0; index < size; ++index) {
    state ^= static_cast<std::uint64_t>(bytes[index]);
    state *= kFnvPrime;
  }
  state_ = state;
}

void Fnv1a64::update_u64(std::uint64_t value) noexcept {
  unsigned char buffer[8];
  for (int index = 0; index < 8; ++index) {
    buffer[index] = static_cast<unsigned char>((value >> (8 * index)) & 0xFFu);
  }
  update(buffer, sizeof(buffer));
}

void Fnv1a64::update_i64(std::int64_t value) noexcept {
  update_u64(static_cast<std::uint64_t>(value));
}

void Fnv1a64::update_byte(std::uint8_t value) noexcept { update(&value, 1); }

void Mix64::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint64_t state = state_;
  std::uint64_t length = length_;
  for (std::size_t index = 0; index < size; ++index) {
    std::uint64_t value = static_cast<std::uint64_t>(bytes[index]);
    value += static_cast<std::uint64_t>(index) + length;
    state += value;
    state += state << 10;
    state ^= state >> 6;
  }
  state += state << 3;
  state ^= state >> 11;
  state += state << 15;
  state_ = state;
  length_ = length + size;
}

void Mix64::update_u64(std::uint64_t value) noexcept {
  unsigned char buffer[8];
  for (int index = 0; index < 8; ++index) {
    buffer[index] = static_cast<unsigned char>((value >> (8 * index)) & 0xFFu);
  }
  update(buffer, sizeof(buffer));
}

void Mix64::update_i64(std::int64_t value) noexcept { update_u64(static_cast<std::uint64_t>(value)); }

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
  // One CRC-32C implementation is shared with the store and the transport, so
  // a frame is never copied into a contiguous buffer just to be checksummed and
  // the writer and the reader can never disagree about the algorithm.
  detail::Crc32cStream stream;
  stream.update(data, size);
  return stream.finish();
}

std::string Digest128::to_string() const {
  char buffer[33];
  std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                static_cast<unsigned long long>(high), static_cast<unsigned long long>(low));
  return std::string(buffer, 32);
}

bool Digest128::parse(std::string_view text, Digest128& out) noexcept {
  if (text.size() != 32) {
    return false;
  }
  Digest128 value{};
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (int index = 0; index < 16; ++index) {
    const char digit = text[static_cast<std::size_t>(index)];
    std::uint64_t nibble = 0;
    if (digit >= '0' && digit <= '9') {
      nibble = static_cast<std::uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      nibble = static_cast<std::uint64_t>(digit - 'a' + 10);
    } else {
      return false;
    }
    high = (high << 4) | nibble;
  }
  for (int index = 16; index < 32; ++index) {
    const char digit = text[static_cast<std::size_t>(index)];
    std::uint64_t nibble = 0;
    if (digit >= '0' && digit <= '9') {
      nibble = static_cast<std::uint64_t>(digit - '0');
    } else if (digit >= 'a' && digit <= 'f') {
      nibble = static_cast<std::uint64_t>(digit - 'a' + 10);
    } else {
      return false;
    }
    low = (low << 4) | nibble;
  }
  value.high = high;
  value.low = low;
  out = value;
  return true;
}

void CanonicalHasher::add_bytes(std::string_view raw) noexcept {
  const std::uint64_t size = static_cast<std::uint64_t>(raw.size());
  fnv_.update_u64(size);
  fnv_.update(raw.data(), raw.size());
  mix_.update_u64(size);
  mix_.update(raw.data(), raw.size());
}

void CanonicalHasher::add_field(std::string_view name, std::string_view value) noexcept {
  add_bytes(name);
  add_bytes(value);
}

void CanonicalHasher::add_field(std::string_view name, std::uint64_t value) noexcept {
  add_bytes(name);
  fnv_.update_u64(value);
  mix_.update_u64(value);
}

void CanonicalHasher::add_field(std::string_view name, std::int64_t value) noexcept {
  add_field(name, static_cast<std::uint64_t>(value));
}

void CanonicalHasher::add_raw(std::string_view text) noexcept {
  fnv_.update(text.data(), text.size());
  mix_.update(text.data(), text.size());
}

Digest128 CanonicalHasher::digest() const noexcept { return Digest128{fnv_.value(), mix_.value()}; }

std::string canonical_u64(std::uint64_t value) { return std::to_string(value); }

std::string canonical_hex(std::uint64_t value) { return hex_encode(value); }

std::string hex_encode(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return std::string(buffer, 16);
}

std::string canonical_field(std::string_view name, std::string_view value) {
  std::string out;
  out.reserve(name.size() + value.size() + 4);
  out.append(name);
  out.push_back('=');
  out.append(value);
  out.push_back('\n');
  return out;
}

std::string canonical_field(std::string_view name, std::uint64_t value) {
  return canonical_field(name, std::string_view(canonical_u64(value)));
}

std::string canonical_field(std::string_view name, std::int64_t value) {
  return canonical_field(name, std::string_view(std::to_string(value)));
}

void fill_random_bytes(std::uint8_t* out, std::size_t size) noexcept {
  if (size == 0) {
    return;
  }
#if defined(_WIN32)
  if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
    return;
  }
#endif
  // Portable fallback keeps the runtime usable everywhere; it is used for
  // identity entropy only, never as an authority check.
  static std::uint64_t counter = 0x243F6A8885A308D3ull;
  for (std::size_t index = 0; index < size; ++index) {
    counter ^= counter << 13;
    counter ^= counter >> 7;
    counter ^= counter << 17;
    out[index] = static_cast<std::uint8_t>(counter & 0xFFu);
  }
}

}  // namespace optical_fabric
