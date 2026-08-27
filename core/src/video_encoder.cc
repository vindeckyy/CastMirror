#include "castcore/video_encoder.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

namespace castcore {

namespace {
bool SoftwareEncodeForced() {
  const char* env = std::getenv("CASTMIRROR_FORCE_SOFTWARE_ENCODE");
  if (env != nullptr && env[0] != '\0') {
    return std::strcmp(env, "0") != 0;
  }
  return ConfigStore::Instance().Get().force_software_encode;
}
}  // namespace

class FFmpegVideoEncoder : public IVideoEncoder {
 public:
  FFmpegVideoEncoder() = default;

  ~FFmpegVideoEncoder() override {
    Cleanup();
  }

  bool Initialize(const VideoEncoderConfig& config) override {
    Cleanup();
    // Fresh session: restart the Cast frame id space and RTP clock.
    next_frame_id_ = 0;
    last_key_frame_id_ = 0;
    rtp_clock_origin_set_ = false;
    return InitEncoder(config, /*allow_vaapi=*/!SoftwareEncodeForced());
  }

  bool Reconfigure(const VideoEncoderConfig& config) override {
    // Adaptive ladder size/fps change: frame ids and the RTP clock origin
    // must survive the reopen or the receiver waits on a hole forever.
    const uint32_t saved_frame_id = next_frame_id_;
    const uint32_t saved_key_id = last_key_frame_id_;
    const std::chrono::steady_clock::time_point saved_origin = rtp_clock_origin_;
    const bool saved_origin_set = rtp_clock_origin_set_;

    Cleanup();
    bool ok = InitEncoder(config, /*allow_vaapi=*/!SoftwareEncodeForced());
    if (!ok) {
      LOG_WARN << "Encoder reconfigure failed with primary backend; retrying software";
      ok = InitEncoder(config, /*allow_vaapi=*/false);
    }
    if (!ok) {
      return false;
    }
    next_frame_id_ = saved_frame_id;
    last_key_frame_id_ = saved_key_id;
    rtp_clock_origin_ = saved_origin;
    rtp_clock_origin_set_ = saved_origin_set;
    return true;
  }

  bool Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) override {
    if (!codec_ctx_) return false;

    uint32_t current_fid = next_frame_id_;
    // RTP timestamps must follow capture time, not assumed fps. Encoding
    // 1080p60 can take longer than 16ms; fake 60fps stamps make the TV
    // drain its 200ms buffer and freeze on a still frame.
    if (!rtp_clock_origin_set_) {
      rtp_clock_origin_ = frame.timestamp;
      rtp_clock_origin_set_ = true;
    }
    int64_t capture_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.timestamp - rtp_clock_origin_).count();
    if (capture_us < 0) {
      capture_us = 0;
    }
    uint32_t rtp_ts = static_cast<uint32_t>((capture_us * 90) / 1000);

    const bool want_key = force_keyframe_.exchange(false) || current_fid == 0;

    if (use_vaapi_) {
      if (!sw_frame_ || !hw_frame_) {
        LOG_ERROR << "VAAPI Encode missing frames";
        return false;
      }
      if (av_frame_make_writable(sw_frame_) < 0) {
        LOG_ERROR << "VAAPI sw_frame_ not writable";
        return false;
      }
      if (!gpu_processor_.ConvertBgraToNv12(
              frame, sw_frame_->data[0], sw_frame_->linesize[0],
              sw_frame_->data[1], sw_frame_->linesize[1])) {
        LOG_ERROR << "ConvertBgraToNv12 failed";
        return false;
      }
      av_frame_unref(hw_frame_);
      if (av_hwframe_get_buffer(hw_frames_ctx_, hw_frame_, 0) < 0) {
        LOG_ERROR << "av_hwframe_get_buffer error";
        force_keyframe_ = true;
        return false;
      }
      if (av_hwframe_transfer_data(hw_frame_, sw_frame_, 0) < 0) {
        LOG_ERROR << "av_hwframe_transfer_data error";
        force_keyframe_ = true;
        return false;
      }
      hw_frame_->pts = static_cast<int64_t>(rtp_ts);
      hw_frame_->pict_type = want_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
      int send_ret = avcodec_send_frame(codec_ctx_, hw_frame_);
      av_frame_unref(hw_frame_);
      if (send_ret < 0) {
        char err_buf[256];
        av_strerror(send_ret, err_buf, sizeof(err_buf));
        LOG_ERROR << "avcodec_send_frame error (VAAPI): " << err_buf;
        force_keyframe_ = true;
        return false;
      }
    } else {
      if (!av_frame_ || av_frame_make_writable(av_frame_) < 0) return false;
      if (!gpu_processor_.ConvertBgraToYuv420p(
              frame, av_frame_->data[0], av_frame_->linesize[0],
              av_frame_->data[1], av_frame_->linesize[1],
              av_frame_->data[2], av_frame_->linesize[2])) {
        return false;
      }
      av_frame_->pts = static_cast<int64_t>(rtp_ts);
      if (want_key) {
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
        force_keyframe_ = true;
        return false;
      }
    }

