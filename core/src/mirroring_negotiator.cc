#include "castcore/mirroring_negotiator.h"
#include "castcore/capability_model.h"
#include "castcore/logger.h"
#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <cctype>
#include <cstdint>
#include <limits>
#include <random>
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
  if (!out || hex.length() != max_len * 2) return false;
  for (unsigned char c : hex) {
    if (!std::isxdigit(c)) return false;
  }
  for (size_t i = 0; i < max_len; ++i) {
    std::string byteString = hex.substr(i * 2, 2);
    out[i] = static_cast<uint8_t>(std::strtoul(byteString.c_str(), nullptr, 16));
  }
  return true;
}

StreamEncryptionKeys MirroringNegotiator::GenerateRandomKeys() {
  StreamEncryptionKeys keys;
  auto fill_random = [](std::array<uint8_t, 16>& buf) {
    if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) {
      LOG_WARN << "RAND_bytes failed in GenerateRandomKeys; using std::random_device fallback";
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<int> dist(0, 255);
      for (auto& b : buf) {
        b = static_cast<uint8_t>(dist(gen));
      }
    }
  };
  fill_random(keys.aes_key);
  fill_random(keys.aes_iv_mask);
  keys.aes_key_hex = BytesToHex(keys.aes_key.data(), keys.aes_key.size());
  keys.aes_iv_mask_hex = BytesToHex(keys.aes_iv_mask.data(), keys.aes_iv_mask.size());
  return keys;
}

