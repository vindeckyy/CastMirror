#include "castcore/c_api.h"
#include "castcore/cast_engine.h"
#include "castcore/types.h"
#include <cstring>
#include <vector>
#include <mutex>

namespace {

struct CallbackState {
  CastMirrorStateCallback state_cb = nullptr;
  void* state_user_data = nullptr;

  CastMirrorDevicesCallback devices_cb = nullptr;
  void* devices_user_data = nullptr;

  CastMirrorStatsCallback stats_cb = nullptr;
  void* stats_user_data = nullptr;

  std::mutex cb_mutex;
  std::vector<castcore::CastDevice> cached_devices;
} g_c_state;

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
}

}  // namespace

extern "C" {

bool castmirror_init(void) {
  bool ok = castcore::CastEngine::Instance().Initialize();
  if (ok) {
    castcore::CastEngine::Instance().SetOnStateChanged(
        [](castcore::SessionState old_state, castcore::SessionState state, const std::string& message) {
          (void)old_state;
          std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
          if (g_c_state.state_cb) {
            g_c_state.state_cb(ConvertState(state), message.c_str(), g_c_state.state_user_data);
          }
        });

    castcore::CastEngine::Instance().SetOnDevicesChanged(
        [](const std::vector<castcore::CastDevice>& devs) {
          std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
          g_c_state.cached_devices = devs;
          if (g_c_state.devices_cb) {
            g_c_state.devices_cb(static_cast<int>(devs.size()), g_c_state.devices_user_data);
          }
        });

    castcore::CastEngine::Instance().SetOnStatsUpdated(
        [](const castcore::StreamStats& stats) {
          std::lock_guard<std::mutex> lock(g_c_state.cb_mutex);
          if (g_c_state.stats_cb) {
            CastMirrorStreamStats c_stats;
            ConvertStats(stats, &c_stats);
            g_c_state.stats_cb(&c_stats, g_c_state.stats_user_data);
          }
        });
  }
  return ok;
}

void castmirror_shutdown(void) {
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

bool castmirror_start_cast(const char* device_id, int display_id, int target_fps, uint32_t bitrate_kbps) {
  if (!device_id) return false;
  castcore::SessionOptions opts;
  opts.capture_fps = target_fps > 0 ? target_fps : 60;
  opts.video_bitrate_kbps = bitrate_kbps;
  opts.enable_audio = true;
  return castcore::CastEngine::Instance().StartCasting(device_id, display_id, opts);
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

}  // extern "C"
