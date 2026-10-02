#include <gtest/gtest.h>
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"
#include <chrono>
#include <cstdlib>

#if defined(_WIN32)
inline int setenv(const char* name, const char* value, int overwrite) {
  if (!overwrite && std::getenv(name)) return 0;
  return _putenv_s(name, value);
}
inline int unsetenv(const char* name) {
  return _putenv_s(name, "");
}
#endif

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
  bool has_annex_b = (ef.data[0] == 0 && ef.data[1] == 0 &&
                      (ef.data[2] == 1 || (ef.data[2] == 0 && ef.data[3] == 1)));
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
  EXPECT_TRUE(name == "libx264" || name == "h264_vaapi" || name == "mf_h264_hw" ||
              name == "mf_h264_sw");
}

// The Media Foundation backend reuses one input media buffer and one input
// sample for the life of a stream, and caches the output sample. Those caches
// are keyed on the frame size, so this walks a sequence of shapes - including
// growing, shrinking and an odd (buffer-snapshotted) size - and asserts every
// frame still encodes. Before the reuse the encoder allocated per frame; a
// stale buffer that was not re-sized on change produced garbage output or a
// dropped frame instead of a failure, so this guards the regression directly.
TEST(EncoderTest, VideoEncoderHandlesSizeChangesAcrossReuse) {
  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_NE(encoder, nullptr);

  const struct {
    int w;
    int h;
  } shapes[] = {
      {320, 240},
      {640, 360},
      {320, 240},
      {1920, 1080},
      {640, 360},
  };

  for (const auto& shape : shapes) {
    VideoEncoderConfig cfg;
    cfg.width = shape.w;
    cfg.height = shape.h;
    cfg.framerate = 30;
    cfg.bitrate_kbps = 1000;
    cfg.codec = VideoCodec::kH264;

    if (encoder->GetConfig().width == 0 || encoder->GetConfig().width != shape.w ||
        encoder->GetConfig().height != shape.h) {
      ASSERT_TRUE(encoder->Reconfigure(cfg)) << shape.w << "x" << shape.h;
    }

    CapturedVideoFrame frame;
    frame.width = shape.w;
    frame.height = shape.h;
    frame.stride = shape.w * 4;
    frame.data.assign(static_cast<size_t>(frame.stride) * shape.h, 0x40);
    frame.timestamp = std::chrono::steady_clock::now();

    // A freshly opened encoder can buffer its first frames before emitting,
    // so encode a few and require that at least one produces a real packet.
    bool produced = false;
    for (int i = 0; i < 8 && !produced; ++i) {
      frame.timestamp += std::chrono::milliseconds(33);
      EncodedFrame ef;
      if (encoder->Encode(frame, ef)) {
        EXPECT_GT(ef.data.size(), 0u) << shape.w << "x" << shape.h;
        produced = true;
      }
    }
    EXPECT_TRUE(produced) << "no packet produced at " << shape.w << "x" << shape.h;
  }
}

// Every frame must leave the encoder carrying its own ID and a timestamp that
// never goes backwards. The MF backend hands out one picture per DrainOutput
// call; a path that packed several output samples into a single EncodedFrame
// would repeat an ID and reuse a timestamp, which the receiver turns into a
// permanent freeze.
TEST(EncoderTest, VideoFrameIdsAndTimestampsAreMonotonic) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  auto t0 = std::chrono::steady_clock::now();
  bool have_last = false;
  uint32_t last_id = 0;
  uint32_t last_ts = 0;

  for (int i = 0; i < 12; ++i) {
    CapturedVideoFrame frame;
    frame.width = 320;
    frame.height = 240;
    frame.stride = 320 * 4;
    frame.data.assign(320 * 240 * 4, 0x40);
    frame.timestamp = t0 + std::chrono::milliseconds(33 * i);

    EncodedFrame ef;
    if (!encoder->Encode(frame, ef)) continue;
    ASSERT_FALSE(ef.data.empty());
    if (have_last) {
      EXPECT_GT(ef.frame_id, last_id);
      EXPECT_GT(ef.rtp_timestamp, last_ts);
    }
    last_id = ef.frame_id;
    last_ts = ef.rtp_timestamp;
    have_last = true;
  }
  EXPECT_TRUE(have_last);
}

// Bitrate is pushed live (no reconfigure), so the path must keep emitting
// well-formed frames across the change rather than wedging on a stream-type
// renegotiation.
TEST(EncoderTest, VideoEncoderSurvivesLiveBitrateChange) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  CapturedVideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.stride = 320 * 4;
  frame.data.assign(320 * 240 * 4, 0x40);
  auto t0 = std::chrono::steady_clock::now();

  encoder->ForceKeyFrame();
  encoder->SetBitrate(4000);
  encoder->SetBitrate(1500);

  int produced = 0;
  for (int i = 0; i < 10; ++i) {
    frame.timestamp = t0 + std::chrono::milliseconds(33 * i);
    EncodedFrame ef;
    if (encoder->Encode(frame, ef) && !ef.data.empty()) ++produced;
  }
  EXPECT_GT(produced, 0);
}

