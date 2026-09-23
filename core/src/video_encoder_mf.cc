#include "castcore/video_encoder_mf.h"
#include "castcore/logger.h"

#if defined(_WIN32)
#include <codecapi.h>
#include <mferror.h>
#include <cstring>
#endif

namespace castcore {

MediaFoundationVideoEncoder::MediaFoundationVideoEncoder() = default;

MediaFoundationVideoEncoder::~MediaFoundationVideoEncoder() {
  Cleanup();
}

void MediaFoundationVideoEncoder::Cleanup() {
#if defined(_WIN32)
  if (mft_) {
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    mft_.Reset();
  }
  codec_api_.Reset();
  input_frame_buf_.clear();
  gpu_src_w_ = gpu_src_h_ = 0;
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
#endif
}

#if defined(_WIN32)
bool MediaFoundationVideoEncoder::SetOutputType() {
  Microsoft::WRL::ComPtr<IMFMediaType> mt;
  if (FAILED(MFCreateMediaType(&mt))) return false;

  mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  mt->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_kbps * 1000);
  MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
  MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, config_.framerate, 1);
  MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

  return SUCCEEDED(mft_->SetOutputType(output_stream_id_, mt.Get(), 0));
}

bool MediaFoundationVideoEncoder::SetInputType() {
  static const GUID kCandidates[] = {MFVideoFormat_NV12, MFVideoFormat_IYUV};
  for (const GUID& fmt : kCandidates) {
    Microsoft::WRL::ComPtr<IMFMediaType> mt;
    if (FAILED(MFCreateMediaType(&mt))) continue;

    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, fmt);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, config_.width, config_.height);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, config_.framerate, 1);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    if (SUCCEEDED(mft_->SetInputType(input_stream_id_, mt.Get(), 0))) {
      input_subtype_ = fmt;
      return true;
    }
  }
  return false;
}

bool MediaFoundationVideoEncoder::ConfigureMft(IMFActivate* activate) {
  mft_.Reset();
  codec_api_.Reset();
  if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&mft_))) || !mft_) return false;

  // Some MFTs use non-zero stream identifiers.
  mft_->GetStreamIDs(1, &input_stream_id_, 1, &output_stream_id_);

  // Output type must be offered before the input type for encoder MFTs.
  if (!SetOutputType()) return false;
  if (!SetInputType()) return false;

  // Codec API tuning — all best-effort; a property that doesn't exist on a
  // given MFT just fails to set.
  if (SUCCEEDED(mft_.As(&codec_api_)) && codec_api_) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVLowLatencyMode, &v);

    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = config_.bitrate_kbps * 1000;
    codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);

    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = eAVEncCommonRateControlMode_CBR;
    codec_api_->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);

    if (config_.gop_size > 0) {
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(config_.gop_size);
      codec_api_->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }
  }

  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  return true;
}
#endif

