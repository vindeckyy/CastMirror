#include "castcore/video_encoder_mf.h"
#include "castcore/logger.h"

#if defined(_WIN32)
#include <codecapi.h>
#include <mferror.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
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
  gpu_src_w_ = gpu_src_h_ = 0;
  // The reusable buffers belong to the stream that just ended; holding them
  // across a reconfigure would keep a full frame of memory pinned for a
  // resolution the next stream may not use.
  input_buffer_.Reset();
  input_sample_.Reset();
  input_buffer_size_ = 0;
  output_sample_.Reset();
  output_buffer_size_ = 0;
  have_output_info_ = false;
  output_provides_samples_ = false;
  async_mft_ = false;
  event_gen_.Reset();
  need_input_ = have_output_ = 0;
  sequence_header_.clear();
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
  // The bitstream is BT.709 limited range, matching the RGB-to-YUV conversion.
  mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
  mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
  mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
  mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

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
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

    if (SUCCEEDED(mft_->SetInputType(input_stream_id_, mt.Get(), 0))) {
      input_subtype_ = fmt;
      return true;
    }
  }
  return false;
}

void MediaFoundationVideoEncoder::EnableAsyncMode() {
  async_mft_ = false;
  event_gen_.Reset();
  need_input_ = have_output_ = 0;
  Microsoft::WRL::ComPtr<IMFAttributes> attrs;
  if (FAILED(mft_->GetAttributes(&attrs)) || !attrs) return;
  UINT32 is_async = 0;
  if (FAILED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) || !is_async) return;
  // Without this, the first ProcessInput fails with
  // MF_E_TRANSFORM_ASYNC_MFT_NOT_SUPPORTED and the stream never produces video.
  attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
  if (SUCCEEDED(mft_.As(&event_gen_)) && event_gen_) {
    async_mft_ = true;
  }
}

void MediaFoundationVideoEncoder::PumpEvents(int wait_ms) {
  if (!event_gen_) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  for (;;) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
    HRESULT hr = event_gen_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
    if (SUCCEEDED(hr) && ev) {
      MediaEventType type = MEUnknown;
      ev->GetType(&type);
      if (type == METransformNeedInput) ++need_input_;
      else if (type == METransformHaveOutput) ++have_output_;
      continue;  // keep draining whatever is queued
    }
    if (hr != MF_E_NO_EVENTS_AVAILABLE) return;  // generator shut down or failed
    if (wait_ms <= 0 || need_input_ > 0 || have_output_ > 0 ||
        std::chrono::steady_clock::now() >= deadline) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void MediaFoundationVideoEncoder::CaptureSequenceHeader() {
  sequence_header_.clear();
  Microsoft::WRL::ComPtr<IMFMediaType> mt;
  if (FAILED(mft_->GetOutputCurrentType(output_stream_id_, &mt)) || !mt) return;
  UINT32 size = 0;
  if (FAILED(mt->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return;
  std::vector<uint8_t> blob(size);
  if (SUCCEEDED(mt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), size, nullptr))) {
    sequence_header_ = std::move(blob);
  }
}

namespace {
// True when the Annex-B buffer carries an SPS (NAL type 7) in its first bytes.
bool HasSpsNal(const uint8_t* d, size_t n) {
  const size_t limit = std::min<size_t>(n, 256);
  for (size_t i = 0; i + 3 < limit; ++i) {
    if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
      const size_t nal = i + (d[i + 2] == 1 ? 3 : 4);
      if (nal < n && (d[nal] & 0x1F) == 7) return true;
    }
  }
  return false;
}
}  // namespace

bool MediaFoundationVideoEncoder::ConfigureMft(IMFActivate* activate) {
  mft_.Reset();
  codec_api_.Reset();
  if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&mft_))) || !mft_) return false;
  EnableAsyncMode();

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

    // No B-frames: the frame-dependency model (each frame references the
    // previous one) and the low-latency budget both assume in-order output.
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 0;
    codec_api_->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);

    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVEncCommonLowLatency, &v);

    if (config_.gop_size > 0) {
      VariantInit(&v);
      v.vt = VT_UI4;
      v.ulVal = static_cast<ULONG>(config_.gop_size);
      codec_api_->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }
  }

  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  CaptureSequenceHeader();
  return true;
}
#endif

