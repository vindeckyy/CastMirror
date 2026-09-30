#include "castcore/c_api.h"
#include "castcore/cast_engine.h"
#include "castcore/types.h"
#include "castcore/config.h"
#include "castcore/net_platform.h"
#include "castcore/display_capture.h"
#include "castcore/video_encoder.h"
#include "castcore/audio_capture.h"
#include "castcore/logger.h"
#include <cstring>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <limits>
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

struct CallbackState {
  CastMirrorStateCallback state_cb = nullptr;
  void* state_user_data = nullptr;

  CastMirrorDevicesCallback devices_cb = nullptr;
  void* devices_user_data = nullptr;

  CastMirrorStatsCallback stats_cb = nullptr;
  void* stats_user_data = nullptr;

  CastMirrorLogCallback log_cb = nullptr;
  void* log_user_data = nullptr;

  std::mutex cb_mutex;
  // Callbacks currently executing, guarded by cb_mutex: castmirror_shutdown()
  // waits for this to reach zero, because a client frees the user_data it
  // registered (a GCHandle in the WinUI client) as soon as shutdown returns.
  int callbacks_in_flight = 0;
  std::condition_variable cb_idle;
  std::vector<castcore::CastDevice> cached_devices;
  std::vector<castcore::DisplayInfo> cached_displays;
  std::vector<castcore::WindowInfo> cached_windows;
} g_c_state;

// Decrements the in-flight count for one callback invocation. The matching
// increment happens under cb_mutex together with the registration read, so
// castmirror_shutdown() cannot miss an invocation that is about to start.
struct CallbackInvocationScope {
  ~CallbackInvocationScope() {
    std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
    if (--g_c_state.callbacks_in_flight == 0) {
      g_c_state.cb_idle.notify_all();
    }
  }
};

CastMirrorState ConvertState(castcore::SessionState s) {
  switch (s) {
    case castcore::SessionState::kIdle:
    case castcore::SessionState::kDiscovering:
    case castcore::SessionState::kReady:
      return CASTMIRROR_STATE_IDLE;
    case castcore::SessionState::kConnecting: return CASTMIRROR_STATE_CONNECTING;
    case castcore::SessionState::kNegotiating: return CASTMIRROR_STATE_NEGOTIATING;
    case castcore::SessionState::kStreaming: return CASTMIRROR_STATE_STREAMING;
    case castcore::SessionState::kReconnecting: return CASTMIRROR_STATE_RECONNECTING;
    case castcore::SessionState::kStopping: return CASTMIRROR_STATE_STOPPING;
    case castcore::SessionState::kFailed: return CASTMIRROR_STATE_FAILED;
  }
  return CASTMIRROR_STATE_IDLE;
}

void ConvertStats(const castcore::StreamStats& in, CastMirrorStreamStats* out) {
  if (!out) return;
  out->bitrate_kbps = in.bitrate_kbps;
  out->current_fps = in.current_fps;
  out->round_trip_time_ms = in.round_trip_time_ms;
  out->packet_loss_fraction = in.packet_loss_fraction;
  out->target_delay_ms = in.target_delay_ms;
  out->width = in.current_resolution.width;
  out->height = in.current_resolution.height;
  out->frames_sent = in.frames_sent;
  out->packets_sent = in.packets_sent;
  out->video_queue_overruns = in.video_queue_overruns;
  out->current_framerate = in.current_framerate;
  out->adaptive_rung_index = in.adaptive_rung_index;
  out->adaptive_rung_count = in.adaptive_rung_count;
  out->adaptive_enabled = in.adaptive_enabled ? 1 : 0;
  std::strncpy(out->encoder_name, in.encoder_name.c_str(), sizeof(out->encoder_name) - 1);
  out->encoder_name[sizeof(out->encoder_name) - 1] = '\0';
  std::strncpy(out->capture_backend, in.capture_backend.c_str(), sizeof(out->capture_backend) - 1);
  out->capture_backend[sizeof(out->capture_backend) - 1] = '\0';
  std::strncpy(out->display_name, in.display_name.c_str(), sizeof(out->display_name) - 1);
  out->display_name[sizeof(out->display_name) - 1] = '\0';
}

