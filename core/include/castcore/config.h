#ifndef CASTCORE_CONFIG_H_
#define CASTCORE_CONFIG_H_

#include "castcore/types.h"
#include <string>
#include <memory>

namespace castcore {

struct AppConfig {
  std::string last_device_id;
  std::string last_device_name;
  std::string last_device_ip;
  int last_display_id = 0;
  bool audio_enabled = true;
  QualityPreset quality_preset = QualityPreset::kAuto;
  int target_delay_ms = 200;
  VideoCodec preferred_video_codec = VideoCodec::kH264;
  uint32_t max_bitrate_kbps = 8000;
  // Per-preset video bitrate overrides in kbps. 0 = use the profile default.
  uint32_t bitrate_kbps_auto = 0;
  uint32_t bitrate_kbps_high = 0;
  uint32_t bitrate_kbps_balanced = 0;
  uint32_t bitrate_kbps_smooth = 0;
  bool enable_tray_on_startup = false;
  bool low_latency_mode = false;
  bool capture_border_hint = true;

  uint32_t GetPresetBitrateOverrideKbps(QualityPreset preset) const;
  uint32_t GetPresetBitrateKbps(QualityPreset preset) const;
  void SetPresetBitrateKbps(QualityPreset preset, uint32_t kbps);
};

class ConfigStore {
 public:
  static ConfigStore& Instance();

  const AppConfig& Get() const;
  AppConfig& Mutable();

  bool Load(const std::string& custom_path = "");
  bool Save(const std::string& custom_path = "");

  static std::string GetDefaultConfigPath();

 private:
  ConfigStore();
  AppConfig config_;
  std::string config_path_;
};

} // namespace castcore

#endif // CASTCORE_CONFIG_H_
