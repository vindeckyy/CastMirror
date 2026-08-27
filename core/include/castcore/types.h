#ifndef CASTCORE_TYPES_H_
#define CASTCORE_TYPES_H_

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include <span>
#include <memory>
#include <functional>

namespace castcore {

// Quality presets for user-visible quality control (Auto / High / Balanced / Smooth)
enum class QualityPreset {
  kAuto,
  kHigh,       // Max quality: 1080p60 / 4K30, high bitrate (~8-12 Mbps)
  kBalanced,   // Balanced: 1080p30 / 1080p60, balanced bitrate (~5 Mbps)
  kSmooth      // Smooth: 720p60 / 720p30, lower bitrate (~2.5-3.5 Mbps), low latency
};

inline const char* QualityPresetToString(QualityPreset preset) {
  switch (preset) {
    case QualityPreset::kAuto: return "Auto";
    case QualityPreset::kHigh: return "High";
    case QualityPreset::kBalanced: return "Balanced";
    case QualityPreset::kSmooth: return "Smooth";
  }
  return "Auto";
}

inline QualityPreset QualityPresetFromString(const std::string& str) {
  if (str == "High") return QualityPreset::kHigh;
  if (str == "Balanced") return QualityPreset::kBalanced;
  if (str == "Smooth") return QualityPreset::kSmooth;
  return QualityPreset::kAuto;
}

// Nominal video bitrate for each quality preset (kbps). Used as the slider
// default until the user customizes that profile.
inline uint32_t QualityPresetDefaultBitrateKbps(QualityPreset preset) {
  switch (preset) {
    case QualityPreset::kHigh: return 12000;
    case QualityPreset::kSmooth: return 5000;
    case QualityPreset::kBalanced:
    case QualityPreset::kAuto:
    default: return 8000;
  }
}

// Application and session states
enum class SessionState {
  kIdle,
  kDiscovering,
  kReady,
  kConnecting,
  kNegotiating,
  kStreaming,
  kReconnecting,
  kStopping,
  kFailed
};

inline const char* SessionStateToString(SessionState state) {
  switch (state) {
    case SessionState::kIdle: return "Idle";
    case SessionState::kDiscovering: return "Discovering";
    case SessionState::kReady: return "Ready";
    case SessionState::kConnecting: return "Connecting";
    case SessionState::kNegotiating: return "Negotiating";
    case SessionState::kStreaming: return "Streaming";
    case SessionState::kReconnecting: return "Reconnecting";
    case SessionState::kStopping: return "Stopping";
    case SessionState::kFailed: return "Failed";
  }
  return "Unknown";
}

// Video Codecs
enum class VideoCodec {
  kH264,
  kVP8,
  kVP9,
  kHEVC,
  kAV1
};

inline const char* VideoCodecToString(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264: return "h264";
    case VideoCodec::kVP8: return "vp8";
    case VideoCodec::kVP9: return "vp9";
    case VideoCodec::kHEVC: return "hevc";
    case VideoCodec::kAV1: return "av1";
  }
  return "h264";
}

// Audio Codecs
enum class AudioCodec {
  kOpus,
  kAAC
};

inline const char* AudioCodecToString(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kOpus: return "opus";
    case AudioCodec::kAAC: return "aac";
  }
  return "opus";
}

// Device status
enum class DeviceStatus {
  kReady,
  kBusy,
  kOffline
};

inline const char* DeviceStatusToString(DeviceStatus status) {
  switch (status) {
    case DeviceStatus::kReady: return "Ready";
    case DeviceStatus::kBusy: return "Busy";
    case DeviceStatus::kOffline: return "Offline";
  }
  return "Offline";
}

// Capability flags from mDNS 'ca' bitmask
enum CapabilityFlags : uint32_t {
  kCapVideoOut = 1 << 0,
  kCapVideoIn = 1 << 1,
  kCapAudioOut = 1 << 2,
  kCapAudioIn = 1 << 3,
  kCapMultizoneGroup = 1 << 5
};

// Information about a discovered Cast Device
struct CastDevice {
  std::string id;             // UUID from 'id' TXT record
  std::string name;           // Friendly name from 'fn' TXT record
  std::string model_name;     // Hardware model from 'md' TXT record (e.g. "Chromecast", "Chromecast Ultra")
  std::string ip_address;     // IPv4 address
  uint16_t port = 8009;       // Port (default 8009)
  DeviceStatus status = DeviceStatus::kReady;
  uint32_t capabilities = 0;  // Bitmask from 'ca'
  std::chrono::steady_clock::time_point last_seen;

