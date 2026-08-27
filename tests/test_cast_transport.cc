#include <gtest/gtest.h>
#include "castcore/cast_transport.h"
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define close closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

using namespace castcore;

namespace {

std::vector<uint8_t> BuildPliPacket(uint32_t receiver_ssrc, uint32_t sender_ssrc) {
  // RTCP Payload-specific feedback (PT 206), FMT 1 (PLI), length 2 (12 bytes)
  std::vector<uint8_t> pkt(12, 0);
  pkt[0] = 0x81; // V=2, P=0, FMT=1 (PLI)
  pkt[1] = 206;  // PT=206
  pkt[2] = 0x00; pkt[3] = 0x02; // length = 2 (12 bytes total)
  pkt[4] = static_cast<uint8_t>((receiver_ssrc >> 24) & 0xFF);
  pkt[5] = static_cast<uint8_t>((receiver_ssrc >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((receiver_ssrc >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(receiver_ssrc & 0xFF);
  pkt[8] = static_cast<uint8_t>((sender_ssrc >> 24) & 0xFF);
  pkt[9] = static_cast<uint8_t>((sender_ssrc >> 16) & 0xFF);
  pkt[10] = static_cast<uint8_t>((sender_ssrc >> 8) & 0xFF);
  pkt[11] = static_cast<uint8_t>(sender_ssrc & 0xFF);
  return pkt;
}

std::vector<uint8_t> BuildDuplicateNackPacket(uint32_t receiver_ssrc,
                                              uint32_t sender_ssrc,
                                              uint8_t frame_id,
                                              uint16_t packet_id) {
  // CAST feedback with two identical loss fields. A receiver may repeat the
  // same NACK while the first retransmit is still in flight.
  std::vector<uint8_t> pkt(28, 0);
  pkt[0] = 0x8F;
  pkt[1] = 206;
  pkt[2] = 0x00; pkt[3] = 0x06;
  pkt[4] = static_cast<uint8_t>((receiver_ssrc >> 24) & 0xFF);
  pkt[5] = static_cast<uint8_t>((receiver_ssrc >> 16) & 0xFF);
  pkt[6] = static_cast<uint8_t>((receiver_ssrc >> 8) & 0xFF);
  pkt[7] = static_cast<uint8_t>(receiver_ssrc & 0xFF);
  pkt[8] = static_cast<uint8_t>((sender_ssrc >> 24) & 0xFF);
  pkt[9] = static_cast<uint8_t>((sender_ssrc >> 16) & 0xFF);
  pkt[10] = static_cast<uint8_t>((sender_ssrc >> 8) & 0xFF);
  pkt[11] = static_cast<uint8_t>(sender_ssrc & 0xFF);
  pkt[12] = 'C'; pkt[13] = 'A'; pkt[14] = 'S'; pkt[15] = 'T';
  pkt[16] = static_cast<uint8_t>(frame_id - 1);
  pkt[17] = 2;
  pkt[18] = 0; pkt[19] = 200;
  for (size_t off : {size_t{20}, size_t{24}}) {
    pkt[off] = frame_id;
    pkt[off + 1] = static_cast<uint8_t>((packet_id >> 8) & 0xFF);
    pkt[off + 2] = static_cast<uint8_t>(packet_id & 0xFF);
    pkt[off + 3] = 0;
  }
  return pkt;
}

} // namespace

TEST(CastTransportTest, AcceptsRtcpFromTargetAndFiltersForeignIp) {
  // 1. Setup local receiver UDP socket
  int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(recv_fd, 0);

  struct sockaddr_in recv_addr{};
  recv_addr.sin_family = AF_INET;
  recv_addr.sin_port = 0; // Ephemeral
  inet_pton(AF_INET, "127.0.0.1", &recv_addr.sin_addr);
  ASSERT_EQ(bind(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), sizeof(recv_addr)), 0);

  socklen_t addr_len = sizeof(recv_addr);
  ASSERT_EQ(getsockname(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), &addr_len), 0);
  uint16_t receiver_port = ntohs(recv_addr.sin_port);

  // 2. Start transport pointing to the receiver
  CastTransport transport;
  ASSERT_TRUE(transport.Start("127.0.0.1", receiver_port));

  // 3. Send one RTP packet so receiver discovers sender's ephemeral port
  RtpPacket p;
  p.data = {0x80, 96, 0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 1, 0x40, 0, 0, 0, 0};
  transport.SendPackets({p});

  uint8_t buf[256];
  struct sockaddr_in sender_addr{};
  socklen_t slen = sizeof(sender_addr);
  ssize_t got = recvfrom(recv_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                         reinterpret_cast<struct sockaddr*>(&sender_addr), &slen);
  ASSERT_GT(got, 0);

  // 4. Send PLI RTCP packet from valid 127.0.0.1 address
  auto pli = BuildPliPacket(10001, 1);
  ssize_t sent = sendto(recv_fd, reinterpret_cast<const char*>(pli.data()), pli.size(), 0,
                        reinterpret_cast<struct sockaddr*>(&sender_addr), sizeof(sender_addr));
  ASSERT_EQ(sent, static_cast<ssize_t>(pli.size()));

  // Wait up to 300ms for transport receive thread to process
  for (int i = 0; i < 30 && transport.GetStats().pli_received == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GE(transport.GetStats().pli_received, 1u);
  uint32_t valid_pli_count = transport.GetStats().pli_received;

  // 5. Send from foreign IP 127.0.0.2 (must be dropped by destination filter)
  int foreign_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(foreign_fd, 0);
  struct sockaddr_in foreign_addr{};
  foreign_addr.sin_family = AF_INET;
  foreign_addr.sin_port = 0;
  ASSERT_GT(inet_pton(AF_INET, "127.0.0.2", &foreign_addr.sin_addr), 0);
  ASSERT_EQ(bind(foreign_fd, reinterpret_cast<struct sockaddr*>(&foreign_addr), sizeof(foreign_addr)), 0);

  sendto(foreign_fd, reinterpret_cast<const char*>(pli.data()), pli.size(), 0,
         reinterpret_cast<struct sockaddr*>(&sender_addr), sizeof(sender_addr));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(transport.GetStats().pli_received, valid_pli_count);
  close(foreign_fd);

  transport.Stop();
  close(recv_fd);
}

TEST(CastTransportTest, ReportsRollingVideoFpsAfterRateChange) {
  int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(recv_fd, 0);
  struct sockaddr_in recv_addr{};
  recv_addr.sin_family = AF_INET;
  recv_addr.sin_port = 0;
  inet_pton(AF_INET, "127.0.0.1", &recv_addr.sin_addr);
  ASSERT_EQ(bind(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), sizeof(recv_addr)), 0);
  socklen_t addr_len = sizeof(recv_addr);
  ASSERT_EQ(getsockname(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), &addr_len), 0);

  CastTransport transport;
  ASSERT_TRUE(transport.Start("127.0.0.1", ntohs(recv_addr.sin_port)));

  auto send_video_frame = [&](uint32_t frame_id) {
    RtpPacket packet;
    packet.frame_id = frame_id;
    packet.data = {0x80, 96, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2,
                   0x40, static_cast<uint8_t>(frame_id), 0, 0, 0, 0, 0};
    ASSERT_TRUE(transport.SendPackets({packet}));
  };

  for (uint32_t i = 0; i < 30; ++i) {
    send_video_frame(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  const double fast_fps = transport.GetStats().current_fps;
  EXPECT_GT(fast_fps, 45.0);
  EXPECT_LT(fast_fps, 75.0);

  for (uint32_t i = 30; i < 45; ++i) {
    send_video_frame(i);
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  const double slow_fps = transport.GetStats().current_fps;
  EXPECT_GT(slow_fps, 20.0);
  EXPECT_LT(slow_fps, 40.0);

  transport.Stop();
  close(recv_fd);
}

TEST(CastTransportTest, SuppressesDuplicateNackRetransmitBurst) {
  int recv_fd = socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(recv_fd, 0);
  struct sockaddr_in recv_addr{};
  recv_addr.sin_family = AF_INET;
  recv_addr.sin_port = 0;
  inet_pton(AF_INET, "127.0.0.1", &recv_addr.sin_addr);
  ASSERT_EQ(bind(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), sizeof(recv_addr)), 0);
  socklen_t addr_len = sizeof(recv_addr);
  ASSERT_EQ(getsockname(recv_fd, reinterpret_cast<struct sockaddr*>(&recv_addr), &addr_len), 0);

  CastTransport transport;
  ASSERT_TRUE(transport.Start("127.0.0.1", ntohs(recv_addr.sin_port)));

  RtpPacket packet;
  packet.frame_id = 10;
  packet.packet_id = 3;
  packet.data = {0x80, 96, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2,
                 0x40, 10, 0, 3, 0, 3, 9};
  ASSERT_TRUE(transport.SendPackets({packet}));

#if defined(_WIN32)
  DWORD timeout_ms = 100;
  setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
  struct timeval timeout{};
  timeout.tv_sec = 0;
  timeout.tv_usec = 100000;
  setsockopt(recv_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

  uint8_t buf[256];
  struct sockaddr_in sender_addr{};
  socklen_t sender_len = sizeof(sender_addr);
  ASSERT_GT(recvfrom(recv_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                     reinterpret_cast<struct sockaddr*>(&sender_addr), &sender_len), 0);
  // Drain an optional sender report emitted beside the initial RTP packet.
  while (recvfrom(recv_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                  reinterpret_cast<struct sockaddr*>(&sender_addr), &sender_len) > 0) {}

  auto nack = BuildDuplicateNackPacket(10001, 2, 10, 3);
  ASSERT_EQ(sendto(recv_fd, reinterpret_cast<const char*>(nack.data()), nack.size(), 0,
                   reinterpret_cast<struct sockaddr*>(&sender_addr), sizeof(sender_addr)),
            static_cast<ssize_t>(nack.size()));

  ASSERT_GT(recvfrom(recv_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                     nullptr, nullptr), 0);
  EXPECT_LE(recvfrom(recv_fd, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                     nullptr, nullptr), 0)
      << "Duplicate NACK entries retransmitted the same RTP packet twice";

  transport.Stop();
  close(recv_fd);
}
