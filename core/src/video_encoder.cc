#include "castcore/video_encoder.h"
#include "castcore/logger.h"
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

namespace castcore {

class FFmpegVideoEncoder : public IVideoEncoder {
 public:
  FFmpegVideoEncoder() = default;

  ~FFmpegVideoEncoder() override {
    Cleanup();
  }

  bool Initialize(const VideoEncoderConfig& config) override {
    Cleanup();
    config_ = config;

    const AVCodec* codec = nullptr;
    if (config_.codec == VideoCodec::kVP8) {
      codec = avcodec_find_encoder_by_name("libvpx");
      if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
    } else {
      codec = avcodec_find_encoder_by_name("libx264");
      if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }

    if (!codec) {
      LOG_ERROR << "Failed to find suitable video encoder codec for " << VideoCodecToString(config_.codec);
      return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
      LOG_ERROR << "Failed to allocate AVCodecContext";
      return false;
    }

    codec_ctx_->width = config_.width;
    codec_ctx_->height = config_.height;
    codec_ctx_->time_base = {1, 90000}; // Cast video timebase: 90kHz
    codec_ctx_->framerate = {config_.framerate, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = static_cast<int64_t>(config_.bitrate_kbps) * 1000;
    codec_ctx_->rc_max_rate = static_cast<int64_t>(config_.bitrate_kbps) * 1200;
    codec_ctx_->rc_buffer_size = static_cast<int>(config_.bitrate_kbps * 1000 / config_.framerate);
    codec_ctx_->gop_size = config_.gop_size > 0 ? config_.gop_size : config_.framerate;
    codec_ctx_->max_b_frames = 0; // 0 B-frames required for Cast display mirroring

    if (config_.codec == VideoCodec::kH264) {
      av_opt_set(codec_ctx_->priv_data, "preset", "ultrafast", 0);
      av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
      av_opt_set(codec_ctx_->priv_data, "profile", config_.profile.c_str(), 0);
      // Repeat SPS/PPS before every keyframe
      av_opt_set(codec_ctx_->priv_data, "repeat-headers", "1", 0);
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
      LOG_ERROR << "Failed to open video encoder codec";
      Cleanup();
      return false;
    }

    av_frame_ = av_frame_alloc();
    av_frame_->format = codec_ctx_->pix_fmt;
    av_frame_->width = codec_ctx_->width;
    av_frame_->height = codec_ctx_->height;
    av_frame_get_buffer(av_frame_, 32);

    av_packet_ = av_packet_alloc();

    gpu_processor_.Initialize(config_.width, config_.height, config_.width, config_.height);

    next_frame_id_ = 1;
    last_key_frame_id_ = 1;

    LOG_INFO << "Initialized Video Encoder: " << codec->name << " ("
             << config_.width << "x" << config_.height << " @ " << config_.framerate
             << "fps, " << config_.bitrate_kbps << " kbps)";

    return true;
  }

  bool Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) override {
    if (!codec_ctx_ || !av_frame_ || !av_packet_) return false;

    if (frame.width != config_.width || frame.height != config_.height) {
      // Reinitialize if resolution changed
      VideoEncoderConfig new_cfg = config_;
      new_cfg.width = frame.width;
      new_cfg.height = frame.height;
      if (!Initialize(new_cfg)) return false;
    }

    int y_stride, u_stride, v_stride;
    std::vector<uint8_t> dst_y, dst_u, dst_v;
    if (!gpu_processor_.ConvertBgraToYuv420p(frame, dst_y, dst_u, dst_v, y_stride, u_stride, v_stride)) {
      return false;
    }

    av_frame_make_writable(av_frame_);

    // Copy to AVFrame planes
    for (int y = 0; y < config_.height; ++y) {
      std::memcpy(av_frame_->data[0] + y * av_frame_->linesize[0],
                  dst_y.data() + y * y_stride, config_.width);
    }
    for (int y = 0; y < config_.height / 2; ++y) {
      std::memcpy(av_frame_->data[1] + y * av_frame_->linesize[1],
                  dst_u.data() + y * u_stride, config_.width / 2);
      std::memcpy(av_frame_->data[2] + y * av_frame_->linesize[2],
                  dst_v.data() + y * v_stride, config_.width / 2);
    }

    uint32_t current_fid = next_frame_id_++;
    // Convert timestamp to 90kHz Cast timebase
    uint32_t rtp_ts = static_cast<uint32_t>((current_fid * 90000) / config_.framerate);

    av_frame_->pts = current_fid;

    if (force_keyframe_.exchange(false) || current_fid == 1) {
      av_frame_->pict_type = AV_PICTURE_TYPE_I;
#if defined(AV_FRAME_FLAG_KEY)
      av_frame_->flags |= AV_FRAME_FLAG_KEY;
#endif
    } else {
      av_frame_->pict_type = AV_PICTURE_TYPE_NONE;
#if defined(AV_FRAME_FLAG_KEY)
      av_frame_->flags &= ~AV_FRAME_FLAG_KEY;
#endif
    }

    if (avcodec_send_frame(codec_ctx_, av_frame_) < 0) {
      LOG_ERROR << "avcodec_send_frame error";
      return false;
    }

    int ret = avcodec_receive_packet(codec_ctx_, av_packet_);
    if (ret < 0) {
      return false; // Need more frames or error
    }

    bool is_key = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
    if (is_key) {
      last_key_frame_id_ = current_fid;
    }

    out_encoded_frame.dependency = is_key ? FrameDependency::kKeyFrame : FrameDependency::kDependent;
    out_encoded_frame.frame_id = current_fid;
    out_encoded_frame.referenced_frame_id = is_key ? current_fid : (current_fid - 1);
    out_encoded_frame.rtp_timestamp = rtp_ts;
    out_encoded_frame.capture_time = frame.timestamp;
    out_encoded_frame.playout_delay = std::chrono::milliseconds(400);

    out_encoded_frame.data.assign(av_packet_->data, av_packet_->data + av_packet_->size);

    av_packet_unref(av_packet_);
    return true;
  }

  void ForceKeyFrame() override {
    force_keyframe_ = true;
  }

  void SetBitrate(uint32_t bitrate_kbps) override {
    config_.bitrate_kbps = bitrate_kbps;
    if (codec_ctx_) {
      codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
      codec_ctx_->rc_max_rate = static_cast<int64_t>(bitrate_kbps) * 1200;
    }
  }

  void SetFramerate(int fps) override {
    config_.framerate = fps;
    if (codec_ctx_) {
      codec_ctx_->framerate = {fps, 1};
    }
  }

  const VideoEncoderConfig& GetConfig() const override {
    return config_;
  }

 private:
  void Cleanup() {
    if (codec_ctx_) {
      avcodec_free_context(&codec_ctx_);
      codec_ctx_ = nullptr;
    }
    if (av_frame_) {
      av_frame_free(&av_frame_);
      av_frame_ = nullptr;
    }
    if (av_packet_) {
      av_packet_free(&av_packet_);
      av_packet_ = nullptr;
    }
  }

  VideoEncoderConfig config_;
  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* av_frame_ = nullptr;
  AVPacket* av_packet_ = nullptr;
  GpuProcessor gpu_processor_;

  uint32_t next_frame_id_ = 1;
  uint32_t last_key_frame_id_ = 1;
  std::atomic<bool> force_keyframe_{false};
};

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(VideoCodec codec) {
  return std::make_unique<FFmpegVideoEncoder>();
}

} // namespace castcore
