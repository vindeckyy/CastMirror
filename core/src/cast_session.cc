#include "castcore/cast_session.h"
#include "castcore/capability_model.h"
#include "castcore/logger.h"
#include <nlohmann/json.hpp>

namespace castcore {

CastSession::CastSession(StateMachine& state_machine)
    : state_machine_(state_machine),
      recovery_(30) {}

CastSession::~CastSession() {
  Stop();
}

bool CastSession::IsActive() const {
  return state_machine_.IsActive();
}

void CastSession::SetErrorCallback(ErrorCallback callback) {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);
  error_callback_ = std::move(callback);
}

bool CastSession::Start(const CastDevice& device,
                       int display_id,
                       QualityPreset preset,
                       bool enable_audio,
                       VideoCodec video_codec) {
  std::lock_guard<std::recursive_mutex> lock(session_mutex_);

  if (state_machine_.IsActive()) {
    LOG_WARN << "CastSession::Start called while session already active";
    return false;
  }

  target_device_ = device;
  display_id_ = display_id;
  preset_ = preset;
  enable_audio_ = enable_audio;
  video_codec_ = video_codec;
  stop_requested_ = false;
  is_streaming_ = false;
  answer_received_ = false;
  launch_received_ = false;

  state_machine_.TransitionTo(SessionState::kConnecting, "Connecting to " + device.name);

  // 1. Initialize Display Capturer to query display details
  display_capture_ = DisplayCaptureFactory::Create();
  auto displays = display_capture_->EnumerateDisplays();
  int disp_w = 1920, disp_h = 1080, disp_fps = 60;
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

  // 2. Recommend initial stats based on Device Capability Model
  current_stats_ = CapabilityModel::GetRecommendedSettings(device, preset, disp_w, disp_h, disp_fps);
  adaptive_controller_.Initialize(current_stats_, preset);

  // 3. Generate AES Keys
  video_keys_ = MirroringNegotiator::GenerateRandomKeys();
  audio_keys_ = enable_audio_ ? MirroringNegotiator::GenerateRandomKeys() : StreamEncryptionKeys{};

  // 4. Create and Connect TLS Cast Channel
  cast_channel_ = std::make_unique<CastChannel>();
  cast_channel_->SetMessageCallback([this](const std::string& ns, const std::string& payload,
                                          const std::string& src, const std::string& dest) {
    OnChannelMessage(ns, payload, src, dest);
  });
  cast_channel_->SetStatusCallback([this](bool connected, const std::string& err) {
    OnChannelStatus(connected, err);
  });

  if (!cast_channel_->Connect(device.ip_address, device.port)) {
    state_machine_.TransitionTo(SessionState::kFailed, "TLS connection failed to " + device.ip_address);
    return false;
  }

  // 5. Send LAUNCH request for Mirroring App
  const char* app_id = (enable_audio_ && !device.HasVideoOut())
      ? kMirroringAudioOnlyAppId : kMirroringAudioVideoAppId;

  launch_request_id_ = cast_channel_->LaunchApp(app_id);

  // Wait for Launch confirmation with 8s timeout
  {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(8), [this] { return launch_received_ || stop_requested_; })) {
      LOG_ERROR << "Timed out waiting for RECEIVER_STATUS from " << device.name;
      state_machine_.TransitionTo(SessionState::kFailed, "App launch timed out");
      Stop();
      return false;
    }
  }

  if (stop_requested_) {
    Stop();
    return false;
  }

  state_machine_.TransitionTo(SessionState::kNegotiating, "Negotiating Offer/Answer");

  // 6. Connect virtual connection to Mirroring App transportId
  cast_channel_->ConnectVirtual(app_transport_id_, "streaming_sender");

  // 7. Send OFFER JSON
  offer_seq_num_ = 1001;
  std::string offer_json = MirroringNegotiator::CreateOfferJson(
      offer_seq_num_, current_stats_, enable_audio_, video_keys_, audio_keys_, video_codec_, current_stats_.target_delay_ms);

  LOG_INFO << "Sending OFFER to Mirroring App (transportId: " << app_transport_id_ << ")...";
  cast_channel_->SendCastMessage(kNamespaceWebrtc, offer_json, app_transport_id_, "streaming_sender");

  // Wait for ANSWER with 5s timeout
  {
    std::unique_lock<std::mutex> lk(cv_mutex_);
    if (!cv_.wait_for(lk, std::chrono::seconds(5), [this] { return answer_received_ || stop_requested_; })) {
      LOG_ERROR << "Timed out waiting for ANSWER from " << device.name;
      state_machine_.TransitionTo(SessionState::kFailed, "Offer/Answer negotiation timed out");
      Stop();
      return false;
    }
  }

  if (stop_requested_) {
    Stop();
    return false;
  }

  // 8. Start Streaming Media
  StartStreamingMedia();

  state_machine_.TransitionTo(SessionState::kStreaming, "Casting Display to " + device.name);
  return true;
}

