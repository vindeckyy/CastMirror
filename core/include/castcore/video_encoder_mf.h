#ifndef CASTCORE_VIDEO_ENCODER_MF_H_
#define CASTCORE_VIDEO_ENCODER_MF_H_

#include "castcore/video_encoder.h"

#if defined(_WIN32)
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <wrl/client.h>
#endif

namespace castcore {

// Hardware H.264 encoder via a Media Foundation Transform. Initialize()
// fails when no hardware MFT is available so callers can fall back to the
// low-latency FFmpeg encoder (the inbox software MFT buffers ~14 frames).
// Input frames arrive as BGRA; they're converted to the MFT's negotiated
// planar format (NV12 preferred, IYUV accepted) through GpuProcessor before
// ProcessInput. Bitstream is emitted as Annex-B in EncodedFrame::data.
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
  std::string EncoderName() const override { return is_hardware_ ? "mf_h264_hw" : "mf_h264_sw"; }
  const VideoEncoderConfig& GetConfig() const override { return config_; }

 private:
  void Cleanup();
#if defined(_WIN32)
  // Tries one activated MFT: configures input/output media types and applies
  // codec API tuning. Returns false if this MFT rejects the configuration.
  bool ConfigureMft(IMFActivate* activate);
  bool SetOutputType();
  bool SetInputType();
  // Converts a BGRA CapturedVideoFrame into the negotiated input pixel format,
  // writing into input_frame_buf_. Returns false on conversion failure.
  bool ConvertInput(const CapturedVideoFrame& frame);
  // Drains all currently-available output samples into out_encoded_frame.
  // Returns false when no output was produced (input still being buffered).
  bool DrainOutput(EncodedFrame& out_encoded_frame);
#endif

  VideoEncoderConfig config_;
  uint32_t next_frame_id_ = 0;
  uint32_t last_key_frame_id_ = 0;
  std::atomic<bool> force_keyframe_{false};
  std::chrono::steady_clock::time_point rtp_clock_origin_{};
  bool rtp_clock_origin_set_ = false;
  bool is_hardware_ = false;
  bool mf_started_ = false;
  int64_t sample_time_100ns_ = 0;

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  // Negotiated input format: MFVideoFormat_NV12 or MFVideoFormat_IYUV.
  GUID input_subtype_ = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
  std::vector<uint8_t> input_frame_buf_;
  GpuProcessor gpu_processor_;
  int gpu_src_w_ = 0;
  int gpu_src_h_ = 0;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_VIDEO_ENCODER_MF_H_
