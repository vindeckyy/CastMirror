#ifndef CASTCORE_CONFIG_H_
#define CASTCORE_CONFIG_H_

#include "castcore/types.h"
#include <nlohmann/json_fwd.hpp>
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
  // UI color scheme preference ("", "light", "dark"). Empty means "follow the
  // system". Only the Windows (WinUI) front-end reads it today; persisted here
  // so settings stay in a single config file.
  std::string ui_theme;

  // Per-device profile persistence
  std::map<std::string, DeviceProfile> device_profiles;

  std::optional<DeviceProfile> GetDeviceProfile(const std::string& device_id) const;
  void SetDeviceProfile(const std::string& device_id, const DeviceProfile& profile);

  uint32_t GetPresetBitrateOverrideKbps(QualityPreset preset) const;
  uint32_t GetPresetBitrateKbps(QualityPreset preset) const;
  void SetPresetBitrateKbps(QualityPreset preset, uint32_t kbps);
  void Validate();
};

// Values a caller wants to differ from the persisted configuration for one
// session. A disengaged optional means "whatever the user saved", which is what
// makes it safe for a caller that knows only two arguments (say the CLI's
// --bitrate) to start a session without resetting everything else.
struct SessionOverrides {
  std::optional<QualityPreset> preset;
  std::optional<bool> enable_audio;
  std::optional<VideoCodec> video_codec;
  std::optional<uint32_t> video_bitrate_kbps;  // unset or 0 = the preset's bitrate
  std::optional<int> target_delay_ms;          // unset or <= 0 = the configured delay
  std::optional<int> capture_fps;              // unset or <= 0 = follow the display
  std::optional<bool> verify_device_cert;
  std::optional<CaptureSource> source;
};

// The single place that turns configuration into a session. Every entry point
// (CLI, C API for the WinUI client, CastEngine's convenience overloads) builds
// its SessionOptions here.
//
// This is not a convenience: CastEngine::StartCasting persists the options back
// into ConfigStore, because a session's settings are also the user's "last
// used" settings. A caller that hand-assembles SessionOptions from struct
// defaults and passes them in does not merely run one session with wrong
// settings - it overwrites the user's saved ones. Routing every entry point
// through one builder is what keeps that impossible.
SessionOptions BuildSessionOptions(const AppConfig& cfg,
                                   const SessionOverrides& overrides = {});

// ---------------------------------------------------------------------------
// Strict JSON getters, shared by ConfigStore::Load (config file) and the C
// API's MergeConfigJson (client JSON). nlohmann's get<T>() silently coerces:
// get<uint32_t>(-1) wraps to 4294967295, get<int>(4294967296) truncates, and
// get<T> on the wrong JSON type throws - which in a per-file parse aborts the
// whole load and silently drops every key after the bad one. These helpers
// check the type first, log the rejected key, and leave *out untouched.
// ---------------------------------------------------------------------------
void ConfigReadInt(const nlohmann::json& j, const char* key, int* out);
void ConfigReadU32(const nlohmann::json& j, const char* key, uint32_t* out);
void ConfigReadBool(const nlohmann::json& j, const char* key, bool* out);
void ConfigReadString(const nlohmann::json& j, const char* key, std::string* out);

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
