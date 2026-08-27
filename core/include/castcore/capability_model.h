#ifndef CASTCORE_CAPABILITY_MODEL_H_
#define CASTCORE_CAPABILITY_MODEL_H_

#include "castcore/types.h"
#include <string>
#include <vector>

namespace castcore {

struct DeviceCapabilities {
  std::string device_family;
  Resolution max_resolution{1920, 1080};
  int max_fps = 60;
  uint32_t max_bitrate_kbps = 10000;
  std::string h264_profile = "high";
  std::string h264_level = "4.2";
  bool supports_vp8 = true;
  bool supports_vp9 = false;
  bool supports_hevc = false;
  int default_target_delay_ms = 200;
  std::vector<Resolution> supported_resolutions;
};

class CapabilityModel {
 public:
  static DeviceCapabilities Evaluate(const CastDevice& device);

  static StreamStats GetRecommendedSettings(const CastDevice& device,
                                            QualityPreset preset,
                                            int display_width,
                                            int display_height,
                                            int display_refresh_rate,
                                            int capture_fps = 0);
};

} // namespace castcore

#endif // CASTCORE_CAPABILITY_MODEL_H_