bool MediaFoundationVideoEncoder::Initialize(const VideoEncoderConfig& config) {
  Cleanup();
  config_ = config;
  next_frame_id_ = 0;
  last_key_frame_id_ = 0;
  rtp_clock_origin_set_ = false;
  sample_time_100ns_ = 0;
  is_hardware_ = false;

#if !defined(_WIN32)
  LOG_ERROR << "MediaFoundationVideoEncoder is only available on Windows";
  return false;
#else
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    LOG_ERROR << "MFStartup failed: hr=0x" << std::hex << hr << std::dec;
    return false;
  }
  mf_started_ = true;

  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  // Hardware MFTs only. The inbox software H.264 MFT has a fixed ~14-frame
  // internal pipeline delay (≈470ms at 30fps), which is unusable for live
  // mirroring; when no hardware MFT is present we return false so the
  // composite encoder falls back to FFmpeg/libx264 (low latency).
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                 &input_info, &output_info, &activates, &count);

  bool configured = false;
  if (SUCCEEDED(hr) && count > 0) {
    for (UINT32 i = 0; i < count && !configured; ++i) {
      if (ConfigureMft(activates[i])) {
        configured = true;
        is_hardware_ = true;
      }
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
  }

  if (!configured) {
    LOG_INFO << "No hardware Media Foundation H.264 encoder available";
    Cleanup();
    return false;
  }

  input_frame_buf_.resize(static_cast<size_t>(config_.width) * config_.height * 3 / 2);

  LOG_INFO << "Initialized Media Foundation H.264 Encoder ("
           << EncoderName() << ", " << config_.width << "x" << config_.height
           << " @ " << config_.framerate << "fps, " << config_.bitrate_kbps << "kbps)";
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

#if defined(_WIN32)
bool MediaFoundationVideoEncoder::ConvertInput(const CapturedVideoFrame& frame) {
  if (gpu_src_w_ != frame.width || gpu_src_h_ != frame.height) {
    if (!gpu_processor_.Initialize(frame.width, frame.height,
                                   config_.width, config_.height)) {
      LOG_ERROR << "GpuProcessor init failed for " << frame.width << "x" << frame.height;
      return false;
    }
    gpu_src_w_ = frame.width;
    gpu_src_h_ = frame.height;
  }

  uint8_t* y = input_frame_buf_.data();
  const int w = config_.width, h = config_.height;
  if (IsEqualGUID(input_subtype_, MFVideoFormat_NV12)) {
    return gpu_processor_.ConvertBgraToNv12(frame, y, w, y + static_cast<size_t>(w) * h, w);
  }
  // IYUV: Y plane + separate U and V planes.
  uint8_t* u = y + static_cast<size_t>(w) * h;
  uint8_t* v = u + static_cast<size_t>(w) * h / 4;
  return gpu_processor_.ConvertBgraToYuv420p(frame, y, w, u, w / 2, v, w / 2);
}

bool MediaFoundationVideoEncoder::DrainOutput(EncodedFrame& out_encoded_frame) {
  bool produced = false;

  // Encoder MFTs generally do not provide output samples (no
  // MFT_OUTPUT_STREAM_PROVIDES_SAMPLES), so ProcessOutput requires a
  // caller-allocated sample sized per GetOutputStreamInfo.
  MFT_OUTPUT_STREAM_INFO stream_info{};
  bool have_stream_info = SUCCEEDED(mft_->GetOutputStreamInfo(output_stream_id_, &stream_info));
  bool provides_samples =
      !have_stream_info || (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

  while (true) {
    Microsoft::WRL::ComPtr<IMFSample> allocated_sample;
    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = output_stream_id_;

    if (!provides_samples) {
      DWORD buffer_size = stream_info.cbSize > 0 ? stream_info.cbSize : 1024 * 1024;
      Microsoft::WRL::ComPtr<IMFMediaBuffer> allocated_buffer;
      if (FAILED(MFCreateMemoryBuffer(buffer_size, &allocated_buffer)) ||
          FAILED(MFCreateSample(&allocated_sample))) {
        break;
      }
      allocated_sample->AddBuffer(allocated_buffer.Get());
      out.pSample = allocated_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
    if (out.pEvents) CoTaskMemFree(out.pEvents);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      break;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      // Re-select an available output type (e.g. after a bitrate change) and
      // refresh the output stream info — the buffer size may have changed.
      for (DWORD i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> mt;
        if (FAILED(mft_->GetOutputAvailableType(output_stream_id_, i, &mt))) break;
        if (SUCCEEDED(mft_->SetOutputType(output_stream_id_, mt.Get(), 0))) break;
      }
      have_stream_info = SUCCEEDED(mft_->GetOutputStreamInfo(output_stream_id_, &stream_info));
      provides_samples =
          !have_stream_info || (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
      continue;
    }
    if (FAILED(hr) || !out.pSample) {
      break;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&buffer)) && buffer) {
      BYTE* data = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
        out_encoded_frame.data.insert(out_encoded_frame.data.end(), data, data + len);
        buffer->Unlock();
        produced = true;
      }
    }
    UINT32 clean = 0;
    if (SUCCEEDED(out.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) {
      out_encoded_frame.dependency = FrameDependency::kKeyFrame;
    }
    if (provides_samples) {
      out.pSample->Release();
    }
  }
  return produced;
}
#endif

bool MediaFoundationVideoEncoder::Encode(const CapturedVideoFrame& frame, EncodedFrame& out_encoded_frame) {
#if !defined(_WIN32)
  (void)frame;
  (void)out_encoded_frame;
  return false;
#else
  if (!mft_) return false;
  uint32_t current_fid = next_frame_id_;
  bool want_key = force_keyframe_.exchange(false);

  if (!rtp_clock_origin_set_) {
    rtp_clock_origin_ = frame.timestamp;
    rtp_clock_origin_set_ = true;
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(frame.timestamp - rtp_clock_origin_);
  uint32_t rtp_ts = static_cast<uint32_t>(elapsed.count() * 90 / 1000);

  if (!ConvertInput(frame)) {
    ++next_frame_id_;
    return false;
  }

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  const DWORD buf_len = static_cast<DWORD>(input_frame_buf_.size());
  if (FAILED(MFCreateMemoryBuffer(buf_len, &buffer))) {
    ++next_frame_id_;
    return false;
  }
  BYTE* dst = nullptr;
  if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) {
    ++next_frame_id_;
    return false;
  }
  std::memcpy(dst, input_frame_buf_.data(), buf_len);
  buffer->Unlock();
  buffer->SetCurrentLength(buf_len);

  Microsoft::WRL::ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample))) {
    ++next_frame_id_;
    return false;
  }
  sample->AddBuffer(buffer.Get());
  int64_t pts_100ns = elapsed.count() * 10;
  sample->SetSampleTime(pts_100ns);
  sample->SetSampleDuration(10000000LL / std::max(config_.framerate, 1));

  if (want_key && codec_api_) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
  }

  HRESULT hr = mft_->ProcessInput(input_stream_id_, sample.Get(), 0);
  if (want_key && codec_api_) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 0;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
  }
  if (hr == MF_E_NOTACCEPTING) {
    // Encoder's input queue is full — drain pending output and retry once.
    EncodedFrame drain_tmp;
    DrainOutput(drain_tmp);
    hr = mft_->ProcessInput(input_stream_id_, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    LOG_WARN << "MFT ProcessInput failed: hr=0x" << std::hex << hr << std::dec;
    force_keyframe_ = true;
    ++next_frame_id_;
    return false;
  }

  out_encoded_frame.dependency = FrameDependency::kDependent;
  out_encoded_frame.data.clear();
  if (!DrainOutput(out_encoded_frame)) {
    // The MFT buffered the input (latency warmup); no packet this call.
    ++next_frame_id_;
    return false;
  }
  ++next_frame_id_;

  bool is_key = out_encoded_frame.dependency == FrameDependency::kKeyFrame;
  if (want_key && !is_key) {
    LOG_WARN << "Forced IDR request did not produce a keyframe (frame " << current_fid << ")";
  }
  if (is_key) {
    last_key_frame_id_ = current_fid;
  }

  out_encoded_frame.frame_id = current_fid;
  out_encoded_frame.referenced_frame_id = is_key ? current_fid : (current_fid - 1);
  out_encoded_frame.rtp_timestamp = rtp_ts;
  out_encoded_frame.capture_time = frame.timestamp;
  int delay_ms = config_.playout_delay_ms > 0 ? config_.playout_delay_ms : 200;
  out_encoded_frame.playout_delay = std::chrono::milliseconds(delay_ms);
  return true;
#endif
}

void MediaFoundationVideoEncoder::ForceKeyFrame() {
  force_keyframe_ = true;
}

void MediaFoundationVideoEncoder::SetBitrate(uint32_t bitrate_kbps) {
  config_.bitrate_kbps = bitrate_kbps;
#if defined(_WIN32)
  if (codec_api_) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = bitrate_kbps * 1000;
    codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
  }
#endif
}

void MediaFoundationVideoEncoder::SetFramerate(int fps) {
  if (fps > 0) config_.framerate = fps;
}

void MediaFoundationVideoEncoder::SetClockOrigin(std::chrono::steady_clock::time_point origin) {
  rtp_clock_origin_ = origin;
  rtp_clock_origin_set_ = true;
}

}  // namespace castcore
