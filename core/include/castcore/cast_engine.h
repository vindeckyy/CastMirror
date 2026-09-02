#ifndef CASTCORE_CAST_ENGINE_H_
#define CASTCORE_CAST_ENGINE_H_

#include "castcore/types.h"
#include "castcore/state_machine.h"
#include "castcore/config.h"
#include "castcore/device_discovery.h"
#include "castcore/display_capture.h"
#include "castcore/cast_session.h"

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>

namespace castcore {

class CastEngine {
 public:
  using DevicesChangedCallback = std::function<void(const std::vector<CastDevice>& devices)>;
  using StateChangedCallback = std::function<void(SessionState old_state, SessionState new_state, const std::string& message)>;
  using StatsUpdatedCallback = std::function<void(const StreamStats& stats)>;

  static CastEngine& Instance();

  bool Initialize();
  void Shutdown();

  void StartDiscovery();
  void StopDiscovery();

  std::vector<CastDevice> GetDevices() const;
  std::vector<DisplayInfo> GetDisplays() const;
  std::vector<WindowInfo> GetWindows() const;
  bool WindowCaptureSupported() const;

  bool StartCasting(const std::string& device_id,
                    int display_id = 0,
                    QualityPreset preset = QualityPreset::kAuto,
                    bool audio_enabled = true,
                    uint32_t bitrate_kbps = 0);

  bool StartCasting(const std::string& device_id, int display_id, const SessionOptions& options);

  // Source-aware entry point: capture a monitor or a single window. Sets
  // options.source and delegates to the (device, display_id, options) overload.
  bool StartCasting(const std::string& device_id, const CaptureSource& source, const SessionOptions& options);

  bool StartCastingLastDevice();
  void StopCasting();

  void SetLiveVideoBitrateKbps(uint32_t kbps);
  void SetLiveAudioBitrateBps(uint32_t bps);

  SessionState GetState() const;
  StreamStats GetStats() const;
  const AppConfig& GetConfig() const;
  std::string GetLastError() const;

  void SetOnDevicesChanged(DevicesChangedCallback callback);
  void SetOnStateChanged(StateChangedCallback callback);
  void SetOnStatsUpdated(StatsUpdatedCallback callback);

  StateMachine& GetStateMachine() { return state_machine_; }
  DeviceDiscovery& GetDiscovery() { return discovery_; }

 private:
  CastEngine();
  ~CastEngine();

  StateMachine state_machine_;
  DeviceDiscovery discovery_;
  std::unique_ptr<CastSession> active_session_;
  mutable std::recursive_mutex engine_mutex_;

  DevicesChangedCallback devices_cb_;
  StateChangedCallback state_cb_;
  StatsUpdatedCallback stats_cb_;

  std::string last_error_;
  std::atomic<bool> is_initialized_{false};
};

} // namespace castcore

#endif // CASTCORE_CAST_ENGINE_H_
