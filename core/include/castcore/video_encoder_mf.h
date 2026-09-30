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
  // writing into dst (which must hold InputBufferSize() bytes). The caller owns
  // dst: the conversion runs straight into the locked MFT media buffer, so no
  // intermediate frame-sized copy exists. Returns false on conversion failure.
  bool ConvertInput(const CapturedVideoFrame& frame, uint8_t* dst);
  // Bytes one converted input frame occupies: NV12 and IYUV are both 3/2
  // bytes per pixel, so the size does not depend on the negotiated subtype.
  size_t InputBufferSize() const {
    return static_cast<size_t>(config_.width) * config_.height * 3 / 2;
  }
  // (Re)allocates the reusable input media buffer + sample when the required
  // size changed. Both are kept for the lifetime of the stream: the MFT copies
  // the sample during ProcessInput, so one buffer can serve every frame.
  bool EnsureInputBuffer();
  // (Re)allocates the reusable output sample for caller-allocated output,
  // growing it when the MFT reports it was too small. Returns false when the
  // buffer cannot be created.
  bool EnsureOutputBuffer();
  // Refreshes the cached output stream info after configuration or a dynamic
  // type change. Returns whether the MFT provides its own samples.
  bool RefreshOutputStreamInfo();
  // Drains the next available output sample into out_encoded_frame. Returns
  // false when no output was produced (input still being buffered). Only the
  // first available sample is taken: a low-latency encoder emits at most one
  // frame per input, and appending several would pack multiple pictures behind
  // a single RTP timestamp. Anything still queued is picked up next call.
  bool DrainOutput(EncodedFrame& out_encoded_frame);
  // Copies one MFT output sample's bytes and clean-point flag into
  // out_encoded_frame. Does not take ownership of sample.
  bool CopyOutputSample(IMFSample* sample, EncodedFrame& out_encoded_frame);
  // Asynchronous MFTs (every GPU vendor's hardware encoder) reject
  // ProcessInput/ProcessOutput until unlocked, then drive the caller through
  // METransformNeedInput/HaveOutput events. PumpEvents() folds pending events
  // into need_input_/have_output_, waiting up to wait_ms for the first one.
  void EnableAsyncMode();
  void PumpEvents(int wait_ms);
  // Copies the codec's SPS/PPS from the negotiated output type, so key frames
  // can be made self-describing when the MFT leaves them out of the bitstream.
  void CaptureSequenceHeader();
#endif

  VideoEncoderConfig config_;
  uint32_t next_frame_id_ = 0;
  uint32_t last_key_frame_id_ = 0;
  std::atomic<bool> force_keyframe_{false};
  std::chrono::steady_clock::time_point rtp_clock_origin_{};
  bool rtp_clock_origin_set_ = false;
  bool is_hardware_ = false;

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<IMFTransform> mft_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  // Negotiated input format: MFVideoFormat_NV12 or MFVideoFormat_IYUV.
  GUID input_subtype_ = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
  GpuProcessor gpu_processor_;
  int gpu_src_w_ = 0;
  int gpu_src_h_ = 0;

  // Reusable input path. The conversion writes straight into the locked
  // input buffer, and the MFT copies out of it during ProcessInput, so both
  // objects are allocated once per stream instead of once per frame. At
  // 1080p that removes a 3.1 MB allocation plus a 3.1 MB memcpy every tick.
  Microsoft::WRL::ComPtr<IMFMediaBuffer> input_buffer_;
  Microsoft::WRL::ComPtr<IMFSample> input_sample_;
  size_t input_buffer_size_ = 0;

  // Reusable output path for MFTs that do not provide their own samples.
  // Sized from MFT_OUTPUT_STREAM_INFO::cbSize and grown on demand when the
  // transform reports MF_E_BUFFER_TOO_SMALL, so an oversized first frame is
  // retried instead of dropped.
  Microsoft::WRL::ComPtr<IMFSample> output_sample_;
  DWORD output_buffer_size_ = 0;
  // Cached stream info, re-read on configuration and on a dynamic type change.
  MFT_OUTPUT_STREAM_INFO output_info_{};
  bool have_output_info_ = false;
  bool output_provides_samples_ = false;

  bool async_mft_ = false;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_gen_;
  int need_input_ = 0;
  int have_output_ = 0;
  std::vector<uint8_t> sequence_header_;  // Annex-B SPS+PPS, may be empty
#endif
};

}  // namespace castcore

#endif  // CASTCORE_VIDEO_ENCODER_MF_H_
