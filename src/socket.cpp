// Optical Fabric 1.0.0 - Summon Software Labs
#include "socket.hpp"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace optical_fabric::detail {

namespace {

#if defined(_WIN32)
[[nodiscard]] std::string last_error_text() {
  return "socket error " + std::to_string(WSAGetLastError());
}
#else
[[nodiscard]] std::string last_error_text() { return std::string(std::strerror(errno)); }
#endif

}  // namespace

Status socket_system_init() {
#if defined(_WIN32)
  static std::once_flag once;
  static Status status = Status{};
  std::call_once(once, []() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      status = Status::failure(ErrorCode::Internal, "WSAStartup failed");
    }
  });
  return status;
#else
  return Status{};
#endif
}

bool socket_valid(SocketHandle socket) noexcept { return socket != kInvalidSocket; }

Status socket_listen(const std::string& host, std::uint16_t port, SocketHandle& out,
                     std::uint16_t& bound_port) {
  const Status initialised = socket_system_init();
  if (!initialised.ok()) {
    return initialised;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const std::string node = host.empty() ? std::string("127.0.0.1") : host;
  if (::getaddrinfo(node.c_str(), service.c_str(), &hints, &result) != 0) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot resolve the bind address");
  }
  SocketHandle listener = kInvalidSocket;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
#if defined(_WIN32)
    const SOCKET handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == INVALID_SOCKET) {
      continue;
    }
    if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(handle, SOMAXCONN) == 0) {
      listener = static_cast<SocketHandle>(handle);
      break;
    }
    ::closesocket(handle);
#else
    const int handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle < 0) {
      continue;
    }
    int reuse = 1;
    ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(handle, candidate->ai_addr, candidate->ai_addrlen) == 0 && ::listen(handle, 64) == 0) {
      listener = handle;
      break;
    }
    ::close(handle);
#endif
  }
  ::freeaddrinfo(result);
  if (listener == kInvalidSocket) {
    return Status::failure(ErrorCode::Internal, "cannot bind the listening socket: " + last_error_text());
  }
  sockaddr_storage address{};
#if defined(_WIN32)
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
#else
  socklen_t length = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
#endif
    socket_close(listener);
    return Status::failure(ErrorCode::Internal, "cannot read back the bound port");
  }
  if (address.ss_family == AF_INET) {
    bound_port = ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
  } else {
    bound_port = ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
  }
  out = listener;
  return Status{};
}

Status socket_accept(SocketHandle listener, SocketHandle& out) {
#if defined(_WIN32)
  const SOCKET handle = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
  if (handle == INVALID_SOCKET) {
    return Status::failure(ErrorCode::Closed, "accept failed: " + last_error_text());
  }
  out = static_cast<SocketHandle>(handle);
#else
  const int handle = ::accept(listener, nullptr, nullptr);
  if (handle < 0) {
    return Status::failure(ErrorCode::Closed, "accept failed: " + last_error_text());
  }
  out = handle;
#endif
  return Status{};
}

Status socket_connect(const std::string& host, std::uint16_t port, SocketHandle& out) {
  const Status initialised = socket_system_init();
  if (!initialised.ok()) {
    return initialised;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
    return Status::failure(ErrorCode::InvalidArgument, "cannot resolve the control-plane address");
  }
  SocketHandle connected = kInvalidSocket;
  for (addrinfo* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
#if defined(_WIN32)
    const SOCKET handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle == INVALID_SOCKET) {
      continue;
    }
    if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      connected = static_cast<SocketHandle>(handle);
      break;
    }
    ::closesocket(handle);
#else
    const int handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (handle < 0) {
      continue;
    }
    if (::connect(handle, candidate->ai_addr, candidate->ai_addrlen) == 0) {
      connected = handle;
      break;
    }
    ::close(handle);
#endif
  }
  ::freeaddrinfo(result);
  if (connected == kInvalidSocket) {
    return Status::failure(ErrorCode::StoreIo, "cannot connect to the control plane");
  }
  out = connected;
  return Status{};
}

Status socket_send_all(SocketHandle socket, const std::string& data) {
  std::size_t total = 0;
  while (total < data.size()) {
    const std::size_t remaining = data.size() - total;
#if defined(_WIN32)
    const int chunk = ::send(static_cast<SOCKET>(socket), data.data() + total,
                             static_cast<int>(remaining), 0);
    if (chunk == SOCKET_ERROR) {
      return Status::failure(ErrorCode::StoreIo, "cannot send on the control connection");
    }
#else
    const ssize_t chunk = ::send(socket, data.data() + total, remaining, MSG_NOSIGNAL);
    if (chunk < 0) {
      return Status::failure(ErrorCode::StoreIo, "cannot send on the control connection");
    }
#endif
    if (chunk == 0) {
      return Status::failure(ErrorCode::StoreIo, "the control connection accepted no bytes");
    }
    total += static_cast<std::size_t>(chunk);
  }
  return Status{};
}

Status socket_receive(SocketHandle socket, char* buffer, std::size_t size, std::size_t& received) {
  received = 0;
  if (size == 0) {
    return Status{};
  }
#if defined(_WIN32)
  const int chunk = ::recv(static_cast<SOCKET>(socket), buffer, static_cast<int>(size), 0);
  if (chunk == SOCKET_ERROR) {
    return Status::failure(ErrorCode::Closed, "the control connection failed");
  }
#else
  const ssize_t chunk = ::recv(socket, buffer, size, 0);
  if (chunk < 0) {
    return Status::failure(ErrorCode::Closed, "the control connection failed");
  }
#endif
  received = static_cast<std::size_t>(chunk);
  return Status{};
}

void socket_shutdown(SocketHandle socket) {
  if (!socket_valid(socket)) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(static_cast<SOCKET>(socket), SD_BOTH);
#else
  ::shutdown(socket, SHUT_RDWR);
#endif
}

void socket_close(SocketHandle socket) {
  if (!socket_valid(socket)) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(static_cast<SOCKET>(socket));
#else
  ::close(socket);
#endif
}

}  // namespace optical_fabric::detail
