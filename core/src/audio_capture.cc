#include "castcore/audio_capture.h"
#include "castcore/logger.h"
#include "castcore/thread_util.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>
#include <cstddef>

#if !defined(_WIN32)
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

bool PulseGetSinkPlayback(const std::string& sink, int* mute_out, pa_cvolume* volume_out) {
  if (sink.empty() || !mute_out || !volume_out) return false;
  PulseSession s;
  if (!s.Connect("CastMirror-sink-get")) return false;
  struct Probe {
    int mute = 0;
    pa_cvolume volume{};
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
        p->volume = info->volume;
        p->ok = true;
      }, &probe);
  if (op) {
    s.Wait(&probe.done);
    pa_operation_unref(op);
  }
  if (!probe.ok) return false;
  *mute_out = probe.mute;
  *volume_out = probe.volume;
  return true;
}

bool PulseSetSinkVolume(const std::string& sink, const pa_cvolume& volume) {
  if (sink.empty()) return false;
  PulseSession s;
  if (!s.Connect("CastMirror-vol-set")) return false;
  struct Result {
    int done = 0;
    int success = 0;
  } result;
  pa_operation* op = pa_context_set_sink_volume_by_name(s.ctx, sink.c_str(), &volume,
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

  void SetHostSilence(bool silence) override {
    silence_host_ = silence;
  }

  bool Start(int sample_rate, int channels) override {
    Stop();
    sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
    channels_ = channels > 0 ? channels : 2;

    ml_ = pa_mainloop_new();
    if (!ml_) {
      LOG_WARN << "PulseAudio mainloop alloc failed, falling back to Synthetic Audio";
      return false;
    }
    ctx_ = pa_context_new(pa_mainloop_get_api(ml_), "CastMirror");
    if (!ctx_) {
      CleanupPulse();
      return false;
    }

    int ctx_phase = 0;
    pa_context_set_state_callback(ctx_, [](pa_context* c, void* userdata) {
      int* phase = static_cast<int*>(userdata);
      switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY: *phase = 1; break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED: *phase = -1; break;
        default: break;
      }
    }, &ctx_phase);

    if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      CleanupPulse();
      return false;
    }
    auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ctx_phase == 0 && std::chrono::steady_clock::now() < connect_deadline) {
      if (pa_mainloop_iterate(ml_, 1, nullptr) < 0) {
        ctx_phase = -1;
        break;
      }
    }
    if (ctx_phase != 1) {
      LOG_WARN << "PulseAudio context failed, falling back to Synthetic Audio";
      pa_context_set_state_callback(ctx_, nullptr, nullptr);
      CleanupPulse();
      return false;
    }
    pa_context_set_state_callback(ctx_, nullptr, nullptr);
    std::string sink_name = PulseDefaultSinkName();
    if (!sink_name.empty()) {
      record_device_ = sink_name + ".monitor";
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = static_cast<uint32_t>(sample_rate_);
    ss.channels = static_cast<uint8_t>(channels_);

    uint32_t frag = static_cast<uint32_t>((sample_rate_ / 100) * channels_ * sizeof(int16_t));
    pa_buffer_attr ba{};
    ba.fragsize = frag;
    ba.maxlength = frag * 3;
    ba.tlength = static_cast<uint32_t>(-1);
    ba.prebuf = static_cast<uint32_t>(-1);
    ba.minreq = static_cast<uint32_t>(-1);

    stream_ = pa_stream_new(ctx_, "System Audio Loopback", &ss, nullptr);
    if (!stream_) {
      CleanupPulse();
      return false;
    }

    int stream_phase = 0;
    pa_stream_set_state_callback(stream_, [](pa_stream* s, void* userdata) {
      int* phase = static_cast<int*>(userdata);
      switch (pa_stream_get_state(s)) {
        case PA_STREAM_READY: *phase = 1; break;
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED: *phase = -1; break;
        default: break;
      }
    }, &stream_phase);

    pa_stream_flags_t flags = static_cast<pa_stream_flags_t>(
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING);
    const char* dev = record_device_.empty() ? nullptr : record_device_.c_str();
    if (pa_stream_connect_record(stream_, dev, &ba, flags) < 0) {
      LOG_WARN << "PulseAudio record connect failed, falling back to Synthetic Audio";
      CleanupPulse();
      return false;
    }
    auto rec_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (stream_phase == 0 && std::chrono::steady_clock::now() < rec_deadline) {
      if (pa_mainloop_iterate(ml_, 1, nullptr) < 0) {
        stream_phase = -1;
        break;
      }
    }
    if (stream_phase != 1) {
      LOG_WARN << "PulseAudio record stream not ready, falling back to Synthetic Audio";
      pa_stream_set_state_callback(stream_, nullptr, nullptr);
      CleanupPulse();
      return false;
    }
    pa_stream_set_state_callback(stream_, nullptr, nullptr);
    if (silence_host_ && !sink_name.empty()) {
      int prev_mute = 0;
      pa_cvolume prev_volume{};
      if (PulseGetSinkPlayback(sink_name, &prev_mute, &prev_volume)) {
        pa_cvolume silent{};
        unsigned ch = prev_volume.channels > 0 ? prev_volume.channels : 2;
        pa_cvolume_set(&silent, ch, PA_VOLUME_MUTED);
        bool vol_ok = PulseSetSinkVolume(sink_name, silent);
        bool unmute_ok = true;
        if (prev_mute) {
          unmute_ok = PulseSetSinkMute(sink_name, 0);
        }
        if (vol_ok && unmute_ok) {
          muted_sink_ = sink_name;
          saved_sink_mute_ = prev_mute;
          saved_sink_volume_ = prev_volume;
          have_saved_volume_ = true;
          LOG_INFO << "Silenced local playback on " << sink_name
                   << " (volume 0, unmuted so monitor keeps running)";
        } else {
          LOG_WARN << "Could not silence local sink " << sink_name << "; host speakers may still play";
        }
      } else {
        LOG_WARN << "Could not read local sink " << sink_name << "; host speakers may still play";
      }
    }

    running_ = true;
    capture_thread_ = std::thread(&PulseAudioCapture::CaptureLoop, this);
    LOG_INFO << "Started PulseAudio Loopback Capture (" << sample_rate_ << " Hz, " << channels_
             << " channels, device=" << (dev ? dev : "default") << ")";
    return true;
  }

  void Stop() override {
    bool was_running = running_.exchange(false);
    if (was_running) {
      if (ml_) {
        pa_mainloop_wakeup(ml_);
      }
      JoinOrDetach(capture_thread_, 500, "PulseAudio capture");
    }
    CleanupPulse();
    if (!muted_sink_.empty()) {
      if (have_saved_volume_) {
        PulseSetSinkVolume(muted_sink_, saved_sink_volume_);
      }
      PulseSetSinkMute(muted_sink_, saved_sink_mute_);
      LOG_INFO << "Restored local playback on " << muted_sink_;
      muted_sink_.clear();
      have_saved_volume_ = false;
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
  void CleanupPulse() {
    if (stream_) {
      pa_stream_set_state_callback(stream_, nullptr, nullptr);
      pa_stream_disconnect(stream_);
      pa_stream_unref(stream_);
      stream_ = nullptr;
    }
    if (ctx_) {
      pa_context_set_state_callback(ctx_, nullptr, nullptr);
      pa_context_disconnect(ctx_);
      pa_context_unref(ctx_);
      ctx_ = nullptr;
    }
    if (ml_) {
      pa_mainloop_free(ml_);
      ml_ = nullptr;
    }
  }

  void EmitPcm(const uint8_t* data, size_t bytes, int samples_per_frame) {
    CapturedAudioFrame af;
    af.sample_rate = sample_rate_;
    af.channels = channels_;
    af.samples_per_channel = samples_per_frame;
    af.timestamp = std::chrono::steady_clock::now();
    af.pcm_data.assign(data, data + bytes);
    AudioCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cb = callback_;
    }
    if (cb) {
      cb(af);
    }
  }

  void CaptureLoop() {
    int samples_per_frame = sample_rate_ / 100;
    size_t frame_bytes = static_cast<size_t>(samples_per_frame * channels_ * sizeof(int16_t));
    std::vector<uint8_t> pending;
    std::vector<uint8_t> silence(frame_bytes, 0);
    auto last_emit = std::chrono::steady_clock::now();
    auto last_cork_warn = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (running_.load()) {
      if (pa_mainloop_prepare(ml_, 20) < 0) {
        break;
      }
      if (pa_mainloop_poll(ml_) < 0) {
        break;
      }
      pa_mainloop_dispatch(ml_);

      if (stream_ && pa_stream_get_state(stream_) == PA_STREAM_READY) {
        if (pa_stream_is_corked(stream_)) {
          auto now = std::chrono::steady_clock::now();
          if (now - last_cork_warn >= std::chrono::seconds(2)) {
            LOG_WARN << "PulseAudio record stream corked/suspended; emitting silence keepalive";
            last_cork_warn = now;
          }
        }
        const void* data = nullptr;
        size_t nbytes = 0;
        if (pa_stream_peek(stream_, &data, &nbytes) >= 0) {
          if (nbytes > 0) {
            if (data) {
              const auto* bytes = static_cast<const uint8_t*>(data);
              pending.insert(pending.end(), bytes, bytes + nbytes);
            }
            pa_stream_drop(stream_);
          }
        }
      }

      while (pending.size() >= frame_bytes) {
        EmitPcm(pending.data(), frame_bytes, samples_per_frame);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame_bytes));
        last_emit = std::chrono::steady_clock::now();
      }

      auto now = std::chrono::steady_clock::now();
      if (now - last_emit >= std::chrono::milliseconds(20)) {
        EmitPcm(silence.data(), silence.size(), samples_per_frame);
        last_emit = now;
      }
    }
  }

  pa_mainloop* ml_ = nullptr;
  pa_context* ctx_ = nullptr;
  pa_stream* stream_ = nullptr;
  std::string record_device_;
  int sample_rate_ = 48000;
  int channels_ = 2;
  std::atomic<bool> running_{false};
  std::atomic<bool> silence_host_{true};
  std::thread capture_thread_;
  std::mutex mutex_;
  AudioCallback callback_;
  std::string muted_sink_;
  int saved_sink_mute_ = 0;
  pa_cvolume saved_sink_volume_{};
  bool have_saved_volume_ = false;
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