// Shared buffer contract for castmirror_get_config_json / castmirror_self_test:
// writes up to buf_len - 1 chars + NUL and returns chars written; with no
// buffer it returns the required size including the terminator.
int WriteJsonOut(const std::string& json, char* out_buf, int buf_len) {
  const int required = static_cast<int>(json.size()) + 1;
  if (!out_buf || buf_len <= 0) return required;
  int len = required - 1;
  if (len > buf_len - 1) len = buf_len - 1;
  if (len > 0) std::memcpy(out_buf, json.data(), static_cast<size_t>(len));
  out_buf[len] = '\0';
  return len;
}

nlohmann::json BuildConfigJson() {
  const auto& cfg = castcore::ConfigStore::Instance().Get();
  nlohmann::json j;
  j["quality_preset"] = castcore::QualityPresetToString(cfg.quality_preset);
  j["max_bitrate_kbps"] = cfg.max_bitrate_kbps;
  j["bitrate_kbps_auto"] = cfg.bitrate_kbps_auto;
  j["bitrate_kbps_high"] = cfg.bitrate_kbps_high;
  j["bitrate_kbps_balanced"] = cfg.bitrate_kbps_balanced;
  j["bitrate_kbps_smooth"] = cfg.bitrate_kbps_smooth;
  j["bitrate_kbps_game"] = cfg.bitrate_kbps_game;
  j["bitrate_kbps_cinema"] = cfg.bitrate_kbps_cinema;
  j["capture_fps"] = cfg.capture_fps;
  j["audio_enabled"] = cfg.audio_enabled;
  j["audio_bitrate_bps"] = cfg.audio_bitrate_bps;
  j["silence_host_speakers"] = cfg.silence_host_speakers;
  j["target_delay_ms"] = cfg.target_delay_ms;
  j["latency_hud_enabled"] = cfg.latency_hud_enabled;
  j["adaptive_enabled"] = cfg.adaptive_enabled;
  j["adaptive_resolution_enabled"] = cfg.adaptive_resolution_enabled;
  j["subnet_scan_enabled"] = cfg.subnet_scan_enabled;
  j["force_software_encode"] = cfg.force_software_encode;
  j["enable_tray_on_startup"] = cfg.enable_tray_on_startup;
  j["close_to_tray"] = cfg.close_to_tray;
  j["notify_on_events"] = cfg.notify_on_events;
  j["verify_device_cert"] = cfg.verify_device_cert;
  j["first_run_complete"] = cfg.first_run_complete;
  j["ui_theme"] = cfg.ui_theme;
  return j;
}

// castmirror_get_config_json used to re-serialize the whole config on every
// call. A caller following the documented size-query-then-fill two-step could
// therefore size its buffer from one document and be handed a *different* (and
// possibly longer) one, silently truncated: the engine rewrites bitrate
// overrides and last-device bookkeeping from its own threads. Cache the dumped
// document and serve the identical bytes to both calls; set_config_json (the
// only write path through this API) invalidates it.
std::mutex g_config_json_mutex;
std::string g_config_json_cache;
bool g_config_json_cache_valid = false;

// Conservative upper bound returned by the size query. Even with the cache a
// rebuild can slip between the two calls, so the size query reports at least
// this much: a caller that allocates it always receives the whole document,
// which is what the two-step contract in c_api.h promises.
constexpr int kJsonOutCapacityBytes = 8192;

int JsonOutSizeHint(int required) {
  return required > kJsonOutCapacityBytes ? required : kJsonOutCapacityBytes;
}

