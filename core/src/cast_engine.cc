#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <csignal>
#include <filesystem>
#include <cstdlib>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
namespace castcore {

namespace {

std::string CastmirrorConfigDir() {
#if defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  std::string home_dir = appdata ? appdata : "C:\\ProgramData";
  return home_dir + "\\CastMirror";
#else
  const char* home = std::getenv("HOME");
  std::string home_dir = home ? home : "/tmp";
  return home_dir + "/.config/castmirror";
#endif
}

void EnterSessionLogging() {
  std::string dir = CastmirrorConfigDir();
  try {
    std::filesystem::create_directories(dir);
  } catch (...) {}
#if defined(_WIN32)
  Logger::Instance().SetSessionFileLogging(dir + "\\castmirror-session.log");
#else
  Logger::Instance().SetSessionFileLogging(dir + "/castmirror-session.log");
#endif
  Logger::Instance().SetMinLevel(LogLevel::kDebug);
}

void LeaveSessionLogging() {
  Logger::Instance().SetMinLevel(LogLevel::kInfo);
  Logger::Instance().ClearSessionFileLogging();
}

}  // namespace

CastEngine& CastEngine::Instance() {
  static CastEngine instance;
  return instance;
}

CastEngine::CastEngine() {
  state_machine_.RegisterCallback([this](SessionState old_s, SessionState new_s, const std::string& msg) {
    StateChangedCallback cb;
    {
      std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
      cb = state_cb_;
    }
    if (cb) {
      cb(old_s, new_s, msg);
    }
  });

  discovery_.SetCallback([this](const std::vector<CastDevice>& devices) {
    DevicesChangedCallback cb;
    {
      std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
      cb = devices_cb_;
    }
    if (cb) {
      cb(devices);
    }
  });
}

CastEngine::~CastEngine() {
  Shutdown();
}

bool CastEngine::Initialize() {
#if !defined(_WIN32)
  std::signal(SIGPIPE, SIG_IGN);
#endif
  if (is_initialized_.exchange(true)) return true;

  std::string dir_path = CastmirrorConfigDir();
#if defined(_WIN32)
  std::string log_file = dir_path + "\\castmirror.log";
#else
  std::string log_file = dir_path + "/castmirror.log";
#endif

  try {
    std::filesystem::create_directories(dir_path);
  } catch (...) {}

  Logger::Instance().SetFileLogging(log_file);
  Logger::Instance().SetMinLevel(LogLevel::kInfo);

  LOG_INFO << "Initializing CastEngine (Logging to " << log_file << ")...";
  ConfigStore::Instance().Load();
  // Wire structured diagnostics JSON sidecar opt-in via Config flag verbose_json (default false)
  // Also respect env var CASTMIRROR_VERBOSE_JSON=1 for CI/benchmarks
  {
    const auto& cfg = ConfigStore::Instance().Get();
    bool want_json = cfg.verbose_json_logging;
    const char* env = std::getenv("CASTMIRROR_VERBOSE_JSON");
    if (env && (std::string(env) == "1" || std::string(env) == "true" || std::string(env) == "TRUE")) {
      want_json = true;
    }
    if (want_json) {
      Logger::Instance().SetVerboseJsonEnabled(true);
      // Ensure sidecar file is initialized at default path
      Logger::Instance().SetJsonLogging(Logger::GetDefaultJsonPath());
    }
  }
  discovery_.Start();
  return true;
}

void CastEngine::Shutdown() {
  if (!is_initialized_.exchange(false)) return;

  LOG_INFO << "Shutting down CastEngine...";
  StopCasting();
  discovery_.Stop();
  ConfigStore::Instance().Save();
}

void CastEngine::StartDiscovery() {
  discovery_.Start();
}

void CastEngine::StopDiscovery() {
  discovery_.Stop();
}

std::vector<CastDevice> CastEngine::GetDevices() const {
  return discovery_.GetDevices();
}

std::vector<DisplayInfo> CastEngine::GetDisplays() const {
  auto capturer = DisplayCaptureFactory::Create();
  return capturer->EnumerateDisplays();
}

std::vector<WindowInfo> CastEngine::GetWindows() const {
  auto capturer = DisplayCaptureFactory::Create();
  return capturer->EnumerateWindows();
}

bool CastEngine::WindowCaptureSupported() const {
  auto capturer = DisplayCaptureFactory::Create();
  return capturer->SupportsWindowCapture();
}

bool CastEngine::StartCasting(const std::string& device_id,
                             int display_id,
                             QualityPreset preset,
                             bool audio_enabled,
                             uint32_t bitrate_kbps) {
  const auto& cfg = ConfigStore::Instance().Get();
  SessionOptions options;
  options.preset = preset;
  options.enable_audio = audio_enabled;
  options.video_bitrate_kbps = bitrate_kbps;
  options.audio_bitrate_bps = cfg.audio_bitrate_bps;
  options.capture_fps = cfg.capture_fps;
  options.target_delay_ms = cfg.target_delay_ms;
  options.silence_host_speakers = cfg.silence_host_speakers;
  options.adaptive_enabled = cfg.adaptive_enabled;
  options.adaptive_resolution_enabled = cfg.adaptive_resolution_enabled;
  return StartCasting(device_id, display_id, options);
}

bool CastEngine::StartCasting(const std::string& device_id, int display_id, const SessionOptions& options) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);

  auto dev_opt = discovery_.FindDeviceById(device_id);
  if (!dev_opt.has_value()) {
    dev_opt = discovery_.FindDeviceByIp(device_id);
  }

  if (!dev_opt.has_value()) {
    const auto& cfg = ConfigStore::Instance().Get();
    struct sockaddr_in sa{};
    if (inet_pton(AF_INET, device_id.c_str(), &sa.sin_addr) > 0) {
      CastDevice d;
      d.id = device_id;
      d.name = (!cfg.last_device_name.empty() && cfg.last_device_ip == device_id)
                   ? cfg.last_device_name
                   : ("Cast Device (" + device_id + ")");
      d.model_name = "Chromecast";
      d.ip_address = device_id;
      d.port = 8009;
      d.capabilities = kCapVideoOut | kCapAudioOut;
      d.status = DeviceStatus::kReady;
      discovery_.AddOrUpdateDevice(d);
      dev_opt = d;
    } else if (!cfg.last_device_ip.empty() && (device_id == cfg.last_device_id || device_id == cfg.last_device_name)) {
      CastDevice d;
      d.id = cfg.last_device_id.empty() ? cfg.last_device_ip : cfg.last_device_id;
      d.name = cfg.last_device_name.empty() ? cfg.last_device_ip : cfg.last_device_name;
      d.model_name = "Chromecast";
      d.ip_address = cfg.last_device_ip;
      d.port = 8009;
      d.capabilities = kCapVideoOut | kCapAudioOut;
      d.status = DeviceStatus::kReady;
      discovery_.AddOrUpdateDevice(d);
      dev_opt = d;
    }
  }

  if (!dev_opt.has_value()) {
    last_error_ = "Target device not found: " + device_id + ". Please verify the TV is online or add by IP.";
    LOG_ERROR << last_error_;
    return false;
  }
  CastDevice dev = dev_opt.value();
  const bool custom_endpoint = dev.model_name == "Custom Chromecast" ||
                               dev.ip_address.rfind("127.", 0) == 0;
  if (!custom_endpoint && dev.port != 8009 && dev.port != 8008) {
    LOG_WARN << "Correcting stale Cast control port " << dev.port << " -> 8009 for " << dev.ip_address;
    dev.port = 8009;
    discovery_.AddOrUpdateDevice(dev);
  }

  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.last_device_id = dev.id;
  cfg.last_device_name = dev.name;
  cfg.last_device_ip = dev.ip_address;
  cfg.last_display_id = display_id;
  // Persist the capture source so "Cast to last" can rebuild it. When the
  // caller passed an explicit options.source we record it; otherwise this is
  // a Monitor source derived from display_id (legacy path).
  CaptureSource persisted_source = options.source.value_or(
      CaptureSource{CaptureSourceKind::kMonitor, display_id, ""});
  cfg.last_source_kind = CaptureSourceKindToString(persisted_source.kind);
  cfg.last_source_id = persisted_source.id;
  cfg.last_source_name = persisted_source.name;
  cfg.quality_preset = options.preset;
  cfg.audio_enabled = options.enable_audio;
  cfg.capture_fps = options.capture_fps;
  cfg.audio_bitrate_bps = options.audio_bitrate_bps;
  cfg.silence_host_speakers = options.silence_host_speakers;
  cfg.adaptive_enabled = options.adaptive_enabled;
  if (options.target_delay_ms > 0) {
    cfg.target_delay_ms = options.target_delay_ms;
  }
  if (options.video_bitrate_kbps > 0) {
    cfg.SetPresetBitrateKbps(options.preset, options.video_bitrate_kbps);
    cfg.max_bitrate_kbps = options.video_bitrate_kbps;
  }
  ConfigStore::Instance().Save();

  EnterSessionLogging();
  active_session_ = std::make_unique<CastSession>(state_machine_);
  active_session_->SetDeviceLookup([this](const std::string& id, const std::string& ip) {
    auto dev = discovery_.FindDeviceById(id);
    if (!dev) dev = discovery_.FindDeviceByIp(ip);
    return dev;
  });
  active_session_->SetErrorCallback([this](const std::string& err) {
    std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
    last_error_ = err;
  });
  last_error_.clear();
  bool ok = active_session_->Start(dev, display_id, options);
  if (!ok) {
    active_session_.reset();
    LeaveSessionLogging();
  }
  return ok;
}

