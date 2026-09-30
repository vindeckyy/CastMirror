#include <gtest/gtest.h>
#include "castcore/rtp_packetizer.h"
#include "castcore/rtcp_parser.h"
#include "castcore/cast_transport.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

using namespace castcore;

TEST(RtpPacketizerTest, KeyframesIncludeReferenceFrameIdLikeOpenscreen) {
  RtpPacketizer packetizer(96, 2, 1460);

  EncodedFrame frame;
  frame.dependency = FrameDependency::kKeyFrame;
  frame.frame_id = 1;
  frame.referenced_frame_id = 1;
  frame.rtp_timestamp = 1500;
  frame.playout_delay = std::chrono::milliseconds(400);
  frame.data.resize(64, 0x55);

  auto packets = packetizer.PacketizeFrame(frame);
  ASSERT_FALSE(packets.empty());
  const auto& pkt = packets[0];
  ASSERT_GE(pkt.data.size(), 23u);
  // K=1, R=1, EXT=1 -> 0xC1
  EXPECT_EQ(pkt.data[12], 0xC1);
  EXPECT_EQ(pkt.data[13], 1u);   // frame id
  EXPECT_EQ(pkt.data[18], 1u);   // referenced frame id always present
}

TEST(RtpPacketizerTest, SplitsLargeFramesAcrossPackets) {
  RtpPacketizer packetizer(96, 2, 1000); // 1000 byte MTU

  EncodedFrame frame;
  frame.dependency = FrameDependency::kKeyFrame;
  frame.frame_id = 10;
  frame.referenced_frame_id = 10;
  frame.rtp_timestamp = 90000;
  frame.playout_delay = std::chrono::milliseconds(400);
  frame.data.resize(2500, 0x55); // 2500 bytes payload -> should split into ~3 packets

  auto packets = packetizer.PacketizeFrame(frame);
  EXPECT_GE(packets.size(), 3u);

  for (size_t i = 0; i < packets.size(); ++i) {
    const auto& pkt = packets[i];
    EXPECT_LE(pkt.data.size(), 1000u);
    EXPECT_EQ(pkt.frame_id, 10u);
    EXPECT_EQ(pkt.packet_id, static_cast<uint16_t>(i));
    EXPECT_EQ(pkt.max_packet_id, static_cast<uint16_t>(packets.size() - 1));

    // Verify RTP Version 2 in byte 0
    EXPECT_EQ(pkt.data[0], 0x80);

    // Verify Marker bit only on last packet
    if (i == packets.size() - 1) {
      EXPECT_EQ(pkt.data[1] & 0x80, 0x80);
    } else {
      EXPECT_EQ(pkt.data[1] & 0x80, 0x00);
    }
  }
}

TEST(RtcpParserTest, ParseCastFeedbackAndLossFields) {
  // Construct simulated Cast RTCP feedback packet
  std::vector<uint8_t> rtcp(36, 0);

  // Common Header: V=2, Subtype=15, PT=206 (Payload Specific), Length = 8 words
  rtcp[0] = 0x8F;
  rtcp[1] = 206;
  rtcp[2] = 0x00; rtcp[3] = 0x08;

  // Receiver SSRC
  rtcp[4] = 0x00; rtcp[5] = 0x00; rtcp[6] = 0x27; rtcp[7] = 0x12; // 10002
  // Sender SSRC
  rtcp[8] = 0x00; rtcp[9] = 0x00; rtcp[10] = 0x00; rtcp[11] = 0x02; // 2

  // Magic 'CAST'
  rtcp[12] = 'C'; rtcp[13] = 'A'; rtcp[14] = 'S'; rtcp[15] = 'T';

  // Checkpoint Frame ID = 15, Loss fields count = 1, Playout Delay = 400ms
  rtcp[16] = 15;
  rtcp[17] = 1;
  rtcp[18] = 0x01; rtcp[19] = 0x90; // 400

  // Loss field 0: Frame ID = 16, Lost Packet ID = 3, Bit vector = 0
  rtcp[20] = 16;
  rtcp[21] = 0x00; rtcp[22] = 0x03;
  rtcp[23] = 0x00;

  RtcpFeedback fb;
  EXPECT_TRUE(RtcpParser::ParseCompoundPacket(rtcp.data(), rtcp.size(), 20, fb));
  EXPECT_EQ(fb.receiver_ssrc, 10002u);
  EXPECT_EQ(fb.sender_ssrc, 2u);
  EXPECT_EQ(fb.checkpoint_frame_id, 15u);
  EXPECT_EQ(fb.current_playout_delay_ms, 400);
  ASSERT_EQ(fb.nacks.size(), 1u);
  EXPECT_EQ(fb.nacks[0].frame_id, 16u);
  EXPECT_EQ(fb.nacks[0].packet_id, 3u);
}

