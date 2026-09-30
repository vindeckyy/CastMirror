#include "castcore/audio_capture_wasapi.h"
#include "castcore/logger.h"
#include "castcore/win_time.h"

#include <chrono>
#include <cstring>
#include <cmath>

#if defined(_WIN32)
#include <algorithm>
#include <functiondiscoverykeys_devpkey.h>
#endif

namespace castcore {

#if defined(_WIN32)
namespace {
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally so we don't need ksuser.lib.
const GUID kSubformatIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// Idle wake cadence of the capture loop. When the audio endpoint is idle no
// capture event arrives, so the loop wakes on this timeout and emits the
// silence that has actually elapsed since the last real capture, keeping the
// audio RTP timeline on wall time. (Windows' default timer granularity makes
// the wake itself late by a few ms; the emitted silence follows the clock,
// not the wake count.)
//
// This is only a poll interval, never the engine's period: the device picks
// that during Initialize and it is logged from GetStreamLatency(). A period
// longer than this timeout is harmless, because the idle branch measures
// silence from the clock and emits nothing while the last buffer's stamp is
// still ahead of "now" (the normal case right after a real capture).
constexpr DWORD kIdleWakeMs = 10;
}  // namespace
#endif

WasapiAudioCapture::WasapiAudioCapture() = default;

WasapiAudioCapture::~WasapiAudioCapture() {
  Stop();
}

void WasapiAudioCapture::SetAudioCallback(AudioCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void WasapiAudioCapture::SetHostSilence(bool silence) {
  // Many Windows audio drivers implement loopback as a copy of the audio
  // engine output *after* the endpoint volume stage, so zeroing the volume
  // would make the capture stream pure silence. The option is therefore not
  // implemented here: capture stays faithful to what the system is playing,
  // and the caller is told instead of being silently ignored.
  if (silence) {
    LOG_WARN << "Host speaker silencing ignored on Windows: it can mute WASAPI loopback capture";
  }
}

bool WasapiAudioCapture::Start(int sample_rate, int channels) {
  Stop();
  output_rate_ = sample_rate > 0 ? sample_rate : 48000;
  output_channels_ = channels > 0 ? channels : 2;

#if !defined(_WIN32)
  LOG_ERROR << "WasapiAudioCapture is only available on Windows";
  return false;
#else
  init_done_ = false;
  init_ok_ = false;
  running_ = true;
  worker_thread_ = std::thread(&WasapiAudioCapture::CaptureThreadMain, this);

  // Wait for the worker to finish WASAPI init so Start() reports real success.
  // Generous cap: device activation can block briefly on a busy audio engine.
  for (int i = 0; i < 200 && !init_done_.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!init_done_.load() || !init_ok_) {
    running_ = false;
    if (worker_thread_.joinable()) worker_thread_.join();
    return false;
  }
  LOG_INFO << "Started WASAPI Loopback Audio Capture (device " << src_rate_
           << " Hz/" << src_channels_ << "ch -> " << output_rate_
           << " Hz/" << output_channels_ << "ch s16)";
  return true;
#endif
}

void WasapiAudioCapture::Stop() {
  running_ = false;
#if defined(_WIN32)
  if (capture_event_) SetEvent(capture_event_);
#endif
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

bool WasapiAudioCapture::IsCapturing() const {
  return running_.load();
}

#if defined(_WIN32)
bool WasapiAudioCapture::InitOnThread() {
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: MMDeviceEnumerator failed: hr=0x" << std::hex << hr << std::dec;
    return false;
  }

  Microsoft::WRL::ComPtr<IMMDevice> device;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: no default render endpoint: hr=0x" << std::hex << hr << std::dec;
    return false;
  }

  Microsoft::WRL::ComPtr<IAudioClient> audio_client;
  hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_client);
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: IAudioClient activation failed: hr=0x" << std::hex << hr << std::dec;
    return false;
  }

  // Shared-mode loopback only accepts the device's own mix format —
  // requesting a custom PCM format fails with AUDCLNT_E_UNSUPPORTED_FORMAT.
  WAVEFORMATEX* mix = nullptr;
  hr = audio_client->GetMixFormat(&mix);
  if (FAILED(hr) || !mix) {
    LOG_ERROR << "WASAPI: GetMixFormat failed: hr=0x" << std::hex << hr << std::dec;
    return false;
  }

  src_rate_ = mix->nSamplesPerSec;
  src_channels_ = mix->nChannels;
  src_is_float_ = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
  if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
    src_is_float_ = IsEqualGUID(ext->SubFormat, kSubformatIeeeFloat);
  }
  if (!src_is_float_ && mix->wBitsPerSample != 16) {
    LOG_WARN << "WASAPI: unexpected mix format tag=" << mix->wFormatTag
             << " bits=" << mix->wBitsPerSample << " — treating as s16";
  }
  LOG_INFO << "WASAPI loopback format: src " << src_rate_ << "Hz/" << src_channels_
           << "ch/" << (src_is_float_ ? "float32" : "s16") << " -> out "
           << output_rate_ << "Hz/" << output_channels_ << "ch (src_is_float="
           << (src_is_float_ ? 1 : 0) << ")";

  capture_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!capture_event_) {
    CoTaskMemFree(mix);
    return false;
  }

  // hnsBufferDuration is only a suggestion in shared mode (0 lets the engine
  // pick the period), so passing a fixed 50 ms would not guarantee the ~10 ms
  // event cadence the capture loop wakes on. Ask for the engine default and
  // log the period it actually chose below.
  hr = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
      0, 0, mix, nullptr);
  CoTaskMemFree(mix);
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: loopback Initialize failed: hr=0x" << std::hex << hr << std::dec;
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
    return false;
  }

  // Report the period the engine settled on. The capture-event cadence (and
  // how often the idle silence path can run) follows it, not kIdleWakeMs.
  REFERENCE_TIME stream_latency = 0;
  if (SUCCEEDED(audio_client->GetStreamLatency(&stream_latency))) {
    LOG_INFO << "WASAPI: engine stream latency " << (stream_latency / 10000.0)
             << " ms (idle wake poll " << kIdleWakeMs << " ms)";
  }

  hr = audio_client->SetEventHandle(capture_event_);
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: SetEventHandle failed: hr=0x" << std::hex << hr << std::dec;
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
    return false;
  }

  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client;
  hr = audio_client->GetService(IID_PPV_ARGS(&capture_client));
  if (FAILED(hr)) {
    LOG_ERROR << "WASAPI: IAudioCaptureClient failed: hr=0x" << std::hex << hr << std::dec;
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
    return false;
  }

  // Stash the client objects as member state so the capture loop can use them;
  // both are only ever touched on this thread.
  audio_client_ = audio_client;
  capture_client_ = capture_client;
  return true;
}

