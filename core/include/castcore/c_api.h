#ifndef CASTCORE_C_API_H_
#define CASTCORE_C_API_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> /* offsetof, for the ABI guards below */

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
  // Appended in v3.
  int recovery_attempt;  // 0 unless the connection dropped and is being retried
  int recovery_elapsed_s;  // seconds spent retrying so far
  char health_hint[160];  // plain-language advice, empty when the stream is healthy
} CastMirrorStreamStats;

// Capture source kinds matching castcore::CaptureSourceKind
typedef enum { CASTMIRROR_SOURCE_MONITOR = 0, CASTMIRROR_SOURCE_WINDOW = 1 } CastMirrorSourceKind;

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

// ---------------------------------------------------------------------------
// ABI guards.
//
// CastMirrorDeviceInfo/CastMirrorStreamStats/CastMirrorDisplayInfo/
// CastMirrorWindowInfo are flattened by hand into managed structs in
// app/winui/Services/CastCoreBridge.cs, which P/Invoke marshals by offset. A
// field inserted in the middle, reordered, or widened therefore corrupts the
// managed side silently, with no error at either end of the boundary. These
// assertions pin the byte layout so any such change fails the build here
// instead. The expected values are the x64 (LP64 and LLP64) layout; every
// member is int/uint16/uint32/uint64/double/char/bool, so both ABIs agree. The
// managed mirrors assert the same numbers via Marshal.SizeOf/Marshal.OffsetOf
// in CastCoreBridge.cs - update both sides in the same commit.
// ---------------------------------------------------------------------------
#if defined(__cplusplus)
#define CASTMIRROR_ABI_ASSERT(cond) static_assert(cond, #cond)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define CASTMIRROR_ABI_ASSERT(cond) _Static_assert(cond, #cond)
#else
#define CASTMIRROR_ABI_ASSERT(cond) /* no compile-time assert before C11 */
#endif

// sizeof == 512, alignof == 8.
CASTMIRROR_ABI_ASSERT(sizeof(CastMirrorStreamStats) == 512);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, bitrate_kbps) == 0);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, current_fps) == 8);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, round_trip_time_ms) == 16);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, packet_loss_fraction) == 24);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, target_delay_ms) == 32);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, width) == 36);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, height) == 40);
// frames_sent is uint64_t: 4 bytes of padding follow the two ints above.
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, frames_sent) == 48);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, packets_sent) == 56);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, video_queue_overruns) == 64);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, current_framerate) == 72);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, adaptive_rung_index) == 76);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, adaptive_rung_count) == 80);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, adaptive_enabled) == 84);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, encoder_name) == 88);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, capture_backend) == 152);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, display_name) == 216);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, recovery_attempt) == 344);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, recovery_elapsed_s) == 348);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorStreamStats, health_hint) == 352);

// sizeof == 450, alignof == 2 (port is the widest scalar before model_name).
CASTMIRROR_ABI_ASSERT(sizeof(CastMirrorDeviceInfo) == 450);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDeviceInfo, id) == 0);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDeviceInfo, name) == 128);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDeviceInfo, ip_address) == 256);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDeviceInfo, port) == 320);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDeviceInfo, model_name) == 322);

// sizeof == 156, alignof == 4 (bool is 1 byte, padded up to the int alignment).
CASTMIRROR_ABI_ASSERT(sizeof(CastMirrorDisplayInfo) == 156);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, id) == 0);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, name) == 4);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, x) == 132);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, y) == 136);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, width) == 140);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, height) == 144);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, refresh_rate) == 148);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorDisplayInfo, is_primary) == 152);

// sizeof == 408, alignof == 4.
CASTMIRROR_ABI_ASSERT(sizeof(CastMirrorWindowInfo) == 408);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, id) == 0);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, title) == 4);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, app_class) == 260);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, x) == 388);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, y) == 392);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, width) == 396);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, height) == 400);
CASTMIRROR_ABI_ASSERT(offsetof(CastMirrorWindowInfo, visible) == 404);

// Callbacks
typedef void (*CastMirrorStateCallback)(CastMirrorState state, const char* message,
                                        void* user_data);