    int ret = avcodec_receive_packet(codec_ctx_, av_packet_);
    if (ret < 0) {
      char err_buf[256];
      av_strerror(ret, err_buf, sizeof(err_buf));
      LOG_WARN << "avcodec_receive_packet returned " << ret << " (" << err_buf << ")";
      force_keyframe_ = true;
      ++next_frame_id_;
      return false;
    }
    ++next_frame_id_;

    bool is_key = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
    if (want_key && !is_key) {
      LOG_WARN << "Forced IDR/PLI did not produce a keyframe (frame " << current_fid << ")";
    }
    if (is_key) {
      last_key_frame_id_ = current_fid;
    }

    out_encoded_frame.dependency = is_key ? FrameDependency::kKeyFrame : FrameDependency::kDependent;
    out_encoded_frame.frame_id = current_fid;
    out_encoded_frame.referenced_frame_id = is_key ? current_fid : (current_fid - 1);
    out_encoded_frame.rtp_timestamp = rtp_ts;
    out_encoded_frame.capture_time = frame.timestamp;
    int delay_ms = config_.playout_delay_ms > 0 ? config_.playout_delay_ms : 200;
    out_encoded_frame.playout_delay = std::chrono::milliseconds(delay_ms);

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
      int fps = std::max(config_.framerate, 1);
      codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
      codec_ctx_->rc_max_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
      codec_ctx_->rc_buffer_size = bitrate_kbps * 1000 * 8 / fps;
      if (use_vaapi_) {
        // h264_vaapi honors the generic bit_rate above; "b" exists on some
        // FFmpeg builds — an AVERROR_OPTION_NOT_FOUND here is expected.
        av_opt_set_int(codec_ctx_->priv_data, "b",
                       static_cast<int64_t>(bitrate_kbps) * 1000,
                       AV_OPT_SEARCH_CHILDREN);
      }
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

  std::string EncoderName() const override {
    if (use_vaapi_) return "h264_vaapi";
    if (config_.codec == VideoCodec::kVP8) return "libvpx";
    return "libx264";
  }

 private:
  bool InitEncoder(const VideoEncoderConfig& config, bool allow_vaapi) {
    config_ = config;

    if (config_.codec == VideoCodec::kH264 && allow_vaapi && TryInitVaapi()) {
      return true;
    }
    if (use_vaapi_) {
      // Half-built VAAPI state must not leak into the software path.
      Cleanup();
      config_ = config;
      if (allow_vaapi) {
        LOG_INFO << "VAAPI H.264 unavailable, using software encoder";
      }
    }
    return InitSoftware();
  }