std::string CachedConfigJson() {
  std::lock_guard<std::mutex> lock(g_config_json_mutex);
  if (!g_config_json_cache_valid) {
    g_config_json_cache = BuildConfigJson().dump();
    g_config_json_cache_valid = true;
  }
  return g_config_json_cache;
}

void InvalidateConfigJsonCache() {
  std::lock_guard<std::mutex> lock(g_config_json_mutex);
  g_config_json_cache_valid = false;
  g_config_json_cache.clear();
}

// Merges known keys from j into cfg. Unknown keys are ignored so older/newer
// front-ends can talk to this build without failing.
//
// A *known* key of the wrong type or out of range is ignored too, but never
// silently: get<uint32_t>() on -1 yields 4294967295, and get<int>() truncates a
// wide value, so a client that sends nonsense used to have it persisted as a
// different setting. The key keeps its previous value and the log names it.
void MergeConfigJson(const nlohmann::json& j, castcore::AppConfig& cfg) {
  // The strict readers (ConfigRead*) are the single implementation of this
  // contract; ConfigStore::Load uses them for the on-disk file, so a hostile
  // or typo'd value is rejected identically through both paths.
  auto get_int = [&j](const char* key, int* out) {
    if (j.contains(key)) castcore::ConfigReadInt(j, key, out);
  };
  auto get_u32 = [&j](const char* key, uint32_t* out) {
    if (j.contains(key)) castcore::ConfigReadU32(j, key, out);
  };
  auto get_bool = [&j](const char* key, bool* out) {
    if (j.contains(key)) castcore::ConfigReadBool(j, key, out);
  };
  auto get_str = [&j](const char* key, std::string* out) {
    if (j.contains(key)) castcore::ConfigReadString(j, key, out);
  };

  if (j.contains("quality_preset") && j["quality_preset"].is_string()) {
    cfg.quality_preset = castcore::QualityPresetFromString(j["quality_preset"].get<std::string>());
  }
  get_u32("max_bitrate_kbps", &cfg.max_bitrate_kbps);
  get_u32("bitrate_kbps_auto", &cfg.bitrate_kbps_auto);
  get_u32("bitrate_kbps_high", &cfg.bitrate_kbps_high);
  get_u32("bitrate_kbps_balanced", &cfg.bitrate_kbps_balanced);
  get_u32("bitrate_kbps_smooth", &cfg.bitrate_kbps_smooth);
  get_u32("bitrate_kbps_game", &cfg.bitrate_kbps_game);
  get_u32("bitrate_kbps_cinema", &cfg.bitrate_kbps_cinema);
  get_int("capture_fps", &cfg.capture_fps);
  get_bool("audio_enabled", &cfg.audio_enabled);
  get_u32("audio_bitrate_bps", &cfg.audio_bitrate_bps);
  get_bool("silence_host_speakers", &cfg.silence_host_speakers);
  get_int("target_delay_ms", &cfg.target_delay_ms);
  get_bool("latency_hud_enabled", &cfg.latency_hud_enabled);
  get_bool("adaptive_enabled", &cfg.adaptive_enabled);
  get_bool("adaptive_resolution_enabled", &cfg.adaptive_resolution_enabled);
  get_bool("subnet_scan_enabled", &cfg.subnet_scan_enabled);
  get_bool("force_software_encode", &cfg.force_software_encode);
  get_bool("enable_tray_on_startup", &cfg.enable_tray_on_startup);
  get_bool("close_to_tray", &cfg.close_to_tray);
  get_bool("notify_on_events", &cfg.notify_on_events);
  get_bool("verify_device_cert", &cfg.verify_device_cert);
  get_bool("first_run_complete", &cfg.first_run_complete);
  get_str("ui_theme", &cfg.ui_theme);
}

