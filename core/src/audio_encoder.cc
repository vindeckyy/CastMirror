#include "castcore/audio_encoder.h"
#include "castcore/logger.h"
#include <opus/opus.h>
#include <cstring>
#include <chrono>

namespace castcore {

class OpusAudioEncoder : public IAudioEncoder {
 public:
  OpusAudioEncoder() = default;

  ~OpusAudioEncoder() override {
    Cleanup();
  }

  bool Initialize(const AudioEncoderConfig& config) override {
    Cleanup();
    config_ = config;

    int error = 0;
    encoder_ = opus_encoder_create(config_.sample_rate, config_.channels,
                                   OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    if (error != OPUS_OK || !encoder_) {
      LOG_ERROR << "Failed to create Opus encoder: " << opus_strerror(error);
      return false;
    }

    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrate_bps));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    next_frame_id_ = 0;
    rtp_clock_origin_set_ = false;

    LOG_INFO << "Initialized Opus Audio Encoder (" << config_.sample_rate
             << " Hz, " << config_.channels << " ch, " << (config_.bitrate_bps / 1000) << " kbps)";

    return true;
  }

  bool Encode(const CapturedAudioFrame& frame, EncodedFrame& out_encoded_frame) override {
    if (!encoder_ || frame.pcm_data.empty()) return false;

    const int16_t* pcm = reinterpret_cast<const int16_t*>(frame.pcm_data.data());
    int samples_per_channel = frame.samples_per_channel;

    std::vector<uint8_t> encoded_buffer(4000);
    opus_int32 bytes_encoded = opus_encode(
        encoder_, pcm, samples_per_channel,
        encoded_buffer.data(), static_cast<opus_int32>(encoded_buffer.size()));

    if (bytes_encoded < 0) {
      LOG_ERROR << "Opus encode failed: " << opus_strerror(bytes_encoded);
      return false;
    }

    uint32_t current_fid = next_frame_id_++;
    if (!rtp_clock_origin_set_) {
      rtp_clock_origin_ = frame.timestamp;
      rtp_clock_origin_set_ = true;
    }
    int64_t capture_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.timestamp - rtp_clock_origin_).count();
    if (capture_us < 0) {
      capture_us = 0;
    }
    uint32_t rtp_ts = static_cast<uint32_t>(
        (capture_us * static_cast<int64_t>(config_.sample_rate)) / 1000000);

    out_encoded_frame.dependency = FrameDependency::kKeyFrame;
    out_encoded_frame.frame_id = current_fid;
    out_encoded_frame.referenced_frame_id = current_fid;
    out_encoded_frame.rtp_timestamp = rtp_ts;
    out_encoded_frame.capture_time = frame.timestamp;
    int delay_ms = config_.playout_delay_ms > 0 ? config_.playout_delay_ms : 200;
    out_encoded_frame.playout_delay = std::chrono::milliseconds(delay_ms);

    out_encoded_frame.data.assign(encoded_buffer.data(), encoded_buffer.data() + bytes_encoded);

    return true;
  }

  const AudioEncoderConfig& GetConfig() const override {
    return config_;
  }

  void SetBitrate(int bitrate_bps) override {
    if (bitrate_bps <= 0) {
      return;
    }
    config_.bitrate_bps = bitrate_bps;
    if (encoder_) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_bps));
    }
  }

  void SetClockOrigin(std::chrono::steady_clock::time_point origin) override {
    rtp_clock_origin_ = origin;
    rtp_clock_origin_set_ = true;
  }

 private:
  void Cleanup() {
    if (encoder_) {
      opus_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
  }

  AudioEncoderConfig config_;
  OpusEncoder* encoder_ = nullptr;
  uint32_t next_frame_id_ = 0;
  std::chrono::steady_clock::time_point rtp_clock_origin_{};
  bool rtp_clock_origin_set_ = false;
};

std::unique_ptr<IAudioEncoder> AudioEncoderFactory::Create(AudioCodec codec) {
  return std::make_unique<OpusAudioEncoder>();
}

} // namespace castcore