bool MediaFoundationVideoEncoder::Initialize(const VideoEncoderConfig& config) {
  Cleanup();
  config_ = config;
  // 4:2:0 subsamples chroma 2x2, so an odd width or height has no valid plane
  // layout: w/2 truncates, and every plane boundary lands mid-sample. Capture
  // already snaps crops to even, but a config or adaptive ladder can still hand
  // us an odd size, and IYUV's separate U/V planes would then be written
  // one byte past where the encoder reads them. Normalize once, here, so every
  // downstream size computation agrees.
  config_.width = std::max(2, config_.width & ~1);
  config_.height = std::max(2, config_.height & ~1);
  next_frame_id_ = 0;
  last_key_frame_id_ = 0;
  rtp_clock_origin_set_ = false;
  is_hardware_ = false;

#if !defined(_WIN32)
  LOG_ERROR << "MediaFoundationVideoEncoder is only available on Windows";
  return false;
#else
  // MFStartup/MFShutdown are process-wide, and MF's own refcount is the only
  // thing keeping a second MF user (another encoder, or a host app's own
  // Media Foundation code) alive. Pairing them per-encoder means an adaptive
  // bitrate change, which reopens the encoder, can drive the count to zero
  // while MFTs are still live in another thread, after which every MF call
  // fails. Start once for the process and never shut down: MF is designed to
  // be initialised for the lifetime of the process, and this file is the only
  // place in CastMirror that uses it.
  static std::once_flag mf_once;
  std::call_once(mf_once, [] {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      LOG_ERROR << "MFStartup failed; Media Foundation encoders are unavailable";
    }
  });

  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  // Hardware MFTs only. The inbox software H.264 MFT has a fixed ~14-frame
  // internal pipeline delay (≈470ms at 30fps), which is unusable for live
  // mirroring; when no hardware MFT is present we return false so the
  // composite encoder falls back to FFmpeg/libx264 (low latency).
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
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

  // Cache the output contract once: whether the MFT supplies its own samples
  // and how large a caller-allocated one must be are both fixed for the
  // configured type, so neither is re-queried per frame.
  RefreshOutputStreamInfo();
  if (!EnsureInputBuffer()) {
    LOG_ERROR << "Could not allocate the Media Foundation input buffer for "
              << config_.width << "x" << config_.height;
    Cleanup();
    return false;
  }

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
bool MediaFoundationVideoEncoder::ConvertInput(const CapturedVideoFrame& frame, uint8_t* dst) {
  if (dst == nullptr) return false;
  if (gpu_src_w_ != frame.width || gpu_src_h_ != frame.height) {
    if (!gpu_processor_.Initialize(frame.width, frame.height,
                                   config_.width, config_.height)) {
      LOG_ERROR << "GpuProcessor init failed for " << frame.width << "x" << frame.height;
      return false;
    }
    gpu_src_w_ = frame.width;
    gpu_src_h_ = frame.height;
  }

  uint8_t* y = dst;
  const int w = config_.width, h = config_.height;
  if (IsEqualGUID(input_subtype_, MFVideoFormat_NV12)) {
    return gpu_processor_.ConvertBgraToNv12(frame, y, w, y + static_cast<size_t>(w) * h, w);
  }
  // IYUV: Y plane + separate U and V planes.
  uint8_t* u = y + static_cast<size_t>(w) * h;
  uint8_t* v = u + static_cast<size_t>(w) * h / 4;
  return gpu_processor_.ConvertBgraToYuv420p(frame, y, w, u, w / 2, v, w / 2);
}

bool MediaFoundationVideoEncoder::EnsureInputBuffer() {
  const size_t needed = InputBufferSize();
  if (input_buffer_ && input_sample_ && input_buffer_size_ >= needed) return true;

  input_buffer_.Reset();
  input_sample_.Reset();
  input_buffer_size_ = 0;
  // A memory buffer is not required to be zeroed, but a stale tail would be
  // converted into pixels the encoder must still be told about; the MFT only
  // reads CurrentLength bytes, and the conversion below always fills the frame
  // plus its letterbox padding, so nothing uninitialised is exposed.
  if (FAILED(MFCreateMemoryBuffer(needed, &input_buffer_)) || !input_buffer_) return false;
  if (FAILED(MFCreateSample(&input_sample_)) || !input_sample_) {
    input_buffer_.Reset();
    return false;
  }
  if (FAILED(input_sample_->AddBuffer(input_buffer_.Get()))) {
    input_buffer_.Reset();
    input_sample_.Reset();
    return false;
  }
  input_buffer_size_ = needed;
  return true;
}