void WasapiAudioCapture::EmitSilenceFrames(int samples, std::chrono::steady_clock::time_point ts) {
  if (pending_.empty()) {
    pending_start_ts_ = ts;
    pending_has_start_ = true;
  }
  pending_.insert(pending_.end(), static_cast<size_t>(samples) * output_channels_, 0);
  FlushPending();
}

void WasapiAudioCapture::FlushPending() {
  const size_t frame_samples = static_cast<size_t>(FrameSamples());  // 10 ms
  const size_t frame_values = frame_samples * output_channels_;
  // A frame's duration must equal its sample count over the output rate; do
  // not integer-divide the rate first (1000000 / 48000 * 480 truncates to
  // 9600 us, making the timeline 4% slow whenever the pending queue is not
  // re-anchored by fresh capture stamps).
  const auto frame_duration = FrameDuration();
  while (pending_.size() >= frame_values) {
    CapturedAudioFrame frame;
    frame.sample_rate = output_rate_;
    frame.channels = output_channels_;
    frame.samples_per_channel = static_cast<int>(frame_samples);
    if (!pending_has_start_) {
      pending_start_ts_ = std::chrono::steady_clock::now();
      pending_has_start_ = true;
    }
    frame.timestamp = pending_start_ts_;
    // Advance the timeline by exactly one frame so audio RTP timestamps track
    // the capture clock sample-for-sample (avoids per-frame jitter of ~10ms
    // that would otherwise show up as A/V sync wobble).
    pending_start_ts_ += frame_duration;
    frame.pcm_data.resize(frame_values * sizeof(int16_t));
    std::memcpy(frame.pcm_data.data(), pending_.data(), frame_values * sizeof(int16_t));
    pending_.erase(pending_.begin(), pending_.begin() + frame_values);
    frames_window_++;

    AudioCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cb = callback_;
    }
    if (cb) cb(frame);
  }
  if (pending_.empty()) {
    pending_has_start_ = false;
  }
}