// Explicit arguments win, everything else comes from ConfigStore so persisted
// settings actually reach a session. The policy itself lives in
// BuildSessionOptions (config.h) because the CLI and CastEngine::StartCasting
// need exactly the same one - the three copies used to disagree, and the CLI's
// copy silently reset the user's audio bitrate and adaptive settings.
castcore::SessionOptions BuildSessionOptions(int preset, bool audio_enabled,
                                             int target_fps, uint32_t bitrate_kbps) {
  castcore::SessionOverrides overrides;
  overrides.preset = static_cast<castcore::QualityPreset>(preset);
  overrides.enable_audio = audio_enabled;
  overrides.capture_fps = target_fps;
  overrides.video_bitrate_kbps = bitrate_kbps;
  return castcore::BuildSessionOptions(castcore::ConfigStore::Instance().Get(), overrides);
}

}  // namespace

extern "C" {

bool castmirror_init(void) {
  bool ok = castcore::CastEngine::Instance().Initialize();
  if (ok) {
    castcore::CastEngine::Instance().SetOnStateChanged(
        [](castcore::SessionState old_state, castcore::SessionState state, const std::string& message) {
          (void)old_state;
          CastMirrorStateCallback cb = nullptr;
          void* user_data = nullptr;
          {
            std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
            cb = g_c_state.state_cb;
            user_data = g_c_state.state_user_data;
            if (cb) ++g_c_state.callbacks_in_flight;
          }
          if (!cb) return;
          CallbackInvocationScope invocation;
          cb(ConvertState(state), message.c_str(), user_data);
        });

    castcore::CastEngine::Instance().SetOnDevicesChanged(
        [](const std::vector<castcore::CastDevice>& devs) {
          CastMirrorDevicesCallback cb = nullptr;
          void* user_data = nullptr;
          {
            std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
            g_c_state.cached_devices = devs;
            cb = g_c_state.devices_cb;
            user_data = g_c_state.devices_user_data;
            if (cb) ++g_c_state.callbacks_in_flight;
          }
          if (!cb) return;
          // Invoke without holding the cache lock so clients can query
          // castmirror_get_device_info from inside the callback.
          CallbackInvocationScope invocation;
          cb(static_cast<int>(devs.size()), user_data);
        });

    castcore::CastEngine::Instance().SetOnStatsUpdated(
        [](const castcore::StreamStats& stats) {
          CastMirrorStatsCallback cb = nullptr;
          void* user_data = nullptr;
          {
            std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
            cb = g_c_state.stats_cb;
            user_data = g_c_state.stats_user_data;
            if (cb) ++g_c_state.callbacks_in_flight;
          }
          if (!cb) return;
          CastMirrorStreamStats c_stats;
          ConvertStats(stats, &c_stats);
          CallbackInvocationScope invocation;
          cb(&c_stats, user_data);
        });

    // Route the core logger to the client log callback. The std::function is
    // installed once; the client's pointer is swapped under cb_mutex on every
    // log, so it can be attached/cleared without re-registering.
    castcore::Logger::Instance().SetCallback(
        [](castcore::LogLevel level, const std::string& message) {
          CastMirrorLogCallback cb = nullptr;
          void* user_data = nullptr;
          {
            std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
            cb = g_c_state.log_cb;
            user_data = g_c_state.log_user_data;
            if (cb) ++g_c_state.callbacks_in_flight;
          }
          if (!cb) return;
          CallbackInvocationScope invocation;
          cb(static_cast<int>(level), message.c_str(), user_data);
        });
  }
  return ok;
}

uint32_t castmirror_abi_version(void) {
  return CASTMIRROR_ABI_VERSION;
}

void castmirror_shutdown(void) {
  // Drop every registered callback before tearing the engine down: a client
  // that re-creates its view model must not leave a dangling native pointer.
  {
    std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
    g_c_state.state_cb = nullptr;
    g_c_state.state_user_data = nullptr;
    g_c_state.devices_cb = nullptr;
    g_c_state.devices_user_data = nullptr;
    g_c_state.stats_cb = nullptr;
    g_c_state.stats_user_data = nullptr;
    g_c_state.log_cb = nullptr;
    g_c_state.log_user_data = nullptr;
  }
  // Clearing the registrations stops *new* callbacks, but one can already be
  // executing on another thread (the client's own thread inside an API call, for
  // instance). Clients free the user_data they registered as soon as this
  // returns - a GCHandle in the WinUI client - so returning while one is still
  // running would be a use-after-free on their side. Wait for them, with a bound
  // so a client callback that blocks cannot hang teardown forever.
  {
    std::unique_lock<std::mutex> lock(g_c_state.cb_mutex);
    g_c_state.cb_idle.wait_for(lock, std::chrono::seconds(2), [] {
      return g_c_state.callbacks_in_flight == 0;
    });
    if (g_c_state.callbacks_in_flight != 0) {
      // The log callback is already detached, so this reaches the core log only.
      LOG_WARN << "castmirror_shutdown: " << g_c_state.callbacks_in_flight
               << " callback(s) still running after 2s; user_data may still be in use";
    }
  }
  castcore::CastEngine::Instance().Shutdown();
}

void castmirror_start_discovery(void) {
  castcore::CastEngine::Instance().StartDiscovery();
}

void castmirror_stop_discovery(void) {
  castcore::CastEngine::Instance().StopDiscovery();
}

int castmirror_get_device_count(void) {
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  return static_cast<int>(g_c_state.cached_devices.size());
}

bool castmirror_get_device_info(int index, CastMirrorDeviceInfo* out_info) {
  if (!out_info) return false;
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_c_state.cached_devices.size()) {
    return false;
  }
  const auto& dev = g_c_state.cached_devices[index];
  std::strncpy(out_info->id, dev.id.c_str(), sizeof(out_info->id) - 1);
  out_info->id[sizeof(out_info->id) - 1] = '\0';
  std::strncpy(out_info->name, dev.name.c_str(), sizeof(out_info->name) - 1);
  out_info->name[sizeof(out_info->name) - 1] = '\0';
  std::strncpy(out_info->ip_address, dev.ip_address.c_str(), sizeof(out_info->ip_address) - 1);
  out_info->ip_address[sizeof(out_info->ip_address) - 1] = '\0';
  out_info->port = dev.port;
  std::strncpy(out_info->model_name, dev.model_name.c_str(), sizeof(out_info->model_name) - 1);
  out_info->model_name[sizeof(out_info->model_name) - 1] = '\0';
  return true;
}

