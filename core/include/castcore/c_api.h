#ifndef CASTCORE_C_API_H_
#define CASTCORE_C_API_H_

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
  #if defined(CASTCORE_EXPORTS)
    #define CASTMIRROR_API __declspec(dllexport)
  #else
    #define CASTMIRROR_API __declspec(dllimport)
  #endif
#else
  #define CASTMIRROR_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// State enumeration matching castcore::SessionState
typedef enum {
  CASTMIRROR_STATE_IDLE = 0,
  CASTMIRROR_STATE_CONNECTING = 1,
  CASTMIRROR_STATE_NEGOTIATING = 2,
  CASTMIRROR_STATE_STREAMING = 3,
  CASTMIRROR_STATE_RECONNECTING = 4,
  CASTMIRROR_STATE_STOPPING = 5,
  CASTMIRROR_STATE_FAILED = 6
} CastMirrorState;

// Simple C device structure for P/Invoke marshalling
typedef struct {
  char id[128];
  char name[128];
  char ip_address[64];
  uint16_t port;
  char model_name[128];
} CastMirrorDeviceInfo;

// C statistics structure for P/Invoke marshalling
typedef struct {
  uint32_t bitrate_kbps;
  double current_fps;
  double round_trip_time_ms;
  double packet_loss_fraction;
  int target_delay_ms;
  int width;
  int height;
  uint64_t frames_sent;
  uint64_t packets_sent;
  uint64_t video_queue_overruns;
} CastMirrorStreamStats;

// Callbacks
typedef void (*CastMirrorStateCallback)(CastMirrorState state, const char* message, void* user_data);
typedef void (*CastMirrorDevicesCallback)(int count, void* user_data);
typedef void (*CastMirrorStatsCallback)(const CastMirrorStreamStats* stats, void* user_data);

// Engine lifecycle
CASTMIRROR_API bool castmirror_init(void);
CASTMIRROR_API void castmirror_shutdown(void);

// Device discovery
CASTMIRROR_API void castmirror_start_discovery(void);
CASTMIRROR_API void castmirror_stop_discovery(void);
CASTMIRROR_API int castmirror_get_device_count(void);
CASTMIRROR_API bool castmirror_get_device_info(int index, CastMirrorDeviceInfo* out_info);

// Casting controls
CASTMIRROR_API bool castmirror_start_cast(const char* device_id, int display_id, int target_fps, uint32_t bitrate_kbps);
CASTMIRROR_API void castmirror_stop_cast(void);
CASTMIRROR_API CastMirrorState castmirror_get_state(void);
CASTMIRROR_API bool castmirror_get_stats(CastMirrorStreamStats* out_stats);
CASTMIRROR_API void castmirror_set_bitrate(uint32_t bitrate_kbps);
CASTMIRROR_API void castmirror_set_playout_delay(int delay_ms);
CASTMIRROR_API void castmirror_set_freeze(bool freeze);
CASTMIRROR_API void castmirror_set_muted(bool muted);

// Callback registration
CASTMIRROR_API void castmirror_set_state_callback(CastMirrorStateCallback cb, void* user_data);
CASTMIRROR_API void castmirror_set_devices_callback(CastMirrorDevicesCallback cb, void* user_data);
CASTMIRROR_API void castmirror_set_stats_callback(CastMirrorStatsCallback cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // CASTCORE_C_API_H_
