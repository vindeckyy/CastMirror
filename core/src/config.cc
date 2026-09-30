#include "castcore/config.h"
#include <mutex>
#include "castcore/logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <limits>

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
    case QualityPreset::kCinema: return bitrate_kbps_cinema;
    case QualityPreset::kHigh: return bitrate_kbps_high;
    case QualityPreset::kGame: return bitrate_kbps_game;
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
    case QualityPreset::kCinema: bitrate_kbps_cinema = kbps; break;
    case QualityPreset::kHigh: bitrate_kbps_high = kbps; break;
    case QualityPreset::kGame: bitrate_kbps_game = kbps; break;
    case QualityPreset::kBalanced: bitrate_kbps_balanced = kbps; break;
    case QualityPreset::kSmooth: bitrate_kbps_smooth = kbps; break;
    case QualityPreset::kAuto:
    default: bitrate_kbps_auto = kbps; break;
  }
}

void ConfigReadInt(const nlohmann::json& j, const char* key, int* out) {
  const auto& value = j[key];
  if (!value.is_number_integer()) {
    LOG_WARN << "Ignoring config key '" << key << "': expected an integer";
    return;
  }
  const int64_t raw = value.get<int64_t>();
  if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) {
    LOG_WARN << "Ignoring config key '" << key << "': " << raw << " does not fit in an int";
    return;
  }
  *out = static_cast<int>(raw);
}

void ConfigReadU32(const nlohmann::json& j, const char* key, uint32_t* out) {
  const auto& value = j[key];
  if (!value.is_number_integer()) {
    LOG_WARN << "Ignoring config key '" << key << "': expected an integer";
    return;
  }
  const int64_t raw = value.get<int64_t>();
  if (raw < 0) {
    LOG_WARN << "Ignoring config key '" << key << "': value must not be negative";
    return;
  }
  if (raw > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
    LOG_WARN << "Ignoring config key '" << key << "': " << raw << " does not fit in 32 bits";
    return;
  }
  *out = static_cast<uint32_t>(raw);
}

void ConfigReadBool(const nlohmann::json& j, const char* key, bool* out) {
  if (!j[key].is_boolean()) {
    LOG_WARN << "Ignoring config key '" << key << "': expected true or false";
    return;
  }
  *out = j[key].get<bool>();
}

void ConfigReadString(const nlohmann::json& j, const char* key, std::string* out) {
  if (!j[key].is_string()) {
    LOG_WARN << "Ignoring config key '" << key << "': expected a string";
    return;
  }
  *out = j[key].get<std::string>();
}

SessionOptions BuildSessionOptions(const AppConfig& cfg, const SessionOverrides& overrides) {
  SessionOptions opts;
  opts.preset = overrides.preset.value_or(cfg.quality_preset);
  opts.enable_audio = overrides.enable_audio.value_or(cfg.audio_enabled);
  opts.video_codec = overrides.video_codec.value_or(cfg.preferred_video_codec);
  opts.audio_bitrate_bps = cfg.audio_bitrate_bps;
  opts.target_delay_ms = cfg.target_delay_ms;
  if (overrides.target_delay_ms && *overrides.target_delay_ms > 0) {
    opts.target_delay_ms = *overrides.target_delay_ms;
  } else if (opts.preset == QualityPreset::kGame) {
    opts.target_delay_ms = 150;  // the preset's whole point is a short buffer
  } else if (opts.preset == QualityPreset::kCinema) {
    opts.target_delay_ms = 400;
  }
  opts.capture_fps = cfg.capture_fps;
  if (overrides.capture_fps && *overrides.capture_fps > 0) {
    opts.capture_fps = *overrides.capture_fps;
  }
  opts.silence_host_speakers = cfg.silence_host_speakers;
  opts.show_cursor = cfg.show_cursor;
  opts.audio_process_name = cfg.audio_process_name;
  opts.reconnect_window_s = cfg.reconnect_window_s;
  opts.adaptive_enabled = cfg.adaptive_enabled;
  // Game keeps the resolution fixed: a mid-game resolution change stalls the
  // picture for the keyframe that follows it.
  opts.adaptive_resolution_enabled =
      opts.preset == QualityPreset::kGame ? false : cfg.adaptive_resolution_enabled;
  opts.verify_device_cert = overrides.verify_device_cert.value_or(cfg.verify_device_cert);
  const uint32_t requested_kbps = overrides.video_bitrate_kbps.value_or(0);
  opts.video_bitrate_kbps =
      requested_kbps > 0 ? requested_kbps : cfg.GetPresetBitrateKbps(opts.preset);
  opts.source = overrides.source;
  return opts;
}

