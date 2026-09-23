#ifndef CASTCORE_C_API_H_
#define CASTCORE_C_API_H_

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
  #if defined(CASTCORE_EXPORTS)
    #define CASTMIRROR_API __declspec(dllexport)
  #elif defined(CASTCORE_STATIC)
    #define CASTMIRROR_API
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
  // Appended in v2 (order is ABI: the C# struct mirrors this layout).
  int current_framerate;
  int adaptive_rung_index;
  int adaptive_rung_count;
  int adaptive_enabled;  // bool as int
  char encoder_name[64];
  char capture_backend[64];
  char display_name[128];
} CastMirrorStreamStats;

// Capture source kinds matching castcore::CaptureSourceKind
typedef enum {
  CASTMIRROR_SOURCE_MONITOR = 0,
  CASTMIRROR_SOURCE_WINDOW = 1
} CastMirrorSourceKind;

// Quality presets matching castcore::QualityPreset ordinals
typedef enum {
  CASTMIRROR_PRESET_AUTO = 0,
  CASTMIRROR_PRESET_HIGH = 1,
  CASTMIRROR_PRESET_BALANCED = 2,
  CASTMIRROR_PRESET_SMOOTH = 3,
  CASTMIRROR_PRESET_GAME = 4,
  CASTMIRROR_PRESET_CINEMA = 5
} CastMirrorQualityPreset;

// C display structure for P/Invoke marshalling
typedef struct {
  int id;
  char name[128];
  int x;
  int y;
  int width;
  int height;
  int refresh_rate;
  bool is_primary;
} CastMirrorDisplayInfo;

// C window structure for P/Invoke marshalling
typedef struct {
  int id;
  char title[256];
  char app_class[128];
  int x;
  int y;
  int width;
  int height;
  bool visible;
} CastMirrorWindowInfo;

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

// Capture source enumeration. The count functions refresh the cached list;
// the info functions read from it, so count+info pairs are consistent.
CASTMIRROR_API int castmirror_get_display_count(void);
CASTMIRROR_API bool castmirror_get_display_info(int index, CastMirrorDisplayInfo* out_info);
CASTMIRROR_API int castmirror_get_window_count(void);
CASTMIRROR_API bool castmirror_get_window_info(int index, CastMirrorWindowInfo* out_info);
CASTMIRROR_API bool castmirror_window_capture_supported(void);

// Casting controls
CASTMIRROR_API bool castmirror_start_cast(const char* device_id, int display_id, int target_fps, uint32_t bitrate_kbps);
// Extended start: source_kind is a CastMirrorSourceKind; preset is a
// CastMirrorQualityPreset; bitrate_kbps 0 means "use the preset default".
CASTMIRROR_API bool castmirror_start_cast_ex(const char* device_id,
                                             int source_kind,
                                             int source_id,
                                             int target_fps,
                                             uint32_t bitrate_kbps,
                                             int preset,
                                             bool audio_enabled);
CASTMIRROR_API void castmirror_stop_cast(void);
CASTMIRROR_API CastMirrorState castmirror_get_state(void);
CASTMIRROR_API bool castmirror_get_stats(CastMirrorStreamStats* out_stats);
// Copies the engine's last error (NUL-terminated) into out_buf. Returns the
// number of characters written excluding the terminator, or -1 on bad args.
CASTMIRROR_API int castmirror_get_last_error(char* out_buf, int buf_len);
CASTMIRROR_API void castmirror_set_bitrate(uint32_t bitrate_kbps);
CASTMIRROR_API void castmirror_set_playout_delay(int delay_ms);
CASTMIRROR_API void castmirror_set_freeze(bool freeze);
CASTMIRROR_API void castmirror_set_muted(bool muted);
CASTMIRROR_API void castmirror_set_audio_bitrate(uint32_t bitrate_bps);
CASTMIRROR_API void castmirror_set_adaptive_resolution_allowed(bool allow);

// Configuration access (AppConfig / config.json). The JSON object uses the
// same keys as the config file; get_config_json always emits every supported
// key, set_config_json merges the known keys it is given and persists.
// Buffer contract: with out_buf == NULL or buf_len <= 0 the call returns the
// required buffer size including the NUL terminator; otherwise it copies up to
// buf_len - 1 characters, always NUL-terminates, and returns the characters
// written.
CASTMIRROR_API int castmirror_get_config_json(char* out_buf, int buf_len);
CASTMIRROR_API bool castmirror_set_config_json(const char* json);

// Hardware/network diagnostics as a JSON object with capture/encoder/audio/
// network entries ({"ok":bool,"detail":string}). Same buffer contract as
// castmirror_get_config_json.
CASTMIRROR_API int castmirror_self_test(char* out_buf, int buf_len);

// Requests an immediate mDNS re-query (and subnet probe when enabled).
CASTMIRROR_API void castmirror_rescan(void);

// Callback registration
CASTMIRROR_API void castmirror_set_state_callback(CastMirrorStateCallback cb, void* user_data);
CASTMIRROR_API void castmirror_set_devices_callback(CastMirrorDevicesCallback cb, void* user_data);
CASTMIRROR_API void castmirror_set_stats_callback(CastMirrorStatsCallback cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // CASTCORE_C_API_H_
