#include <gtest/gtest.h>
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"

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

TEST(EncoderTest, VideoEncoderProducesH264AnnexBNALUs) {
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
}
