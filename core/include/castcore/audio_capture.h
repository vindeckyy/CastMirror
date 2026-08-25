#ifndef CASTCORE_AUDIO_CAPTURE_H_
#define CASTCORE_AUDIO_CAPTURE_H_

#include "castcore/types.h"
#include <vector>
#include <memory>
#include <functional>

namespace castcore {

class IAudioCapture {
 public:
  using AudioCallback = std::function<void(const CapturedAudioFrame& frame)>;

  virtual ~IAudioCapture() = default;

  virtual bool Start(int sample_rate = 48000, int channels = 2) = 0;
  virtual void Stop() = 0;
  virtual bool IsCapturing() const = 0;

  virtual void SetAudioCallback(AudioCallback callback) = 0;
};

class AudioCaptureFactory {
 public:
  static std::unique_ptr<IAudioCapture> Create();
  static std::unique_ptr<IAudioCapture> CreateSynthetic();
};

} // namespace castcore

#endif // CASTCORE_AUDIO_CAPTURE_H_
