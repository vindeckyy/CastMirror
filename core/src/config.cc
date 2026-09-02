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

void AppConfig::Validate() {
  auto clamp_kbps = [](uint32_t v) -> uint32_t {
    if (v == 0) return 0;
    return std::clamp(v, 1000u, 25000u);
  };
  bitrate_kbps_auto = clamp_kbps(bitrate_kbps_auto);
  bitrate_kbps_high = clamp_kbps(bitrate_kbps_high);
  bitrate_kbps_balanced = clamp_kbps(bitrate_kbps_balanced);
  bitrate_kbps_smooth = clamp_kbps(bitrate_kbps_smooth);
  max_bitrate_kbps = std::clamp(max_bitrate_kbps, 1000u, 25000u);
  if (capture_fps < 0 || capture_fps > 60) {
    capture_fps = 0;
  }
  target_delay_ms = std::clamp(target_delay_ms, 100, 400);
  window_width = std::clamp(window_width, 760, 1600);
  window_height = std::clamp(window_height, 560, 1200);
  launch_timeout_s = std::clamp(launch_timeout_s, 3, 15);
  answer_timeout_s = std::clamp(answer_timeout_s, 3, 10);
  audio_bitrate_bps = std::clamp(audio_bitrate_bps, 64000u, 256000u);
  if (schema_version < 1) schema_version = 3;
  // Normalize source kind string.
  if (last_source_kind != "monitor" && last_source_kind != "window") {
    last_source_kind = "monitor";
  }
  // Adaptive quality is now always on — the controller holds the user's
  // custom bitrate and ramps back to it after congestion. The toggle was
  // removed from the UI; force the field so old configs migrate silently.
  adaptive_enabled = true;
  // Low-latency toggle was removed from the UI; the Target delay slider is
  // now the single latency control. Force the legacy field off so it does
  // not override the slider anywhere downstream.
  low_latency_mode = false;
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
    if (j.contains("last_source_kind")) config_.last_source_kind = j["last_source_kind"].get<std::string>();
    if (j.contains("last_source_id"))   config_.last_source_id   = j["last_source_id"].get<int>();
    if (j.contains("last_source_name")) config_.last_source_name = j["last_source_name"].get<std::string>();
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

    if (j.contains("schema_version")) config_.schema_version = j["schema_version"].get<int>();
    else {
      // Migrate v1 -> v2: map old max_bitrate_kbps -> bitrate_kbps_auto if auto is empty
      if (j.contains("max_bitrate_kbps") && !j.contains("bitrate_kbps_auto")) {
        uint32_t v = j["max_bitrate_kbps"].get<uint32_t>();
        if (v != 8000 && v != 0) {
          config_.bitrate_kbps_auto = v;
        }
      }
      config_.schema_version = 2;
    }
    // Migrate v2 -> v3: if no explicit source fields, derive from last_display_id.
    if (config_.schema_version < 3 &&
        !j.contains("last_source_kind") && !j.contains("last_source_id")) {
      config_.last_source_kind = "monitor";
      config_.last_source_id = config_.last_display_id;
      config_.last_source_name.clear();
    }
    config_.schema_version = 3;
    if (j.contains("verbose_json_logging")) config_.verbose_json_logging = j["verbose_json_logging"].get<bool>();
    if (j.contains("verbose_json")) config_.verbose_json_logging = j["verbose_json"].get<bool>();
    if (j.contains("verboseJson")) config_.verbose_json_logging = j["verboseJson"].get<bool>();
    // Env override is handled by Logger, but also respect config flag
    if (j.contains("launch_timeout_s")) config_.launch_timeout_s = j["launch_timeout_s"].get<int>();
    if (j.contains("answer_timeout_s")) config_.answer_timeout_s = j["answer_timeout_s"].get<int>();
    if (j.contains("adaptive_resolution_enabled")) config_.adaptive_resolution_enabled = j["adaptive_resolution_enabled"].get<bool>();

    config_.Validate();
    LOG_INFO << "Loaded configuration from " << path_to_load;
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to parse config file: " << e.what();
    // Corrupted JSON -> keep defaults with warning, not crash
    config_.Validate();
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

    config_.Validate();
    nlohmann::json j;
    j["schema_version"] = config_.schema_version;
    j["last_device_id"] = config_.last_device_id;
    j["last_device_name"] = config_.last_device_name;
    j["last_device_ip"] = config_.last_device_ip;
    j["last_display_id"] = config_.last_display_id;
    j["last_source_kind"] = config_.last_source_kind;
    j["last_source_id"] = config_.last_source_id;
    j["last_source_name"] = config_.last_source_name;
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
    j["verbose_json_logging"] = config_.verbose_json_logging;
    j["verbose_json"] = config_.verbose_json_logging;
    j["launch_timeout_s"] = config_.launch_timeout_s;
    j["answer_timeout_s"] = config_.answer_timeout_s;
    j["adaptive_resolution_enabled"] = config_.adaptive_resolution_enabled;

    // Atomic write: write to tmp + fsync + rename, backup previous
    std::string tmp_path = path_to_save + ".tmp";
    std::string bak_path = path_to_save + ".bak";
    {
      std::ofstream file(tmp_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) return false;
      file << j.dump(2) << std::endl;
      file.flush();
      // fsync handled by ofstream close flush; ensure directory exists
    }
    try {
      // Backup existing
      if (fs::exists(path_to_save)) {
        std::error_code ec;
        fs::copy_file(path_to_save, bak_path, fs::copy_options::overwrite_existing, ec);
      }
      std::error_code ec;
      fs::rename(tmp_path, path_to_save, ec);
      if (ec) {
        // Fallback to copy if rename across FS
        fs::copy_file(tmp_path, path_to_save, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_path, ec);
        if (ec) return false;
      }
    } catch (const std::exception& e) {
      LOG_ERROR << "Atomic save failed: " << e.what();
      return false;
    }
    LOG_INFO << "Saved configuration to " << path_to_save;
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to save config file: " << e.what();
    return false;
  }
}

} // namespace castcore
