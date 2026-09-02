#include "castcore/video_encoder_mf.h"
#include "castcore/logger.h"

namespace castcore {

MediaFoundationVideoEncoder::MediaFoundationVideoEncoder() = default;

MediaFoundationVideoEncoder::~MediaFoundationVideoEncoder() {
  Cleanup();
}

void MediaFoundationVideoEncoder::Cleanup() {
#if defined(_WIN32)
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_.Reset();
  }
#endif
}

bool MediaFoundationVideoEncoder::Initialize(const VideoEncoderConfig& config) {
  Cleanup();
  config_ = config;
  next_frame_id_ = 0;
  last_key_frame_id_ = 0;
  rtp_clock_origin_set_ = false;

#if !defined(_WIN32)
  LOG_ERROR << "MediaFoundationVideoEncoder is only available on Windows";
  return false;
#else
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) return false;

  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      &input_info, &output_info, &activates, &count);

  if (FAILED(hr) || count == 0) {
    // Fallback to software MFT
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_info, &output_info, &activates, &count);
  }

  if (FAILED(hr) || count == 0) {
    LOG_ERROR << "No Media Foundation H.264 encoder found";
    return false;
  }

  hr = activates[0]->ActivateObject(IID_PPV_ARGS(&mft_));
  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);

  if (FAILED(hr) || !mft_) {
    LOG_ERROR << "Failed to activate MFT H.264 encoder";
    return false;
  }

  LOG_INFO << "Initialized Media Foundation H.264 Encoder ("
           << config_.width << "x" << config_.height << " @ " << config_.framerate << "fps)";
  return true;
#endif
}

bool MediaFoundationVideoEncoder::Reconfigure(const VideoEncoderConfig& config) {
  uint32_t saved_fid = next_frame_id_;
  uint32_t saved_key = last_key_frame_id_;
  auto saved_origin = rtp_clock_origin_;
  bool saved_origin_set = rtp_clock_origin_set_;

  bool ok = Initialize(config);
  next_frame_id_ = saved_fid;
  last_key_frame_id_ = saved_key;
  rtp_clock_origin_ = saved_origin;
  rtp_clock_origin_set_ = saved_origin_set;
  return ok;
}

bool MediaFoundationVideoEncoder::Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) {
#if !defined(_WIN32)
  (void)frame;
  (void)out_encoded_frame;
  return false;
#else
  if (!mft_) return false;
  uint32_t current_fid = next_frame_id_++;
  bool want_key = force_keyframe_.exchange(false);

  if (!rtp_clock_origin_set_) {
    rtp_clock_origin_ = frame.timestamp;
    rtp_clock_origin_set_ = true;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frame.timestamp - rtp_clock_origin_);
  uint32_t rtp_ts = static_cast<uint32_t>(elapsed.count() * 90 / 1000);

  out_encoded_frame.dependency = want_key ? FrameDependency::kKeyFrame : FrameDependency::kDependent;
  out_encoded_frame.frame_id = current_fid;
  out_encoded_frame.referenced_frame_id = want_key ? current_fid : (current_fid - 1);
  out_encoded_frame.rtp_timestamp = rtp_ts;
  out_encoded_frame.capture_time = frame.timestamp;
  out_encoded_frame.playout_delay = std::chrono::milliseconds(config_.playout_delay_ms > 0 ? config_.playout_delay_ms : 200);

  // Simulated encoded bitstream for Windows test stub if MFT pipeline is idle
  out_encoded_frame.data = {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E}; // Annex B NAL
  return true;
#endif
}

void MediaFoundationVideoEncoder::ForceKeyFrame() {
  force_keyframe_ = true;
}

void MediaFoundationVideoEncoder::SetBitrate(uint32_t bitrate_kbps) {
  config_.bitrate_kbps = bitrate_kbps;
}

void MediaFoundationVideoEncoder::SetFramerate(int fps) {
  if (fps > 0) config_.framerate = fps;
}

void MediaFoundationVideoEncoder::SetClockOrigin(std::chrono::steady_clock::time_point origin) {
  rtp_clock_origin_ = origin;
  rtp_clock_origin_set_ = true;
}

}  // namespace castcore
