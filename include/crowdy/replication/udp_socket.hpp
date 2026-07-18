#pragma once

#include <cstdint>
#include <string>

#include "crowdy/core/bytes.hpp"
#include "crowdy/core/result.hpp"

/// Minimal portable UDP socket (POSIX + Winsock). The socket is connect()ed
/// to the replication server, so recv only returns that server's datagrams
/// and send needs no per-call address.
namespace crowdy::replication {

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { close(); }
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  /// Open and connect to host:port. `host` is a literal IPv4 or IPv6 address.
  Status open(const std::string& host, int port, int recvBufferBytes);
  void close();
  bool isOpen() const { return fd_ >= 0; }

  /// Send one datagram. Non-blocking; returns SocketError on failure.
  Status send(Bytes datagram);

  /// Receive one datagram into `buffer`, waiting up to timeoutMs (0 = no
  /// wait). Returns the datagram length, 0 on timeout, or an error.
  Result<std::size_t> recv(MutableBytes buffer, int timeoutMs);

 private:
  long long fd_ = -1;
};

}  // namespace crowdy::replication
