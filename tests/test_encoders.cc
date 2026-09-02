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

TEST(EncoderTest, MultiSliceProducesMultipleSlices) {
  setenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE", "1", 1);

  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 480;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kH264;
  cfg.slices = 4;
  cfg.intra_refresh = false;

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

  // Count slice NALUs (nal_unit_type == 1 [non-IDR slice] or 5 [IDR slice])
  int slice_count = 0;
  const auto& data = ef.data;
  for (size_t i = 0; i + 3 < data.size(); ++i) {
    if (data[i] == 0 && data[i + 1] == 0) {
      size_t header_idx = 0;
      if (data[i + 2] == 1) {
        header_idx = i + 3;
      } else if (i + 4 < data.size() && data[i + 2] == 0 && data[i + 3] == 1) {
        header_idx = i + 4;
      }
      if (header_idx > 0 && header_idx < data.size()) {
        int nal_type = data[header_idx] & 0x1F;
        if (nal_type == 1 || nal_type == 5) {
          slice_count++;
        }
        i = header_idx;
      }
    }
  }

  // Multi-slice encoding must produce multiple slice NALUs per frame.
  EXPECT_GE(slice_count, cfg.slices);

  unsetenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE");
}

TEST(EncoderTest, IntraRefreshConfiguration) {
  setenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE", "1", 1);

  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kH264;
  cfg.intra_refresh = true;
  cfg.slices = 2;
  cfg.low_latency_tune = true;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));
  EXPECT_TRUE(encoder->GetConfig().intra_refresh);
  EXPECT_EQ(encoder->GetConfig().slices, 2);
  EXPECT_TRUE(encoder->GetConfig().low_latency_tune);

  CapturedVideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.stride = 320 * 4;
  frame.data.resize(320 * 240 * 4, 0x55);

  EncodedFrame ef;
  EXPECT_TRUE(encoder->Encode(frame, ef));
  EXPECT_GT(ef.data.size(), 0u);

  // Reconfigure with intra_refresh = false
  cfg.intra_refresh = false;
  ASSERT_TRUE(encoder->Reconfigure(cfg));
  EXPECT_FALSE(encoder->GetConfig().intra_refresh);
  EXPECT_TRUE(encoder->GetConfig().low_latency_tune);

  EXPECT_TRUE(encoder->Encode(frame, ef));
  EXPECT_GT(ef.data.size(), 0u);

  unsetenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE");
}

TEST(EncoderTest, VP8VideoEncoderProducesValidFrames) {
  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 360;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kVP8;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kVP8);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.stride = 640 * 4;
  frame.timestamp = std::chrono::steady_clock::now();
  frame.data.resize(640 * 360 * 4, 0x77);

  EncodedFrame ef0;
  EXPECT_TRUE(encoder->Encode(frame, ef0));
  EXPECT_GT(ef0.data.size(), 0u);
  EXPECT_EQ(ef0.dependency, FrameDependency::kKeyFrame);
  EXPECT_EQ(ef0.frame_id, 0u);

  frame.timestamp += std::chrono::milliseconds(33);
  EncodedFrame ef1;
  EXPECT_TRUE(encoder->Encode(frame, ef1));
  EXPECT_GT(ef1.data.size(), 0u);
  EXPECT_EQ(ef1.dependency, FrameDependency::kDependent);
  EXPECT_EQ(ef1.frame_id, 1u);
  EXPECT_EQ(ef1.referenced_frame_id, 0u);
}

TEST(EncoderTest, VP8VideoEncoderReconfigureKeepsFrameIds) {
  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 360;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kVP8;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kVP8);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 640;
  frame.height = 360;
  frame.stride = 640 * 4;
  frame.timestamp = std::chrono::steady_clock::now();
  frame.data.resize(640 * 360 * 4, 0x88);

  EncodedFrame ef;
  ASSERT_TRUE(encoder->Encode(frame, ef));
  EXPECT_EQ(ef.frame_id, 0u);

  frame.timestamp += std::chrono::milliseconds(33);
  ASSERT_TRUE(encoder->Encode(frame, ef));
  EXPECT_EQ(ef.frame_id, 1u);

  // Reconfigure resolution down
  cfg.width = 480;
  cfg.height = 270;
  ASSERT_TRUE(encoder->Reconfigure(cfg));

  frame.width = 480;
  frame.height = 270;
  frame.stride = 480 * 4;
  frame.data.resize(480 * 270 * 4, 0x99);
  frame.timestamp += std::chrono::milliseconds(33);

  ASSERT_TRUE(encoder->Encode(frame, ef));
  EXPECT_EQ(ef.frame_id, 2u);
}

TEST(EncoderTest, VP8RtpTimestampsFollowCaptureClock) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kVP8;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kVP8);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.stride = 320 * 4;
  frame.data.resize(320 * 240 * 4, 0x44);

  auto t0 = std::chrono::steady_clock::now();
  frame.timestamp = t0;
  EncodedFrame ef0;
  ASSERT_TRUE(encoder->Encode(frame, ef0));

  frame.timestamp = t0 + std::chrono::milliseconds(100);
  EncodedFrame ef1;
  ASSERT_TRUE(encoder->Encode(frame, ef1));

  // 100ms at the 90 kHz Cast video clock = 9000 ticks
  EXPECT_EQ(ef1.rtp_timestamp - ef0.rtp_timestamp, 9000u);
}

TEST(EncoderTest, AV1EncoderStubReturnsNullptr) {
  auto encoder = VideoEncoderFactory::Create(VideoCodec::kAV1);
  EXPECT_EQ(encoder, nullptr);
}

TEST(EncoderTest, VP9VideoEncoderProducesValidFrames) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kVP9;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kVP9);
  ASSERT_NE(encoder, nullptr);
  if (encoder->Initialize(cfg)) {
    CapturedVideoFrame frame;
    frame.width = 320;
    frame.height = 240;
    frame.stride = 320 * 4;
    frame.data.resize(320 * 240 * 4, 0x55);
    frame.timestamp = std::chrono::steady_clock::now();

    EncodedFrame ef;
    EXPECT_TRUE(encoder->Encode(frame, ef));
    EXPECT_GT(ef.data.size(), 0u);
    EXPECT_EQ(ef.dependency, FrameDependency::kKeyFrame);
    EXPECT_EQ(ef.frame_id, 0u);
  } else {
    // If libvpx-vp9 is not built into system ffmpeg, verify clean rejection
    SUCCEED() << "VP9 encoder not available in system ffmpeg";
  }
}

TEST(EncoderTest, HEVCVideoEncoderHardwareGatedFailsGracefullyWithoutHardware) {
  VideoEncoderConfig cfg;
  cfg.width = 1920;
  cfg.height = 1080;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 8000;
  cfg.codec = VideoCodec::kHEVC;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kHEVC);
  ASSERT_NE(encoder, nullptr);
  // HEVC is strictly HW-only (no software fallback to preserve GPL boundary).
  // Either VAAPI is available and it initializes, or it returns false cleanly.
  bool ok = encoder->Initialize(cfg);
  if (ok) {
    CapturedVideoFrame frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.stride = 1920 * 4;
    frame.data.resize(1920 * 1080 * 4, 0x11);
    frame.timestamp = std::chrono::steady_clock::now();

    EncodedFrame ef;
    EXPECT_TRUE(encoder->Encode(frame, ef));
  } else {
    SUCCEED() << "HEVC rejected gracefully when VAAPI hardware is absent";
  }
}