bool MediaFoundationVideoEncoder::RefreshOutputStreamInfo() {
  have_output_info_ = SUCCEEDED(mft_->GetOutputStreamInfo(output_stream_id_, &output_info_));
  // Assume the MFT provides samples when it will not say: that is the safe
  // default, because a caller-allocated sample on a providing MFT leaks a
  // reference the MFT never releases, whereas the reverse just costs a
  // slightly slower path.
  output_provides_samples_ =
      !have_output_info_ || (output_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
  // cbSize is the required capacity of a caller-allocated output sample. Zero
  // means "unspecified", so fall back to a generous frame-sized bound; an
  // undersized buffer is still handled by the MF_E_BUFFER_TOO_SMALL retry.
  if (!output_provides_samples_ && output_buffer_size_ == 0) {
    output_buffer_size_ = std::max<DWORD>(
        output_info_.cbSize, static_cast<DWORD>(config_.width * config_.height));
  }
  return have_output_info_;
}

bool MediaFoundationVideoEncoder::EnsureOutputBuffer() {
  if (output_sample_) return true;
  if (output_buffer_size_ == 0) output_buffer_size_ = 1024 * 1024;
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(MFCreateMemoryBuffer(output_buffer_size_, &buffer)) || !buffer) return false;
  Microsoft::WRL::ComPtr<IMFSample> sample;
  if (FAILED(MFCreateSample(&sample)) || !sample) return false;
  if (FAILED(sample->AddBuffer(buffer.Get()))) return false;
  output_sample_ = sample;
  return true;
}

bool MediaFoundationVideoEncoder::DrainOutput(EncodedFrame& out_encoded_frame) {
  // Encoder MFTs generally do not provide output samples (no
  // MFT_OUTPUT_STREAM_PROVIDES_SAMPLES), so ProcessOutput requires a
  // caller-allocated sample sized per GetOutputStreamInfo. Both the sizing and
  // the allocation are cached, so a steady stream allocates nothing per frame.
  Microsoft::WRL::ComPtr<IMFSample> mft_owned_sample;
  MFT_OUTPUT_DATA_BUFFER out{};
  out.dwStreamID = output_stream_id_;

  if (!output_provides_samples_) {
    if (!EnsureOutputBuffer()) return false;
    out.pSample = output_sample_.Get();
  }

  DWORD status = 0;
  HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);
  if (out.pEvents) CoTaskMemFree(out.pEvents);

  if (hr == MF_E_BUFFERTOOSMALL) {
    // MFT_OUTPUT_DATA_BUFFER carries no size field, and the transform does not
    // report the capacity it needs, so grow geometrically and retry. cbSize is
    // the documented bound and normally sufficient; this only covers an MFT
    // that under-reports it (or one very large first frame). Bounded so a
    // pathological transform cannot spin allocating.
    //
    // `out.pSample` is deliberately NOT reused after the retry: growing the
    // buffer releases the old sample, so that pointer would dangle. The retry
    // keeps its own output data buffer and only the winning sample is reported.
    Microsoft::WRL::ComPtr<IMFSample> produced;
    for (int attempt = 0; attempt < 3 && !output_provides_samples_; ++attempt) {
      const DWORD grown = output_buffer_size_ + output_buffer_size_ / 2;
      output_buffer_size_ = std::max<DWORD>(grown, output_buffer_size_ + 4096);
      output_sample_.Reset();
      if (!EnsureOutputBuffer()) return false;

      MFT_OUTPUT_DATA_BUFFER retry{};
      retry.dwStreamID = output_stream_id_;
      retry.pSample = output_sample_.Get();
      status = 0;
      hr = mft_->ProcessOutput(0, 1, &retry, &status);
      if (retry.pEvents) CoTaskMemFree(retry.pEvents);
      if (hr == MF_E_BUFFERTOOSMALL) continue;
      if (SUCCEEDED(hr) && retry.pSample) produced = retry.pSample;
      break;
    }
    if (hr == MF_E_BUFFERTOOSMALL) {
      LOG_WARN << "MFT output buffer still too small at " << output_buffer_size_
               << " bytes; dropping this frame";
      return false;
    }
    if (FAILED(hr) || !produced) return false;
    return CopyOutputSample(produced.Get(), out_encoded_frame);
  }

  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return false;
  }
  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // Re-select an available output type (e.g. after a bitrate change) and
    // refresh the output stream info — the buffer size may have changed.
    for (DWORD i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IMFMediaType> mt;
      if (FAILED(mft_->GetOutputAvailableType(output_stream_id_, i, &mt))) break;
      if (SUCCEEDED(mft_->SetOutputType(output_stream_id_, mt.Get(), 0))) break;
    }
    // The negotiated type can change the required size, so drop the cached
    // sample and re-derive it rather than writing into the old capacity.
    output_sample_.Reset();
    output_buffer_size_ = 0;
    RefreshOutputStreamInfo();
    return false;
  }
  if (FAILED(hr) || !out.pSample) {
    return false;
  }
  if (output_provides_samples_) {
    // The MFT allocated this sample; ProcessOutput handed us the reference, so
    // hold it until the copy below is done or it leaks.
    mft_owned_sample.Attach(out.pSample);  // adopt, don't AddRef: that would leak one sample per frame
  }
  return CopyOutputSample(out.pSample, out_encoded_frame);
}

