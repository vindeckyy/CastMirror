#include "castcore/mirroring_negotiator.h"
#include "castcore/logger.h"
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <sstream>
#include <iomanip>

namespace castcore {

std::string MirroringNegotiator::BytesToHex(const uint8_t* data, size_t len) {
  std::ostringstream ss;
  for (size_t i = 0; i < len; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
  }
  return ss.str();
}

bool MirroringNegotiator::HexToBytes(const std::string& hex, uint8_t* out, size_t max_len) {
  if (hex.length() != max_len * 2) return false;
  for (size_t i = 0; i < max_len; ++i) {
    std::string byteString = hex.substr(i * 2, 2);
    out[i] = static_cast<uint8_t>(std::strtoul(byteString.c_str(), nullptr, 16));
  }
  return true;
}

StreamEncryptionKeys MirroringNegotiator::GenerateRandomKeys() {
  StreamEncryptionKeys keys;
  RAND_bytes(keys.aes_key.data(), static_cast<int>(keys.aes_key.size()));
  RAND_bytes(keys.aes_iv_mask.data(), static_cast<int>(keys.aes_iv_mask.size()));
  keys.aes_key_hex = BytesToHex(keys.aes_key.data(), keys.aes_key.size());
  keys.aes_iv_mask_hex = BytesToHex(keys.aes_iv_mask.data(), keys.aes_iv_mask.size());
  return keys;
}

std::string MirroringNegotiator::CreateOfferJson(int seq_num,
                                                const StreamStats& video_settings,
                                                bool include_audio,
                                                const StreamEncryptionKeys& video_keys,
                                                const StreamEncryptionKeys& audio_keys,
                                                VideoCodec video_codec,
                                                int target_delay_ms,
                                                int audio_bitrate_bps) {
  nlohmann::json offer_root;
  offer_root["type"] = "OFFER";
  offer_root["seqNum"] = seq_num;

  nlohmann::json offer_body;
  offer_body["castMode"] = "mirroring";
  offer_body["receiverGetStatus"] = true;

  nlohmann::json supported_streams = nlohmann::json::array();
  int stream_idx = 0;

  if (include_audio) {
    nlohmann::json audio_stream;
    audio_stream["index"] = stream_idx++;
    audio_stream["type"] = "audio_source";
    audio_stream["codecName"] = "opus";
    audio_stream["rtpProfile"] = "cast";
    audio_stream["rtpPayloadType"] = 127; // AndroidTV / standard hack
    audio_stream["ssrc"] = 1;
    audio_stream["bitRate"] = audio_bitrate_bps > 0 ? audio_bitrate_bps : 192000;
    audio_stream["timeBase"] = "1/48000";
    audio_stream["channels"] = 2;
    audio_stream["samplingRate"] = 48000;
    audio_stream["targetDelay"] = target_delay_ms;
    audio_stream["aesKey"] = audio_keys.aes_key_hex;
    audio_stream["aesIvMask"] = audio_keys.aes_iv_mask_hex;
    supported_streams.push_back(audio_stream);
  }

  nlohmann::json video_stream;
  video_stream["index"] = stream_idx++;
  video_stream["type"] = "video_source";
  video_stream["codecName"] = VideoCodecToString(video_codec);
  video_stream["rtpProfile"] = "cast";
  video_stream["rtpPayloadType"] = 96;
  video_stream["ssrc"] = 2;
  video_stream["bitRate"] = static_cast<int>(video_settings.bitrate_kbps * 1000);
  video_stream["timeBase"] = "1/90000";
  video_stream["maxFrameRate"] = std::to_string(video_settings.current_framerate) + "/1";
  video_stream["maxBitRate"] = static_cast<int>(video_settings.bitrate_kbps * 1500);
  video_stream["targetDelay"] = target_delay_ms;
  video_stream["aesKey"] = video_keys.aes_key_hex;
  video_stream["aesIvMask"] = video_keys.aes_iv_mask_hex;
  video_stream["profile"] = "high";
  video_stream["level"] = "4.2";

  nlohmann::json resolutions = nlohmann::json::array();
  resolutions.push_back({{"width", video_settings.current_resolution.width},
                         {"height", video_settings.current_resolution.height}});
  if (video_settings.current_resolution.width != 1920 || video_settings.current_resolution.height != 1080) {
    resolutions.push_back({{"width", 1920}, {"height", 1080}});
  }
  resolutions.push_back({{"width", 1280}, {"height", 720}});
  video_stream["resolutions"] = resolutions;

  supported_streams.push_back(video_stream);

  offer_body["supportedStreams"] = supported_streams;
  offer_root["offer"] = offer_body;

  return offer_root.dump();
}

std::string MirroringNegotiator::CreateStatusJson(int seq_num, const StreamStats& stats) {
  nlohmann::json root;
  root["type"] = "STATUS";
  root["seqNum"] = seq_num;
  root["result"] = "ok";

  nlohmann::json status = nlohmann::json::array();
  nlohmann::json video;
  video["ssrc"] = 2;
  video["type"] = "video";
  video["fps"] = stats.current_fps > 0 ? stats.current_fps : stats.current_framerate;
  video["bitRate"] = static_cast<int>(stats.bitrate_kbps * 1000);
  video["rtt"] = stats.round_trip_time_ms;
  video["loss"] = stats.packet_loss_fraction;
  video["width"] = stats.current_resolution.width;
  video["height"] = stats.current_resolution.height;
  status.push_back(video);
  root["status"] = status;
  return root.dump();
}

bool MirroringNegotiator::ParseAnswerJson(const std::string& answer_json,
                                         const StreamEncryptionKeys& video_keys,
                                         const StreamEncryptionKeys& audio_keys,
                                         NegotiatedSessionParams& out_params) {
  try {
    nlohmann::json j = nlohmann::json::parse(answer_json);
    if (!j.contains("type") || j["type"] != "ANSWER") {
      LOG_ERROR << "Expected ANSWER message, got: " << answer_json;
      return false;
    }

    if (j.contains("result") && j["result"] == "error") {
      LOG_ERROR << "Receiver rejected OFFER with error";
      return false;
    }

    if (!j.contains("answer")) {
      LOG_ERROR << "Missing 'answer' field in ANSWER message";
      return false;
    }

    const auto& ans = j["answer"];
    if (!ans.contains("udpPort") || !ans.contains("sendIndexes") || !ans.contains("ssrcs")) {
      LOG_ERROR << "Incomplete answer object in ANSWER message";
      return false;
    }

    out_params.receiver_udp_port = ans["udpPort"].get<uint16_t>();
    std::vector<int> send_indexes = ans["sendIndexes"].get<std::vector<int>>();
    std::vector<uint32_t> ssrcs = ans["ssrcs"].get<std::vector<uint32_t>>();

    if (send_indexes.size() != ssrcs.size()) {
      LOG_ERROR << "Mismatch between sendIndexes size and ssrcs size";
      return false;
    }

    out_params.has_audio = false;
    out_params.has_video = false;

    for (size_t i = 0; i < send_indexes.size(); ++i) {
      int idx = send_indexes[i];
      uint32_t r_ssrc = ssrcs[i];

      if (idx == 0 && audio_keys.aes_key_hex.size() == 32) {
        out_params.has_audio = true;
        out_params.audio_stream.stream_index = 0;
        out_params.audio_stream.codec_name = "opus";
        out_params.audio_stream.sender_ssrc = 1;
        out_params.audio_stream.receiver_ssrc = r_ssrc;
        out_params.audio_stream.rtp_payload_type = 127;
        out_params.audio_stream.keys = audio_keys;
      } else {
        out_params.has_video = true;
        out_params.video_stream.stream_index = idx;
        out_params.video_stream.codec_name = "h264";
        out_params.video_stream.sender_ssrc = 2;
        out_params.video_stream.receiver_ssrc = r_ssrc;
        out_params.video_stream.rtp_payload_type = 96;
        out_params.video_stream.keys = video_keys;
      }
    }

    LOG_INFO << "Successfully negotiated Cast Mirroring session! UDP Port: "
             << out_params.receiver_udp_port << ", Video: " << (out_params.has_video ? "YES" : "NO")
             << ", Audio: " << (out_params.has_audio ? "YES" : "NO");

    return true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Exception parsing ANSWER JSON: " << e.what();
    return false;
  }
}

} // namespace castcore
