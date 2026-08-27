#include "castcore/config.h"
#include "castcore/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace castcore {

namespace fs = std::filesystem;

ConfigStore& ConfigStore::Instance() {
  static ConfigStore instance;
  return instance;
}

ConfigStore::ConfigStore() {
  config_path_ = GetDefaultConfigPath();
}

const AppConfig& ConfigStore::Get() const {
  return config_;
}

AppConfig& ConfigStore::Mutable() {
  return config_;
}

uint32_t AppConfig::GetPresetBitrateOverrideKbps(QualityPreset preset) const {
  switch (preset) {
    case QualityPreset::kHigh: return bitrate_kbps_high;
    case QualityPreset::kBalanced: return bitrate_kbps_balanced;
    case QualityPreset::kSmooth: return bitrate_kbps_smooth;
    case QualityPreset::kAuto:
    default: return bitrate_kbps_auto;
  }
}

uint32_t AppConfig::GetPresetBitrateKbps(QualityPreset preset) const {
  uint32_t override_kbps = GetPresetBitrateOverrideKbps(preset);
  return override_kbps > 0 ? override_kbps : QualityPresetDefaultBitrateKbps(preset);
}

void AppConfig::SetPresetBitrateKbps(QualityPreset preset, uint32_t kbps) {
  switch (preset) {
    case QualityPreset::kHigh: bitrate_kbps_high = kbps; break;
    case QualityPreset::kBalanced: bitrate_kbps_balanced = kbps; break;
    case QualityPreset::kSmooth: bitrate_kbps_smooth = kbps; break;
    case QualityPreset::kAuto:
    default: bitrate_kbps_auto = kbps; break;
  }
}

std::string ConfigStore::GetDefaultConfigPath() {
#if defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  if (appdata) {
    return (fs::path(appdata) / "CastMirror" / "config.json").string();
  }
  return "config.json";
#else
  const char* home = std::getenv("HOME");
  if (home) {
    return (fs::path(home) / ".config" / "castmirror" / "config.json").string();
  }
  return "config.json";
#endif
}

bool ConfigStore::Load(const std::string& custom_path) {
  std::string path_to_load = custom_path.empty() ? config_path_ : custom_path;
  if (!fs::exists(path_to_load)) {
    LOG_INFO << "Config file does not exist at " << path_to_load << ", using defaults.";
    return false;
  }

  try {
    std::ifstream file(path_to_load);
    if (!file.is_open()) return false;

    nlohmann::json j;
    file >> j;

    if (j.contains("last_device_id")) config_.last_device_id = j["last_device_id"].get<std::string>();
    if (j.contains("last_device_name")) config_.last_device_name = j["last_device_name"].get<std::string>();
    if (j.contains("last_device_ip")) config_.last_device_ip = j["last_device_ip"].get<std::string>();
    if (j.contains("last_display_id")) config_.last_display_id = j["last_display_id"].get<int>();
    if (j.contains("audio_enabled")) config_.audio_enabled = j["audio_enabled"].get<bool>();
    if (j.contains("quality_preset")) config_.quality_preset = QualityPresetFromString(j["quality_preset"].get<std::string>());
    if (j.contains("target_delay_ms")) config_.target_delay_ms = j["target_delay_ms"].get<int>();
    if (j.contains("max_bitrate_kbps")) config_.max_bitrate_kbps = j["max_bitrate_kbps"].get<uint32_t>();
    if (j.contains("bitrate_kbps_auto")) config_.bitrate_kbps_auto = j["bitrate_kbps_auto"].get<uint32_t>();
    if (j.contains("bitrate_kbps_high")) config_.bitrate_kbps_high = j["bitrate_kbps_high"].get<uint32_t>();
    if (j.contains("bitrate_kbps_balanced")) config_.bitrate_kbps_balanced = j["bitrate_kbps_balanced"].get<uint32_t>();
    if (j.contains("bitrate_kbps_smooth")) config_.bitrate_kbps_smooth = j["bitrate_kbps_smooth"].get<uint32_t>();
    if (j.contains("enable_tray_on_startup")) config_.enable_tray_on_startup = j["enable_tray_on_startup"].get<bool>();
    if (j.contains("low_latency_mode")) config_.low_latency_mode = j["low_latency_mode"].get<bool>();
    if (j.contains("capture_border_hint")) config_.capture_border_hint = j["capture_border_hint"].get<bool>();
    if (j.contains("capture_fps")) config_.capture_fps = j["capture_fps"].get<int>();
    if (j.contains("audio_bitrate_bps")) config_.audio_bitrate_bps = j["audio_bitrate_bps"].get<uint32_t>();
    if (j.contains("silence_host_speakers")) config_.silence_host_speakers = j["silence_host_speakers"].get<bool>();
    if (j.contains("adaptive_enabled")) config_.adaptive_enabled = j["adaptive_enabled"].get<bool>();
    if (j.contains("subnet_scan_enabled")) config_.subnet_scan_enabled = j["subnet_scan_enabled"].get<bool>();
    if (j.contains("portal_restore_token")) config_.portal_restore_token = j["portal_restore_token"].get<std::string>();
    if (j.contains("first_run_complete")) config_.first_run_complete = j["first_run_complete"].get<bool>();
    if (j.contains("window_width")) config_.window_width = j["window_width"].get<int>();
    if (j.contains("window_height")) config_.window_height = j["window_height"].get<int>();
    if (j.contains("notify_on_events")) config_.notify_on_events = j["notify_on_events"].get<bool>();
    if (j.contains("force_software_encode")) config_.force_software_encode = j["force_software_encode"].get<bool>();
    if (j.contains("force_x11_capture")) config_.force_x11_capture = j["force_x11_capture"].get<bool>();
    if (j.contains("close_to_tray")) config_.close_to_tray = j["close_to_tray"].get<bool>();

    LOG_INFO << "Loaded configuration from " << path_to_load;
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to parse config file: " << e.what();
    return false;
  }
}

