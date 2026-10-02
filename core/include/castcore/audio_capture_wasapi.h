#ifndef CASTCORE_AUDIO_CAPTURE_WASAPI_H_
#define CASTCORE_AUDIO_CAPTURE_WASAPI_H_

#include "castcore/audio_capture.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <cmath>
#include <vector>

#if defined(_WIN32)
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#endif

namespace castcore {
// Walks one packet of source audio through a linear-interpolation resampler,
// carrying the fractional read position across packets. SampleAt reads the
// source frame at index i and writes L/R into the out parameters.
//
// Returns how many output frames were appended to out (one L/R pair each).
// phase is the fractional read position in source frames, in (-1, step): a
// negative value means the current output sample started in the previous
// packet and is finished here from the sample it retained. Discarding that
// negative carry instead of finishing the sample loses a fraction of a source
// sample per packet, which shows up as the audio clock drifting away from the
// video clock.
//
// Keeping this a free function rather than inline in the capture thread is
// what makes the rate arithmetic testable without a live audio endpoint.
struct ResamplerCarry {
  // Fractional read position, carried between packets.
  double pos = 0.0;
  // Last sample of the previous packet, and whether it is valid.
  bool have_tail = false;
  float tail_l = 0.0f;
  float tail_r = 0.0f;
};

template <typename SampleAt>
int ResamplePacket(int src_rate, int out_rate, int frames, ResamplerCarry& carry,
                   SampleAt&& sample_at, std::vector<int16_t>& out) {
  const double step = static_cast<double>(src_rate) / out_rate;
  double pos = carry.pos;
  // -1.0 is a valid carry: it means a whole output sample still needs the
  // previous packet's last value. Clamping it away loses one sample per packet
  // whenever the ratio is a whole number (a 48 kHz 24-bit or 32-bit endpoint),
  // which is 0.21% of the audio and drifts the audio clock against video.
  if (pos < -1.0 || !(pos < step)) {
    pos = 0.0;
  }
  const double frame_count = static_cast<double>(frames);
  int produced = 0;
  while (pos + 1.0 < frame_count) {
    float l0, r0, l1, r1;
    double frac;
    if (pos < 0.0) {
      if (!carry.have_tail) {
        pos += step;
        continue;
      }
      l0 = carry.tail_l;
      r0 = carry.tail_r;
      sample_at(0, l1, r1);
      frac = pos + 1.0;
    } else {
      const int i0 = static_cast<int>(pos);
      frac = pos - static_cast<double>(i0);
      sample_at(i0, l0, r0);
      sample_at(i0 + 1, l1, r1);
    }
    float l = static_cast<float>(l0 * (1.0 - frac) + l1 * frac);
    float r = static_cast<float>(r0 * (1.0 - frac) + r1 * frac);
    l = std::max(-1.0f, std::min(1.0f, l));
    r = std::max(-1.0f, std::min(1.0f, r));
    out.push_back(static_cast<int16_t>(l * 32767.0f));
    out.push_back(static_cast<int16_t>(r * 32767.0f));
    ++produced;
    pos += step;
  }
  carry.pos = pos - frame_count;
  if (carry.pos < -1.0) {
    carry.pos = 0.0;
  } else if (carry.pos >= step) {
    carry.pos = std::fmod(carry.pos, step);
  }
  // Retain this packet's final sample so a carry that lands before the next
  // packet's first sample can be interpolated rather than discarded.
  if (frames > 0) {
    sample_at(frames - 1, carry.tail_l, carry.tail_r);
    carry.have_tail = true;
  }
  return produced;
}

// WASAPI shared-mode loopback capture. Captures the default render device's
// output mix in its native format (usually 32-bit float) and converts to the
// engine's contract: 48 kHz stereo S16LE, delivered in 10 ms frames.
//
// All COM objects are created, used and released on the worker thread — the
// WASAPI interfaces are apartment-bound and must not be touched from the
// caller's thread without marshaling.
class WasapiAudioCapture : public IAudioCapture {
 public:
  WasapiAudioCapture();
  ~WasapiAudioCapture() override;

  bool Start(int sample_rate = 48000, int channels = 2) override;
  void Stop() override;
  bool IsCapturing() const override;
  void SetHostSilence(bool silence) override;
  void SetTargetProcess(uint32_t pid) override { target_pid_ = pid; }
  void SetAudioCallback(AudioCallback callback) override;