int castmirror_get_display_count(void) {
  auto displays = castcore::CastEngine::Instance().GetDisplays();
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.cached_displays = std::move(displays);
  return static_cast<int>(g_c_state.cached_displays.size());
}

bool castmirror_get_display_info(int index, CastMirrorDisplayInfo* out_info) {
  if (!out_info) return false;
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_c_state.cached_displays.size()) {
    return false;
  }
  const auto& display = g_c_state.cached_displays[index];
  std::memset(out_info, 0, sizeof(*out_info));
  out_info->id = display.id;
  std::strncpy(out_info->name, display.name.c_str(), sizeof(out_info->name) - 1);
  out_info->x = display.x;
  out_info->y = display.y;
  out_info->width = display.width;
  out_info->height = display.height;
  out_info->refresh_rate = display.refresh_rate;
  out_info->is_primary = display.is_primary;
  return true;
}

int castmirror_get_window_count(void) {
  auto windows = castcore::CastEngine::Instance().GetWindows();
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.cached_windows = std::move(windows);
  return static_cast<int>(g_c_state.cached_windows.size());
}

bool castmirror_get_window_info(int index, CastMirrorWindowInfo* out_info) {
  if (!out_info) return false;
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  if (index < 0 || static_cast<size_t>(index) >= g_c_state.cached_windows.size()) {
    return false;
  }
  const auto& window = g_c_state.cached_windows[index];
  std::memset(out_info, 0, sizeof(*out_info));
  out_info->id = window.id;
  std::strncpy(out_info->title, window.title.c_str(), sizeof(out_info->title) - 1);
  std::strncpy(out_info->app_class, window.app_class.c_str(), sizeof(out_info->app_class) - 1);
  out_info->x = window.x;
  out_info->y = window.y;
  out_info->width = window.width;
  out_info->height = window.height;
  out_info->visible = window.visible;
  return true;
}

