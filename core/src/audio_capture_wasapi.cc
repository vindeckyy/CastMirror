#include "castcore/audio_capture_wasapi.h"
#include "castcore/logger.h"

#include <chrono>
#include <thread>
#include <cstring>

namespace castcore {

WasapiAudioCapture::WasapiAudioCapture() = default;

WasapiAudioCapture::~WasapiAudioCapture() {
  Stop();
}

void WasapiAudioCapture::SetAudioCallback(AudioCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

bool WasapiAudioCapture::Start(int sample_rate, int channels) {
  Stop();
  sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
  channels_ = channels > 0 ? channels : 2;

#if !defined(_WIN32)
  LOG_ERROR << "WasapiAudioCapture is only available on Windows";
  return false;
#else
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr)) return false;

  Microsoft::WRL::ComPtr<IMMDevice> default_device;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device);
  if (FAILED(hr)) return false;

  hr = default_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audio_client_);
  if (FAILED(hr)) return false;

  WAVEFORMATEX wfx{};
  wfx.wFormatTag = WAVE_FORMAT_PCM;
  wfx.nChannels = static_cast<WORD>(channels_);
  wfx.nSamplesPerSec = static_cast<DWORD>(sample_rate_);
  wfx.wBitsPerSample = 16;
  wfx.nBlockAlign = wfx.nChannels * (wfx.wBitsPerSample / 8);
  wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

  hr = audio_client_->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK,
      100000, // 10ms buffer in 100ns units
      0,
      &wfx,
      nullptr);
  if (FAILED(hr)) return false;

  hr = audio_client_->GetService(IID_PPV_ARGS(&capture_client_));
  if (FAILED(hr)) return false;

  hr = audio_client_->Start();
  if (FAILED(hr)) return false;

  running_ = true;
  worker_thread_ = std::thread(&WasapiAudioCapture::CaptureLoop, this);
  LOG_INFO << "Started WASAPI Loopback Audio Capture (" << sample_rate_ << " Hz, " << channels_ << " ch)";
  return true;
#endif
}

void WasapiAudioCapture::Stop() {
  running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
#if defined(_WIN32)
  if (audio_client_) {
    audio_client_->Stop();
    capture_client_.Reset();
    audio_client_.Reset();
  }
#endif
}

bool WasapiAudioCapture::IsCapturing() const {
  return running_.load();
}

void WasapiAudioCapture::CaptureLoop() {
#if defined(_WIN32)
  while (running_) {
    UINT32 packet_length = 0;
    HRESULT hr = capture_client_->GetNextPacketSize(&packet_length);
    if (FAILED(hr)) break;

    while (packet_length > 0) {
      BYTE* data = nullptr;
      UINT32 num_frames_read = 0;
      DWORD flags = 0;

      hr = capture_client_->GetBuffer(&data, &num_frames_read, &flags, nullptr, nullptr);
      if (SUCCEEDED(hr)) {
        CapturedAudioFrame frame;
        frame.sample_rate = sample_rate_;
        frame.channels = channels_;
        frame.samples_per_channel = static_cast<int>(num_frames_read);
        frame.timestamp = std::chrono::steady_clock::now();

        size_t byte_count = num_frames_read * channels_ * sizeof(int16_t);
        frame.pcm_data.resize(byte_count);

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::memset(frame.pcm_data.data(), 0, byte_count);
        } else if (data) {
          std::memcpy(frame.pcm_data.data(), data, byte_count);
        }

        capture_client_->ReleaseBuffer(num_frames_read);

        AudioCallback cb;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          cb = callback_;
        }
        if (cb) cb(frame);
      }

      hr = capture_client_->GetNextPacketSize(&packet_length);
      if (FAILED(hr)) break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
#endif
}

}  // namespace castcore
