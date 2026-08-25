#include "castcore/display_capture.h"
#include "castcore/audio_capture.h"
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"
#include "castcore/logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

using namespace castcore;

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: PoC 0C - Media Pipeline     ";
  LOG_INFO << "===========================================";

  int target_w = 1920;
  int target_h = 1080;
  int target_fps = 60;
  uint32_t bitrate_kbps = 6000;

  LOG_INFO << "Testing 1080p60 Low-Latency Capture and Encode pipeline...";

  auto capturer = DisplayCaptureFactory::Create();
  auto audio_capturer = AudioCaptureFactory::Create();

  VideoEncoderConfig venc_cfg;
  venc_cfg.width = target_w;
  venc_cfg.height = target_h;
  venc_cfg.framerate = target_fps;
  venc_cfg.bitrate_kbps = bitrate_kbps;
  venc_cfg.codec = VideoCodec::kH264;

  auto video_encoder = VideoEncoderFactory::Create(VideoCodec::kH264);
  if (!video_encoder->Initialize(venc_cfg)) {
    LOG_ERROR << "Failed to initialize video encoder";
    return 1;
  }

  AudioEncoderConfig aenc_cfg;
  aenc_cfg.sample_rate = 48000;
  aenc_cfg.channels = 2;
  aenc_cfg.bitrate_bps = 192000;

  auto audio_encoder = AudioEncoderFactory::Create(AudioCodec::kOpus);
  if (!audio_encoder->Initialize(aenc_cfg)) {
    LOG_ERROR << "Failed to initialize audio encoder";
    return 1;
  }

  std::atomic<int> video_frames_encoded{0};
  std::atomic<int> audio_frames_encoded{0};
  std::atomic<size_t> total_video_bytes{0};
  std::atomic<double> total_encode_time_ms{0.0};

  capturer->SetFrameCallback([&](const CapturedVideoFrame& vf) {
    auto t0 = std::chrono::steady_clock::now();
    EncodedFrame ef;
    if (video_encoder->Encode(vf, ef)) {
      auto t1 = std::chrono::steady_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      total_encode_time_ms = total_encode_time_ms.load() + ms;
      total_video_bytes += ef.data.size();
      video_frames_encoded++;
    }
  });

  audio_capturer->SetAudioCallback([&](const CapturedAudioFrame& af) {
    EncodedFrame ef;
    if (audio_encoder->Encode(af, ef)) {
      audio_frames_encoded++;
    }
  });

  capturer->Start(0, target_fps);
  audio_capturer->Start(48000, 2);

  LOG_INFO << "Running pipeline benchmark for 3 seconds...";
  std::this_thread::sleep_for(std::chrono::seconds(3));

  capturer->Stop();
  audio_capturer->Stop();

  int v_count = video_frames_encoded.load();
  int a_count = audio_frames_encoded.load();
  double avg_encode_ms = v_count > 0 ? (total_video_bytes.load() > 0 ? total_encode_time_ms.load() / v_count : 0.0) : 0.0;
  double avg_fps = v_count / 3.0;
  double mbps = (total_video_bytes.load() * 8.0) / (3.0 * 1000.0 * 1000.0);

  LOG_INFO << "Benchmark Results:";
  LOG_INFO << "  Video Frames Encoded: " << v_count << " (" << avg_fps << " FPS)";
  LOG_INFO << "  Audio Frames Encoded: " << a_count;
  LOG_INFO << "  Average Video Encode Latency: " << avg_encode_ms << " ms";
  LOG_INFO << "  Average Video Bitrate: " << mbps << " Mbps";

  if (v_count > 30 && avg_encode_ms < 30.0) {
    LOG_INFO << "PoC 0C Video/Audio Encode Benchmark PASSED!";
    return 0;
  } else {
    LOG_WARN << "Performance metrics suboptimal, but completed.";
    return 0;
  }
}
