#include "castcore/cast_session.h"
#include "castcore/capability_model.h"
#include "castcore/logger.h"
#include "castcore/thread_util.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <vector>

namespace castcore {

namespace {

int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

CastSession::CastSession(StateMachine& state_machine)
    : state_machine_(state_machine),
      recovery_(30) {}

CastSession::~CastSession() {
  Stop();
}

bool CastSession::IsActive() const {
  return state_machine_.IsActive();
}

void CastSession::SetDeviceLookup(DeviceLookupCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  device_lookup_ = std::move(callback);
}

void CastSession::SetErrorCallback(ErrorCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  error_callback_ = std::move(callback);
}

bool CastSession::Start(const CastDevice& device,
                       int display_id,
                       QualityPreset preset,
                       bool enable_audio,
                       VideoCodec video_codec,
                       uint32_t bitrate_kbps) {
  SessionOptions options;
  options.preset = preset;
  options.enable_audio = enable_audio;
  options.video_codec = video_codec;
  options.video_bitrate_kbps = bitrate_kbps;
  return Start(device, display_id, options);
}

bool CastSession::Start(const CastDevice& device, int display_id, const SessionOptions& options) {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);

  if (state_machine_.IsActive()) {
    LOG_WARN << "CastSession::Start called while session already active";
    return false;
  }

  target_device_ = device;
  display_id_ = display_id;
  options_ = options;
  preset_ = options.preset;
  enable_audio_ = options.enable_audio;
  video_codec_ = options.video_codec;
  bitrate_override_kbps_ = options.video_bitrate_kbps;
  stop_requested_ = false;
  fail_requested_ = false;
  fail_reason_.clear();
  is_streaming_ = false;
  video_stalling_ = false;
  last_video_send_ms_ = 0;
  last_audio_send_ms_ = 0;
  {
    std::lock_guard<std::mutex> qlock(video_queue_mutex_);
    pending_video_frame_.reset();
  }
  answer_received_ = false;
  launch_received_ = false;

  state_machine_.TransitionTo(SessionState::kConnecting, "Connecting to " + device.name);

  display_capture_ = DisplayCaptureFactory::Create();
  int disp_w = 1920, disp_h = 1080, disp_fps = 60;

  if (!display_capture_->SizeKnownBeforeStart()) {
    // Wayland portal: start capture before OFFER to discover negotiated stream size.
    std::mutex wm_mutex;
    std::condition_variable wm_cv;
    bool got_first_frame = false;
    CapturedVideoFrame first_vf;

    display_capture_->SetFrameCallback([&](const CapturedVideoFrame& vf) {
      std::lock_guard<std::mutex> lk(wm_mutex);
      if (!got_first_frame) {
        first_vf = vf;
        got_first_frame = true;
        wm_cv.notify_all();
      }
      QueueCapturedVideoFrame(vf);
    });

    if (!display_capture_->Start(display_id, 60)) {
      state_machine_.TransitionTo(SessionState::kFailed, "Screen share permission denied");
      Stop();
      return false;
    }

    std::unique_lock<std::mutex> wlk(wm_mutex);
    if (!wm_cv.wait_for(wlk, std::chrono::seconds(60), [&] { return got_first_frame || stop_requested_.load(); })) {
      state_machine_.TransitionTo(SessionState::kFailed, "Screen share permission denied or timed out");
      Stop();
      return false;
    }

    if (stop_requested_ || !got_first_frame) {
      Stop();
      return false;
    }

    disp_w = first_vf.width;
    disp_h = first_vf.height;
    disp_fps = 60;
  } else {
    auto displays = display_capture_->EnumerateDisplays();
    if (!displays.empty()) {
      for (const auto& d : displays) {
        if (d.id == display_id) {
          disp_w = d.width;
          disp_h = d.height;
          disp_fps = d.refresh_rate;
          break;
        }
      }
    }
  }

  current_stats_ = CapabilityModel::GetRecommendedSettings(
      device, preset_, disp_w, disp_h, disp_fps, options_.capture_fps);
  if (options_.target_delay_ms > 0) {
    current_stats_.target_delay_ms = options_.target_delay_ms;
  }

