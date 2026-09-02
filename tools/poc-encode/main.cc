#include "castcore/display_capture.h"
#include "castcore/audio_capture.h"
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"
#include "castcore/logger.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace castcore;

static void PrintHelp(const char* prog) {
  std::cout << "Usage: " << prog << " [options] [preset] [duration]\n"
            << "PoC 0C - Media Pipeline benchmark (1080p60 default)\n"
            << "\nOptions:\n"
            << "  --width W        capture width (default 1920)\n"
            << "  --height H       capture height (default 1080)\n"
            << "  --fps FPS        target fps (default 60)\n"
            << "  --duration SEC   benchmark duration seconds (default 3, bench uses 60)\n"
            << "  --bitrate KBPS   video bitrate kbps (default 6000)\n"
            << "  --preset NAME    preset shorthand: 1080p60, 1080p30, 720p60, 720p30, 4k30\n"
            << "  --json PATH      write machine-readable JSON metrics to PATH\n"
            << "  --help, -h       show this help\n"
            << "\nPositional shorthands (kept for bench compatibility):\n"
            << "  1080p60          same as --preset 1080p60\n"
            << "  60, 60s          same as --duration 60\n"
            << "  1920x1080        same as --width 1920 --height 1080\n"
            << "\nExamples:\n"
            << "  " << prog << "                          # 3s 1080p60 (legacy)\n"
            << "  " << prog << " --duration 60            # 60s 1080p60 for bench\n"
            << "  " << prog << " 1080p60 60s               # bench shorthand\n"
            << "  " << prog << " --preset 1080p60 --duration 60\n";
}

static bool IsNumberWithSuffix(const std::string& s) {
  if (s.empty()) return false;
  std::string t = s;
  if (t.back() == 's' || t.back() == 'S') t.pop_back();
  if (t.empty()) return false;
  for (char c : t) if (!isdigit(c) && c!='-' ) return false;
  return true;
}

static int ParseDuration(const std::string& s) {
  std::string t = s;
  if (!t.empty() && (t.back()=='s' || t.back()=='S')) t.pop_back();
  try { return std::stoi(t); } catch(...) { return -1; }
}

static void ApplyPreset(const std::string& preset, int& w, int& h, int& fps) {
  std::string p = preset;
  for (auto& c : p) c = tolower(c);
  if (p=="1080p60" || p=="1080p") { w=1920; h=1080; fps=60; }
  else if (p=="1080p30") { w=1920; h=1080; fps=30; }
  else if (p=="720p60" || p=="720p") { w=1280; h=720; fps=60; }
  else if (p=="720p30") { w=1280; h=720; fps=30; }
  else if (p=="4k60" || p=="2160p60" || p=="4k") { w=3840; h=2160; fps=60; }
  else if (p=="4k30" || p=="2160p30") { w=3840; h=2160; fps=30; }
  else if (p=="540p30") { w=960; h=540; fps=30; }
}

