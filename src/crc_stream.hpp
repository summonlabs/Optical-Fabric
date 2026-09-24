// Optical Fabric 1.0.0 - Summon Software Labs
// Incremental CRC-32C, so a frame can be checksummed without copying its
// header and payload into one buffer.
#pragma once

#include <cstddef>
#include <cstdint>

namespace optical_fabric::detail {

class Crc32cStream {
 public:
  void update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] std::uint32_t finish() const noexcept { return state_ ^ 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

}  // namespace optical_fabric::detail
