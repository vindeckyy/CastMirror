#include "castcore/audio_capture.h"
#include "castcore/logger.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <chrono>
#include <cstring>

#if !defined(_WIN32)
  #include <pulse/simple.h>
  #include <pulse/error.h>
#endif

namespace castcore {

class SyntheticAudioCapture : public IAudioCapture {
 public:
  SyntheticAudioCapture() = default;
  ~SyntheticAudioCapture() override { Stop(); }

  bool Start(int sample_rate, int channels) override {
    Stop();
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;
    running_ = true;
    capture_thread_ = std::thread(&SyntheticAudioCapture::CaptureLoop, this);
    LOG_INFO << "Started Synthetic Audio Capture (" << sample_rate_ << " Hz, " << channels_ << " channels)";
    return true;
  }

  void Stop() override {
    if (!running_.exchange(false)) return;
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  void SetAudioCallback(AudioCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

 private:
  void CaptureLoop() {
    // 10ms frame size = 480 samples @ 48kHz
    int samples_per_frame = sample_rate_ / 100;
    auto frame_duration = std::chrono::microseconds(10000);
    double phase = 0.0;
    double freq = 440.0; // 440 Hz standard A tone (very low volume for testing)
    double phase_increment = 2.0 * M_PI * freq / sample_rate_;

    std::vector<int16_t> pcm_buf(samples_per_frame * channels_);

    while (running_.load()) {
      auto start_time = std::chrono::steady_clock::now();

      for (int i = 0; i < samples_per_frame; ++i) {
        // Low amplitude ~5% volume
        int16_t sample = static_cast<int16_t>(std::sin(phase) * 1500.0);
        for (int ch = 0; ch < channels_; ++ch) {
          pcm_buf[i * channels_ + ch] = sample;
        }
        phase += phase_increment;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
      }

      CapturedAudioFrame af;
      af.sample_rate = sample_rate_;
      af.channels = channels_;
      af.samples_per_channel = samples_per_frame;
      af.timestamp = start_time;
      af.pcm_data.resize(pcm_buf.size() * sizeof(int16_t));
      std::memcpy(af.pcm_data.data(), pcm_buf.data(), af.pcm_data.size());

      AudioCallback cb;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
      }
      if (cb) {
        cb(af);
      }

      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time);
      if (elapsed < frame_duration) {
        std::this_thread::sleep_for(frame_duration - elapsed);
      }
    }
  }

  int sample_rate_ = 48000;
  int channels_ = 2;
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  AudioCallback callback_;
};

#if !defined(_WIN32)
class PulseAudioCapture : public IAudioCapture {
 public:
  PulseAudioCapture() = default;
  ~PulseAudioCapture() override { Stop(); }

  bool Start(int sample_rate, int channels) override {
    Stop();
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = sample_rate_;
    ss.channels = channels_;

    pa_buffer_attr ba;
    ba.maxlength = static_cast<uint32_t>(-1);
    ba.tlength = static_cast<uint32_t>(-1);
    ba.prebuf = static_cast<uint32_t>(-1);
    ba.minreq = static_cast<uint32_t>(-1);
    ba.fragsize = static_cast<uint32_t>((sample_rate_ / 100) * channels_ * sizeof(int16_t)); // 10ms frame size

    int error = 0;
    pa_simple_ = pa_simple_new(nullptr, "CastMirror", PA_STREAM_RECORD, nullptr,
                               "System Audio Loopback", &ss, nullptr, &ba, &error);
    if (!pa_simple_) {
      LOG_WARN << "PulseAudio record failed (" << pa_strerror(error) << "), falling back to Synthetic Audio";
      return false;
    }

    running_ = true;
    capture_thread_ = std::thread(&PulseAudioCapture::CaptureLoop, this);
    LOG_INFO << "Started PulseAudio Loopback Capture (" << sample_rate_ << " Hz, " << channels_ << " channels)";
    return true;
  }

  void Stop() override {
    if (!running_.exchange(false)) return;
    if (pa_simple_) {
      pa_simple_flush(pa_simple_, nullptr);
    }
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    if (pa_simple_) {
      pa_simple_free(pa_simple_);
      pa_simple_ = nullptr;
    }
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  void SetAudioCallback(AudioCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

 private:
  void CaptureLoop() {
    int samples_per_frame = sample_rate_ / 100; // 10ms
    size_t buffer_bytes = samples_per_frame * channels_ * sizeof(int16_t);
    std::vector<uint8_t> buffer(buffer_bytes);

    while (running_.load()) {
      int error = 0;
      if (pa_simple_read(pa_simple_, buffer.data(), buffer.size(), &error) < 0) {
        LOG_WARN << "PulseAudio read error: " << pa_strerror(error);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      CapturedAudioFrame af;
      af.sample_rate = sample_rate_;
      af.channels = channels_;
      af.samples_per_channel = samples_per_frame;
      af.timestamp = std::chrono::steady_clock::now();
      af.pcm_data = buffer;

      AudioCallback cb;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
      }
      if (cb) {
        cb(af);
      }
    }
  }

  pa_simple* pa_simple_ = nullptr;
  int sample_rate_ = 48000;
  int channels_ = 2;
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  AudioCallback callback_;
};
#endif

std::unique_ptr<IAudioCapture> AudioCaptureFactory::Create() {
#if !defined(_WIN32)
  auto pa = std::make_unique<PulseAudioCapture>();
  if (pa->Start(48000, 2)) {
    return pa;
  }
#endif
  return std::make_unique<SyntheticAudioCapture>();
}

std::unique_ptr<IAudioCapture> AudioCaptureFactory::CreateSynthetic() {
  return std::make_unique<SyntheticAudioCapture>();
}

} // namespace castcore