void CastSession::StartStreamingMedia() {
  LOG_INFO << "Starting live media capture and encoding pipeline...";

  // Setup Crypto
  video_crypto_ = std::make_unique<FrameCrypto>(video_keys_.aes_key, video_keys_.aes_iv_mask);
  if (enable_audio_) {
    audio_crypto_ = std::make_unique<FrameCrypto>(audio_keys_.aes_key, audio_keys_.aes_iv_mask);
  }

  // Setup RTP Packetizers
  video_packetizer_ = std::make_unique<RtpPacketizer>(
      negotiated_params_.video_stream.rtp_payload_type,
      negotiated_params_.video_stream.sender_ssrc);

  if (enable_audio_) {
    audio_packetizer_ = std::make_unique<RtpPacketizer>(
        negotiated_params_.audio_stream.rtp_payload_type,
        negotiated_params_.audio_stream.sender_ssrc);
  }

  // Setup Media Transport
  transport_ = std::make_unique<CastTransport>();
  transport_->SetPliCallback([this] {
    if (video_encoder_) {
      video_encoder_->ForceKeyFrame();
    }
  });
  transport_->SetFeedbackCallback([this](const RtcpFeedback& fb) {
    adaptive_controller_.OnFeedback(fb);
  });

  if (!transport_->Start(target_device_.ip_address, negotiated_params_.receiver_udp_port)) {
    LOG_ERROR << "Failed to start media transport UDP socket";
    return;
  }

  // Setup Video Encoder
  VideoEncoderConfig venc_cfg;
  venc_cfg.width = current_stats_.current_resolution.width;
  venc_cfg.height = current_stats_.current_resolution.height;
  venc_cfg.framerate = current_stats_.current_framerate;
  venc_cfg.bitrate_kbps = current_stats_.bitrate_kbps;
  venc_cfg.codec = video_codec_;

  video_encoder_ = VideoEncoderFactory::Create(video_codec_);
  video_encoder_->Initialize(venc_cfg);

  // Setup Audio Encoder
  if (enable_audio_) {
    AudioEncoderConfig aenc_cfg;
    aenc_cfg.sample_rate = 48000;
    aenc_cfg.channels = 2;
    aenc_cfg.bitrate_bps = 192000;
    aenc_cfg.codec = AudioCodec::kOpus;

    audio_encoder_ = AudioEncoderFactory::Create(AudioCodec::kOpus);
    audio_encoder_->Initialize(aenc_cfg);

    audio_capture_ = AudioCaptureFactory::Create();
    audio_capture_->SetAudioCallback([this](const CapturedAudioFrame& af) {
      ProcessAudioFrame(af);
    });
    audio_capture_->Start(48000, 2);
  }

  // Hook Display Capture callback and start
  display_capture_->SetFrameCallback([this](const CapturedVideoFrame& vf) {
    ProcessVideoFrame(vf);
  });
  display_capture_->Start(display_id_, current_stats_.current_framerate);

  is_streaming_ = true;

  // Start background adaptation thread
  adapt_thread_ = std::thread(&CastSession::AdaptationLoop, this);
}

