// Optical Fabric 1.0.0 - Summon Software Labs
// Minimal blocking TCP socket wrapper for the loopback control plane.
//
// Loopback only: the server binds 127.0.0.1 (or ::1) and Optical Fabric makes
// no claim about multi-host operation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "optical_fabric/error.hpp"

namespace optical_fabric::detail {

#if defined(_WIN32)
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~static_cast<std::uintptr_t>(0));
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

/// Idempotent process-wide socket subsystem initialisation.
[[nodiscard]] Status socket_system_init();

[[nodiscard]] Status socket_listen(const std::string& host, std::uint16_t port, SocketHandle& out,
                                   std::uint16_t& bound_port);
[[nodiscard]] Status socket_accept(SocketHandle listener, SocketHandle& out);
[[nodiscard]] Status socket_connect(const std::string& host, std::uint16_t port, SocketHandle& out);
[[nodiscard]] Status socket_send_all(SocketHandle socket, const std::string& data);

/// Reads at least one byte. `received` is zero when the peer closed cleanly.
[[nodiscard]] Status socket_receive(SocketHandle socket, char* buffer, std::size_t size,
                                    std::size_t& received);

/// Unblocks a blocked accept/receive on this socket. Idempotent.
void socket_shutdown(SocketHandle socket);
void socket_close(SocketHandle socket);
[[nodiscard]] bool socket_valid(SocketHandle socket) noexcept;

}  // namespace optical_fabric::detail