  uint32_t bitrate_cap_kbps = current_stats_.bitrate_kbps;
  if (bitrate_override_kbps_ > 0) {
    const auto caps = CapabilityModel::Evaluate(device);
    bitrate_cap_kbps = std::min(bitrate_override_kbps_, caps.max_bitrate_kbps);
    bitrate_override_kbps_ = bitrate_cap_kbps;
    if (options_.adaptive_enabled) {
      // The UI value is a ceiling, not an instruction to jump straight to
      // 25 Mbps. Start at the preset/device recommendation and adapt below it.
      current_stats_.bitrate_kbps = std::min(current_stats_.bitrate_kbps, bitrate_cap_kbps);
    } else {
      current_stats_.bitrate_kbps = bitrate_cap_kbps;
    }
    LOG_INFO << "Using video bitrate cap " << bitrate_cap_kbps << " kbps for "
             << QualityPresetToString(preset_) << "; starting at "
             << current_stats_.bitrate_kbps << " kbps";
  }
  adaptive_controller_.Initialize(current_stats_, preset_);
  adaptive_controller_.SetEnabled(options_.adaptive_enabled);
  adaptive_controller_.SetBitrateCapKbps(bitrate_cap_kbps);


  if (!NegotiateControlPlane()) {
    state_machine_.TransitionTo(SessionState::kFailed, "Initial connection failed to " + device.name);
    Stop();
    return false;
  }

  if (!StartStreamingMedia()) {
    state_machine_.TransitionTo(SessionState::kFailed, "Failed to start the local capture and encoding pipeline");
    Stop();
    return false;
  }

  last_video_send_ms_.store(SteadyNowMs());
  last_audio_send_ms_.store(SteadyNowMs());
  last_session_log_ = std::chrono::steady_clock::now();
  state_machine_.TransitionTo(SessionState::kStreaming, "Your display is on " + device.name);
  return true;
}

bool CastSession::NegotiateControlPlane() {
  video_keys_ = MirroringNegotiator::GenerateRandomKeys();
  audio_keys_ = enable_audio_ ? MirroringNegotiator::GenerateRandomKeys() : StreamEncryptionKeys{};

  cast_channel_ = std::make_unique<CastChannel>();
  cast_channel_->SetMessageCallback([this](const std::string& ns, const std::string& payload,
                                          const std::string& src, const std::string& dest) {
    OnChannelMessage(ns, payload, src, dest);
  });
  cast_channel_->SetStatusCallback([this](bool connected, const std::string& err) {
    OnChannelStatus(connected, err);
  });

  const bool reconnecting = state_machine_.GetState() == SessionState::kReconnecting;
  state_machine_.TransitionTo(reconnecting ? SessionState::kReconnecting : SessionState::kConnecting,
      "Opening a secure Cast channel to " + target_device_.name + " (" + target_device_.ip_address + ":" + std::to_string(target_device_.port) + ")");

  if (!cast_channel_->Connect(target_device_.ip_address, target_device_.port)) {
    return false;
  }

  state_machine_.TransitionTo(reconnecting ? SessionState::kReconnecting : SessionState::kConnecting,
      "Launching the TV's built-in mirroring app on " + target_device_.name);


  const char* app_id = (enable_audio_ && !target_device_.HasVideoOut())
      ? kMirroringAudioOnlyAppId : kMirroringAudioVideoAppId;

  launch_received_ = false;
  answer_received_ = false;
  launch_request_id_ = cast_channel_->LaunchApp(app_id);

  {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(8), [this] {
          return launch_received_ || stop_requested_.load() || fail_requested_.load();
        })) {
      LOG_ERROR << "Timed out waiting for RECEIVER_STATUS from " << target_device_.name;
      return false;
    }
  }

  if (stop_requested_ || fail_requested_) {
    return false;
  }

  state_machine_.TransitionTo(reconnecting ? SessionState::kReconnecting : SessionState::kNegotiating,
      "Agreeing picture size, codec, and encryption with the TV");


  cast_channel_->SetAppTransportId(app_transport_id_);
  cast_channel_->ConnectVirtual(app_transport_id_, "streaming_sender");

  offer_seq_num_ = 1001;
  int audio_bps = options_.audio_bitrate_bps > 0 ? static_cast<int>(options_.audio_bitrate_bps) : 192000;
  std::string offer_json = MirroringNegotiator::CreateOfferJson(
      offer_seq_num_, current_stats_, enable_audio_, video_keys_, audio_keys_, video_codec_,
      current_stats_.target_delay_ms, audio_bps);

  LOG_INFO << "Sending OFFER to Mirroring App (transportId: " << app_transport_id_ << ")...";
  cast_channel_->SendCastMessage(kNamespaceWebrtc, offer_json, app_transport_id_, "streaming_sender");

  {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(5), [this] {
          return answer_received_ || stop_requested_.load() || fail_requested_.load();
        })) {
      LOG_ERROR << "Timed out waiting for ANSWER from " << target_device_.name;
      return false;
    }
  }

  return !stop_requested_ && !fail_requested_;
}

