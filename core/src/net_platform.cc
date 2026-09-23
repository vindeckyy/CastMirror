#include "castcore/net_platform.h"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <iphlpapi.h>
  #include <cerrno>
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <ifaddrs.h>
  #include <net/if.h>
  #include <unistd.h>
  #include <cerrno>
#endif

namespace castcore {

bool EnsureSocketInit() {
#if defined(_WIN32)
  static std::once_flag once;
  static bool ok = false;
  std::call_once(once, [] {
    WSADATA wsa{};
    ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
  });
  return ok;
#else
  return true;
#endif
}

std::vector<IPv4Interface> EnumerateIPv4Interfaces() {
  std::vector<IPv4Interface> out;

#if defined(_WIN32)
  if (!EnsureSocketInit()) {
    return out;
  }

  ULONG buf_len = 16 * 1024;
  std::vector<uint8_t> buf(buf_len);
  auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
  ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                          GAA_FLAG_SKIP_MULTICAST |
                                          GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addresses, &buf_len);
  if (rc == ERROR_BUFFER_OVERFLOW) {
    buf.resize(buf_len);
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                        GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, addresses, &buf_len);
  }
  if (rc != NO_ERROR) {
    return out;
  }

  for (auto* aa = addresses; aa != nullptr; aa = aa->Next) {
    const bool up = (aa->OperStatus == IfOperStatusUp) &&
                    (aa->Flags & IP_ADAPTER_NO_MULTICAST) == 0;
    for (auto* ua = aa->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
      if (!ua->Address.lpSockaddr ||
          ua->Address.lpSockaddr->sa_family != AF_INET) {
        continue;
      }
      auto* sin = reinterpret_cast<struct sockaddr_in*>(ua->Address.lpSockaddr);
      IPv4Interface iface;
      char ip_buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
      iface.address = ip_buf;
      iface.prefix_len = ua->OnLinkPrefixLength;
      uint64_t mask64 = iface.prefix_len >= 32
                            ? 0xFFFFFFFFULL
                            : (0xFFFFFFFFULL << (32 - iface.prefix_len)) & 0xFFFFFFFFULL;
      struct in_addr mask_addr{};
      mask_addr.s_addr = htonl(static_cast<uint32_t>(mask64));
      char mask_buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &mask_addr, mask_buf, sizeof(mask_buf));
      iface.netmask = mask_buf;
      iface.is_loopback = (aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK) ||
                          iface.address.rfind("127.", 0) == 0;
      iface.is_up = up;
      out.push_back(iface);
    }
  }
#else
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return out;
  }
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    IPv4Interface iface;
    char ip_buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &sa->sin_addr, ip_buf, sizeof(ip_buf));
    iface.address = ip_buf;
    iface.is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
    iface.is_up = (ifa->ifa_flags & IFF_UP) != 0;
    if (ifa->ifa_netmask) {
      auto* nm = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_netmask);
      char mask_buf[INET_ADDRSTRLEN]{};
      inet_ntop(AF_INET, &nm->sin_addr, mask_buf, sizeof(mask_buf));
      iface.netmask = mask_buf;
      uint32_t m = ntohl(nm->sin_addr.s_addr);
      iface.prefix_len = static_cast<uint32_t>(__builtin_popcount(m));
    }
    out.push_back(iface);
  }
  freeifaddrs(ifaddr);
#endif

  return out;
}

std::string LocalIpForTarget(const std::string& target_ip) {
  if (!EnsureSocketInit()) {
    return {};
  }
  int fd = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    return {};
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9);  // discard port; UDP connect sends nothing
  if (inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr) <= 0) {
    CloseSocket(fd);
    return {};
  }
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSocket(fd);
    return {};
  }
  struct sockaddr_in local{};
  socklen_t len = sizeof(local);
  std::string out;
  if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
    char ip_buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &local.sin_addr, ip_buf, sizeof(ip_buf))) {
      out = ip_buf;
    }
  }
  CloseSocket(fd);
  return out;
}

int SocketLastError() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool SocketErrorIsTransient(int err) {
#if defined(_WIN32)
  return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT || err == WSAEINTR ||
         err == WSAEINPROGRESS;
#else
  return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

std::string SocketErrorString() {
  int err = SocketLastError();
#if defined(_WIN32)
  char buf[256]{};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(err),
                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 buf, sizeof(buf) - 1, nullptr);
  std::string s = buf;
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
    s.pop_back();
  }
  return s.empty() ? ("WSA error " + std::to_string(err)) : s;
#else
  return std::strerror(err);
#endif
}

void CloseSocket(int fd) {
  if (fd < 0) {
    return;
  }
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(fd));
#else
  close(fd);
#endif
}

}  // namespace castcore
