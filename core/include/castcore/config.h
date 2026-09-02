#ifndef CASTCORE_CONFIG_H_
#define CASTCORE_CONFIG_H_

#include "castcore/types.h"
#include <string>
#include <memory>
#include <map>
#include <optional>

namespace castcore {

struct DeviceProfile {
  int target_width = 0;
  int target_height = 0;
  int target_fps = 0;
  uint32_t bitrate_kbps = 0;
  int target_delay_ms = 200;
  QualityPreset preset = QualityPreset::kAuto;
  VideoCodec preferred_video_codec = VideoCodec::kH264;
};

struct AppConfig {
  std::string last_device_id;
  std::string last_device_name;
  std::string last_device_ip;
  int last_display_id = 0;
  // Last capture source: kind ("monitor"/"window"), id, and name. For
  // windows the id is not stable across restarts, so the name is used to
  // re-resolve on "cast to last" and falls back to monitor if missing.
  std::string last_source_kind = "monitor";
  int last_source_id = 0;
  std::string last_source_name;
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
  uint32_t bitrate_kbps_game = 0;
  uint32_t bitrate_kbps_cinema = 0;
  bool enable_tray_on_startup = false;
  bool low_latency_mode = false;
  bool capture_border_hint = true;
  int capture_fps = 0;  // 0 = auto from display
  uint32_t audio_bitrate_bps = 192000;
  bool silence_host_speakers = true;
  bool adaptive_enabled = true;
  bool subnet_scan_enabled = false;
  std::string portal_restore_token;
  bool first_run_complete = false;
  int window_width = 920;
  int window_height = 700;
  bool notify_on_events = true;
  bool force_software_encode = false;
  bool force_x11_capture = false;
  bool close_to_tray = true;
  int schema_version = 3;
  bool verbose_json_logging = false;
  int launch_timeout_s = 8;
  int answer_timeout_s = 5;
  bool adaptive_resolution_enabled = true;
  bool verify_device_cert = true;
  bool latency_hud_enabled = false;

  // Per-device profile persistence
  std::map<std::string, DeviceProfile> device_profiles;

  std::optional<DeviceProfile> GetDeviceProfile(const std::string& device_id) const;
  void SetDeviceProfile(const std::string& device_id, const DeviceProfile& profile);

  uint32_t GetPresetBitrateOverrideKbps(QualityPreset preset) const;
  uint32_t GetPresetBitrateKbps(QualityPreset preset) const;
  void SetPresetBitrateKbps(QualityPreset preset, uint32_t kbps);
  void Validate();
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