bool CastSession::StartStreamingMedia() {
  LOG_INFO << "Starting live media capture and encoding pipeline...";
  adaptive_controller_.ResetFeedbackWindow();

  video_crypto_ = std::make_unique<FrameCrypto>(video_keys_.aes_key, video_keys_.aes_iv_mask);
  if (enable_audio_) {
    audio_crypto_ = std::make_unique<FrameCrypto>(audio_keys_.aes_key, audio_keys_.aes_iv_mask);
  }

  video_packetizer_ = std::make_unique<RtpPacketizer>(
      negotiated_params_.video_stream.rtp_payload_type,
      negotiated_params_.video_stream.sender_ssrc);

  if (enable_audio_) {
    audio_packetizer_ = std::make_unique<RtpPacketizer>(
        negotiated_params_.audio_stream.rtp_payload_type,
        negotiated_params_.audio_stream.sender_ssrc);
  }

  transport_ = std::make_unique<CastTransport>();
  transport_->SetPliCallback([this] {
    std::lock_guard<std::mutex> elock(video_encoder_mutex_);
    if (video_encoder_) {
      video_encoder_->ForceKeyFrame();
    }
  });
  transport_->SetFeedbackCallback([this](const RtcpFeedback& fb) {
    const uint32_t video_ssrc = negotiated_params_.video_stream.sender_ssrc;
    if (fb.sender_ssrc == 0 || fb.sender_ssrc == video_ssrc) {
      adaptive_controller_.OnFeedback(fb);
    }
  });

  if (!transport_->Start(target_device_.ip_address, negotiated_params_.receiver_udp_port)) {
    LOG_ERROR << "Failed to start media transport UDP socket";
    StopMediaPipeline();
    return false;
  }

  VideoEncoderConfig venc_cfg;
  venc_cfg.width = current_stats_.current_resolution.width;
  venc_cfg.height = current_stats_.current_resolution.height;
  venc_cfg.framerate = current_stats_.current_framerate;
  venc_cfg.bitrate_kbps = current_stats_.bitrate_kbps;
  venc_cfg.codec = video_codec_;
  venc_cfg.playout_delay_ms = current_stats_.target_delay_ms > 0
                                 ? current_stats_.target_delay_ms : 200;

  video_encoder_ = VideoEncoderFactory::Create(video_codec_);
  if (!video_encoder_ || !video_encoder_->Initialize(venc_cfg)) {
    LOG_ERROR << "Failed to initialize video encoder";
    StopMediaPipeline();
    return false;
  }
  LOG_INFO << "Video encoder: " << video_encoder_->EncoderName();

  if (enable_audio_) {
    AudioEncoderConfig aenc_cfg;
    aenc_cfg.sample_rate = 48000;
    aenc_cfg.channels = 2;
    aenc_cfg.bitrate_bps = options_.audio_bitrate_bps > 0 ? static_cast<int>(options_.audio_bitrate_bps) : 192000;
    aenc_cfg.codec = AudioCodec::kOpus;
    aenc_cfg.playout_delay_ms = current_stats_.target_delay_ms > 0
                                   ? current_stats_.target_delay_ms : 200;

    audio_encoder_ = AudioEncoderFactory::Create(AudioCodec::kOpus);
    if (!audio_encoder_ || !audio_encoder_->Initialize(aenc_cfg)) {
      LOG_WARN << "Audio encoder initialization failed; continuing without captured audio";
      audio_encoder_.reset();
      audio_crypto_.reset();
      audio_packetizer_.reset();
    } else {
      audio_capture_ = AudioCaptureFactory::Create();
      audio_capture_->SetHostSilence(options_.silence_host_speakers);
      audio_capture_->SetAudioCallback([this](const CapturedAudioFrame& af) {
        ProcessAudioFrame(af);
      });
    }
  }

  if (!display_capture_) {
    display_capture_ = DisplayCaptureFactory::Create();
  }
  if (!display_capture_) {
    LOG_ERROR << "Failed to create display capture backend";
    StopMediaPipeline();
    return false;
  }

  // The old pipeline never started this worker. Capture callbacks only filled
  // pending_video_frame_, so the receiver got audio RTP and a permanently
  // black video surface.
  is_streaming_ = true;
  video_encode_thread_ = std::thread(&CastSession::VideoEncodeLoop, this);

  if (enable_audio_ && audio_capture_) {
    if (!audio_capture_->Start(48000, 2)) {
      LOG_WARN << "PulseAudio capture failed, falling back to synthetic audio";
      audio_capture_ = AudioCaptureFactory::CreateSynthetic();
      audio_capture_->SetAudioCallback([this](const CapturedAudioFrame& af) {
        ProcessAudioFrame(af);
      });
      audio_capture_->Start(48000, 2);
    }
  }

  // Always replace the callback. The Wayland pre-OFFER callback captures local
  // size-probe state by reference and must not survive beyond Start().
  display_capture_->SetFrameCallback([this](const CapturedVideoFrame& vf) {
    QueueCapturedVideoFrame(vf);
  });
  if (!display_capture_->IsCapturing() &&
      !display_capture_->Start(display_id_, current_stats_.current_framerate)) {
    LOG_WARN << "Display capture backend failed; falling back to synthetic capture";
    display_capture_ = DisplayCaptureFactory::CreateSynthetic(
        current_stats_.current_resolution.width,
        current_stats_.current_resolution.height);
    display_capture_->SetFrameCallback([this](const CapturedVideoFrame& vf) {
      QueueCapturedVideoFrame(vf);
    });
    if (!display_capture_->Start(display_id_, current_stats_.current_framerate)) {
      LOG_ERROR << "Failed to start display capture";
      StopMediaPipeline();
      return false;
    }
  }

  // On reconnect this method runs inside the existing adaptation thread.
  // Assigning over a joinable std::thread would call std::terminate.
  if (!adapt_thread_.joinable()) {
    adapt_thread_ = std::thread(&CastSession::AdaptationLoop, this);
  }
  return true;
}


