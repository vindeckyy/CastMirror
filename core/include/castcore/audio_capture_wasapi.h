#ifndef CASTCORE_AUDIO_CAPTURE_WASAPI_H_
#define CASTCORE_AUDIO_CAPTURE_WASAPI_H_

#include "castcore/audio_capture.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#endif

namespace castcore {

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
  int output_rate_ = 48000;
  int output_channels_ = 2;

  // Resampler state: fractional read position in source frames.
  double resample_pos_ = 0.0;
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
