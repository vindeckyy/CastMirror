#ifndef CASTCORE_AUDIO_CAPTURE_WASAPI_H_
#define CASTCORE_AUDIO_CAPTURE_WASAPI_H_

#include "castcore/audio_capture.h"
#include <atomic>
#include <thread>
#include <mutex>

#if defined(_WIN32)
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#endif

namespace castcore {

class WasapiAudioCapture : public IAudioCapture {
 public:
  WasapiAudioCapture();
  ~WasapiAudioCapture() override;

  bool Start(int sample_rate = 48000, int channels = 2) override;
  void Stop() override;
  bool IsCapturing() const override;
  void SetAudioCallback(AudioCallback callback) override;

 private:
  void CaptureLoop();

  std::atomic<bool> running_{false};
  std::thread worker_thread_;
  std::mutex mutex_;
  AudioCallback callback_;
  int sample_rate_ = 48000;
  int channels_ = 2;

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<IAudioClient> audio_client_;
  Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_client_;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_AUDIO_CAPTURE_WASAPI_H_
