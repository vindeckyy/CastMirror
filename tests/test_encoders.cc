#include <gtest/gtest.h>
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"
#include <chrono>

using namespace castcore;

TEST(EncoderTest, OpusAudioEncoderProducesValidFrames) {
  AudioEncoderConfig cfg;
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  cfg.bitrate_bps = 128000;

  auto encoder = AudioEncoderFactory::Create(AudioCodec::kOpus);
  ASSERT_TRUE(encoder->Initialize(cfg));

  // 10ms frame (480 samples stereo = 960 int16 samples)
  CapturedAudioFrame frame;
  frame.sample_rate = 48000;
  frame.channels = 2;
  frame.samples_per_channel = 480;
  frame.pcm_data.resize(480 * 2 * sizeof(int16_t), 0);

  EncodedFrame ef;
  EXPECT_TRUE(encoder->Encode(frame, ef));
  EXPECT_GT(ef.data.size(), 0u);
  EXPECT_EQ(ef.dependency, FrameDependency::kKeyFrame);
  EXPECT_EQ(ef.frame_id, 0u);
}

TEST(EncoderTest, AudioRtpTimestampsFollowCaptureClock) {
  AudioEncoderConfig cfg;
  cfg.sample_rate = 48000;
  cfg.channels = 2;
  cfg.bitrate_bps = 128000;

  auto encoder = AudioEncoderFactory::Create(AudioCodec::kOpus);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedAudioFrame frame;
  frame.sample_rate = 48000;
  frame.channels = 2;
  frame.samples_per_channel = 480;
  frame.pcm_data.resize(480 * 2 * sizeof(int16_t), 0);

  auto t0 = std::chrono::steady_clock::now();
  frame.timestamp = t0;
  EncodedFrame first;
  ASSERT_TRUE(encoder->Encode(frame, first));

  frame.timestamp = t0 + std::chrono::milliseconds(100);
  EncodedFrame second;
  ASSERT_TRUE(encoder->Encode(frame, second));

  // 100ms at the 48 kHz Cast audio clock, even if a capture gap skipped frames.
  EXPECT_EQ(second.rtp_timestamp - first.rtp_timestamp, 4800u);
}

TEST(EncoderTest, VideoEncoderProducesH264AnnexBNALUs) {
  // Force software x264 so Annex-B NAL start codes are guaranteed across
  // hardware/driver variations in CI.
  setenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE", "1", 1);

  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 480;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 640;
  frame.height = 480;
  frame.stride = 640 * 4;
  frame.data.resize(640 * 480 * 4, 0x80);

  EncodedFrame ef;
  EXPECT_TRUE(encoder->Encode(frame, ef));
  EXPECT_GT(ef.data.size(), 0u);
  EXPECT_EQ(ef.dependency, FrameDependency::kKeyFrame);
  EXPECT_EQ(ef.frame_id, 0u);

  // Check for Annex-B start code (0x00 0x00 0x00 0x01 or 0x00 0x00 0x01)
  ASSERT_GE(ef.data.size(), 4u);
  bool has_annex_b = (ef.data[0] == 0 && ef.data[1] == 0 && (ef.data[2] == 1 || (ef.data[2] == 0 && ef.data[3] == 1)));
  EXPECT_TRUE(has_annex_b);

  unsetenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE");
}

TEST(EncoderTest, VideoEncoderReconfigureKeepsFrameIds) {
  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 360;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.stride = 640 * 4;
  frame.data.resize(static_cast<size_t>(frame.stride) * frame.height, 0);

  EncodedFrame ef0;
  ASSERT_TRUE(encoder->Encode(frame, ef0));
  uint32_t fid0 = ef0.frame_id;

  VideoEncoderConfig cfg2 = cfg;
  cfg2.width = 320;
  cfg2.height = 180;
  cfg2.bitrate_kbps = 1000;
  ASSERT_TRUE(encoder->Reconfigure(cfg2));
  EXPECT_EQ(encoder->GetConfig().width, 320);

  frame.width = 320;
  frame.height = 180;
  frame.stride = 320 * 4;
  frame.data.resize(static_cast<size_t>(frame.stride) * frame.height, 0);

  EncodedFrame ef1;
  ASSERT_TRUE(encoder->Encode(frame, ef1));
  EXPECT_EQ(ef1.frame_id, fid0 + 1);
}

TEST(EncoderTest, VideoEncoderNameIsNonEmpty) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));
  std::string name = encoder->EncoderName();
  EXPECT_TRUE(name == "libx264" || name == "h264_vaapi");
}

TEST(EncoderTest, VideoRtpTimestampsFollowCaptureClock) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 60;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.stride = 320 * 4;
  frame.data.resize(320 * 240 * 4, 0x80);

  auto t0 = std::chrono::steady_clock::now();
  frame.timestamp = t0;
  EncodedFrame first;
  ASSERT_TRUE(encoder->Encode(frame, first));

  frame.timestamp = t0 + std::chrono::milliseconds(100);
  EncodedFrame second;
  ASSERT_TRUE(encoder->Encode(frame, second));

  // 100ms at the 90 kHz Cast video clock.
  EXPECT_EQ(second.rtp_timestamp - first.rtp_timestamp, 9000u);
  EXPECT_EQ(second.frame_id, first.frame_id + 1);
}
