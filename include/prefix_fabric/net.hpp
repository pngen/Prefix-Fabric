// Prefix Fabric - Winsock TCP transport + length-prefixed framing.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "prefix_fabric/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

namespace prefix_fabric {

// One-time Winsock initialisation (idempotent).
bool NetInit();
void NetShutdown();

// A blocking TCP connection with 4-byte length-prefixed framing (max frame
// bound enforced before allocation).
class TcpConnection {
 public:
  TcpConnection() = default;
  ~TcpConnection() { Close(); }
  TcpConnection(TcpConnection&& o) noexcept { sock_ = o.sock_; o.sock_ = INVALID_SOCKET; }
  TcpConnection& operator=(TcpConnection&& o) noexcept { if (this != &o) { Close(); sock_ = o.sock_; o.sock_ = INVALID_SOCKET; } return *this; }
  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  // Connect to host:port. Returns an error on failure (does not throw).
  Result<void> Connect(const std::string& host, std::uint16_t port, int timeout_ms = 5000);
  // Take ownership of an accepted socket.
  void Adopt(SOCKET s);
  bool IsOpen() const { return sock_ != INVALID_SOCKET; }
  SOCKET NativeHandle() const { return sock_; }

  // Send one frame (4-byte length + payload). Returns error on failure/short.
  Result<void> SendFrame(std::string_view payload);
  // Receive one frame; `max_len` bounds the accepted payload size.
  Result<std::string> RecvFrame(std::uint64_t max_len);
  // Send a u32 (little-endian) then payload, then flush.
  Result<void> SendAll(const void* data, std::size_t len);
  Result<void> RecvAll(void* buf, std::size_t len);

  void Close();
 private:
  SOCKET sock_ = INVALID_SOCKET;
};

// A TCP listener (host:port) and blocking accept.
class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener() { Close(); }
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Bind and listen on port (0 => ephemeral). Returns the bound port.
  Result<std::uint16_t> Listen(const std::string& host, std::uint16_t port);
  Result<TcpConnection> Accept();
  void Close();
  std::uint16_t Port() const { return port_; }
 private:
  SOCKET listen_ = INVALID_SOCKET;
  std::uint16_t port_ = 0;
};

}  // namespace prefix_fabric