void WasapiAudioCapture::TrackLevel(float peak_s16, double buffer_age_ms) {
  peak_window_ = std::max(peak_window_, peak_s16);
  age_sum_ms_ += buffer_age_ms;
  age_samples_++;
  LogLevelWindow(std::chrono::steady_clock::now());
}

void WasapiAudioCapture::LogLevelWindow(std::chrono::steady_clock::time_point now) {
  if (last_level_log_.time_since_epoch().count() == 0) {
    last_level_log_ = now;
    return;
  }
  if (now - last_level_log_ < std::chrono::seconds(5)) {
    return;
  }
  const double elapsed_s = std::max(
      0.001, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_level_log_).count());
  LOG_DEBUG << "WASAPI loopback audio peak (last 5s, s16 scale): "
            << static_cast<int>(peak_window_)
            << ", buffer age avg: " << (age_sum_ms_ / std::max(1, age_samples_)) << " ms"
            << ", frames/s: " << (frames_window_ / elapsed_s)
            << ", capture buffers/s: " << (buffers_window_ / elapsed_s)
            << ", pending: " << pending_.size() << " values";
  peak_window_ = 0.0f;
  age_sum_ms_ = 0.0;
  age_samples_ = 0;
  frames_window_ = 0;
  buffers_window_ = 0;
  last_level_log_ = now;
}

void WasapiAudioCapture::ConvertAndEmit(const BYTE* data, UINT32 frames, bool silent,
                                        std::chrono::steady_clock::time_point buffer_ts) {
  if (frames == 0) return;
  buffers_window_++;
  if (silent) {
    // Produce the equivalent output-frame count of silence.
    int out_frames = static_cast<int>(
        (static_cast<double>(frames) * output_rate_) / src_rate_ + 0.5);
    EmitSilenceFrames(out_frames, buffer_ts);
    return;
  }
  if (!data) return;
  if (pending_.empty()) {
    // First sample in the queue defines the capture time of the frame chunk
    // that FlushPending will emit.
    pending_start_ts_ = buffer_ts;
    pending_has_start_ = true;
  }
  const double buffer_age_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
          std::chrono::steady_clock::now() - buffer_ts)
          .count();

  auto sample_at = [&](UINT32 frame, int ch) -> float {
    if (src_is_float_) {
      return reinterpret_cast<const float*>(data)[static_cast<size_t>(frame) * src_channels_ + ch];
    }
    return reinterpret_cast<const int16_t*>(data)[static_cast<size_t>(frame) * src_channels_ + ch] / 32768.0f;
  };

  // Downmix to stereo (or mono->stereo duplicate). For >2ch sources take the
  // front L/R pair — the engine only carries stereo.
  const int ch_l = 0;
  const int ch_r = src_channels_ > 1 ? 1 : 0;

  if (src_rate_ == output_rate_ && src_channels_ == output_channels_ &&
      output_channels_ == 2) {
    if (!src_is_float_) {
      // Fast path: device already mixes 48k s16 stereo.
      const int16_t* s16 = reinterpret_cast<const int16_t*>(data);
      const size_t count = static_cast<size_t>(frames) * 2;
      float peak = 0.0f;
      for (size_t i = 0; i < count; ++i) {
        peak = std::max(peak, std::abs(static_cast<float>(s16[i])));
      }
      pending_.insert(pending_.end(), s16, s16 + count);
      resample_pos_ = 0.0;
      TrackLevel(peak, buffer_age_ms);
      FlushPending();
      return;
    }
    // Fast path: float32 stereo at the output rate — just clamp and convert.
    const float* f32 = reinterpret_cast<const float*>(data);
    const size_t count = static_cast<size_t>(frames) * 2;
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      const float v = std::max(-1.0f, std::min(1.0f, f32[i]));
      peak = std::max(peak, std::abs(v));
      pending_.push_back(static_cast<int16_t>(v * 32767.0f));
    }
    resample_pos_ = 0.0;
    TrackLevel(peak * 32767.0f, buffer_age_ms);
    FlushPending();
    return;
  }

  // Linear-interpolation resampler from src_rate_ to output_rate_.
  const double step = static_cast<double>(src_rate_) / output_rate_;
  double pos = resample_pos_;
  // A packet can leave a negative phase carry (the loop exits on the final
  // sample when downsampling or at exactly 1:1). Clamp into [0, step) so the
  // unsigned sample-index cast below can never wrap to a huge value.
  if (!(pos >= 0.0) || pos >= step) {
    pos = 0.0;
  }
  const double frame_count = static_cast<double>(frames);
  float peak = 0.0f;
  while (pos + 1.0 < frame_count) {
    const UINT32 i0 = static_cast<UINT32>(pos);
    const double frac = pos - static_cast<double>(i0);
    float l = static_cast<float>(sample_at(i0, ch_l) * (1.0 - frac) + sample_at(i0 + 1, ch_l) * frac);
    float r = static_cast<float>(sample_at(i0, ch_r) * (1.0 - frac) + sample_at(i0 + 1, ch_r) * frac);
    l = std::max(-1.0f, std::min(1.0f, l));
    r = std::max(-1.0f, std::min(1.0f, r));
    peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
    pending_.push_back(static_cast<int16_t>(l * 32767.0f));
    pending_.push_back(static_cast<int16_t>(r * 32767.0f));
    pos += step;
  }
  TrackLevel(peak * 32767.0f, buffer_age_ms);
  resample_pos_ = pos - frame_count;
  if (resample_pos_ < 0.0) {
    resample_pos_ = 0.0;
  } else if (resample_pos_ >= step) {
    resample_pos_ = std::fmod(resample_pos_, step);
  }
  FlushPending();
}