TEST(RtcpParserTest, ExpandsCheckpointAgainstMatchingSsrc) {
  std::vector<uint8_t> rtcp(36, 0);
  rtcp[0] = 0x8F;
  rtcp[1] = 206;
  rtcp[2] = 0x00; rtcp[3] = 0x08;
  rtcp[8] = 0x00; rtcp[9] = 0x00; rtcp[10] = 0x00; rtcp[11] = 0x02; // media SSRC 2
  rtcp[12] = 'C'; rtcp[13] = 'A'; rtcp[14] = 'S'; rtcp[15] = 'T';
  rtcp[16] = 20; // truncated checkpoint
  rtcp[17] = 0;
  rtcp[18] = 0x00; rtcp[19] = 0xC8;

  std::map<uint32_t, uint32_t> last_by_ssrc;
  last_by_ssrc[1] = 2000; // audio far ahead
  last_by_ssrc[2] = 50;   // video

  RtcpFeedback fb;
  EXPECT_TRUE(RtcpParser::ParseCompoundPacket(rtcp.data(), rtcp.size(), last_by_ssrc, fb));
  EXPECT_EQ(fb.sender_ssrc, 2u);
  EXPECT_EQ(fb.checkpoint_frame_id, 20u);
}

TEST(RtcpCacheTest, IgnoresTruncatedCheckpointAheadOfLastSent) {
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(0, 250), 0u);
  // 33 frames ahead: beyond the truncation window, keep the whole cache.
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(283, 250), 0u);
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(10, 250), 10u);
  // 20 frames ahead (within the 8-bit truncation window) must NOT erase the
  // newest in-flight frame (250): only up to last_sent - 1.
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(270, 250), 249u);
}