void CastSession::ProcessVideoFrame(const CapturedVideoFrame& vf) {
  if (!is_streaming_.load() || !video_encoder_ || !video_crypto_ || !video_packetizer_ || !transport_) {
    return;
  }

  EncodedFrame raw_frame;
  if (!video_encoder_->Encode(vf, raw_frame)) {
    return;
  }

  // Encrypt payload in-place
  std::vector<uint8_t> encrypted_payload = video_crypto_->Encrypt(raw_frame.frame_id, raw_frame.data);
  raw_frame.data = std::move(encrypted_payload);

  // Packetize into Cast RTP packets
  auto packets = video_packetizer_->PacketizeFrame(raw_frame);

  // Send over UDP
  transport_->SendPackets(packets);
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
  transport_->SendPackets(packets);
}

void CastSession::AdaptationLoop() {
  while (is_streaming_.load() && !stop_requested_.load()) {
    for (int i = 0; i < 10; ++i) {
      if (!is_streaming_.load() || stop_requested_.load()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!is_streaming_.load() || stop_requested_.load()) break;

    StreamStats updated;
    if (adaptive_controller_.CheckAdaptation(updated)) {
      std::lock_guard<std::recursive_mutex> lock(session_mutex_);
      current_stats_ = updated;
      if (video_encoder_) {
        video_encoder_->SetBitrate(updated.bitrate_kbps);
        video_encoder_->SetFramerate(updated.current_framerate);
      }
    }
  }
}

void CastSession::Stop() {
  bool expected = false;
  if (stop_requested_.compare_exchange_strong(expected, true)) {
    LOG_INFO << "Stopping Cast Session (hard 500ms budget)...";

    // 1. Immediately stop capture and media threads
    is_streaming_ = false;
    {
      std::lock_guard<std::mutex> lk(cv_mutex_);
      cv_.notify_all();
    }

    if (display_capture_) {
      display_capture_->Stop();
    }
    if (audio_capture_) {
      audio_capture_->Stop();
    }
    if (adapt_thread_.joinable()) {
      adapt_thread_.join();
    }

    if (transport_) {
      transport_->Stop();
    }

    // 2. Send STOP message to Cast Channel
    if (cast_channel_ && cast_channel_->IsConnected() && !app_session_id_.empty()) {
      cast_channel_->StopApp(app_session_id_);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      cast_channel_->Disconnect();
    }

    state_machine_.TransitionTo(SessionState::kIdle, "Cast Stopped");
  }
}

void CastSession::OnChannelMessage(const std::string& ns, const std::string& payload,
                                  const std::string& src_id, const std::string& dest_id) {
  if (ns == kNamespaceReceiver) {
    HandleReceiverStatus(payload);
  } else if (ns == kNamespaceWebrtc) {
    HandleWebrtcMessage(payload);
  }
}

void CastSession::OnChannelStatus(bool is_connected, const std::string& error_msg) {
  if (!is_connected && is_streaming_.load() && !stop_requested_.load()) {
    LOG_WARN << "Cast Channel connection lost during active streaming";
    state_machine_.TransitionTo(SessionState::kReconnecting, "Connection lost, attempting recovery...");
    recovery_.StartRecovery("TLS socket closed");
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
      const auto& app = apps[0];
      std::string app_id = app.value("appId", "");
      if (app_id == kMirroringAudioVideoAppId || app_id == kMirroringAudioOnlyAppId) {
        app_session_id_ = app.value("sessionId", "");
        app_transport_id_ = app.value("transportId", "");
        launch_received_ = true;
        LOG_INFO << "Mirroring App confirmed running! sessionId: " << app_session_id_
                 << ", transportId: " << app_transport_id_;
        std::lock_guard<std::mutex> lk(cv_mutex_);
        cv_.notify_all();
      }
    }
  } catch (...) {}
}

void CastSession::HandleWebrtcMessage(const std::string& payload) {
  try {
    auto j = nlohmann::json::parse(payload);
    if (j.contains("type") && j["type"] == "ANSWER") {
      if (MirroringNegotiator::ParseAnswerJson(payload, video_keys_, audio_keys_, negotiated_params_)) {
        answer_received_ = true;
        std::lock_guard<std::mutex> lk(cv_mutex_);
        cv_.notify_all();
      }
    }
  } catch (...) {}
}

StreamStats CastSession::GetStats() const {
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
  return s;
}

} // namespace castcore
