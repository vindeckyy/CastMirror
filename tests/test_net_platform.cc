// Coverage for the socket helpers every network path depends on: interface
// enumeration, the netmask/prefix derivation, the LocalIpForTarget UDP-connect
// sniff, and the transient-error classifier.

#include <gtest/gtest.h>
#include "castcore/net_platform.h"

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace castcore;

namespace {

bool IsDottedQuad(const std::string& s) {
  struct in_addr addr{};
  return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

// Reconstructs the dotted-quad netmask for a CIDR prefix exactly the way
// net_platform.cc:79-86 does, so the reported netmask and prefix must agree.
std::string MaskForPrefix(uint32_t prefix_len) {
  uint64_t mask64 = prefix_len >= 32
                        ? 0xFFFFFFFFULL
                        : (0xFFFFFFFFULL << (32 - prefix_len)) & 0xFFFFFFFFULL;
  struct in_addr a{};
  a.s_addr = htonl(static_cast<uint32_t>(mask64));
  char buf[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

}  // namespace

TEST(NetPlatformTest, EnumerateIPv4InterfacesHonoursInvariants) {
  const auto interfaces = EnumerateIPv4Interfaces();
  for (const auto& iface : interfaces) {
    SCOPED_TRACE(iface.address + " mask " + iface.netmask + " /" +
                 std::to_string(iface.prefix_len));

    EXPECT_TRUE(IsDottedQuad(iface.address)) << "address must be a dotted quad";
    EXPECT_TRUE(IsDottedQuad(iface.netmask)) << "netmask must be a dotted quad";
    EXPECT_LE(iface.prefix_len, 32u) << "a prefix length can never exceed 32";

    // 127.0.0.0/8 is always the loopback range and must be flagged as such.
    if (iface.address.rfind("127.", 0) == 0) {
      EXPECT_TRUE(iface.is_loopback);
    }

    // On Windows the netmask is derived from the prefix; on POSIX the prefix is
    // derived from the netmask. Either way the two must describe the same mask.
    EXPECT_EQ(iface.netmask, MaskForPrefix(iface.prefix_len));
  }
}

TEST(NetPlatformTest, EnsureSocketInitIsIdempotent) {
  EXPECT_TRUE(EnsureSocketInit());
  EXPECT_TRUE(EnsureSocketInit());
}

TEST(NetPlatformTest, LocalIpForTargetResolvesLoopback) {
  const std::string local_ip = LocalIpForTarget("127.0.0.1");
  ASSERT_FALSE(local_ip.empty()) << "UDP connect to 127.0.0.1 must report the local address";
  EXPECT_TRUE(IsDottedQuad(local_ip));
  EXPECT_EQ(local_ip.rfind("127.", 0), 0u) << "expected a loopback source address, got " << local_ip;
}

TEST(NetPlatformTest, LocalIpForTargetRejectsBogusTargetsWithoutThrowing) {
  EXPECT_EQ(LocalIpForTarget("not-an-ip"), "");
  EXPECT_EQ(LocalIpForTarget(""), "");
  EXPECT_EQ(LocalIpForTarget("999.999.999.999"), "");

  // TEST-NET-3 is never routable: the call must return (empty or a genuine local
  // address) rather than throw or hang.
  const std::string ip = LocalIpForTarget("203.0.113.7");
  EXPECT_TRUE(ip.empty() || IsDottedQuad(ip));
}

TEST(NetPlatformTest, CloseSocketIgnoresInvalidDescriptor) {
  CloseSocket(-1);  // must be a no-op, not a crash

  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  ASSERT_GE(fd, 0);
  CloseSocket(fd);
}

TEST(NetPlatformTest, SocketErrorStringIsNeverEmpty) {
  EXPECT_FALSE(SocketErrorString().empty());
}

TEST(NetPlatformTest, SocketErrorIsTransientClassifiesPlatformCodes) {
#if defined(_WIN32)
  EXPECT_TRUE(SocketErrorIsTransient(WSAEWOULDBLOCK));
  EXPECT_TRUE(SocketErrorIsTransient(WSAETIMEDOUT));
  EXPECT_TRUE(SocketErrorIsTransient(WSAEINTR));
  EXPECT_TRUE(SocketErrorIsTransient(WSAEINPROGRESS));
  EXPECT_FALSE(SocketErrorIsTransient(WSAECONNREFUSED));
  EXPECT_FALSE(SocketErrorIsTransient(WSAECONNRESET));
  EXPECT_FALSE(SocketErrorIsTransient(0));
#else
  EXPECT_TRUE(SocketErrorIsTransient(EAGAIN));
  EXPECT_TRUE(SocketErrorIsTransient(EWOULDBLOCK));
  EXPECT_TRUE(SocketErrorIsTransient(EINTR));
  EXPECT_FALSE(SocketErrorIsTransient(ECONNREFUSED));
  EXPECT_FALSE(SocketErrorIsTransient(ECONNRESET));
  EXPECT_FALSE(SocketErrorIsTransient(0));
#endif
}
