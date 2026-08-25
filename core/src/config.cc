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
    if (j.contains("enable_tray_on_startup")) config_.enable_tray_on_startup = j["enable_tray_on_startup"].get<bool>();
    if (j.contains("low_latency_mode")) config_.low_latency_mode = j["low_latency_mode"].get<bool>();
    if (j.contains("capture_border_hint")) config_.capture_border_hint = j["capture_border_hint"].get<bool>();

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
    j["enable_tray_on_startup"] = config_.enable_tray_on_startup;
    j["low_latency_mode"] = config_.low_latency_mode;
    j["capture_border_hint"] = config_.capture_border_hint;

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