std::string MirroringNegotiator::CreateOfferJson(int seq_num, const StreamStats& video_settings,
                                                 bool include_audio,
                                                 const StreamEncryptionKeys& video_keys,
                                                 const StreamEncryptionKeys& audio_keys,
                                                 VideoCodec video_codec, int target_delay_ms,
                                                 int audio_bitrate_bps) {
  nlohmann::json offer_root;
  offer_root["type"] = "OFFER";
  offer_root["seqNum"] = seq_num;

  nlohmann::json offer_body;
  offer_body["castMode"] = "mirroring";
  offer_body["receiverGetStatus"] = true;

  nlohmann::json supported_streams = nlohmann::json::array();
  int stream_idx = 0;

  const int delay_ms = target_delay_ms > 0 ? target_delay_ms : 200;
  if (include_audio) {
    nlohmann::json audio_stream;
    audio_stream["index"] = stream_idx++;
    audio_stream["type"] = "audio_source";
    audio_stream["codecName"] = "opus";
    audio_stream["rtpProfile"] = "cast";
    audio_stream["rtpPayloadType"] = 127;  // AndroidTV / standard hack
    audio_stream["ssrc"] = 1;
    audio_stream["bitRate"] = audio_bitrate_bps > 0 ? audio_bitrate_bps : 192000;
    audio_stream["timeBase"] = "1/48000";
    audio_stream["channels"] = 2;
    audio_stream["samplingRate"] = 48000;
    audio_stream["targetDelay"] = delay_ms;
    audio_stream["aesKey"] = audio_keys.aes_key_hex;
    audio_stream["aesIvMask"] = audio_keys.aes_iv_mask_hex;
    supported_streams.push_back(audio_stream);
  }

  const int fps = video_settings.current_framerate > 0 ? video_settings.current_framerate : 60;
  const int64_t video_bps = static_cast<int64_t>(video_settings.bitrate_kbps) * 1000LL;
  nlohmann::json video_stream;
  video_stream["index"] = stream_idx++;
  video_stream["type"] = "video_source";
  video_stream["codecName"] = VideoCodecToString(video_codec);
  video_stream["rtpProfile"] = "cast";
  video_stream["rtpPayloadType"] = 96;
  video_stream["ssrc"] = 2;
  video_stream["bitRate"] = video_bps;
  video_stream["timeBase"] = "1/90000";
  video_stream["maxFrameRate"] = std::to_string(fps) + "/1";
  // Cast's maxBitRate is in bits per second, the same unit as bitRate above.
  video_stream["maxBitRate"] = video_bps;
  video_stream["targetDelay"] = delay_ms;
  video_stream["aesKey"] = video_keys.aes_key_hex;
  video_stream["aesIvMask"] = video_keys.aes_iv_mask_hex;
  if (video_codec == VideoCodec::kH264) {
    video_stream["profile"] = "high";
    video_stream["level"] = "4.2";
  }

  // Advertise the negotiated capture resolution plus the next lower rung the
  // encoder can actually be reconfigured to. The ladder comes from the
  // capability model rather than a hardcoded 1080p/720p pair, and a fallback is
  // only offered when it is genuinely smaller than what we are encoding (so a
  // 720p or smaller source never advertises an upscale).
  const Resolution negotiated = video_settings.current_resolution;
  nlohmann::json resolutions = nlohmann::json::array();
  resolutions.push_back({{"width", negotiated.width}, {"height", negotiated.height}});
  if (negotiated.width > 0 && negotiated.height > 0) {
    const DeviceCapabilities caps = CapabilityModel::Evaluate(CastDevice{});
    for (const auto& rung : caps.supported_resolutions) {
      if (resolutions.size() >= 2) break;
      if (rung.width < negotiated.width && rung.height < negotiated.height) {
        resolutions.push_back({{"width", rung.width}, {"height", rung.height}});
      }
    }
  }
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
  video["bitRate"] = static_cast<int64_t>(stats.bitrate_kbps) * 1000LL;
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
    if (!ans.is_object() || !ans.contains("udpPort") || !ans.contains("sendIndexes") ||
        !ans.contains("ssrcs")) {
      LOG_ERROR << "Incomplete answer object in ANSWER message";
      return false;
    }

    if (!ans["udpPort"].is_number_integer()) {
      LOG_ERROR << "Invalid udpPort type in ANSWER message";
      return false;
    }
    const int64_t udp_port = ans["udpPort"].get<int64_t>();
    if (udp_port <= 0 || udp_port > 65535) {
      LOG_ERROR << "Out-of-range udpPort in ANSWER message: " << udp_port;
      return false;
    }

    if (!ans["sendIndexes"].is_array() || !ans["ssrcs"].is_array()) {
      LOG_ERROR << "sendIndexes and ssrcs must be arrays in ANSWER message";
      return false;
    }

    std::vector<int> send_indexes;
    std::vector<uint32_t> ssrcs;
    send_indexes.reserve(ans["sendIndexes"].size());
    ssrcs.reserve(ans["ssrcs"].size());

    for (const auto& idx_val : ans["sendIndexes"]) {
      if (!idx_val.is_number_integer()) {
        LOG_ERROR << "Non-integer element in sendIndexes";
        return false;
      }
      const int64_t idx = idx_val.get<int64_t>();
      if (idx < 0 || idx > std::numeric_limits<int>::max()) {
        LOG_ERROR << "Out-of-range index in sendIndexes: " << idx;
        return false;
      }
      send_indexes.push_back(static_cast<int>(idx));
    }

    for (const auto& ssrc_val : ans["ssrcs"]) {
      if (!ssrc_val.is_number_integer()) {
        LOG_ERROR << "Non-integer element in ssrcs";
        return false;
      }
      const int64_t ssrc = ssrc_val.get<int64_t>();
      if (ssrc < 0 || ssrc > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        LOG_ERROR << "Out-of-range ssrc in ANSWER: " << ssrc;
        return false;
      }
      ssrcs.push_back(static_cast<uint32_t>(ssrc));
    }

    if (send_indexes.empty() || send_indexes.size() != ssrcs.size()) {
      LOG_ERROR << "Invalid or mismatched sendIndexes / ssrcs sizes in ANSWER";
      return false;
    }

    out_params = NegotiatedSessionParams{};
    out_params.receiver_udp_port = static_cast<uint16_t>(udp_port);

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

}  // namespace castcore