namespace {

// Middle 32 bits of the NTP timestamp offset_ms from now, i.e. the value a
// receiver echoes back in LSR. Uses the same +2208988800 s epoch offset as the
// RTCP Sender Report path.
constexpr uint64_t kNtpUnixEpochOffset = 2208988800ULL;

uint32_t NtpMid32OffsetMs(int64_t offset_ms) {
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const int64_t target_us = now_us + offset_ms * 1000;
  const uint32_t seconds = static_cast<uint32_t>(target_us / 1000000);
  const uint32_t fraction = static_cast<uint32_t>(
      ((target_us % 1000000) * 65536) / 1000000);
  return ((seconds + static_cast<uint32_t>(kNtpUnixEpochOffset)) << 16) |
         (fraction & 0xFFFFu);
}

void WriteBe32(std::vector<uint8_t>& buf, size_t off, uint32_t value) {
  buf[off] = static_cast<uint8_t>((value >> 24) & 0xFF);
  buf[off + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  buf[off + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  buf[off + 3] = static_cast<uint8_t>(value & 0xFF);
}

}  // namespace

TEST(RtcpParserTest, ReceiverReportComputesRoundTripTimeFromLsrAndDlsr) {
  // 32-byte RR: header + sender SSRC + one 24-byte report block.
  std::vector<uint8_t> rr(32, 0);
  rr[0] = 0x81;  // V=2, RC=1
  rr[1] = 201;   // PT=201 Receiver Report
  rr[2] = 0x00; rr[3] = 0x07;  // length = 7 words -> 32 bytes
  WriteBe32(rr, 4, 10002);     // SSRC of packet sender (the receiver)
  WriteBe32(rr, 8, 2);         // SSRC_1 (our video stream)
  // LSR: our last SR was 250 ms ago. DLSR: receiver held it 100 ms.
  WriteBe32(rr, 24, NtpMid32OffsetMs(-250));
  WriteBe32(rr, 28, 6553);  // 100 ms in 1/65536 s units

  RtcpFeedback fb;
  ASSERT_TRUE(RtcpParser::ParseCompoundPacket(rr.data(), rr.size(), 20, fb));
  EXPECT_EQ(fb.receiver_ssrc, 10002u);
  // rtt = 250 ms - 100 ms = 150 ms. A parser that ignored DLSR would report
  // ~250 ms, and the old code reported 0.
  EXPECT_NE(fb.rtt_ms, 0.0);
  EXPECT_GT(fb.rtt_ms, 100.0);
  EXPECT_LT(fb.rtt_ms, 220.0);
}

TEST(RtcpParserTest, ReceiverReportWithZeroLsrLeavesRttAtZero) {
  std::vector<uint8_t> rr(32, 0);
  rr[0] = 0x81;
  rr[1] = 201;
  rr[2] = 0x00; rr[3] = 0x07;
  WriteBe32(rr, 4, 10002);
  WriteBe32(rr, 8, 2);
  // LSR stays 0: the receiver has never seen an SR from us.
  WriteBe32(rr, 28, 6553);

  RtcpFeedback fb;
  ASSERT_TRUE(RtcpParser::ParseCompoundPacket(rr.data(), rr.size(), 20, fb));
  EXPECT_EQ(fb.rtt_ms, 0.0);
}

TEST(RtcpParserTest, KeepsAllLossFieldsUpToThe255FieldSpecLimit) {
  constexpr int kLossFields = 200;
  constexpr size_t kHeaderBytes = 20;
  const size_t message_len = kHeaderBytes + static_cast<size_t>(kLossFields) * 4;
  std::vector<uint8_t> rtcp(message_len, 0);

  rtcp[0] = 0x8F;  // V=2, FMT=15
  rtcp[1] = 206;   // PT=206 Payload-Specific Feedback
  const uint16_t words = static_cast<uint16_t>(message_len / 4 - 1);
  rtcp[2] = static_cast<uint8_t>(words >> 8);
  rtcp[3] = static_cast<uint8_t>(words & 0xFF);
  WriteBe32(rtcp, 4, 10002);  // receiver SSRC
  WriteBe32(rtcp, 8, 2);      // media SSRC
  rtcp[12] = 'C'; rtcp[13] = 'A'; rtcp[14] = 'S'; rtcp[15] = 'T';
  rtcp[16] = 10;                            // checkpoint frame id
  rtcp[17] = static_cast<uint8_t>(kLossFields);
  rtcp[18] = 0x00; rtcp[19] = 0xC8;         // 200 ms playout delay

  for (int i = 0; i < kLossFields; ++i) {
    const size_t off = kHeaderBytes + static_cast<size_t>(i) * 4;
    rtcp[off] = 11;                         // frame id
    rtcp[off + 1] = static_cast<uint8_t>((i >> 8) & 0xFF);
    rtcp[off + 2] = static_cast<uint8_t>(i & 0xFF);
    rtcp[off + 3] = 0;                      // no follow-up bit vector
  }

  std::map<uint32_t, uint32_t> last_by_ssrc;
  last_by_ssrc[2] = 20;
  RtcpFeedback fb;
  ASSERT_TRUE(RtcpParser::ParseCompoundPacket(rtcp.data(), rtcp.size(), last_by_ssrc, fb));
  EXPECT_EQ(fb.nacks.size(), static_cast<size_t>(kLossFields))
      << "NACK loss fields beyond 32 must not be dropped";
  EXPECT_TRUE(fb.has_playout_delay);
  EXPECT_EQ(fb.current_playout_delay_ms, 200);
}

TEST(RtcpParserTest, ApplicationDefinedSubtype1IsNotAPictureLossIndicator) {
  // PT 204 (Application-Defined) with FMT 1 is NOT a PLI; only PT 206 is.
  std::vector<uint8_t> pkt(12, 0);
  pkt[0] = 0x81;  // V=2, FMT=1
  pkt[1] = 204;   // Application-Defined
  pkt[2] = 0x00; pkt[3] = 0x02;
  WriteBe32(pkt, 4, 10002);
  WriteBe32(pkt, 8, 2);

  RtcpFeedback fb;
  RtcpParser::ParseCompoundPacket(pkt.data(), pkt.size(), 20, fb);
  EXPECT_FALSE(fb.picture_loss_indicator)
      << "PT 204/FMT 1 must not be parsed as a Picture Loss Indication";
}

TEST(RtcpParserTest, PayloadSpecificSubtype1IsStillAPictureLossIndicator) {
  std::vector<uint8_t> pkt(12, 0);
  pkt[0] = 0x81;  // V=2, FMT=1
  pkt[1] = 206;   // Payload-Specific Feedback
  pkt[2] = 0x00; pkt[3] = 0x02;
  WriteBe32(pkt, 4, 10002);
  WriteBe32(pkt, 8, 2);

  RtcpFeedback fb;
  EXPECT_TRUE(RtcpParser::ParseCompoundPacket(pkt.data(), pkt.size(), 20, fb));
  EXPECT_TRUE(fb.picture_loss_indicator);
}

TEST(RtcpParserTest, ExtendedReportBlockTypeIsReadFromTheRightOffset) {
  // XR layout: header[0..3], report author SSRC[4..7], first block BT at [8].
  std::vector<uint8_t> xr(16, 0);
  xr[0] = 0x80;
  xr[1] = 207;   // Extended Report
  xr[2] = 0x00; xr[3] = 0x03;  // length 3 -> 16 bytes
  WriteBe32(xr, 4, 10002);     // report author SSRC
  xr[8] = 4;                   // BT 4 = Receiver Reference Time Report
  xr[9] = 0;
  xr[10] = 0x00; xr[11] = 0x01;
  WriteBe32(xr, 12, NtpMid32OffsetMs(0));

  RtcpFeedback fb;
  EXPECT_TRUE(RtcpParser::ParseCompoundPacket(xr.data(), xr.size(), 20, fb))
      << "XR block type must be read from block[8]";

  // Same packet with the author SSRC equal to 4 and BT 0 at [8]: the old code
  // read block[4] and would have wrongly accepted it.
  std::vector<uint8_t> xr_bogus = xr;
  WriteBe32(xr_bogus, 4, 4);
  xr_bogus[8] = 0;  // BT 0 = not a Receiver Reference Time Report

  RtcpFeedback fb2;
  EXPECT_FALSE(RtcpParser::ParseCompoundPacket(xr_bogus.data(), xr_bogus.size(), 20, fb2))
      << "A report author SSRC of 4 must not be mistaken for the block type";
}