  bool TryInitVaapi() {
    if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI,
                               nullptr, nullptr, 0) < 0) {
      hw_device_ctx_ = nullptr;
      return false;
    }
    const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
      av_buffer_unref(&hw_device_ctx_);
      return false;
    }

    hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
    if (!hw_frames_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      return false;
    }
    AVHWFramesContext* frames = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = config_.width;
    frames->height = config_.height;
    frames->initial_pool_size = 8 + 4;  // pool: in-flight + reordering margin
    if (av_hwframe_ctx_init(hw_frames_ctx_) < 0) {
      av_buffer_unref(&hw_frames_ctx_);
      av_buffer_unref(&hw_device_ctx_);
      return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
      av_buffer_unref(&hw_frames_ctx_);
      av_buffer_unref(&hw_device_ctx_);
      return false;
    }
    codec_ctx_->width = config_.width;
    codec_ctx_->height = config_.height;
    codec_ctx_->time_base = {1, 90000};  // Cast video timebase: 90kHz
    codec_ctx_->framerate = {config_.framerate, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
    codec_ctx_->bit_rate = static_cast<int64_t>(config_.bitrate_kbps) * 1000;
    codec_ctx_->rc_max_rate = codec_ctx_->bit_rate;
    int fps = std::max(config_.framerate, 1);
    codec_ctx_->rc_buffer_size = config_.bitrate_kbps * 1000 * 8 / fps;
    codec_ctx_->gop_size = config_.gop_size > 0 ? config_.gop_size : config_.framerate;
    codec_ctx_->max_b_frames = 0;  // 0 B-frames required for Cast display mirroring
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->profile = AV_PROFILE_H264_HIGH;
    codec_ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
    av_opt_set_int(codec_ctx_->priv_data, "async_depth", 1, 0);
    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
      Cleanup();
      return false;
    }

    sw_frame_ = av_frame_alloc();
    sw_frame_->format = AV_PIX_FMT_NV12;
    sw_frame_->width = config_.width;
    sw_frame_->height = config_.height;
    if (av_frame_get_buffer(sw_frame_, 32) < 0) {
      Cleanup();
      return false;
    }
    hw_frame_ = av_frame_alloc();
    av_packet_ = av_packet_alloc();
    if (!hw_frame_ || !av_packet_) {
      Cleanup();
      return false;
    }

    if (!gpu_processor_.Initialize(config_.width, config_.height, config_.width, config_.height)) {
      Cleanup();
      return false;
    }

    use_vaapi_ = true;
    LOG_INFO << "Initialized Video Encoder: h264_vaapi ("
             << config_.width << "x" << config_.height << " @ " << config_.framerate
             << "fps, " << config_.bitrate_kbps << " kbps)";
    return true;
  }

  bool InitSoftware() {
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
    codec_ctx_->time_base = {1, 90000};  // Cast video timebase: 90kHz
    codec_ctx_->framerate = {config_.framerate, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = static_cast<int64_t>(config_.bitrate_kbps) * 1000;
    codec_ctx_->rc_max_rate = static_cast<int64_t>(config_.bitrate_kbps) * 1000;
    codec_ctx_->gop_size = config_.gop_size > 0 ? config_.gop_size : config_.framerate;
    codec_ctx_->max_b_frames = 0;  // 0 B-frames required for Cast display mirroring
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // ~8 frame VBV (~130ms at 60fps): enough for a real I-frame, still low-latency.
    int fps = std::max(config_.framerate, 1);
    codec_ctx_->rc_buffer_size = config_.bitrate_kbps * 1000 * 8 / fps;

    if (config_.codec == VideoCodec::kH264) {
      // ultrafast forces Constrained Baseline (no CABAC / 8x8), which looks
      // like a bitrate starvation problem at 1080p60. superfast keeps High.
      av_opt_set(codec_ctx_->priv_data, "preset", "superfast", 0);
      av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
      av_opt_set(codec_ctx_->priv_data, "profile", config_.profile.c_str(), 0);
      av_opt_set(codec_ctx_->priv_data, "repeat-headers", "1", 0);
      av_opt_set(codec_ctx_->priv_data, "intra-refresh", "1", 0);
      av_opt_set(codec_ctx_->priv_data, "forced-idr", "1", 0);
      std::ostringstream x264p;
      x264p << "keyint=" << codec_ctx_->gop_size
            << ":min-keyint=" << codec_ctx_->gop_size
            << ":scenecut=0:intra-refresh=1:bframes=0:rc-lookahead=0:"
               "sync-lookahead=0:sliced-threads=1:aud=1";
      av_opt_set(codec_ctx_->priv_data, "x264-params", x264p.str().c_str(), 0);
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

    use_vaapi_ = false;
    LOG_INFO << "Initialized Video Encoder: " << codec->name << " ("
             << config_.width << "x" << config_.height << " @ " << config_.framerate
             << "fps, " << config_.bitrate_kbps << " kbps, vbv="
             << (codec_ctx_->rc_buffer_size / 1000) << " kb)";

    return true;
  }

  void Cleanup() {
    if (codec_ctx_) {
      avcodec_free_context(&codec_ctx_);
      codec_ctx_ = nullptr;
    }
    if (av_frame_) {
      av_frame_free(&av_frame_);
      av_frame_ = nullptr;
    }
    if (sw_frame_) {
      av_frame_free(&sw_frame_);
      sw_frame_ = nullptr;
    }
    if (hw_frame_) {
      av_frame_free(&hw_frame_);
      hw_frame_ = nullptr;
    }
    if (av_packet_) {
      av_packet_free(&av_packet_);
      av_packet_ = nullptr;
    }
    if (hw_frames_ctx_) {
      av_buffer_unref(&hw_frames_ctx_);
      hw_frames_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      hw_device_ctx_ = nullptr;
    }
    use_vaapi_ = false;
  }

  VideoEncoderConfig config_;
  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* av_frame_ = nullptr;     // software path input (YUV420P)
  AVFrame* sw_frame_ = nullptr;     // VAAPI path staging (NV12, system memory)
  AVFrame* hw_frame_ = nullptr;     // VAAPI path upload target
  AVPacket* av_packet_ = nullptr;
  AVBufferRef* hw_device_ctx_ = nullptr;
  AVBufferRef* hw_frames_ctx_ = nullptr;
  bool use_vaapi_ = false;
  GpuProcessor gpu_processor_;

  uint32_t next_frame_id_ = 0;
  uint32_t last_key_frame_id_ = 0;
  std::atomic<bool> force_keyframe_{false};
  std::chrono::steady_clock::time_point rtp_clock_origin_{};
  bool rtp_clock_origin_set_ = false;
};

std::unique_ptr<IVideoEncoder> VideoEncoderFactory::Create(VideoCodec codec) {
  return std::make_unique<FFmpegVideoEncoder>();
}

} // namespace castcore