bool castmirror_window_capture_supported(void) {
  return castcore::CastEngine::Instance().WindowCaptureSupported();
}

bool castmirror_start_cast(const char* device_id, int display_id, int target_fps, uint32_t bitrate_kbps) {
  if (!device_id) return false;
  const auto& cfg = castcore::ConfigStore::Instance().Get();
  castcore::SessionOptions opts = BuildSessionOptions(
      static_cast<int>(cfg.quality_preset), cfg.audio_enabled, target_fps, bitrate_kbps);
  return castcore::CastEngine::Instance().StartCasting(device_id, display_id, opts);
}

bool castmirror_start_cast_ex(const char* device_id,
                              int source_kind,
                              int source_id,
                              int target_fps,
                              uint32_t bitrate_kbps,
                              int preset,
                              bool audio_enabled) {
  if (!device_id) return false;
  int preset_value = static_cast<int>(castcore::QualityPreset::kAuto);
  if (preset >= static_cast<int>(castcore::QualityPreset::kAuto) &&
      preset <= static_cast<int>(castcore::QualityPreset::kCinema)) {
    preset_value = preset;
  }
  castcore::SessionOptions opts = BuildSessionOptions(preset_value, audio_enabled, target_fps, bitrate_kbps);
  castcore::CaptureSource source;
  source.kind = (source_kind == CASTMIRROR_SOURCE_WINDOW)
                    ? castcore::CaptureSourceKind::kWindow
                    : castcore::CaptureSourceKind::kMonitor;
  source.id = source_id;
  return castcore::CastEngine::Instance().StartCasting(device_id, source, opts);
}

int castmirror_get_last_error(char* out_buf, int buf_len) {
  if (!out_buf || buf_len <= 0) return -1;
  std::string error = castcore::CastEngine::Instance().GetLastError();
  int len = static_cast<int>(error.size());
  if (len > buf_len - 1) len = buf_len - 1;
  if (len > 0) std::memcpy(out_buf, error.data(), static_cast<size_t>(len));
  out_buf[len] = '\0';
  return len;
}

void castmirror_stop_cast(void) {
  castcore::CastEngine::Instance().StopCasting();
}

CastMirrorState castmirror_get_state(void) {
  return ConvertState(castcore::CastEngine::Instance().GetState());
}

bool castmirror_get_stats(CastMirrorStreamStats* out_stats) {
  if (!out_stats) return false;
  auto stats = castcore::CastEngine::Instance().GetStats();
  ConvertStats(stats, out_stats);
  return true;
}

void castmirror_set_bitrate(uint32_t bitrate_kbps) {
  castcore::CastEngine::Instance().SetLiveVideoBitrateKbps(bitrate_kbps);
}

void castmirror_set_playout_delay(int delay_ms) {
  castcore::CastEngine::Instance().SetPlayoutDelayMs(delay_ms);
}

void castmirror_set_freeze(bool freeze) {
  castcore::CastEngine::Instance().SetFreezeStream(freeze);
}

void castmirror_set_muted(bool muted) {
  castcore::CastEngine::Instance().SetLiveAudioMuted(muted);
}

void castmirror_set_state_callback(CastMirrorStateCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.state_cb = cb;
  g_c_state.state_user_data = user_data;
}

void castmirror_set_devices_callback(CastMirrorDevicesCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.devices_cb = cb;
  g_c_state.devices_user_data = user_data;
}

void castmirror_set_stats_callback(CastMirrorStatsCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.stats_cb = cb;
  g_c_state.stats_user_data = user_data;
}

