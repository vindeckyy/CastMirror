#include "castcore/audio_capture.h"
#include "castcore/logger.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <chrono>
#include <cstring>
#include <string>

#if !defined(_WIN32)
  #include <pulse/simple.h>
  #include <pulse/error.h>
  #include <pulse/pulseaudio.h>
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

namespace {

struct PulseSession {
  pa_mainloop* ml = nullptr;
  pa_context* ctx = nullptr;
  int phase = 0;  // 0 connecting, 1 ready, -1 fail

  bool Connect(const char* app_name) {
    ml = pa_mainloop_new();
    if (!ml) return false;
    ctx = pa_context_new(pa_mainloop_get_api(ml), app_name);
    if (!ctx) return false;
    pa_context_set_state_callback(ctx, [](pa_context* c, void* userdata) {
      auto* s = static_cast<PulseSession*>(userdata);
      switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY: s->phase = 1; break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED: s->phase = -1; break;
        default: break;
      }
    }, this);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      phase = -1;
      return false;
    }
    while (phase == 0) {
      if (pa_mainloop_iterate(ml, 1, nullptr) < 0) {
        phase = -1;
        break;
      }
    }
    return phase == 1;
  }

  void Wait(int* done) {
    while (*done == 0 && phase == 1) {
      if (pa_mainloop_iterate(ml, 1, nullptr) < 0) break;
    }
  }

  ~PulseSession() {
    if (ctx) {
      pa_context_disconnect(ctx);
      pa_context_unref(ctx);
    }
    if (ml) pa_mainloop_free(ml);
  }
};

std::string PulseDefaultSinkName() {
  PulseSession s;
  if (!s.Connect("CastMirror-probe")) return {};
  struct Probe {
    std::string name;
    int done = 0;
  } probe;
  pa_operation* op = pa_context_get_server_info(s.ctx, [](pa_context*, const pa_server_info* info, void* userdata) {
    auto* p = static_cast<Probe*>(userdata);
    if (info && info->default_sink_name && info->default_sink_name[0]) {
      p->name = info->default_sink_name;
    }
    p->done = 1;
  }, &probe);
  if (op) {
    s.Wait(&probe.done);
    pa_operation_unref(op);
  }
  return probe.name;
}

bool PulseGetSinkMute(const std::string& sink, int* mute_out) {
  if (sink.empty() || !mute_out) return false;
  PulseSession s;
  if (!s.Connect("CastMirror-mute-get")) return false;
  struct Probe {
    int mute = 0;
    int done = 0;
    bool ok = false;
  } probe;
  pa_operation* op = pa_context_get_sink_info_by_name(s.ctx, sink.c_str(),
      [](pa_context*, const pa_sink_info* info, int eol, void* userdata) {
        auto* p = static_cast<Probe*>(userdata);
        if (eol || !info) {
          p->done = 1;
          return;
        }
        p->mute = info->mute;
        p->ok = true;
      }, &probe);
  if (op) {
    s.Wait(&probe.done);
    pa_operation_unref(op);
  }
  if (!probe.ok) return false;
  *mute_out = probe.mute;
  return true;
}

bool PulseSetSinkMute(const std::string& sink, int mute) {
  if (sink.empty()) return false;
  PulseSession s;
  if (!s.Connect("CastMirror-mute-set")) return false;
  struct Result {
    int done = 0;
    int success = 0;
  } result;
  pa_operation* op = pa_context_set_sink_mute_by_name(s.ctx, sink.c_str(), mute,
      [](pa_context*, int success, void* userdata) {
        auto* r = static_cast<Result*>(userdata);
        r->success = success;
        r->done = 1;
      }, &result);
  if (op) {
    s.Wait(&result.done);
    pa_operation_unref(op);
  }
  return result.done == 1 && result.success != 0;
}

} // namespace

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

    uint32_t frag = static_cast<uint32_t>((sample_rate_ / 100) * channels_ * sizeof(int16_t)); // 10ms
    pa_buffer_attr ba;
    ba.fragsize = frag;
    ba.maxlength = frag * 3; // cap capture queue to ~30ms
    ba.tlength = static_cast<uint32_t>(-1);
    ba.prebuf = static_cast<uint32_t>(-1);
    ba.minreq = static_cast<uint32_t>(-1);

    std::string sink_name = PulseDefaultSinkName();
    std::string sink_monitor;
    if (!sink_name.empty()) {
      sink_monitor = sink_name + ".monitor";
    }
    const char* try_devs[2];
    int n_devs = 0;
    if (!sink_monitor.empty()) {
      try_devs[n_devs++] = sink_monitor.c_str();
    }

    int error = 0;
    const char* used_dev = nullptr;
    for (int i = 0; i < n_devs; ++i) {
      error = 0;
      pa_simple_ = pa_simple_new(nullptr, "CastMirror", PA_STREAM_RECORD, try_devs[i],
                                 "System Audio Loopback", &ss, nullptr, &ba, &error);
      if (pa_simple_) {
        used_dev = try_devs[i];
        break;
      }
    }
    if (!pa_simple_) {
      LOG_WARN << "PulseAudio record failed (" << pa_strerror(error) << "), falling back to Synthetic Audio";
      return false;
    }

    // Mute local speakers while capturing the sink monitor so audio is heard
    // on the Cast receiver only. Restore the previous mute state on Stop().
    if (!sink_name.empty()) {
      int prev_mute = 0;
      if (PulseGetSinkMute(sink_name, &prev_mute) && PulseSetSinkMute(sink_name, 1)) {
        muted_sink_ = sink_name;
        saved_sink_mute_ = prev_mute;
        LOG_INFO << "Muted local playback on " << sink_name << " while casting";
      } else {
        LOG_WARN << "Could not mute local sink " << sink_name << "; host speakers may still play";
      }
    }

    running_ = true;
    capture_thread_ = std::thread(&PulseAudioCapture::CaptureLoop, this);
    LOG_INFO << "Started PulseAudio Loopback Capture (" << sample_rate_ << " Hz, " << channels_
             << " channels, device=" << (used_dev ? used_dev : "unknown") << ")";
    return true;
  }

  void Stop() override {
    bool was_running = running_.exchange(false);
    if (was_running) {
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
    if (!muted_sink_.empty()) {
      PulseSetSinkMute(muted_sink_, saved_sink_mute_);
      LOG_INFO << "Restored local playback on " << muted_sink_;
      muted_sink_.clear();
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
  std::string muted_sink_;
  int saved_sink_mute_ = 0;
};
#endif

std::unique_ptr<IAudioCapture> AudioCaptureFactory::Create() {
#if !defined(_WIN32)
  return std::make_unique<PulseAudioCapture>();
#else
  return std::make_unique<SyntheticAudioCapture>();
#endif
}

std::unique_ptr<IAudioCapture> AudioCaptureFactory::CreateSynthetic() {
  return std::make_unique<SyntheticAudioCapture>();
}

} // namespace castcore
