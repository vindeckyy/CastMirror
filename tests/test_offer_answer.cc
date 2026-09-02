#include <gtest/gtest.h>
#include "castcore/mirroring_negotiator.h"
#include <nlohmann/json.hpp>
#include <fstream>

using namespace castcore;

TEST(OfferAnswerTest, CreateValidOfferJson) {
  StreamStats stats;
  stats.current_resolution = {1920, 1080};
  stats.current_framerate = 60;
  stats.bitrate_kbps = 6000;
  stats.target_delay_ms = 400;

  auto v_keys = MirroringNegotiator::GenerateRandomKeys();
  auto a_keys = MirroringNegotiator::GenerateRandomKeys();

  EXPECT_EQ(v_keys.aes_key_hex.length(), 32u);
  EXPECT_EQ(v_keys.aes_iv_mask_hex.length(), 32u);

  std::string offer_str = MirroringNegotiator::CreateOfferJson(
      1001, stats, true, v_keys, a_keys, VideoCodec::kH264, 400);

  auto j = nlohmann::json::parse(offer_str);
  EXPECT_EQ(j["type"], "OFFER");
  EXPECT_EQ(j["seqNum"], 1001);
  EXPECT_TRUE(j.contains("offer"));

  const auto& offer = j["offer"];
  EXPECT_EQ(offer["castMode"], "mirroring");
  EXPECT_TRUE(offer["supportedStreams"].is_array());
  EXPECT_EQ(offer["supportedStreams"].size(), 2u);

  const auto& audio_stream = offer["supportedStreams"][0];
  EXPECT_EQ(audio_stream["type"], "audio_source");
  EXPECT_EQ(audio_stream["codecName"], "opus");
  EXPECT_EQ(audio_stream["aesKey"], a_keys.aes_key_hex);

  const auto& video_stream = offer["supportedStreams"][1];
  EXPECT_EQ(video_stream["type"], "video_source");
  EXPECT_EQ(video_stream["codecName"], "h264");
  EXPECT_EQ(video_stream["aesKey"], v_keys.aes_key_hex);
}

TEST(OfferAnswerTest, ParseAnswerJson) {
  std::string answer_json = R"({
    "type": "ANSWER",
    "seqNum": 1001,
    "result": "ok",
    "answer": {
      "castMode": "mirroring",
      "udpPort": 33533,
      "sendIndexes": [0, 1],
      "ssrcs": [10001, 10002]
    }
  })";

  auto v_keys = MirroringNegotiator::GenerateRandomKeys();
  auto a_keys = MirroringNegotiator::GenerateRandomKeys();

  NegotiatedSessionParams params;
  EXPECT_TRUE(MirroringNegotiator::ParseAnswerJson(answer_json, v_keys, a_keys, params));
  EXPECT_EQ(params.receiver_udp_port, 33533);
  EXPECT_TRUE(params.has_audio);
  EXPECT_TRUE(params.has_video);
  EXPECT_EQ(params.audio_stream.receiver_ssrc, 10001u);
  EXPECT_EQ(params.video_stream.receiver_ssrc, 10002u);
}

TEST(OfferAnswerTest, OfferUsesCustomAudioBitrate) {
  StreamStats stats;
  stats.current_resolution = {1920, 1080};
  stats.current_framerate = 60;
  stats.bitrate_kbps = 6000;
  auto v_keys = MirroringNegotiator::GenerateRandomKeys();
  auto a_keys = MirroringNegotiator::GenerateRandomKeys();
  auto offer_str = MirroringNegotiator::CreateOfferJson(
      1, stats, true, v_keys, a_keys, VideoCodec::kH264, 200, 96000);
  auto j = nlohmann::json::parse(offer_str);
  EXPECT_EQ(j["offer"]["supportedStreams"][0]["bitRate"], 96000);
}

TEST(OfferAnswerTest, CreateStatusJsonForGetStatus) {
  StreamStats stats;
  stats.current_fps = 59.5;
  stats.bitrate_kbps = 8000;
  stats.round_trip_time_ms = 12.0;
  stats.packet_loss_fraction = 0.01;
  stats.current_resolution = {1920, 1080};
  auto s = MirroringNegotiator::CreateStatusJson(42, stats);
  auto j = nlohmann::json::parse(s);
  EXPECT_EQ(j["type"], "STATUS");
  EXPECT_EQ(j["seqNum"], 42);
  EXPECT_EQ(j["result"], "ok");
  EXPECT_TRUE(j["status"].is_array());
  EXPECT_EQ(j["status"][0]["ssrc"], 2);
}

TEST(OfferAnswerTest, OfferMatchesChromeGoldenNetlogSchema) {
  std::string golden_path = "tests/data/chrome_offer_golden.json";
  std::ifstream golden_file(golden_path);
  if (!golden_file.is_open()) {
    golden_path = "../tests/data/chrome_offer_golden.json";
    golden_file.open(golden_path);
  }
  ASSERT_TRUE(golden_file.is_open()) << "Failed to open chrome_offer_golden.json";

  nlohmann::json golden = nlohmann::json::parse(golden_file);

  StreamStats stats;
  stats.current_resolution = {1920, 1080};
  stats.current_framerate = 60;
  stats.bitrate_kbps = 6000;
  stats.target_delay_ms = 400;

  StreamEncryptionKeys v_keys;
  v_keys.aes_key_hex = "0123456789abcdef0123456789abcdef";
  v_keys.aes_iv_mask_hex = "fedcba9876543210fedcba9876543210";

  StreamEncryptionKeys a_keys;
  a_keys.aes_key_hex = "0123456789abcdef0123456789abcdef";
  a_keys.aes_iv_mask_hex = "fedcba9876543210fedcba9876543210";

  std::string actual_str = MirroringNegotiator::CreateOfferJson(
      1001, stats, true, v_keys, a_keys, VideoCodec::kH264, 400, 192000);

  nlohmann::json actual = nlohmann::json::parse(actual_str);

  // Assert top-level keys
  EXPECT_EQ(actual["type"], golden["type"]);
  EXPECT_EQ(actual["seqNum"], golden["seqNum"]);
  ASSERT_TRUE(actual.contains("offer"));

  const auto& actual_offer = actual["offer"];
  const auto& golden_offer = golden["offer"];
  EXPECT_EQ(actual_offer["castMode"], golden_offer["castMode"]);
  EXPECT_EQ(actual_offer["receiverGetStatus"], golden_offer["receiverGetStatus"]);

  // Assert stream count
  ASSERT_EQ(actual_offer["supportedStreams"].size(), golden_offer["supportedStreams"].size());

  // Check audio stream schema
  const auto& actual_audio = actual_offer["supportedStreams"][0];
  const auto& golden_audio = golden_offer["supportedStreams"][0];
  for (auto it = golden_audio.begin(); it != golden_audio.end(); ++it) {
    EXPECT_TRUE(actual_audio.contains(it.key())) << "Missing audio field: " << it.key();
    EXPECT_EQ(actual_audio[it.key()], it.value()) << "Audio field mismatch on: " << it.key();
  }

  // Check video stream schema
  const auto& actual_video = actual_offer["supportedStreams"][1];
  const auto& golden_video = golden_offer["supportedStreams"][1];
  for (auto it = golden_video.begin(); it != golden_video.end(); ++it) {
    EXPECT_TRUE(actual_video.contains(it.key())) << "Missing video field: " << it.key();
    EXPECT_EQ(actual_video[it.key()], it.value()) << "Video field mismatch on: " << it.key();
  }
}

