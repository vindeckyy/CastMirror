#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include "castcore/mirroring_negotiator.h"
#include "castcore/frame_crypto.h"
#include "castcore/rtp_packetizer.h"
#include "castcore/cast_transport.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace castcore;

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "  CastMirror: PoC 0B - Cast Streaming Test ";
  LOG_INFO << "===========================================";

  std::string target_ip = (argc > 1) ? argv[1] : "127.0.0.1";
  uint16_t udp_port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 33533;

  LOG_INFO << "Setting up Cast RTP/RTCP transport to " << target_ip << ":" << udp_port;

  auto video_keys = MirroringNegotiator::GenerateRandomKeys();
  auto audio_keys = MirroringNegotiator::GenerateRandomKeys();

  FrameCrypto video_crypto(video_keys.aes_key, video_keys.aes_iv_mask);
  FrameCrypto audio_crypto(audio_keys.aes_key, audio_keys.aes_iv_mask);

  RtpPacketizer video_packetizer(96, 2, 1460);
  RtpPacketizer audio_packetizer(127, 1, 1460);

  CastTransport transport;
  transport.SetPliCallback([] {
    LOG_INFO << "[RTCP] Received Picture Loss Indicator";
  });
  transport.SetFeedbackCallback([](const RtcpFeedback& fb) {
    LOG_INFO << "[RTCP] Feedback: Checkpoint=" << fb.checkpoint_frame_id
             << ", Loss=" << (fb.fraction_lost * 100.0) << "%, NACKs=" << fb.nacks.size();
  });

  if (!transport.Start(target_ip, udp_port)) {
    LOG_ERROR << "Failed to start transport to " << target_ip << ":" << udp_port;
    return 1;
  }

  LOG_INFO << "Streaming 180 synthetic test frames (~3 seconds @ 60fps)...";

  // 64x48 synthetic test frame
  std::vector<uint8_t> dummy_video(64 * 48 * 3 / 2, 0x80);
  // 10ms Opus audio frame (silence)
  std::vector<uint8_t> dummy_audio(40, 0xF8);

  for (uint32_t i = 1; i <= 180; ++i) {
    bool is_key = (i % 30 == 1);
    uint32_t rtp_ts = (i * 90000) / 60;

    EncodedFrame vf;
    vf.dependency = is_key ? FrameDependency::kKeyFrame : FrameDependency::kDependent;
    vf.frame_id = i;
    vf.referenced_frame_id = is_key ? i : (i - 1);
    vf.rtp_timestamp = rtp_ts;
    vf.playout_delay = std::chrono::milliseconds(400);

    // Encrypt
    vf.data = video_crypto.Encrypt(i, dummy_video);

    // Packetize and Send
    auto packets = video_packetizer.PacketizeFrame(vf);
    transport.SendPackets(packets);

    // Audio frame
    EncodedFrame af;
    af.dependency = FrameDependency::kKeyFrame;
    af.frame_id = i;
    af.referenced_frame_id = i;
    af.rtp_timestamp = (i * 48000) / 60;
    af.playout_delay = std::chrono::milliseconds(400);
    af.data = audio_crypto.Encrypt(i, dummy_audio);

    auto audio_packets = audio_packetizer.PacketizeFrame(af);
    transport.SendPackets(audio_packets);

    std::this_thread::sleep_for(std::chrono::microseconds(16666));
  }

  auto stats = transport.GetStats();
  LOG_INFO << "Streaming completed! Total Packets Sent: " << stats.packets_sent
           << ", Frames Sent: " << stats.frames_sent;
  LOG_INFO << "PoC 0B Cast Streaming test passed successfully!";

  transport.Stop();
  return 0;
}
