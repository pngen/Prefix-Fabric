// Prefix Fabric - Winsock TCP transport implementation.
// Copyright 2026 Summon Software Labs. Licensed under Apache-2.0.
#include "prefix_fabric/net.hpp"

#include <cstring>
#include <thread>
#include <vector>

namespace prefix_fabric {

#ifdef _WIN32
static bool g_net_inited = false;
bool NetInit() {
  if (g_net_inited) return true;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  g_net_inited = true;
  return true;
}
void NetShutdown() { if (g_net_inited) { WSACleanup(); g_net_inited = false; } }
#else
bool NetInit() { return true; }
void NetShutdown() {}
#endif

Result<void> TcpConnection::Connect(const std::string& host, std::uint16_t port, int timeout_ms) {
  if (IsOpen()) Close();
  struct addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_INET;
  struct addrinfo* res = nullptr;
  std::string portstr = std::to_string(port);
#ifdef _WIN32
  if (getaddrinfo(host.c_str(), portstr.c_str(), &hints, &res) != 0)
    return Result<void>::Err(Error{ErrorCode::kNetworkError, "getaddrinfo"});
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) { freeaddrinfo(res); return Result<void>::Err(Error{ErrorCode::kNetworkError, "socket"}); }
  u_long nonblock = 1;
  ioctlsocket(s, FIONBIO, &nonblock);
  int rc = ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
  freeaddrinfo(res);
  if (rc == SOCKET_ERROR) {
    fd_set wset; FD_ZERO(&wset); FD_SET(s, &wset);
    timeval tv{}; tv.tv_usec = timeout_ms * 1000;
    int sel = select(0, nullptr, &wset, nullptr, &tv);
    if (sel <= 0) { closesocket(s); return Result<void>::Err(Error{ErrorCode::kNetworkError, "connect timeout"}); }
    int err = 0; int elen = sizeof(err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &elen);
    if (err != 0) { closesocket(s); return Result<void>::Err(Error{ErrorCode::kNetworkError, "connect"}); }
  }
  u_long blocking = 0;
  ioctlsocket(s, FIONBIO, &blocking);
  sock_ = s;
  return Result<void>::Ok();
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { freeaddrinfo(res); return Result<void>::Err(Error{ErrorCode::kNetworkError, "socket"}); }
  if (::connect(s, res->ai_addr, res->ai_addrlen) != 0) { freeaddrinfo(res); closesocket(s); return Result<void>::Err(Error{ErrorCode::kNetworkError, "connect"}); }
  freeaddrinfo(res);
  sock_ = s;
  return Result<void>::Ok();
#endif
}

void TcpConnection::Adopt(SOCKET s) { if (IsOpen()) Close(); sock_ = s; }

Result<void> TcpConnection::SendAll(const void* data, std::size_t len) {
  const char* p = static_cast<const char*>(data);
  std::size_t total = 0;
  while (total < len) {
    int n = send(sock_, p + total, static_cast<int>(len - total), 0);
    if (n <= 0) return Result<void>::Err(Error{ErrorCode::kNetworkError, "send"});
    total += static_cast<std::size_t>(n);
  }
  return Result<void>::Ok();
}

Result<void> TcpConnection::RecvAll(void* buf, std::size_t len) {
  char* p = static_cast<char*>(buf);
  std::size_t total = 0;
  while (total < len) {
    int n = recv(sock_, p + total, static_cast<int>(len - total), 0);
    if (n == 0) return Result<void>::Err(Error{ErrorCode::kNetworkError, "eof"});
    if (n < 0) return Result<void>::Err(Error{ErrorCode::kNetworkError, "recv"});
    total += static_cast<std::size_t>(n);
  }
  return Result<void>::Ok();
}

Result<void> TcpConnection::SendFrame(std::string_view payload) {
  if (payload.size() > 0xFFFFFFFFULL) return Result<void>::Err(Error{ErrorCode::kProtocolError, "frame too big"});
  std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  std::uint8_t hdr[4];
  hdr[0] = static_cast<std::uint8_t>(len & 0xffu); hdr[1] = static_cast<std::uint8_t>((len >> 8) & 0xffu);
  hdr[2] = static_cast<std::uint8_t>((len >> 16) & 0xffu); hdr[3] = static_cast<std::uint8_t>((len >> 24) & 0xffu);
  auto r = SendAll(hdr, 4);
  if (r.is_err()) return r;
  return SendAll(payload.data(), payload.size());
}

Result<std::string> TcpConnection::RecvFrame(std::uint64_t max_len) {
  std::uint8_t hdr[4];
  auto r = RecvAll(hdr, 4);
  if (r.is_err()) return Result<std::string>::Err(r.error());
  std::uint32_t len = (static_cast<std::uint32_t>(hdr[0])) | (static_cast<std::uint32_t>(hdr[1]) << 8) |
                      (static_cast<std::uint32_t>(hdr[2]) << 16) | (static_cast<std::uint32_t>(hdr[3]) << 24);
  if (len > max_len) return Result<std::string>::Err(Error{ErrorCode::kProtocolError, "frame exceeds max_len"});
  std::string out(len, static_cast<char>(0));
  if (len > 0) { auto r2 = RecvAll(out.data(), len); if (r2.is_err()) return Result<std::string>::Err(r2.error()); }
  return out;
}

void TcpConnection::Close() { if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; } }

Result<std::uint16_t> TcpListener::Listen(const std::string& host, std::uint16_t port) {
  (void)host;
  if (listen_ != INVALID_SOCKET) Close();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) return Result<std::uint16_t>::Err(Error{ErrorCode::kNetworkError, "socket"});
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(s);
    return Result<std::uint16_t>::Err(Error{ErrorCode::kNetworkError, "bind"});
  }
  if (listen(s, 16) == SOCKET_ERROR) { closesocket(s); return Result<std::uint16_t>::Err(Error{ErrorCode::kNetworkError, "listen"}); }
  sockaddr_in bound{};
#ifdef _WIN32
  int len = sizeof(bound);
  getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
#else
  socklen_t len = sizeof(bound);
  getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len);
#endif
  listen_ = s;
  port_ = ntohs(bound.sin_port);
  return Result<std::uint16_t>::Ok(port_);
}

Result<TcpConnection> TcpListener::Accept() {
  sockaddr_in caddr{};
#ifdef _WIN32
  int clen = sizeof(caddr);
#else
  socklen_t clen = sizeof(caddr);
#endif
  SOCKET c = accept(listen_, reinterpret_cast<sockaddr*>(&caddr), &clen);
  if (c == INVALID_SOCKET) return Result<TcpConnection>::Err(Error{ErrorCode::kNetworkError, "accept"});
  TcpConnection conn;
  conn.Adopt(c);
  return Result<TcpConnection>::Ok(std::move(conn));
}

void TcpListener::Close() { if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; } }

}  // namespace prefix_fabric