void castmirror_set_log_callback(CastMirrorLogCallback cb, void* user_data) {
  std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
  g_c_state.log_cb = cb;
  g_c_state.log_user_data = user_data;
}

int castmirror_get_config_json(char* out_buf, int buf_len) {
  const std::string json = CachedConfigJson();
  if (!out_buf || buf_len <= 0) {
    return JsonOutSizeHint(static_cast<int>(json.size()) + 1);
  }
  return WriteJsonOut(json, out_buf, buf_len);
}

bool castmirror_set_config_json(const char* json) {
  if (!json) return false;
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(json);
  } catch (const std::exception&) {
    return false;
  }
  if (!parsed.is_object()) return false;

  auto& cfg = castcore::ConfigStore::Instance().Mutable();
  MergeConfigJson(parsed, cfg);
  cfg.Validate();
  const bool saved = castcore::ConfigStore::Instance().Save();
  // The cached dump no longer matches the stored config.
  InvalidateConfigJsonCache();
  return saved;
}

int castmirror_self_test(char* out_buf, int buf_len) {
  nlohmann::json result;
  result["capture"] = nlohmann::json::object();
  result["encoder"] = nlohmann::json::object();
  result["audio"] = nlohmann::json::object();
  result["network"] = nlohmann::json::object();

  {
    auto capture = castcore::DisplayCaptureFactory::Create();
    int display_count = 0;
    if (capture) {
      display_count = static_cast<int>(capture->EnumerateDisplays().size());
    }
    bool ok = capture != nullptr && display_count > 0;
    result["capture"]["ok"] = ok;
    result["capture"]["detail"] =
        ok ? (capture->BackendName() + " (" + std::to_string(display_count) + " display(s))")
           : "No display capture backend available";
  }

  {
    auto encoder = castcore::VideoEncoderFactory::Create(castcore::VideoCodec::kH264);
    bool ok = false;
    std::string detail = "No H.264 encoder available";
    if (encoder) {
      castcore::VideoEncoderConfig cfg;
      cfg.width = 1280;
      cfg.height = 720;
      cfg.framerate = 30;
      cfg.bitrate_kbps = 4000;
      ok = encoder->Initialize(cfg);
      detail = ok ? encoder->EncoderName() : "Encoder initialization failed";
    }
    result["encoder"]["ok"] = ok;
    result["encoder"]["detail"] = detail;
  }

  {
    auto audio = castcore::AudioCaptureFactory::Create();
    bool ok = audio != nullptr;
#if defined(_WIN32)
    const char* detail = ok ? "WASAPI loopback" : "Audio capture unavailable";
#else
    const char* detail = ok ? "PulseAudio / PipeWire monitor" : "Audio capture unavailable";
#endif
    result["audio"]["ok"] = ok;
    result["audio"]["detail"] = detail;
  }

  {
    bool ok = castcore::EnsureSocketInit();
    if (ok) {
      int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
      ok = fd >= 0;
      if (fd >= 0) {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
      }
    }
    result["network"]["ok"] = ok;
    result["network"]["detail"] =
        ok ? "UDP sockets available" : "Could not create a UDP socket";
  }

  const std::string json = result.dump();
  if (!out_buf || buf_len <= 0) {
    return JsonOutSizeHint(static_cast<int>(json.size()) + 1);
  }
  return WriteJsonOut(json, out_buf, buf_len);
}

void castmirror_rescan(void) {
  castcore::CastEngine::Instance().GetDiscovery().TriggerScan();
}

void castmirror_set_audio_bitrate(uint32_t bitrate_bps) {
  castcore::CastEngine::Instance().SetLiveAudioBitrateBps(bitrate_bps);
}

void castmirror_set_adaptive_resolution_allowed(bool allow) {
  castcore::CastEngine::Instance().SetAdaptiveResolutionChangeAllowed(allow);
}

}  // extern "C"