 private:
  void CaptureThreadMain();
#if defined(_WIN32)
  // Runs on the worker thread. Returns false if loopback capture could not be
  // set up on the default render endpoint.
  bool InitOnThread();
  // Per-application capture through the Windows 10 2004+ "process loopback" virtual
  // device. Fails, and the caller falls back to nothing, on older Windows.
  bool InitProcessLoopback();
  void CleanupOnThread();
  // Resample/downmix one packet of device-format audio into pending_ (S16
  // stereo at output_rate_) and flush complete 10 ms frames to the callback.
  // buffer_ts is the QPC-based time at which this packet was rendered, mapped
  // onto the steady_clock timeline.
  void ConvertAndEmit(const BYTE* data, UINT32 frames, bool silent,
                      std::chrono::steady_clock::time_point buffer_ts);
  // One source sample as a float in [-1, 1], for any supported device format.
  float ReadSample(const BYTE* data, size_t index) const;
  // Re-opens the loopback stream on the current default endpoint after the
  // default device changed or the old one was invalidated (headphones plugged
  // in, HDMI sink switched). Keeps the callback and timeline; returns false if
  // no endpoint could be opened within the retry budget.
  bool RestartOnThread();
  void ReleaseStreamOnThread();
  void EmitSilenceFrames(int samples, std::chrono::steady_clock::time_point ts);
  void FlushPending();
  // Samples per channel in one emitted frame (10 ms at output_rate_) and that
  // frame's exact wall-clock duration (sample count over rate, no integer
  // truncation). The idle silence path and FlushPending both advance the
  // timeline with these, so the two can never disagree.
  int FrameSamples() const { return output_rate_ / 100; }
  std::chrono::microseconds FrameDuration() const {
    return std::chrono::microseconds(1000000LL * FrameSamples() / output_rate_);
  }
  // Tracks the recent capture peak and buffer age; logs at debug level every
  // ~5s so "no audio on the TV" can be told apart from "no audio at the
  // source", and so the QPC-based timestamp can be sanity-checked.
  void TrackLevel(float peak_s16, double buffer_age_ms);
  // Emits that ~5s diagnostics window when it is due. Called from TrackLevel
  // and from the idle silence path, so the emitted-frames rate is visible even
  // when the endpoint delivers no capture packets at all.
  void LogLevelWindow(std::chrono::steady_clock::time_point now);
#endif

  uint32_t target_pid_ = 0;  // 0 = whole system
  int output_rate_ = 48000;
  int output_channels_ = 2;
  std::atomic<bool> running_{false};
  std::atomic<bool> init_done_{false};
  bool init_ok_ = false;
  std::thread worker_thread_;
  std::mutex mutex_;
  AudioCallback callback_;

#if defined(_WIN32)
  // Owned by the worker thread; never touched from other threads.
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client_;

  HANDLE capture_event_ = nullptr;

  // Device mix format (owned by the worker thread).
  enum class SrcFormat { kFloat32, kInt16, kInt24, kInt32 };
  SrcFormat src_format_ = SrcFormat::kFloat32;
  bool src_is_float_ = false;
  // Set by the endpoint notification client (any thread) when the default
  // render device changes; the capture loop then re-opens the stream.
  std::atomic<bool> device_changed_{false};
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IMMNotificationClient> notifier_;
  int src_rate_ = 48000;
  int src_channels_ = 2;

  // Resampler phase carried between packets, including the retained tail
  // sample that finishes an output sample spanning a packet boundary.
  ResamplerCarry carry_;
  // Accumulated converted S16 stereo samples awaiting a full 10 ms frame.
  std::vector<int16_t> pending_;
  // Capture time of the first sample currently in pending_.
  std::chrono::steady_clock::time_point pending_start_ts_{};
  bool pending_has_start_ = false;
  float peak_window_ = 0.0f;
  double age_sum_ms_ = 0.0;
  int age_samples_ = 0;
  std::chrono::steady_clock::time_point last_level_log_{};
  // Rate limit for the data-discontinuity warning: the flag can be set on
  // every packet while the endpoint struggles, so it logs at most once per 5s.
  std::chrono::steady_clock::time_point last_discontinuity_log_{};
  // Diagnostics for the same ~5s window: frames emitted to the callback and
  // capture buffers processed. Reset whenever the window is logged.
  int frames_window_ = 0;
  int buffers_window_ = 0;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_AUDIO_CAPTURE_WASAPI_H_
