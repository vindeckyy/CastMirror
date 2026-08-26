#ifndef CASTCORE_VIDEO_ENCODER_H_
#define CASTCORE_VIDEO_ENCODER_H_

#include "castcore/types.h"
#include "castcore/gpu_processor.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

namespace castcore {

struct VideoEncoderConfig {
  int width = 1920;
  int height = 1080;
  int framerate = 60;
  uint32_t bitrate_kbps = 6000;
  VideoCodec codec = VideoCodec::kH264;
  std::string profile = "high";
  std::string level = "4.2";
  int gop_size = 60; // 1 second keyframe interval
  int playout_delay_ms = 200;
};

class IVideoEncoder {
 public:
  virtual ~IVideoEncoder() = default;

  virtual bool Initialize(const VideoEncoderConfig& config) = 0;
  virtual bool Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) = 0;
  virtual void ForceKeyFrame() = 0;
  virtual void SetBitrate(uint32_t bitrate_kbps) = 0;
  virtual void SetFramerate(int fps) = 0;

  virtual const VideoEncoderConfig& GetConfig() const = 0;
};

class VideoEncoderFactory {
 public:
  static std::unique_ptr<IVideoEncoder> Create(VideoCodec codec = VideoCodec::kH264);
};

} // namespace castcore

#endif // CASTCORE_VIDEO_ENCODER_H_