void WasapiAudioCapture::CleanupOnThread() {
  if (audio_client_) {
    audio_client_->Stop();
  }
  capture_client_.Reset();
  audio_client_.Reset();
  if (capture_event_) {
    CloseHandle(capture_event_);
    capture_event_ = nullptr;
  }
  pending_.clear();
  resample_pos_ = 0.0;
}
#endif

void WasapiAudioCapture::CaptureThreadMain() {
#if defined(_WIN32)
  // The worker thread owns COM for the capture client; initializing COM on the
  // caller thread (as before) left capture_client_ calls on an uninitialized
  // apartment.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_held = SUCCEEDED(hr);

  init_ok_ = InitOnThread();
  // A failed setup publishes right away so Start() doesn't wait out its cap;
  // a successful one is published below, only once Start()'s outcome is
  // known, so the caller never reports success for a stream that never ran.
  if (!init_ok_) init_done_ = true;

  if (init_ok_) {
    // Start() can still fail after GetService succeeded: the endpoint may be
    // invalidated in between (unplugged, switched, reconfigured). Ignoring the
    // HRESULT would leave the loop below waiting on an event that never fires
    // while the idle path keeps emitting silence — a dead stream that looks
    // healthy and never recovers. Leave the loop instead, so CleanupOnThread()
    // runs and the caller sees capture stopped.
    hr = audio_client_->Start();
    const bool started = SUCCEEDED(hr);
    if (!started) {
      LOG_ERROR << "WASAPI: IAudioClient::Start failed: hr=0x" << std::hex << hr
                << (hr == AUDCLNT_E_DEVICE_INVALIDATED ? " (AUDCLNT_E_DEVICE_INVALIDATED)" : "")
                << std::dec;
      // No retry: Start() cannot revive an invalidated client — recovery needs
      // a fresh Activate()/Initialize() against the current endpoint.
      init_ok_ = false;
    }
    init_done_ = true;

    while (started && running_) {
      DWORD wait = WaitForSingleObject(capture_event_, kIdleWakeMs);
      if (wait == WAIT_TIMEOUT) {
        // Endpoint idle: WASAPI delivers no packets, so emit exactly the
        // silence needed to keep the emission timeline on wall time. After a
        // real buffer the flushed frame stamps are ~10-20 ms ahead of now
        // (buffer age is negative), so a timeout that merely raced a live
        // capture event emits nothing — otherwise silence would be added on
        // top of the real audio and the receiver's byte queue would grow
        // without bound. In genuine idle the 10 ms frame grid falls behind the
        // ~10.5 ms wake cadence and whole frames are emitted to catch up, so
        // the long-run rate stays exactly one frame per frame duration.
        const auto now = std::chrono::steady_clock::now();
        if (pending_start_ts_.time_since_epoch().count() == 0) {
          // Session start only: never anchor in the past.
          pending_start_ts_ = now;
        }
        const int frame_samples = FrameSamples();
        const auto frame_duration = FrameDuration();
        int64_t frames = 0;
        if (now > pending_start_ts_) {
          frames = std::chrono::duration_cast<std::chrono::microseconds>(now - pending_start_ts_)
                       .count() /
                   frame_duration.count();
          if (frames > 5) {
            // Long stall: the missed silence is gone. Cap at 50 ms and let the
            // timeline skip ahead rather than burst a backlog.
            pending_start_ts_ = now - 5 * frame_duration;
            frames = 5;
          }
        }
        // Single call: FlushPending splits the inserted silence into separate
        // 10 ms frames (stamped pending_start_ts_ + k*frame_duration) and
        // already advances pending_start_ts_ by frames*frame_duration, so the
        // timeline must not be advanced again here.
        EmitSilenceFrames(static_cast<int>(frames) * frame_samples, pending_start_ts_);
        LogLevelWindow(now);
        continue;
      }
      if (wait != WAIT_OBJECT_0) break;

      UINT32 packet_length = 0;
      if (FAILED(capture_client_->GetNextPacketSize(&packet_length))) break;
      while (packet_length > 0) {
        BYTE* data = nullptr;
        UINT32 num_frames = 0;
        DWORD flags = 0;
        UINT64 qpc_position = 0;
        // pu64QPCPosition is the QPC time at which the first sample in the
        // buffer was rendered. Using it (instead of "now") makes the audio
        // RTP timestamp reflect the source timeline, matching the video
        // frame's DXGI LastPresentTime.
        hr = capture_client_->GetBuffer(&data, &num_frames, &flags, nullptr, &qpc_position);
        if (FAILED(hr)) break;
        // TIMESTAMP_ERROR means qpc_position cannot be trusted (glitch or
        // resync); projecting the RTP timeline from it would be wrong, so fall
        // back to "now" like the cursor-only video path does.
        const bool ts_bad = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        auto buffer_ts = (qpc_position != 0 && !ts_bad)
                             ? QpcTicksToSteadyClock(qpc_position)
                             : std::chrono::steady_clock::now();
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
          // The engine dropped or reordered frames, so the queued samples no
          // longer continue into this buffer: the projection state
          // (pending_start_ts_ / resample_pos_) belongs to a timeline that
          // ended. Re-anchor it to this buffer's own stamp instead of dropping
          // the queue, which would strand a source whose packet converts to
          // less than one frame. Rate limited (5s window, like the level
          // diagnostics): a struggling endpoint sets this on every packet.
          const auto now = std::chrono::steady_clock::now();
          if (last_discontinuity_log_.time_since_epoch().count() == 0 ||
              now - last_discontinuity_log_ >= std::chrono::seconds(5)) {
            LOG_WARN << "WASAPI: loopback data discontinuity — re-anchoring audio timeline";
            last_discontinuity_log_ = now;
          }
          if (pending_.empty()) {
            // An empty queue anchors natively: ConvertAndEmit's first-sample
            // path stamps it at buffer_ts.
            pending_has_start_ = false;
          } else {
            // Restamp the partial queue to end where this buffer starts, so
            // the frames that follow project from buffer_ts, not the old
            // anchor.
            const int64_t queued_us =
                1000000LL * static_cast<int64_t>(pending_.size()) /
                (static_cast<int64_t>(output_channels_) * output_rate_);
            pending_start_ts_ = buffer_ts - std::chrono::microseconds(queued_us);
            pending_has_start_ = true;
          }
          resample_pos_ = 0.0;
        }
        ConvertAndEmit(data, num_frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, buffer_ts);
        capture_client_->ReleaseBuffer(num_frames);
        if (FAILED(capture_client_->GetNextPacketSize(&packet_length))) break;
      }
    }
  }

  CleanupOnThread();
  running_ = false;
  if (com_held) CoUninitialize();
#else
  running_ = false;
#endif
}

}  // namespace castcore
