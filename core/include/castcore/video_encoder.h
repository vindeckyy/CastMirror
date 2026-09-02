#ifndef CASTCORE_VIDEO_ENCODER_H_
#define CASTCORE_VIDEO_ENCODER_H_

#include "castcore/types.h"
#include "castcore/gpu_processor.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

namespace castcore {

struct VideoEncoderConfig {
  int width = 1920;
  int height = 1080;
  int framerate = 60;
  uint32_t bitrate_kbps = 6000;
  VideoCodec codec = VideoCodec::kH264;
  std::string profile = "high";
  std::string level = "4.2";
  int gop_size = 0; // 0 = auto (intra_refresh avoids periodic IDRs)
  int playout_delay_ms = 200;
  int slices = 4;
  bool intra_refresh = true;
  bool low_latency_tune = true;
};

class IVideoEncoder {
 public:
  virtual ~IVideoEncoder() = default;

  virtual bool Initialize(const VideoEncoderConfig& config) = 0;
  virtual bool Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) = 0;
  virtual void ForceKeyFrame() = 0;
  virtual void SetBitrate(uint32_t bitrate_kbps) = 0;
  virtual void SetFramerate(int fps) = 0;

  // Set a shared clock origin for RTP timestamp generation. When set before
  // the first Encode() call, both audio and video RTP timestamps reference
  // the same absolute time, keeping A/V in sync on the receiver.
  virtual void SetClockOrigin(std::chrono::steady_clock::time_point origin) = 0;

  // Reopen the encoder at a new size/fps/bitrate (adaptive ladder). Cast
  // frame ids and the RTP clock origin are preserved so the receiver never
  // sees an id jump or timestamp reset. Returns false if reopen failed.
  virtual bool Reconfigure(const VideoEncoderConfig& config) = 0;

  // Actual backend in use ("h264_vaapi", "libx264", "libvpx", ...).
  virtual std::string EncoderName() const = 0;

  virtual const VideoEncoderConfig& GetConfig() const = 0;
};

class VideoEncoderFactory {
 public:
  static std::unique_ptr<IVideoEncoder> Create(VideoCodec codec = VideoCodec::kH264);
};

} // namespace castcore

#endif // CASTCORE_VIDEO_ENCODER_H_