int main(int argc, char** argv) {
  LOG_INFO << "===========================================";
  LOG_INFO << "   CastMirror: PoC 0C - Media Pipeline     ";
  LOG_INFO << "===========================================";

  int target_w = 1920;
  int target_h = 1080;
  int target_fps = 60;
  uint32_t bitrate_kbps = 6000;
  int duration_sec = 3;
  std::string json_path;

  // Parse arguments (keep legacy: no args = 3s 1080p60)
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg=="--help" || arg=="-h") {
      PrintHelp(argv[0]);
      return 0;
    } else if (arg=="--width" && i+1 < argc) {
      target_w = std::stoi(argv[++i]);
    } else if (arg=="--height" && i+1 < argc) {
      target_h = std::stoi(argv[++i]);
    } else if (arg=="--fps" && i+1 < argc) {
      target_fps = std::stoi(argv[++i]);
    } else if ((arg=="--duration" || arg=="-d") && i+1 < argc) {
      duration_sec = ParseDuration(argv[++i]);
    } else if (arg=="--bitrate" && i+1 < argc) {
      bitrate_kbps = static_cast<uint32_t>(std::stoi(argv[++i]));
    } else if (arg=="--preset" && i+1 < argc) {
      ApplyPreset(argv[++i], target_w, target_h, target_fps);
    } else if (arg=="--json" && i+1 < argc) {
      json_path = argv[++i];
    } else if (arg.rfind("--",0)==0) {
      LOG_WARN << "Unknown option: " << arg;
    } else {
      // Positional shorthands
      std::string low = arg;
      for (auto& c: low) c = tolower(c);
      if (low=="1080p60" || low=="1080p30" || low=="720p60" || low=="720p30" || low=="4k60" || low=="4k30" || low=="4k" || low=="720p" || low=="1080p" || low=="2160p60" || low=="2160p30" || low=="540p30") {
        ApplyPreset(low, target_w, target_h, target_fps);
      } else if (low.find('x') != std::string::npos) {
        // e.g. 1920x1080
        size_t xpos = low.find('x');
        try {
          target_w = std::stoi(low.substr(0, xpos));
          target_h = std::stoi(low.substr(xpos+1));
        } catch(...) {}
      } else if (IsNumberWithSuffix(arg)) {
        int d = ParseDuration(arg);
        if (d > 0) duration_sec = d;
      } else {
        LOG_WARN << "Ignoring unknown positional arg: " << arg;
      }
    }
  }

  if (duration_sec <= 0) duration_sec = 3;
  if (target_w <= 0) target_w = 1920;
  if (target_h <= 0) target_h = 1080;
  if (target_fps <= 0) target_fps = 60;

  LOG_INFO << "Testing " << target_w << "x" << target_h << "@" << target_fps
           << " Low-Latency Capture and Encode pipeline for " << duration_sec << "s...";
  LOG_INFO << "Config: " << target_w << "x" << target_h << " @" << target_fps
           << "fps, " << bitrate_kbps << " kbps, duration " << duration_sec << "s";

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
  std::vector<double> encode_samples;
  std::mutex samples_mutex;

  capturer->SetFrameCallback([&](const CapturedVideoFrame& vf) {
    auto t0 = std::chrono::steady_clock::now();
    EncodedFrame ef;
    if (video_encoder->Encode(vf, ef)) {
      auto t1 = std::chrono::steady_clock::now();
      double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      {
        std::lock_guard<std::mutex> lock(samples_mutex);
        encode_samples.push_back(ms);
      }
      // atomic double add via load/store loop (avoid fetch_add for double)
      double prev = total_encode_time_ms.load();
      double next;
      do {
        next = prev + ms;
      } while (!total_encode_time_ms.compare_exchange_weak(prev, next));
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

  LOG_INFO << "Running pipeline benchmark for " << duration_sec << " seconds...";
  std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

  capturer->Stop();
  audio_capturer->Stop();

  int v_count = video_frames_encoded.load();
  int a_count = audio_frames_encoded.load();
  double avg_encode_ms = 0.0;
  double p50_ms = 0.0, p95_ms = 0.0;
  {
    std::lock_guard<std::mutex> lock(samples_mutex);
    if (!encode_samples.empty()) {
      std::vector<double> sorted = encode_samples;
      std::sort(sorted.begin(), sorted.end());
      auto percentile = [&](double p) -> double {
        if (sorted.empty()) return 0.0;
        size_t idx = static_cast<size_t>(std::ceil(p * sorted.size())) - 1;
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
      };
      p50_ms = percentile(0.50);
      p95_ms = percentile(0.95);
      if (v_count > 0) avg_encode_ms = total_encode_time_ms.load() / v_count;
      else avg_encode_ms = percentile(0.5);
    } else if (v_count > 0) {
      avg_encode_ms = total_encode_time_ms.load() / v_count;
      p50_ms = avg_encode_ms;
      p95_ms = avg_encode_ms;
    }
  }
  // Fallback if no samples but still frames counted (should not happen)
  if (p50_ms==0 && avg_encode_ms>0) { p50_ms = avg_encode_ms; p95_ms = avg_encode_ms; }

  double avg_fps = duration_sec > 0 ? (v_count / static_cast<double>(duration_sec)) : 0.0;
  double mbps = duration_sec > 0 ? (total_video_bytes.load() * 8.0) / (duration_sec * 1000.0 * 1000.0) : 0.0;

  LOG_INFO << "Benchmark Results:";
  LOG_INFO << "  Video Frames Encoded: " << v_count << " (" << avg_fps << " FPS)";
  LOG_INFO << "  Audio Frames Encoded: " << a_count;
  LOG_INFO << "  Average Video Encode Latency: " << avg_encode_ms << " ms";
  LOG_INFO << "  p50 Video Encode Latency: " << p50_ms << " ms";
  LOG_INFO << "  p95 Video Encode Latency: " << p95_ms << " ms";
  LOG_INFO << "  Average Video Bitrate: " << mbps << " Mbps";
  // Machine-readable for bench harness (stripped of color by script)
  LOG_INFO << "BENCH_METRIC encode_ms_p50=" << p50_ms << " encode_ms_p95=" << p95_ms
           << " encode_ms_avg=" << avg_encode_ms << " fps=" << avg_fps
           << " frames=" << v_count << " bitrate_mbps=" << mbps;
  LOG_INFO << "BENCH_ENCODE_CSV " << p50_ms << "," << p95_ms << "," << avg_fps;

  // Also emit to stdout without logger prefix for simpler parsing (bench script greps both)
  std::cout << "BENCH_ENCODE p50=" << p50_ms << " p95=" << p95_ms
            << " avg=" << avg_encode_ms << " fps=" << avg_fps
            << " frames=" << v_count << " duration=" << duration_sec << std::endl;

  if (!json_path.empty()) {
    try {
      FILE* f = fopen(json_path.c_str(), "w");
      if (f) {
        fprintf(f, "{\"width\":%d,\"height\":%d,\"fps\":%d,\"duration_sec\":%d,"
                   "\"frames\":%d,\"fps_measured\":%.3f,"
                   "\"encode_ms_p50\":%.3f,\"encode_ms_p95\":%.3f,\"encode_ms_avg\":%.3f,"
                   "\"bitrate_mbps\":%.3f}\n",
                target_w, target_h, target_fps, duration_sec,
                v_count, avg_fps, p50_ms, p95_ms, avg_encode_ms, mbps);
        fclose(f);
        LOG_INFO << "Wrote JSON metrics to " << json_path;
      }
    } catch(...) {}
  }

  if (v_count > 30 && avg_encode_ms < 30.0) {
    LOG_INFO << "PoC 0C Video/Audio Encode Benchmark PASSED!";
    return 0;
  } else {
    LOG_WARN << "Performance metrics suboptimal, but completed.";
    return 0;
  }
}
