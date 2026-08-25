#include "castcore/cast_engine.h"
#include "castcore/logger.h"
#include <csignal>

namespace castcore {

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

  LOG_INFO << "Initializing CastEngine...";
  ConfigStore::Instance().Load();
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

bool CastEngine::StartCasting(const std::string& device_id,
                             int display_id,
                             QualityPreset preset,
                             bool audio_enabled) {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);

  auto dev_opt = discovery_.FindDeviceById(device_id);
  if (!dev_opt.has_value()) {
    dev_opt = discovery_.FindDeviceByIp(device_id);
  }

  if (!dev_opt.has_value()) {
    LOG_ERROR << "Target device not found: " << device_id;
    return false;
  }

  CastDevice dev = dev_opt.value();

  // Save as last device
  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.last_device_id = dev.id;
  cfg.last_device_name = dev.name;
  cfg.last_device_ip = dev.ip_address;
  cfg.last_display_id = display_id;
  cfg.quality_preset = preset;
  cfg.audio_enabled = audio_enabled;
  ConfigStore::Instance().Save();

  active_session_ = std::make_unique<CastSession>(state_machine_);
  return active_session_->Start(dev, display_id, preset, audio_enabled);
}

bool CastEngine::StartCastingLastDevice() {
  const auto& cfg = ConfigStore::Instance().Get();
  if (cfg.last_device_id.empty() && cfg.last_device_ip.empty()) {
    LOG_WARN << "No previous Cast device found in config.";
    return false;
  }

  std::string target = !cfg.last_device_id.empty() ? cfg.last_device_id : cfg.last_device_ip;
  return StartCasting(target, cfg.last_display_id, cfg.quality_preset, cfg.audio_enabled);
}

void CastEngine::StopCasting() {
  std::lock_guard<std::recursive_mutex> lock(engine_mutex_);
  if (active_session_) {
    active_session_->Stop();
    active_session_.reset();
  }
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