bool ConfigStore::Save(const std::string& custom_path) {
  std::string path_to_save = custom_path.empty() ? config_path_ : custom_path;

  try {
    fs::path dir = fs::path(path_to_save).parent_path();
    if (!dir.empty() && !fs::exists(dir)) {
      fs::create_directories(dir);
    }

    nlohmann::json j;
    j["last_device_id"] = config_.last_device_id;
    j["last_device_name"] = config_.last_device_name;
    j["last_device_ip"] = config_.last_device_ip;
    j["last_display_id"] = config_.last_display_id;
    j["audio_enabled"] = config_.audio_enabled;
    j["quality_preset"] = QualityPresetToString(config_.quality_preset);
    j["target_delay_ms"] = config_.target_delay_ms;
    j["max_bitrate_kbps"] = config_.max_bitrate_kbps;
    j["bitrate_kbps_auto"] = config_.bitrate_kbps_auto;
    j["bitrate_kbps_high"] = config_.bitrate_kbps_high;
    j["bitrate_kbps_balanced"] = config_.bitrate_kbps_balanced;
    j["bitrate_kbps_smooth"] = config_.bitrate_kbps_smooth;
    j["enable_tray_on_startup"] = config_.enable_tray_on_startup;
    j["low_latency_mode"] = config_.low_latency_mode;
    j["capture_border_hint"] = config_.capture_border_hint;
    j["capture_fps"] = config_.capture_fps;
    j["audio_bitrate_bps"] = config_.audio_bitrate_bps;
    j["silence_host_speakers"] = config_.silence_host_speakers;
    j["adaptive_enabled"] = config_.adaptive_enabled;
    j["subnet_scan_enabled"] = config_.subnet_scan_enabled;
    j["portal_restore_token"] = config_.portal_restore_token;
    j["first_run_complete"] = config_.first_run_complete;
    j["window_width"] = config_.window_width;
    j["window_height"] = config_.window_height;
    j["notify_on_events"] = config_.notify_on_events;
    j["force_software_encode"] = config_.force_software_encode;
    j["force_x11_capture"] = config_.force_x11_capture;
    j["close_to_tray"] = config_.close_to_tray;

    std::ofstream file(path_to_save);
    if (!file.is_open()) return false;

    file << j.dump(2) << std::endl;
    LOG_INFO << "Saved configuration to " << path_to_save;
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to save config file: " << e.what();
    return false;
  }
}

} // namespace castcore