bool CastEngine::StartCasting(const std::string& device_id, const CaptureSource& source, const SessionOptions& options) {
  SessionOptions opts = options;
  opts.source = source;
  // For Monitor sources, pass source.id as the legacy display_id so backends
  // that only implement Start(int,int) keep working. For Window sources the
  // display_id is unused by source-aware backends.
  return StartCasting(device_id, source.id, opts);
}

bool CastEngine::StartCastingLastDevice() {
  const auto& cfg = ConfigStore::Instance().Get();
  if (cfg.last_device_id.empty() && cfg.last_device_ip.empty()) {
    LOG_WARN << "No previous Cast device found in config.";
    return false;
  }

  std::string target = !cfg.last_device_id.empty() ? cfg.last_device_id : cfg.last_device_ip;
  SessionOptions options;
  options.preset = cfg.quality_preset;
  options.enable_audio = cfg.audio_enabled;
  options.video_bitrate_kbps = cfg.GetPresetBitrateKbps(cfg.quality_preset);
  options.audio_bitrate_bps = cfg.audio_bitrate_bps;
  options.capture_fps = cfg.capture_fps;
  options.target_delay_ms = cfg.target_delay_ms;
  options.silence_host_speakers = cfg.silence_host_speakers;
  options.adaptive_resolution_enabled = cfg.adaptive_resolution_enabled;
  options.adaptive_enabled = cfg.adaptive_enabled;

  // Rebuild the last capture source. For windows the persisted id is not
  // stable across restarts, so try to re-resolve by name; if not found, fall
  // back to the last monitor so "Cast to last" never silently targets a
  // stale window handle.
  CaptureSourceKind last_kind = CaptureSourceKindFromString(cfg.last_source_kind);
  if (last_kind == CaptureSourceKind::kWindow && !cfg.last_source_name.empty()) {
    auto windows = GetWindows();
    bool found = false;
    for (const auto& w : windows) {
      if (w.title == cfg.last_source_name) {
        options.source = CaptureSource{CaptureSourceKind::kWindow, w.id, w.title,
                                       w.x, w.y, w.width, w.height};
        found = true;
        break;
      }
    }
    if (!found) {
      LOG_WARN << "Last-cast window '" << cfg.last_source_name
               << "' is no longer open; falling back to monitor "
               << cfg.last_display_id;
      options.source = CaptureSource{CaptureSourceKind::kMonitor, cfg.last_display_id, ""};
    }
    return StartCasting(target, options.source->id, options);
  }
  // Monitor (default): use the legacy display_id path.
  return StartCasting(target, cfg.last_display_id, options);
}