// Encodes a fixed, moving-noise sequence so every frame costs roughly the same
// bits, then returns the total payload size.
static size_t MeasureEncodedBytes(IVideoEncoder* encoder, int frames) {
  CapturedVideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.stride = 320 * 4;
  frame.data.resize(static_cast<size_t>(frame.stride) * frame.height);
  auto t0 = std::chrono::steady_clock::now();

  size_t total = 0;
  for (int i = 0; i < frames; ++i) {
    // Deterministic high-frequency content: flat colour costs almost nothing
    // and would make a rate change invisible in the byte count.
    for (size_t p = 0; p + 3 < frame.data.size(); p += 4) {
      const uint8_t v = static_cast<uint8_t>((p / 4 + i * 37) % 256);
      frame.data[p] = v;
      frame.data[p + 1] = static_cast<uint8_t>(255 - v);
      frame.data[p + 2] = static_cast<uint8_t>((v * 3) % 256);
      frame.data[p + 3] = 0xFF;
    }
    frame.timestamp = t0 + std::chrono::milliseconds(33 * i);
    EncodedFrame ef;
    if (encoder->Encode(frame, ef) && !ef.data.empty()) total += ef.data.size();
  }
  return total;
}

// SetBitrate is now the only adaptation path on a live session: a rate step
// that silently did nothing would leave the encoder frozen at its startup rate
// while the controller kept logging new targets. So the in-place setter must
// actually move the encoded rate, not merely record the new config value.
TEST(EncoderTest, SetBitrateChangesTheEncodedRateInPlace) {
  setenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE", "1", 1);

  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 4000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));

  // Warm up first: x264's rate-control state and the VBV queue need frames
  // before the output size reflects the requested rate.
  MeasureEncodedBytes(encoder.get(), 10);
  const size_t at_4000 = MeasureEncodedBytes(encoder.get(), 30);

  encoder->SetBitrate(400);
  encoder->ForceKeyFrame();
  MeasureEncodedBytes(encoder.get(), 10);
  const size_t at_400 = MeasureEncodedBytes(encoder.get(), 30);

  EXPECT_EQ(encoder->GetConfig().bitrate_kbps, 400u);
  EXPECT_LT(at_400, at_4000) << "SetBitrate must change the encoded rate in place: 30 frames were "
                             << at_4000 << " bytes at 4000 kbps but " << at_400
                             << " bytes at 400 kbps";
}

// Same check with geometry unchanged across a Reconfigure, which is the path
// used when the adaptive ladder changes resolution or framerate.
TEST(EncoderTest, ReconfiguredEncoderHonoursTheNewBitrate) {
  setenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE", "1", 1);

  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 4000;
  cfg.codec = VideoCodec::kH264;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  ASSERT_TRUE(encoder->Initialize(cfg));
  MeasureEncodedBytes(encoder.get(), 10);
  const size_t at_4000 = MeasureEncodedBytes(encoder.get(), 30);

  cfg.bitrate_kbps = 400;
  ASSERT_TRUE(encoder->Reconfigure(cfg));
  MeasureEncodedBytes(encoder.get(), 10);
  const size_t at_400 = MeasureEncodedBytes(encoder.get(), 30);

  EXPECT_LT(at_400, at_4000) << "Reconfigure must reopen at the new rate: " << at_4000
                             << " bytes at "
                             << "4000 kbps but " << at_400 << " bytes at 400 kbps";
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

TEST(EncoderTest, UnsupportedCodecsFallBackToH264) {
  // Cast Streaming mirroring only carries h264 and vp8; the factory must not
  // hand back a backend that emits a codec the receiver rejects.
  for (VideoCodec codec : {VideoCodec::kVP9, VideoCodec::kHEVC, VideoCodec::kAV1}) {
    VideoEncoderConfig cfg;
    cfg.width = 320;
    cfg.height = 240;
    cfg.framerate = 30;
    cfg.bitrate_kbps = 1000;
    cfg.codec = codec;

    auto encoder = VideoEncoderFactory::Create(codec);
    ASSERT_NE(encoder, nullptr);
    ASSERT_TRUE(encoder->Initialize(cfg));
    EXPECT_EQ(encoder->GetConfig().codec, VideoCodec::kH264);
  }
}

TEST(EncoderTest, VP9RequestFallsBackToH264AndStillEncodes) {
  VideoEncoderConfig cfg;
  cfg.width = 320;
  cfg.height = 240;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 1000;
  cfg.codec = VideoCodec::kVP9;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kVP9);
  ASSERT_NE(encoder, nullptr);
  ASSERT_TRUE(encoder->Initialize(cfg));
  EXPECT_EQ(encoder->GetConfig().codec, VideoCodec::kH264);

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
}

TEST(EncoderTest, HEVCRequestFallsBackToH264) {
  VideoEncoderConfig cfg;
  cfg.width = 640;
  cfg.height = 360;
  cfg.framerate = 30;
  cfg.bitrate_kbps = 2000;
  cfg.codec = VideoCodec::kHEVC;

  auto encoder = VideoEncoderFactory::Create(VideoCodec::kHEVC);
  ASSERT_NE(encoder, nullptr);
  ASSERT_TRUE(encoder->Initialize(cfg));
  EXPECT_EQ(encoder->GetConfig().codec, VideoCodec::kH264);
}