std::optional<DeviceProfile> AppConfig::GetDeviceProfile(const std::string& device_id) const {
  auto it = device_profiles.find(device_id);
  if (it != device_profiles.end()) {
    return it->second;
  }
  return std::nullopt;
}
void AppConfig::SetDeviceProfile(const std::string& device_id, const DeviceProfile& profile) {
  if (!device_id.empty()) {
    device_profiles[device_id] = profile;
  }
}

void AppConfig::Validate() {
  reconnect_window_s = std::clamp(reconnect_window_s, 10, 600);
  auto clamp_kbps = [](uint32_t v) -> uint32_t {
    if (v == 0) return 0;
    return std::clamp(v, 1000u, 25000u);
  };
  bitrate_kbps_auto = clamp_kbps(bitrate_kbps_auto);
  bitrate_kbps_high = clamp_kbps(bitrate_kbps_high);
  bitrate_kbps_balanced = clamp_kbps(bitrate_kbps_balanced);
  bitrate_kbps_smooth = clamp_kbps(bitrate_kbps_smooth);
  bitrate_kbps_game = clamp_kbps(bitrate_kbps_game);
  bitrate_kbps_cinema = clamp_kbps(bitrate_kbps_cinema);
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
  // Tests and portable runs redirect the config away from the user profile;
  // without this a test binary would rewrite the user's real settings.
  const char* override_dir = std::getenv("CASTMIRROR_CONFIG_DIR");
  if (override_dir && override_dir[0] != '\0') {
    return (fs::path(override_dir) / "config.json").string();
  }
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

    if (j.contains("last_device_id")) ConfigReadString(j, "last_device_id", &config_.last_device_id);
    if (j.contains("last_device_name")) ConfigReadString(j, "last_device_name", &config_.last_device_name);
    if (j.contains("last_device_ip")) ConfigReadString(j, "last_device_ip", &config_.last_device_ip);
    if (j.contains("last_display_id")) ConfigReadInt(j, "last_display_id", &config_.last_display_id);
    if (j.contains("last_source_kind")) ConfigReadString(j, "last_source_kind", &config_.last_source_kind);
    if (j.contains("last_source_id")) ConfigReadInt(j, "last_source_id", &config_.last_source_id);
    if (j.contains("last_source_name")) ConfigReadString(j, "last_source_name", &config_.last_source_name);
    if (j.contains("audio_enabled")) ConfigReadBool(j, "audio_enabled", &config_.audio_enabled);
    if (j.contains("quality_preset") && j["quality_preset"].is_string()) {
      config_.quality_preset = QualityPresetFromString(j["quality_preset"].get<std::string>());
    }
    if (j.contains("target_delay_ms")) ConfigReadInt(j, "target_delay_ms", &config_.target_delay_ms);
    if (j.contains("max_bitrate_kbps")) ConfigReadU32(j, "max_bitrate_kbps", &config_.max_bitrate_kbps);
    if (j.contains("bitrate_kbps_auto")) ConfigReadU32(j, "bitrate_kbps_auto", &config_.bitrate_kbps_auto);
    if (j.contains("bitrate_kbps_high")) ConfigReadU32(j, "bitrate_kbps_high", &config_.bitrate_kbps_high);
    if (j.contains("bitrate_kbps_balanced")) ConfigReadU32(j, "bitrate_kbps_balanced", &config_.bitrate_kbps_balanced);
    if (j.contains("bitrate_kbps_smooth")) ConfigReadU32(j, "bitrate_kbps_smooth", &config_.bitrate_kbps_smooth);
    if (j.contains("bitrate_kbps_game")) ConfigReadU32(j, "bitrate_kbps_game", &config_.bitrate_kbps_game);
    if (j.contains("bitrate_kbps_cinema")) ConfigReadU32(j, "bitrate_kbps_cinema", &config_.bitrate_kbps_cinema);

    if (j.contains("device_profiles") && j["device_profiles"].is_object()) {
      config_.device_profiles.clear();
      for (auto& [dev_id, prof_j] : j["device_profiles"].items()) {
        if (!prof_j.is_object()) {
          LOG_WARN << "Ignoring device_profiles entry '" << dev_id << "': expected an object";
          continue;
        }
        DeviceProfile prof;
        if (prof_j.contains("target_width")) ConfigReadInt(prof_j, "target_width", &prof.target_width);
        if (prof_j.contains("target_height")) ConfigReadInt(prof_j, "target_height", &prof.target_height);
        if (prof_j.contains("target_fps")) ConfigReadInt(prof_j, "target_fps", &prof.target_fps);
        if (prof_j.contains("bitrate_kbps")) ConfigReadU32(prof_j, "bitrate_kbps", &prof.bitrate_kbps);
        if (prof_j.contains("target_delay_ms")) ConfigReadInt(prof_j, "target_delay_ms", &prof.target_delay_ms);
        if (prof_j.contains("preset") && prof_j["preset"].is_string()) {
          prof.preset = QualityPresetFromString(prof_j["preset"].get<std::string>());
        }
        config_.device_profiles[dev_id] = prof;
      }
    }
    if (j.contains("enable_tray_on_startup")) ConfigReadBool(j, "enable_tray_on_startup", &config_.enable_tray_on_startup);
    if (j.contains("low_latency_mode")) ConfigReadBool(j, "low_latency_mode", &config_.low_latency_mode);
    if (j.contains("capture_border_hint")) ConfigReadBool(j, "capture_border_hint", &config_.capture_border_hint);
    if (j.contains("capture_fps")) ConfigReadInt(j, "capture_fps", &config_.capture_fps);
    if (j.contains("audio_bitrate_bps")) ConfigReadU32(j, "audio_bitrate_bps", &config_.audio_bitrate_bps);
    if (j.contains("silence_host_speakers")) ConfigReadBool(j, "silence_host_speakers", &config_.silence_host_speakers);
    if (j.contains("adaptive_enabled")) ConfigReadBool(j, "adaptive_enabled", &config_.adaptive_enabled);
    if (j.contains("subnet_scan_enabled")) ConfigReadBool(j, "subnet_scan_enabled", &config_.subnet_scan_enabled);
    if (j.contains("portal_restore_token")) ConfigReadString(j, "portal_restore_token", &config_.portal_restore_token);
    if (j.contains("first_run_complete")) ConfigReadBool(j, "first_run_complete", &config_.first_run_complete);
    if (j.contains("window_width")) ConfigReadInt(j, "window_width", &config_.window_width);
    if (j.contains("window_height")) ConfigReadInt(j, "window_height", &config_.window_height);
    if (j.contains("notify_on_events")) ConfigReadBool(j, "notify_on_events", &config_.notify_on_events);
    if (j.contains("force_software_encode")) ConfigReadBool(j, "force_software_encode", &config_.force_software_encode);
    if (j.contains("force_x11_capture")) ConfigReadBool(j, "force_x11_capture", &config_.force_x11_capture);
    if (j.contains("close_to_tray")) ConfigReadBool(j, "close_to_tray", &config_.close_to_tray);

    if (j.contains("schema_version")) ConfigReadInt(j, "schema_version", &config_.schema_version);
    else {
      // Migrate v1 -> v2: map old max_bitrate_kbps -> bitrate_kbps_auto if auto is empty
      if (j.contains("max_bitrate_kbps") && !j.contains("bitrate_kbps_auto") &&
          j["max_bitrate_kbps"].is_number_integer()) {
        const int64_t v = j["max_bitrate_kbps"].get<int64_t>();
        if (v > 0 && v <= std::numeric_limits<uint32_t>::max() &&
            static_cast<uint32_t>(v) != 8000) {
          config_.bitrate_kbps_auto = static_cast<uint32_t>(v);
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
    if (j.contains("verbose_json_logging")) ConfigReadBool(j, "verbose_json_logging", &config_.verbose_json_logging);
    if (j.contains("verbose_json")) ConfigReadBool(j, "verbose_json", &config_.verbose_json_logging);
    if (j.contains("verboseJson")) ConfigReadBool(j, "verboseJson", &config_.verbose_json_logging);
    // Env override is handled by Logger, but also respect config flag
    if (j.contains("launch_timeout_s")) ConfigReadInt(j, "launch_timeout_s", &config_.launch_timeout_s);
    if (j.contains("answer_timeout_s")) ConfigReadInt(j, "answer_timeout_s", &config_.answer_timeout_s);
    if (j.contains("adaptive_resolution_enabled")) ConfigReadBool(j, "adaptive_resolution_enabled", &config_.adaptive_resolution_enabled);
    if (j.contains("verify_device_cert")) ConfigReadBool(j, "verify_device_cert", &config_.verify_device_cert);
    if (j.contains("latency_hud_enabled")) ConfigReadBool(j, "latency_hud_enabled", &config_.latency_hud_enabled);
    if (j.contains("global_hotkeys")) ConfigReadBool(j, "global_hotkeys", &config_.global_hotkeys);
    if (j.contains("show_cursor")) ConfigReadBool(j, "show_cursor", &config_.show_cursor);
    if (j.contains("audio_process_name")) ConfigReadString(j, "audio_process_name", &config_.audio_process_name);
    if (j.contains("reconnect_window_s")) ConfigReadInt(j, "reconnect_window_s", &config_.reconnect_window_s);
    if (j.contains("ui_theme")) ConfigReadString(j, "ui_theme", &config_.ui_theme);

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
  // Saves come from the UI, C API, engine and session threads; two writers
  // sharing one .tmp file would interleave and corrupt it.
  static std::mutex save_mutex;
  std::lock_guard<std::mutex> save_lock(save_mutex);
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
    j["bitrate_kbps_game"] = config_.bitrate_kbps_game;
    j["bitrate_kbps_cinema"] = config_.bitrate_kbps_cinema;

    nlohmann::json profiles_j = nlohmann::json::object();
    for (const auto& [dev_id, prof] : config_.device_profiles) {
      nlohmann::json p;
      p["target_width"] = prof.target_width;
      p["target_height"] = prof.target_height;
      p["target_fps"] = prof.target_fps;
      p["bitrate_kbps"] = prof.bitrate_kbps;
      p["target_delay_ms"] = prof.target_delay_ms;
      p["preset"] = QualityPresetToString(prof.preset);
      profiles_j[dev_id] = p;
    }
    j["device_profiles"] = profiles_j;
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
    j["verify_device_cert"] = config_.verify_device_cert;
    j["latency_hud_enabled"] = config_.latency_hud_enabled;
    j["show_cursor"] = config_.show_cursor;
    j["audio_process_name"] = config_.audio_process_name;
    j["global_hotkeys"] = config_.global_hotkeys;
    j["reconnect_window_s"] = config_.reconnect_window_s;
    j["ui_theme"] = config_.ui_theme;

    // Atomic write: write to tmp + fsync + rename, backup previous
    std::string tmp_path = path_to_save + ".tmp";
    std::string bak_path = path_to_save + ".bak";
    {
      std::ofstream file(tmp_path, std::ios::out | std::ios::trunc);
      if (!file.is_open()) return false;
      file << j.dump(2) << std::endl;
      file.flush();
      if (!file.good()) {
        LOG_ERROR << "Failed to write configuration to " << tmp_path;
        return false;  // keep the previous file rather than renaming a truncated one over it
      }
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