void CastEngine::StopCasting() {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->Stop();
    active_session_.reset();
  }
  LeaveSessionLogging();
}

void CastEngine::SetLiveVideoBitrateKbps(uint32_t kbps) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->SetLiveVideoBitrateKbps(kbps);
  }
  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.SetPresetBitrateKbps(cfg.quality_preset, kbps);
  cfg.max_bitrate_kbps = kbps;
  ConfigStore::Instance().Save();
}

void CastEngine::SetLiveAudioBitrateBps(uint32_t bps) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->SetLiveAudioBitrateBps(bps);
  }
  ConfigStore::Instance().Mutable().audio_bitrate_bps = bps;
  ConfigStore::Instance().Save();
}

void CastEngine::SetFreezeStream(bool freeze) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->SetStreamFrozen(freeze);
  }
}

bool CastEngine::IsStreamFrozen() const {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    return active_session_->IsStreamFrozen();
  }
  return false;
}

void CastEngine::SetLiveAudioMuted(bool muted) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->SetAudioMuted(muted);
  }
}

bool CastEngine::IsLiveAudioMuted() const {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    return active_session_->IsAudioMuted();
  }
  return false;
}

SessionState CastEngine::GetState() const {
  return state_machine_.GetState();
}

StreamStats CastEngine::GetStats() const {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    return active_session_->GetStats();
  }
  return StreamStats{};
}

const AppConfig& CastEngine::GetConfig() const {
  return ConfigStore::Instance().Get();
}

std::string CastEngine::GetLastError() const {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  return last_error_;
}

void CastEngine::SetOnDevicesChanged(DevicesChangedCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  devices_cb_ = std::move(callback);
}

void CastEngine::SetOnStateChanged(StateChangedCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  state_cb_ = std::move(callback);
}

void CastEngine::SetOnStatsUpdated(StatsUpdatedCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  stats_cb_ = std::move(callback);
}

} // namespace castcore