  bool HasVideoOut() const {
    return (capabilities & kCapVideoOut) != 0 || capabilities == 0;
  }
  bool HasAudioOut() const {
    return (capabilities & kCapAudioOut) != 0 || capabilities == 0;
  }
};

// Display / Monitor Information
struct DisplayInfo {
  int id = 0;
  std::string name;
  int x = 0;
  int y = 0;
  int width = 1920;
  int height = 1080;
  int refresh_rate = 60;
  bool is_primary = false;
};

// Frame Resolution
struct Resolution {
  int width = 1920;
  int height = 1080;

  bool operator==(const Resolution& o) const {
    return width == o.width && height == o.height;
  }
  bool operator!=(const Resolution& o) const {
    return !(*this == o);
  }
};

// Frame Dependency
enum class FrameDependency {
  kKeyFrame,
  kDependent
};

// Encoded Frame Metadata and Buffer
struct EncodedFrame {
  FrameDependency dependency = FrameDependency::kDependent;
  uint32_t frame_id = 0;
  uint32_t referenced_frame_id = 0;
  uint32_t rtp_timestamp = 0;
  std::chrono::steady_clock::time_point capture_time;
  std::chrono::milliseconds playout_delay{200};
  std::vector<uint8_t> data;
};

// Raw Captured Video Frame (BGRA / RGBA)
struct CapturedVideoFrame {
  int width = 0;
  int height = 0;
  int stride = 0;
  std::chrono::steady_clock::time_point timestamp;
  std::vector<uint8_t> data;
};

// Raw Audio Frame (PCM 48kHz Stereo S16LE)
struct CapturedAudioFrame {
  int sample_rate = 48000;
  int channels = 2;
  int samples_per_channel = 480; // 10ms frame
  std::chrono::steady_clock::time_point timestamp;
  std::vector<uint8_t> pcm_data;
};

// Live Streaming Performance Statistics
struct StreamStats {
  double current_fps = 0.0;
  uint32_t bitrate_kbps = 0;
  double round_trip_time_ms = 0.0;
  double packet_loss_fraction = 0.0;
  uint32_t packets_sent = 0;
  uint32_t frames_sent = 0;
  uint32_t nacks_received = 0;
  uint32_t pli_received = 0;
  int target_delay_ms = 200;
  Resolution current_resolution{1920, 1080};
  int current_framerate = 60;
  std::string active_codec = "h264";

  std::string encoder_name;
  std::string capture_backend;
  std::string display_name;
  std::string device_name;
  std::string device_ip;
  int adaptive_rung_index = 0;
  int adaptive_rung_count = 0;
  bool adaptive_enabled = true;
  int recovery_attempt = 0;
  int recovery_elapsed_s = 0;
  std::string health_hint;
};

// Options for one mirroring session (GUI / engine / session).
struct SessionOptions {
  QualityPreset preset = QualityPreset::kAuto;
  bool enable_audio = true;
  VideoCodec video_codec = VideoCodec::kH264;
  uint32_t video_bitrate_kbps = 0;      // 0 = preset default
  uint32_t audio_bitrate_bps = 192000;
  int capture_fps = 0;                  // 0 = follow display refresh
  int target_delay_ms = 200;
  bool silence_host_speakers = true;
  bool adaptive_enabled = true;
};

// Standard Cast App IDs
inline constexpr const char* kMirroringAudioVideoAppId = "0F5096E8";
inline constexpr const char* kMirroringAudioOnlyAppId = "85CDB22F";

// Cast Namespaces
inline constexpr const char* kNamespaceConnection = "urn:x-cast:com.google.cast.tp.connection";
inline constexpr const char* kNamespaceHeartbeat = "urn:x-cast:com.google.cast.tp.heartbeat";
inline constexpr const char* kNamespaceReceiver = "urn:x-cast:com.google.cast.receiver";
inline constexpr const char* kNamespaceWebrtc = "urn:x-cast:com.google.cast.webrtc";
inline constexpr const char* kNamespaceDeviceAuth = "urn:x-cast:com.google.cast.tp.deviceauth";
inline constexpr const char* kNamespaceMedia = "urn:x-cast:com.google.cast.media";

// Standard Sender / Receiver IDs
inline constexpr const char* kPlatformSenderId = "sender-0";
inline constexpr const char* kPlatformReceiverId = "receiver-0";
inline constexpr const char* kBroadcastId = "*";

} // namespace castcore

#endif // CASTCORE_TYPES_H_
