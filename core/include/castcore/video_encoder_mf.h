#ifndef CASTCORE_VIDEO_ENCODER_MF_H_
#define CASTCORE_VIDEO_ENCODER_MF_H_

#include "castcore/video_encoder.h"

#if defined(_WIN32)
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#endif

namespace castcore {

class MediaFoundationVideoEncoder : public IVideoEncoder {
 public:
  MediaFoundationVideoEncoder();
  ~MediaFoundationVideoEncoder() override;

  bool Initialize(const VideoEncoderConfig& config) override;
  bool Reconfigure(const VideoEncoderConfig& config) override;
  bool Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) override;
  void ForceKeyFrame() override;
  void SetBitrate(uint32_t bitrate_kbps) override;
  void SetFramerate(int fps) override;
  void SetClockOrigin(std::chrono::steady_clock::time_point origin) override;

 private:
  void Cleanup();

  VideoEncoderConfig config_;
  uint32_t next_frame_id_ = 0;
  uint32_t last_key_frame_id_ = 0;
  std::atomic<bool> force_keyframe_{false};
  std::chrono::steady_clock::time_point rtp_clock_origin_{};
  bool rtp_clock_origin_set_ = false;

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_VIDEO_ENCODER_MF_H_
