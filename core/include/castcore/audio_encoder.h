#ifndef CASTCORE_AUDIO_ENCODER_H_
#define CASTCORE_AUDIO_ENCODER_H_

#include "castcore/types.h"
#include <vector>
#include <memory>

namespace castcore {

struct AudioEncoderConfig {
  int sample_rate = 48000;
  int channels = 2;
  int bitrate_bps = 192000;
  AudioCodec codec = AudioCodec::kOpus;
  int playout_delay_ms = 200;
};

class IAudioEncoder {
 public:
  virtual ~IAudioEncoder() = default;

  virtual bool Initialize(const AudioEncoderConfig& config) = 0;
  virtual bool Encode(const CapturedAudioFrame& frame, EncodedFrame& out_encoded_frame) = 0;
  virtual void SetBitrate(int bitrate_bps) = 0;

  virtual const AudioEncoderConfig& GetConfig() const = 0;
};

class AudioEncoderFactory {
 public:
  static std::unique_ptr<IAudioEncoder> Create(AudioCodec codec = AudioCodec::kOpus);
};

} // namespace castcore

#endif // CASTCORE_AUDIO_ENCODER_H_