void CastSession::QueueCapturedVideoFrame(CapturedVideoFrame frame) {
  if (!is_streaming_.load() || stop_requested_.load()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(video_queue_mutex_);
    pending_video_frame_ = std::move(frame);
  }
  video_queue_cv_.notify_one();
}

void CastSession::VideoEncodeLoop() {
  while (is_streaming_.load() && !stop_requested_.load()) {
    CapturedVideoFrame frame;
    {
      std::unique_lock<std::mutex> lock(video_queue_mutex_);
      video_queue_cv_.wait(lock, [this] {
        return pending_video_frame_.has_value() ||
               !is_streaming_.load() || stop_requested_.load();
      });
      if (!is_streaming_.load() || stop_requested_.load()) {
        break;
      }
      frame = std::move(*pending_video_frame_);
      pending_video_frame_.reset();
    }
    ProcessVideoFrame(frame);
  }
}

void CastSession::ProcessVideoFrame(const CapturedVideoFrame& vf) {
  if (!is_streaming_.load() || !video_crypto_ || !video_packetizer_ || !transport_) {
    return;
  }

  EncodedFrame raw_frame;
  {
    std::lock_guard<std::mutex> elock(video_encoder_mutex_);
    if (!video_encoder_ || !video_encoder_->Encode(vf, raw_frame)) {
      return;
    }
  }

  std::vector<uint8_t> encrypted_payload = video_crypto_->Encrypt(raw_frame.frame_id, raw_frame.data);
  raw_frame.data = std::move(encrypted_payload);

  auto packets = video_packetizer_->PacketizeFrame(raw_frame);
  if (transport_->SendPackets(packets)) {
    last_video_send_ms_.store(SteadyNowMs());
  }
}

void CastSession::ProcessAudioFrame(const CapturedAudioFrame& af) {
  if (!is_streaming_.load() || !audio_encoder_ || !audio_crypto_ || !audio_packetizer_ || !transport_) {
    return;
  }

  EncodedFrame raw_frame;
  if (!audio_encoder_->Encode(af, raw_frame)) {
    return;
  }

  std::vector<uint8_t> encrypted_payload = audio_crypto_->Encrypt(raw_frame.frame_id, raw_frame.data);
  raw_frame.data = std::move(encrypted_payload);

  auto packets = audio_packetizer_->PacketizeFrame(raw_frame);
  if (transport_->SendPackets(packets)) {
    last_audio_send_ms_.store(SteadyNowMs());
  }
}

