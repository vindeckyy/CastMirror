#include "castcore/capability_model.h"
#include <algorithm>
#include <cctype>

namespace castcore {

namespace {

std::string ToLower(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

} // namespace

DeviceCapabilities CapabilityModel::Evaluate(const CastDevice& device) {
  DeviceCapabilities caps;
  std::string model = ToLower(device.model_name);

  // Common resolutions
  caps.supported_resolutions = {
    {3840, 2160},
    {2560, 1440},
    {1920, 1080},
    {1280, 720},
    {960, 540},
    {640, 360}
  };

  if (model.find("nest hub") != std::string::npos || model.find("google home hub") != std::string::npos) {
    caps.device_family = "Nest Hub";
    caps.max_resolution = {1280, 720};
    caps.max_fps = 60;
    caps.max_bitrate_kbps = 5000;
    caps.h264_level = "3.1";
  } else if (model.find("ultra") != std::string::npos) {
    caps.device_family = "Chromecast Ultra";
    caps.max_resolution = {3840, 2160};
    caps.max_fps = 60;
    caps.max_bitrate_kbps = 25000;
    caps.h264_level = "5.1";
    caps.supports_vp9 = true;
  } else if (model.find("h2g2-42") != std::string::npos ||
             (model.find("nc2-6a5") != std::string::npos && model.find("nc2-6a5-d") == std::string::npos)) {
    caps.device_family = "Chromecast Gen 1/2";
    caps.max_resolution = {1920, 1080};
    caps.max_fps = 30;
    caps.max_bitrate_kbps = 8000;
    caps.h264_level = "4.1";
  } else if (model.find("google tv") != std::string::npos || model.find("streamer") != std::string::npos || model.find("android tv") != std::string::npos || model.find("bravia") != std::string::npos) {
    caps.device_family = "Chromecast with Google TV";
    caps.max_resolution = {3840, 2160};
    caps.max_fps = 60;
    caps.max_bitrate_kbps = 25000;
    caps.h264_level = "5.1";
    caps.supports_vp9 = true;
    caps.supports_hevc = true;
  } else if (model.find("chromecast") != std::string::npos) {
    // Check if Gen 3 or Gen 1/2
    caps.device_family = "Chromecast Gen 3";
    caps.max_resolution = {1920, 1080};
    caps.max_fps = 60;
    caps.max_bitrate_kbps = 15000;
    caps.h264_level = "4.2";
  } else {
    // Generic / Cast TV
    caps.device_family = "Generic Cast TV";
    caps.max_resolution = {1920, 1080};
    caps.max_fps = 60;
    caps.max_bitrate_kbps = 12000;
    caps.h264_level = "4.2";
  }

  return caps;
}

StreamStats CapabilityModel::GetRecommendedSettings(const CastDevice& device,
                                                    QualityPreset preset,
                                                    int display_width,
                                                    int display_height,
                                                    int display_refresh_rate,
                                                    int capture_fps) {
  DeviceCapabilities caps = Evaluate(device);
  StreamStats stats;
  stats.active_codec = "h264";
  stats.target_delay_ms = caps.default_target_delay_ms;

  // Bound display resolution to device capability
  int target_w = std::min(display_width, caps.max_resolution.width);
  int target_h = std::min(display_height, caps.max_resolution.height);
  int target_fps = std::min(display_refresh_rate > 0 ? display_refresh_rate : 60, caps.max_fps);
  if (capture_fps > 0) {
    target_fps = capture_fps;
  }

  switch (preset) {
    case QualityPreset::kHigh:
      stats.current_resolution = {target_w, target_h};
      stats.current_framerate = target_fps;
      stats.bitrate_kbps = std::min(caps.max_bitrate_kbps, target_w >= 3840 ? 20000u : (target_fps >= 60 ? 12000u : 8000u));
      stats.target_delay_ms = 200;
      break;

    case QualityPreset::kBalanced:
      stats.current_resolution = {std::min(target_w, 1920), std::min(target_h, 1080)};
      stats.current_framerate = capture_fps > 0 ? target_fps : std::min(target_fps, 60);
      stats.bitrate_kbps = std::min(caps.max_bitrate_kbps, 8000u);
      stats.target_delay_ms = 200;
      break;

    case QualityPreset::kSmooth:
      stats.current_resolution = {std::min(target_w, 1280), std::min(target_h, 720)};
      stats.current_framerate = capture_fps > 0 ? target_fps : std::min(target_fps, 60);
      stats.bitrate_kbps = std::min(caps.max_bitrate_kbps, 5000u);
      stats.target_delay_ms = 200; // Low latency mode
      break;

    case QualityPreset::kAuto:
    default:
      // Auto defaults to Balanced initially, then adapts dynamically
      stats.current_resolution = {std::min(target_w, 1920), std::min(target_h, 1080)};
      stats.current_framerate = capture_fps > 0 ? target_fps : std::min(target_fps, 60);
      stats.bitrate_kbps = std::min(caps.max_bitrate_kbps, 8000u);
      stats.target_delay_ms = 200;
      break;
  }

  return stats;
}

} // namespace castcore