typedef void (*CastMirrorDevicesCallback)(int count, void* user_data);
typedef void (*CastMirrorStatsCallback)(const CastMirrorStreamStats* stats, void* user_data);
// Log callback: level is a castcore::LogLevel ordinal (0 Debug .. 4 Fatal).
typedef void (*CastMirrorLogCallback)(int level, const char* message, void* user_data);

// ---------------------------------------------------------------------------
// Threading and shutdown contract.
//
// Every function in this header is safe to call from any thread; engine state
// is internally serialized. Registered callbacks may run on any engine thread
// and must not block - they execute on the thread that raised the event.
//
// castmirror_init is idempotent: repeated calls return the engine's status
// without re-initializing. castmirror_shutdown is also idempotent. It stops
// the active cast, stops discovery, persists the config, and detaches every
// registered callback, then waits up to ~2 seconds for callbacks already in
// flight. The user's side of the contract: a callback's user_data must outlive
// its last invocation, so free it only after castmirror_shutdown returns (the
// WinUI client frees a GCHandle there). If a client callback blocks past the
// ~2s wait, shutdown logs a warning and user_data may still be in use - keep
// callbacks short and non-blocking.
// ---------------------------------------------------------------------------
// Engine lifecycle
CASTMIRROR_API bool castmirror_init(void);
CASTMIRROR_API void castmirror_shutdown(void);

// ---------------------------------------------------------------------------
// ABI version.
//
// Bumped on every change to a CastMirror* struct's size or layout, or to a
// function signature. The managed client (app/winui/Services/CastCoreBridge.cs)
// declares the version it was compiled against, asserts the same struct sizes
// and offsets, and refuses to call into a DLL that reports a different version:
// a mismatch means P/Invoke marshals into the wrong offsets and neither side
// sees an error at the boundary. The static asserts below cannot catch that
// case - they only prove the native side is self-consistent.
//
// History: 2 - current layouts (stats gained current_framerate,
// adaptive_rung_*, encoder_name, capture_backend, display_name).
// ---------------------------------------------------------------------------
#define CASTMIRROR_ABI_VERSION 3u
CASTMIRROR_API uint32_t castmirror_abi_version(void);

// Apps that currently have an audio session on the default playback device, as a
// JSON array of {"pid":123,"name":"chrome.exe","title":"Google Chrome"}. Same
// two-step buffer contract as castmirror_get_config_json: with no buffer it returns
// the size needed (including the terminator). Empty array off Windows.
CASTMIRROR_API int castmirror_get_audio_apps(char* out_buf, int buf_len);

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
CASTMIRROR_API bool castmirror_start_cast(const char* device_id, int display_id, int target_fps,
                                          uint32_t bitrate_kbps);
// Extended start: source_kind is a CastMirrorSourceKind; preset is a
// CastMirrorQualityPreset; bitrate_kbps 0 means "use the preset default".
CASTMIRROR_API bool castmirror_start_cast_ex(const char* device_id, int source_kind, int source_id,
                                             int target_fps, uint32_t bitrate_kbps, int preset,
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
// Buffer contract (two-step): with out_buf == NULL or buf_len <= 0 the call
// returns a buffer size, including the NUL terminator, that is guaranteed to
// hold the whole document. It is a bound, not the exact length - the engine
// can rewrite the document between the two calls, so a caller that sizes its
// buffer from the first call and passes it to the second always receives the
// complete JSON. With a buffer the call copies up to buf_len - 1 characters,
// always NUL-terminates, and returns the characters written; a caller that
// passes a smaller buffer than the size query reported gets truncated JSON.
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
// Deprecated: the engine never calls this. Poll castmirror_get_stats() about once a
// second while a session is active; that is what the Windows client does.
CASTMIRROR_API void castmirror_set_stats_callback(CastMirrorStatsCallback cb, void* user_data);
// Receives the engine's diagnostic log stream (subject to the logger's minimum
// level). Pass NULL to detach. The callback may run on any engine thread.
CASTMIRROR_API void castmirror_set_log_callback(CastMirrorLogCallback cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // CASTCORE_C_API_H_