bool MediaFoundationVideoEncoder::CopyOutputSample(IMFSample* sample,
                                                   EncodedFrame& out_encoded_frame) {
  if (sample == nullptr) return false;
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;
  BYTE* data = nullptr;
  DWORD len = 0;
  if (FAILED(buffer->Lock(&data, nullptr, &len)) || data == nullptr) return false;
  // One picture per call. Assigning rather than appending keeps a stray extra
  // output sample from being packed behind this frame's RTP timestamp.
  out_encoded_frame.data.assign(data, data + len);
  buffer->Unlock();
  UINT32 clean = 0;
  if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) {
    out_encoded_frame.dependency = FrameDependency::kKeyFrame;
    // A receiver that joins or recovers on this key frame needs SPS/PPS in the
    // bitstream. Some MFTs only publish them in the media type, so add them.
    if (!sequence_header_.empty() &&
        !HasSpsNal(out_encoded_frame.data.data(), out_encoded_frame.data.size())) {
      out_encoded_frame.data.insert(out_encoded_frame.data.begin(),
                                    sequence_header_.begin(), sequence_header_.end());
    }
  }
  return true;
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

  // The MFT copies the input sample during ProcessInput, so one buffer and one
  // sample serve every frame: the conversion writes straight into the locked
  // media buffer and there is no per-frame allocation or intermediate copy.
  // Frame ids are assigned only to frames that actually come out of the
  // encoder. Advancing the counter on a miss left a hole the next dependent
  // frame pointed at, so the receiver stalled until the next key frame. After
  // any miss the following frame is a key frame instead.
  if (!EnsureInputBuffer()) {
    force_keyframe_ = true;
    return false;
  }
  BYTE* dst = nullptr;
  if (FAILED(input_buffer_->Lock(&dst, nullptr, nullptr)) || dst == nullptr) {
    force_keyframe_ = true;
    return false;
  }
  const bool converted = ConvertInput(frame, dst);
  input_buffer_->Unlock();
  if (!converted) {
    force_keyframe_ = true;
    return false;
  }
  const DWORD buf_len = static_cast<DWORD>(InputBufferSize());
  if (FAILED(input_buffer_->SetCurrentLength(buf_len))) {
    force_keyframe_ = true;
    return false;
  }

  int64_t pts_100ns = elapsed.count() * 10;
  input_sample_->SetSampleTime(pts_100ns);
  input_sample_->SetSampleDuration(10000000LL / std::max(config_.framerate, 1));

  if (want_key && codec_api_) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = 1;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
  }

  if (async_mft_) {
    // The MFT announces readiness for input; submitting earlier is an error.
    PumpEvents(50);
    if (need_input_ <= 0) {
      LOG_WARN << "Async MFT did not request input within 50 ms";
      force_keyframe_ = true;
      return false;
    }
    --need_input_;
  }
  HRESULT hr = mft_->ProcessInput(input_stream_id_, input_sample_.Get(), 0);
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
    hr = mft_->ProcessInput(input_stream_id_, input_sample_.Get(), 0);
  }
  if (FAILED(hr)) {
    LOG_WARN << "MFT ProcessInput failed: hr=0x" << std::hex << hr << std::dec;
    force_keyframe_ = true;
    return false;
  }

  out_encoded_frame.dependency = FrameDependency::kDependent;
  out_encoded_frame.data.clear();
  if (async_mft_) {
    PumpEvents(50);
    if (have_output_ <= 0) {
      force_keyframe_ = true;  // input was consumed; make the next frame self-contained
      return false;
    }
    --have_output_;
  }
  if (!DrainOutput(out_encoded_frame)) {
    // The MFT buffered the input (latency warmup); no packet this call, and no
    // frame id is spent on it.
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
