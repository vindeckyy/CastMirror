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

namespace castcore {

class CastSession {
 public:
  using ErrorCallback = std::function<void(const std::string& error_message)>;

  CastSession(StateMachine& state_machine);
  ~CastSession();

  bool Start(const CastDevice& device,
             int display_id,
             QualityPreset preset,
             bool enable_audio = true,
             VideoCodec video_codec = VideoCodec::kH264);

  void Stop();
  bool IsActive() const;

  StreamStats GetStats() const;
  void SetErrorCallback(ErrorCallback callback);

 private:
  void OnChannelMessage(const std::string& ns, const std::string& payload,
                        const std::string& src_id, const std::string& dest_id);
  void OnChannelStatus(bool is_connected, const std::string& error_msg);

  void HandleReceiverStatus(const std::string& payload);
  void HandleWebrtcMessage(const std::string& payload);

  void StartStreamingMedia();
  void StopStreamingMedia();

  void ProcessVideoFrame(const CapturedVideoFrame& frame);
  void ProcessAudioFrame(const CapturedAudioFrame& frame);

  void AdaptationLoop();

  StateMachine& state_machine_;

  CastDevice target_device_;
  int display_id_ = 0;
  QualityPreset preset_ = QualityPreset::kAuto;
  bool enable_audio_ = true;
  VideoCodec video_codec_ = VideoCodec::kH264;

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

  std::thread adapt_thread_;
  ErrorCallback error_callback_;
};

} // namespace castcore

#endif // CASTCORE_CAST_SESSION_H_
