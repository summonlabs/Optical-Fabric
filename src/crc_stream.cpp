// Optical Fabric 1.0.0 - Summon Software Labs
#include "crc_stream.hpp"

#include <array>

namespace optical_fabric::detail {

namespace {

constexpr std::array<std::uint32_t, 256> build_table() {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t polynomial = 0x82F63B78u;  // reflected CRC-32C
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (value >> 1) ^ polynomial : value >> 1;
    }
    table[index] = value;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = build_table();

}  // namespace

void Crc32cStream::update(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  std::uint32_t state = state_;
  for (std::size_t index = 0; index < size; ++index) {
    state = kTable[(state ^ bytes[index]) & 0xFFu] ^ (state >> 8);
  }
  state_ = state;
}

}  // namespace optical_fabric::detail
