#ifndef CASTCORE_NET_PLATFORM_H_
#define CASTCORE_NET_PLATFORM_H_

#include <cstdint>
#include <string>
#include <vector>

namespace castcore {

// One-time socket subsystem initialization. On Windows this calls
// WSAStartup(2,2) exactly once (thread-safe); elsewhere it is a no-op.
// Must be called before any socket() usage. Safe to call repeatedly.
bool EnsureSocketInit();

// A local IPv4 interface suitable for multicast / LAN traffic.
struct IPv4Interface {
  std::string address;  // dotted-quad interface address, e.g. "192.168.1.10"
  std::string netmask;  // dotted-quad netmask, e.g. "255.255.255.0"
  uint32_t prefix_len = 0;  // CIDR prefix length (24 for the example above)
  bool is_loopback = false;
  bool is_up = true;
};

// Enumerate all local IPv4 interfaces (cross-platform: getifaddrs on POSIX,
// GetAdaptersAddresses on Windows). Interfaces that are down or lack an
// IPv4 unicast address are included with is_up=false so callers can filter.
std::vector<IPv4Interface> EnumerateIPv4Interfaces();

// Returns the local IPv4 address that would be used to reach `target_ip`
// (a connected UDP socket lookup; no traffic is sent). Empty string on
// failure — callers should fall back to a sensible default.
std::string LocalIpForTarget(const std::string& target_ip);

// Returns a human-readable description of the last socket error
// (WSAGetLastError on Windows, errno elsewhere).
std::string SocketErrorString();

// Last socket error code, normalized: on Windows this is WSAGetLastError(),
// elsewhere errno.
int SocketLastError();

// Returns true when the socket error code represents a transient
// "try again" condition (EAGAIN/EWOULDBLOCK/WSAEWOULDBLOCK/WSAETIMEDOUT/EINTR).
bool SocketErrorIsTransient(int err);

// Portable close for a socket fd.
void CloseSocket(int fd);

}  // namespace castcore

#endif  // CASTCORE_NET_PLATFORM_H_
