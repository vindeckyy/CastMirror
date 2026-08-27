#include <gtest/gtest.h>
#include "castcore/rtp_packetizer.h"
#include "castcore/rtcp_parser.h"
#include "castcore/cast_transport.h"
#include <map>

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
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(283, 250), 0u);
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(10, 250), 10u);
  EXPECT_EQ(CastTransport::SafeCacheEraseLimit(270, 250), 250u);
}