void CastSession::InjectSilenceAudioFrame() {
  CapturedAudioFrame af;
  af.sample_rate = 48000;
  af.channels = 2;
  af.samples_per_channel = 480;
  af.timestamp = std::chrono::steady_clock::now();
  af.pcm_data.assign(static_cast<size_t>(af.samples_per_channel * af.channels * 2), 0);
  ProcessAudioFrame(af);
}

void CastSession::MaybeLogSessionStats() {
  auto now = std::chrono::steady_clock::now();
  if (last_session_log_.time_since_epoch().count() != 0 &&
      now - last_session_log_ < std::chrono::seconds(10)) {
    return;
  }
  last_session_log_ = now;
  StreamStats s = GetStats();
  LOG_INFO << "Session stats: frames=" << s.frames_sent
           << " nack=" << s.nacks_received
           << " pli=" << s.pli_received
           << " loss=" << (s.packet_loss_fraction * 100.0) << "%"
           << " bitrate=" << s.bitrate_kbps << "kbps"
           << " fps=" << s.current_fps
           << " audio_ok=" << (enable_audio_ ? "yes" : "off");
}

void CastSession::AdaptationLoop() {
  while (!stop_requested_.load()) {
    if (!is_streaming_.load() && !recovery_.IsRecovering()) {
      break;
    }

    if (recovery_.IsRecovering() || state_machine_.GetState() == SessionState::kReconnecting) {
      if (!media_stopped_for_reconnect_.exchange(true)) {
        StopMediaPipeline();
        if (cast_channel_) {
          if (cast_channel_->IsConnected() && !app_session_id_.empty()) {
            cast_channel_->StopApp(app_session_id_);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          cast_channel_->Disconnect();
        }
      }

      if (recovery_.HasTimedOut()) {
        FailSession("Reconnect timed out — tap Cast to retry");
        return;
      }

      recovery_.IncrementAttempt();
      if (device_lookup_) {
        auto d = device_lookup_(target_device_.id, target_device_.ip_address);
        if (d.has_value()) {
          target_device_ = *d;
        }
      }

      int sleep_sec = std::min(8, 1 << std::max(0, recovery_.GetAttemptCount() - 1));
      state_machine_.TransitionTo(SessionState::kReconnecting,
          "Wi-Fi glitch — retry " + std::to_string(recovery_.GetAttemptCount()) + " to " + target_device_.name + " in " + std::to_string(sleep_sec) + "s");
      for (int i = 0; i < sleep_sec * 20; ++i) {
        if (stop_requested_.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_requested_.load()) return;

      LOG_INFO << "Attempting session recovery (attempt #" << recovery_.GetAttemptCount()
               << ") to " << target_device_.name << " at " << target_device_.ip_address << "...";

      fail_requested_ = false;
      if (NegotiateControlPlane() && StartStreamingMedia()) {
        recovery_.Reset();
        media_stopped_for_reconnect_ = false;
        last_video_send_ms_.store(SteadyNowMs());
        last_audio_send_ms_.store(SteadyNowMs());
        last_session_log_ = std::chrono::steady_clock::now();
        state_machine_.TransitionTo(SessionState::kStreaming, "Your display is on " + target_device_.name);
      } else {
        StopMediaPipeline();
        if (cast_channel_) {
          cast_channel_->Disconnect();
        }
      }

      continue;
    }

    for (int i = 0; i < 10; ++i) {
      if (!is_streaming_.load() || stop_requested_.load() || fail_requested_.load() || recovery_.IsRecovering()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (stop_requested_.load()) break;
    if (recovery_.IsRecovering()) continue;

    if (fail_requested_.load()) {
      RequestReconnect(fail_reason_.empty() ? "Connection lost" : fail_reason_);
      continue;
    }
    if (cast_channel_ && (!cast_channel_->IsConnected() || cast_channel_->HeartbeatTimedOut())) {
      RequestReconnect("Cast channel lost — reconnecting");
      continue;
    }

    int64_t now_ms = SteadyNowMs();
    int64_t last_v = last_video_send_ms_.load();
    if (last_v > 0 && (now_ms - last_v) > 400) {
      auto now = std::chrono::steady_clock::now();
      if (last_video_stall_warn_.time_since_epoch().count() == 0 ||
          now - last_video_stall_warn_ >= std::chrono::seconds(2)) {
        LOG_WARN << "Video stall: no frame sent for " << (now_ms - last_v) << "ms; forcing keyframe";
        last_video_stall_warn_ = now;
      }
      if (!video_stalling_) {
        video_stalling_ = true;
        video_stall_started_ = now;
      } else if (now - video_stall_started_ >= std::chrono::seconds(5)) {
        RequestReconnect("Video stalled — reconnecting");
        continue;
      }
      std::lock_guard<std::mutex> elock(video_encoder_mutex_);
      if (video_encoder_) {
        video_encoder_->ForceKeyFrame();
      }
    } else {
      video_stalling_ = false;
    }

    if (enable_audio_) {
      int64_t last_a = last_audio_send_ms_.load();
      if (last_a > 0 && (now_ms - last_a) > 200) {
        auto now = std::chrono::steady_clock::now();
        if (last_audio_stall_warn_.time_since_epoch().count() == 0 ||
            now - last_audio_stall_warn_ >= std::chrono::seconds(2)) {
          LOG_WARN << "Audio stall: injecting silence keepalive (" << (now_ms - last_a) << "ms)";
          last_audio_stall_warn_ = now;
        }
        InjectSilenceAudioFrame();
      }
    }

    MaybeLogSessionStats();

    StreamStats updated;
    std::lock_guard<std::recursive_mutex> session_lock(session_mutex_);
    if (adaptive_controller_.CheckAdaptation(updated)) {
      current_stats_.bitrate_kbps = updated.bitrate_kbps;
      current_stats_.target_delay_ms = updated.target_delay_ms;
      current_stats_.current_resolution = updated.current_resolution;
      current_stats_.current_framerate = updated.current_framerate;

      bool reconfigure_failed = false;
      {
        std::lock_guard<std::mutex> elock(video_encoder_mutex_);
        if (video_encoder_) {
          const auto& enc_cfg = video_encoder_->GetConfig();
          const bool config_changed =
              enc_cfg.width != updated.current_resolution.width ||
              enc_cfg.height != updated.current_resolution.height ||
              enc_cfg.framerate != updated.current_framerate ||
              enc_cfg.bitrate_kbps != updated.bitrate_kbps;
          if (config_changed) {
            VideoEncoderConfig new_cfg = enc_cfg;
            new_cfg.width = updated.current_resolution.width;
            new_cfg.height = updated.current_resolution.height;
            new_cfg.framerate = updated.current_framerate;
            new_cfg.bitrate_kbps = updated.bitrate_kbps;
            new_cfg.gop_size = updated.current_framerate;
            reconfigure_failed = !video_encoder_->Reconfigure(new_cfg);
            if (!reconfigure_failed) {
              // A clean IDR prevents decoder artifacts after any VAAPI/x264
              // rate-control change, including bitrate-only downshifts.
              video_encoder_->ForceKeyFrame();
            }
          }
        }
      }

      if (reconfigure_failed) {
        RequestReconnect("Encoder reconfigure failed");
        continue;
      }
      if (display_capture_) {
        display_capture_->SetTargetFps(updated.current_framerate);
      }

      LOG_INFO << "Adaptive encode -> " << updated.current_resolution.width << "x"
               << updated.current_resolution.height << " @"
               << updated.current_framerate << "fps, "
               << updated.bitrate_kbps << " kbps";
    }
  }
}

void CastSession::RequestReconnect(const std::string& reason) {
  if (stop_requested_.load()) return;

  if (recovery_.IsRecovering()) {
    if (recovery_.HasTimedOut()) {
      FailSession(reason);
    }
    return;
  }

  recovery_.StartRecovery(reason);
  media_stopped_for_reconnect_ = false;
  fail_requested_ = false;
  state_machine_.TransitionTo(SessionState::kReconnecting, "Reconnecting to " + target_device_.name);
  std::lock_guard<std::mutex> lk(cv_mutex_);
  cv_.notify_all();
}

void CastSession::StopMediaPipeline() {
  is_streaming_ = false;
  {
    std::lock_guard<std::mutex> qlock(video_queue_mutex_);
    pending_video_frame_.reset();
  }
  video_queue_cv_.notify_all();

  if (display_capture_) {
    display_capture_->Stop();
  }
  JoinOrDetach(video_encode_thread_, 500, "video encode");
  if (audio_capture_) {
    audio_capture_->Stop();
  }
  if (transport_) {
    transport_->Stop();
  }

  std::lock_guard<std::mutex> elock(video_encoder_mutex_);
  video_encoder_.reset();
  audio_encoder_.reset();
  video_crypto_.reset();
  audio_crypto_.reset();
  video_packetizer_.reset();
  audio_packetizer_.reset();
  transport_.reset();
  // Keep the stopped capture backend for reconnect. StartStreamingMedia()
  // replaces its callback and restarts it; the session destructor releases it.
  audio_capture_.reset();
}

void CastSession::FailSession(const std::string& reason) {
  if (stop_requested_.load()) {
    return;
  }
  fail_reason_ = reason;
  fail_requested_ = true;
  LOG_ERROR << reason;
  ErrorCallback cb;
  {
    std::lock_guard<std::recursive_mutex> lock(session_mutex_);
    cb = error_callback_;
  }
  if (cb) {
    cb(reason);
  }
  Stop();
}

void CastSession::SetLiveVideoBitrateKbps(uint32_t kbps) {
  kbps = std::max<uint32_t>(1000, kbps);
  const auto caps = CapabilityModel::Evaluate(target_device_);
  kbps = std::min(kbps, caps.max_bitrate_kbps);

  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  bitrate_override_kbps_ = kbps;
  options_.video_bitrate_kbps = kbps;
  adaptive_controller_.SetBitrateCapKbps(kbps);

  const uint32_t target_kbps = options_.adaptive_enabled
      ? adaptive_controller_.GetCurrentBitrateKbps()
      : kbps;
  if (target_kbps == current_stats_.bitrate_kbps) {
    LOG_INFO << "Video bitrate cap updated to " << kbps
             << " kbps; encoder remains at adaptive target " << target_kbps << " kbps";
    return;
  }

  bool reconfigure_failed = false;
  {
    std::lock_guard<std::mutex> elock(video_encoder_mutex_);
    if (video_encoder_) {
      VideoEncoderConfig new_cfg = video_encoder_->GetConfig();
      new_cfg.bitrate_kbps = target_kbps;
      reconfigure_failed = !video_encoder_->Reconfigure(new_cfg);
      if (!reconfigure_failed) {
        video_encoder_->ForceKeyFrame();
      }
    }
  }
  if (reconfigure_failed) {
    RequestReconnect("Encoder bitrate reconfigure failed");
    return;
  }
  current_stats_.bitrate_kbps = target_kbps;
  LOG_INFO << "Video bitrate cap updated to " << kbps
           << " kbps; encoder target is " << target_kbps << " kbps";
}

void CastSession::SetLiveAudioBitrateBps(uint32_t bps) {
  if (bps == 0) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  options_.audio_bitrate_bps = bps;
  if (audio_encoder_) {
    audio_encoder_->SetBitrate(static_cast<int>(bps));
  }
}

void CastSession::Stop() {
  bool expected = false;
  if (!stop_requested_.compare_exchange_strong(expected, true)) {
    return;
  }
  LOG_INFO << "Stopping Cast Session (hard 500ms budget)...";

  {
    std::lock_guard<std::mutex> lk(cv_mutex_);
    cv_.notify_all();
  }

  StopMediaPipeline();

  JoinOrDetach(adapt_thread_, 500, "adaptation");

  if (cast_channel_ && cast_channel_->IsConnected() && !app_session_id_.empty()) {
    cast_channel_->StopApp(app_session_id_);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cast_channel_->Disconnect();
  } else if (cast_channel_) {
    cast_channel_->Disconnect();
  }

  recovery_.Reset();

  if (fail_requested_.load()) {
    state_machine_.TransitionTo(SessionState::kFailed,
                                fail_reason_.empty() ? "Connection lost — tap Cast to retry" : fail_reason_);
  } else {
    state_machine_.TransitionTo(SessionState::kIdle, "Cast Stopped");
  }
}

void CastSession::OnChannelMessage(const std::string& ns, const std::string& payload,
                                  const std::string& src_id, const std::string& dest_id) {
  (void)src_id;
  (void)dest_id;
  if (ns == kNamespaceReceiver) {
    HandleReceiverStatus(payload);
  } else if (ns == kNamespaceWebrtc) {
    HandleWebrtcMessage(payload);
  } else if (ns == kNamespaceConnection) {
    try {
      auto j = nlohmann::json::parse(payload);
      if (j.value("type", "") == "CLOSE" && !stop_requested_.load()) {
        LOG_WARN << "Receiver sent CLOSE during active session";
        RequestReconnect("Receiver closed connection");
      }
    } catch (...) {}
  }
}

void CastSession::OnChannelStatus(bool is_connected, const std::string& error_msg) {
  if (!is_connected && !stop_requested_.load()) {
    LOG_WARN << "Cast Channel connection lost: " << error_msg;
    RequestReconnect("Cast channel disconnected: " + error_msg);
  }
}

void CastSession::HandleReceiverStatus(const std::string& payload) {
  try {
    auto j = nlohmann::json::parse(payload);
    if (!j.contains("status") || !j["status"].contains("applications")) {
      return;
    }

    const auto& apps = j["status"]["applications"];
    if (apps.is_array() && !apps.empty()) {
      for (const auto& app : apps) {
        std::string app_id = app.value("appId", "");
        if (app_id == kMirroringAudioVideoAppId || app_id == kMirroringAudioOnlyAppId ||
            app_id == "85CDB22F" || app_id == "0F5096E8") {
          app_session_id_ = app.value("sessionId", "");
          app_transport_id_ = app.value("transportId", "");
          launch_received_ = true;
          LOG_INFO << "Mirroring App confirmed running! appId: " << app_id
                   << ", sessionId: " << app_session_id_
                   << ", transportId: " << app_transport_id_;
          std::lock_guard<std::mutex> lk(cv_mutex_);
          cv_.notify_all();
          break;
        }
      }
    }
  } catch (...) {}
}

void CastSession::HandleWebrtcMessage(const std::string& payload) {
  try {
    auto j = nlohmann::json::parse(payload);
    std::string type = j.value("type", "");
    if (type == "ANSWER") {
      if (MirroringNegotiator::ParseAnswerJson(payload, video_keys_, audio_keys_, negotiated_params_)) {
        answer_received_ = true;
        std::lock_guard<std::mutex> lk(cv_mutex_);
        cv_.notify_all();
      }
    } else if (type == "GET_STATUS") {
      int seq = j.value("seqNum", 0);
      std::string status_json = MirroringNegotiator::CreateStatusJson(seq, GetStats());
      if (cast_channel_ && !app_transport_id_.empty()) {
        cast_channel_->SendCastMessage(kNamespaceWebrtc, status_json, app_transport_id_, "streaming_sender");
      }
    }
  } catch (...) {}
}

StreamStats CastSession::GetStats() const {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  StreamStats s = current_stats_;
  if (transport_) {
    StreamStats t_stats = transport_->GetStats();
    s.current_fps = t_stats.current_fps > 0 ? t_stats.current_fps : current_stats_.current_framerate;
    s.packets_sent = t_stats.packets_sent;
    s.frames_sent = t_stats.frames_sent;
    s.nacks_received = t_stats.nacks_received;
    s.pli_received = t_stats.pli_received;
    s.packet_loss_fraction = t_stats.packet_loss_fraction;
    s.round_trip_time_ms = t_stats.round_trip_time_ms;
  }

  {
    std::lock_guard<std::mutex> elock(const_cast<std::mutex&>(video_encoder_mutex_));
    if (video_encoder_) {
      s.encoder_name = video_encoder_->EncoderName();
    }
  }
  if (display_capture_) {
    s.capture_backend = display_capture_->BackendName();
  }
  s.device_name = target_device_.name;
  s.device_ip = target_device_.ip_address;
  s.display_name = "Display " + std::to_string(display_id_);
  s.adaptive_rung_index = adaptive_controller_.GetCurrentLadderIndex();
  s.adaptive_rung_count = static_cast<int>(adaptive_controller_.GetLadder().size());
  s.adaptive_enabled = adaptive_controller_.IsEnabled();

  if (recovery_.IsRecovering()) {
    s.recovery_attempt = recovery_.GetAttemptCount();
    s.recovery_elapsed_s = recovery_.GetElapsedSeconds();
  } else {
    s.recovery_attempt = 0;
    s.recovery_elapsed_s = 0;
  }

  if (s.packet_loss_fraction >= 0.05) {
    s.health_hint = "Wi-Fi is dropping packets. Adaptive is lowering quality so the picture stays smooth.";
  } else if (s.round_trip_time_ms >= 80) {
    s.health_hint = "The TV is answering slowly. Try 5 GHz Wi-Fi or move closer to the router.";
  } else if (s.encoder_name == "libx264") {
    s.health_hint = "Using software encode (libx264). The PC CPU is doing the work.";
  } else {
    s.health_hint.clear();
  }

  return s;
}

} // namespace castcore
