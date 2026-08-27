#ifndef CASTCORE_CAST_SESSION_H_
#define CASTCORE_CAST_SESSION_H_

#include "castcore/types.h"
#include "castcore/state_machine.h"
#include "castcore/cast_channel.h"
#include "castcore/mirroring_negotiator.h"
#include "castcore/frame_crypto.h"
#include "castcore/rtp_packetizer.h"
#include "castcore/cast_transport.h"
#include "castcore/display_capture.h"
#include "castcore/audio_capture.h"
#include "castcore/video_encoder.h"
#include "castcore/audio_encoder.h"
#include "castcore/adaptive_controller.h"
#include "castcore/session_recovery.h"

#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <optional>
#include <chrono>

namespace castcore {

class CastSession {
 public:
  using ErrorCallback = std::function<void(const std::string& error_message)>;
  using DeviceLookupCallback = std::function<std::optional<CastDevice>(const std::string& id, const std::string& ip)>;

  CastSession(StateMachine& state_machine);
  ~CastSession();

  void SetDeviceLookup(DeviceLookupCallback callback);
  bool Start(const CastDevice& device,
             int display_id,
             QualityPreset preset,
             bool enable_audio = true,
             VideoCodec video_codec = VideoCodec::kH264,
             uint32_t bitrate_kbps = 0);

  bool Start(const CastDevice& device, int display_id, const SessionOptions& options);

  void Stop();
  bool IsActive() const;

  StreamStats GetStats() const;
  void SetErrorCallback(ErrorCallback callback);
  void SetLiveVideoBitrateKbps(uint32_t kbps);
  void SetLiveAudioBitrateBps(uint32_t bps);

 private:
  void OnChannelMessage(const std::string& ns, const std::string& payload,
                        const std::string& src_id, const std::string& dest_id);
  void OnChannelStatus(bool is_connected, const std::string& error_msg);

  void HandleReceiverStatus(const std::string& payload);
  void HandleWebrtcMessage(const std::string& payload);

  bool NegotiateControlPlane();
  bool StartStreamingMedia();
  void StopMediaPipeline();
  void RequestReconnect(const std::string& reason);
  void QueueCapturedVideoFrame(CapturedVideoFrame frame);
  void VideoEncodeLoop();
  void ProcessVideoFrame(const CapturedVideoFrame& frame);
  void ProcessAudioFrame(const CapturedAudioFrame& frame);

  void AdaptationLoop();
  void FailSession(const std::string& reason);
  void InjectSilenceAudioFrame();
  void MaybeLogSessionStats();

  DeviceLookupCallback device_lookup_;
  std::atomic<bool> media_stopped_for_reconnect_{false};
  StateMachine& state_machine_;

  CastDevice target_device_;
  int display_id_ = 0;
  SessionOptions options_;
  QualityPreset preset_ = QualityPreset::kAuto;
  bool enable_audio_ = true;
  VideoCodec video_codec_ = VideoCodec::kH264;
  uint32_t bitrate_override_kbps_ = 0;

  std::unique_ptr<CastChannel> cast_channel_;
  std::unique_ptr<CastTransport> transport_;
  std::unique_ptr<IDisplayCapture> display_capture_;
  std::unique_ptr<IAudioCapture> audio_capture_;
  std::unique_ptr<IVideoEncoder> video_encoder_;
  std::unique_ptr<IAudioEncoder> audio_encoder_;
  std::unique_ptr<FrameCrypto> video_crypto_;
  std::unique_ptr<FrameCrypto> audio_crypto_;
  std::unique_ptr<RtpPacketizer> video_packetizer_;
  std::unique_ptr<RtpPacketizer> audio_packetizer_;
  AdaptiveController adaptive_controller_;
  SessionRecovery recovery_;

  StreamEncryptionKeys video_keys_;
  StreamEncryptionKeys audio_keys_;
  NegotiatedSessionParams negotiated_params_;
  StreamStats current_stats_;

  std::string app_session_id_;
  std::string app_transport_id_;
  int launch_request_id_ = 0;
  int offer_seq_num_ = 1001;

  std::atomic<bool> is_streaming_{false};
  std::atomic<bool> stop_requested_{false};

  mutable std::recursive_mutex session_mutex_;
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  bool answer_received_ = false;
  bool launch_received_ = false;

  std::mutex video_queue_mutex_;
  std::condition_variable video_queue_cv_;
  std::optional<CapturedVideoFrame> pending_video_frame_;
  std::thread video_encode_thread_;
  std::mutex video_encoder_mutex_;

  std::thread adapt_thread_;
  ErrorCallback error_callback_;

  std::atomic<int64_t> last_video_send_ms_{0};
  std::atomic<int64_t> last_audio_send_ms_{0};
  std::atomic<bool> fail_requested_{false};
  std::string fail_reason_;
  std::chrono::steady_clock::time_point last_session_log_{};
  std::chrono::steady_clock::time_point last_video_stall_warn_{};
  std::chrono::steady_clock::time_point last_audio_stall_warn_{};
  std::chrono::steady_clock::time_point video_stall_started_{};
  bool video_stalling_ = false;
};

} // namespace castcore

#endif // CASTCORE_CAST_SESSION_H_
