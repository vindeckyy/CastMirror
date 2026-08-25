#ifndef CASTCORE_MIRRORING_NEGOTIATOR_H_
#define CASTCORE_MIRRORING_NEGOTIATOR_H_

#include "castcore/types.h"
#include <string>
#include <vector>
#include <array>
#include <memory>

namespace castcore {

struct StreamEncryptionKeys {
  std::array<uint8_t, 16> aes_key{};
  std::array<uint8_t, 16> aes_iv_mask{};
  std::string aes_key_hex;
  std::string aes_iv_mask_hex;
};

struct NegotiatedStreamParams {
  int stream_index = 0;
  std::string codec_name;
  uint32_t sender_ssrc = 0;
  uint32_t receiver_ssrc = 0;
  uint8_t rtp_payload_type = 96;
  int bit_rate = 5000000;
  int target_delay_ms = 400;
  StreamEncryptionKeys keys;
};

struct NegotiatedSessionParams {
  uint16_t receiver_udp_port = 0;
  bool has_audio = false;
  bool has_video = false;
  NegotiatedStreamParams audio_stream;
  NegotiatedStreamParams video_stream;
  Resolution max_display_resolution{1920, 1080};
};

class MirroringNegotiator {
 public:
  static StreamEncryptionKeys GenerateRandomKeys();

  static std::string CreateOfferJson(int seq_num,
                                     const StreamStats& video_settings,
                                     bool include_audio,
                                     const StreamEncryptionKeys& video_keys,
                                     const StreamEncryptionKeys& audio_keys,
                                     VideoCodec video_codec = VideoCodec::kH264,
                                     int target_delay_ms = 400);

  static bool ParseAnswerJson(const std::string& answer_json,
                              const StreamEncryptionKeys& video_keys,
                              const StreamEncryptionKeys& audio_keys,
                              NegotiatedSessionParams& out_params);

  static std::string BytesToHex(const uint8_t* data, size_t len);
  static bool HexToBytes(const std::string& hex, uint8_t* out, size_t max_len);
};

} // namespace castcore

#endif // CASTCORE_MIRRORING_NEGOTIATOR_H_
