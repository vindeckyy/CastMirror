#include <gtest/gtest.h>
#include "castcore/capability_model.h"

using namespace castcore;

TEST(CapabilityModelTest, ClassifyChromecastDevices) {
  CastDevice cc3;
  cc3.model_name = "Chromecast";
  auto caps3 = CapabilityModel::Evaluate(cc3);
  EXPECT_EQ(caps3.device_family, "Chromecast Gen 3");
  EXPECT_EQ(caps3.max_resolution.width, 1920);
  EXPECT_EQ(caps3.max_fps, 60);

  CastDevice ultra;
  ultra.model_name = "Chromecast Ultra";
  auto caps_ultra = CapabilityModel::Evaluate(ultra);
  EXPECT_EQ(caps_ultra.device_family, "Chromecast Ultra");
  EXPECT_EQ(caps_ultra.max_resolution.width, 3840);
  EXPECT_EQ(caps_ultra.max_fps, 60);
  EXPECT_TRUE(caps_ultra.supports_vp9);

  CastDevice nesthub;
  nesthub.model_name = "Nest Hub";
  auto caps_nest = CapabilityModel::Evaluate(nesthub);
  EXPECT_EQ(caps_nest.device_family, "Nest Hub");
  EXPECT_EQ(caps_nest.max_resolution.width, 1280);
  EXPECT_EQ(caps_nest.max_fps, 60);

  CastDevice gen2;
  gen2.model_name = "NC2-6A5";
  auto caps_gen2 = CapabilityModel::Evaluate(gen2);
  EXPECT_EQ(caps_gen2.device_family, "Chromecast Gen 1/2");
  EXPECT_EQ(caps_gen2.max_resolution.width, 1920);
  EXPECT_EQ(caps_gen2.max_fps, 30);
  EXPECT_EQ(caps_gen2.h264_level, "4.1");
}
TEST(CapabilityModelTest, PresetRecommendations) {
  CastDevice dev;
  dev.model_name = "Chromecast Ultra";

  auto stats_high =
      CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kHigh, 3840, 2160, 60);
  EXPECT_EQ(stats_high.current_resolution.width, 3840);
  EXPECT_EQ(stats_high.current_resolution.height, 2160);
  EXPECT_EQ(stats_high.current_framerate, 60);

  auto stats_smooth =
      CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kSmooth, 3840, 2160, 60);
  EXPECT_EQ(stats_smooth.current_resolution.width, 1280);
  EXPECT_EQ(stats_smooth.current_resolution.height, 720);
  EXPECT_EQ(stats_smooth.target_delay_ms, 200);

  auto stats_auto =
      CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kAuto, 1920, 1080, 60);
  EXPECT_EQ(stats_auto.target_delay_ms, 200);
  EXPECT_EQ(stats_auto.current_resolution.width, 1920);
  EXPECT_GE(stats_auto.bitrate_kbps, 8000u);
}

// A frame rate the user picks is honoured up to what the receiver can decode.
TEST(CapabilityModelTest, UserCaptureFpsIsClampedToDeviceMax) {
  CastDevice dev;
  dev.model_name = "Chromecast Ultra";
  const auto caps = CapabilityModel::Evaluate(dev);
  auto stats =
      CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kAuto, 1920, 1080, 60, 120);
  EXPECT_EQ(stats.current_framerate, caps.max_fps);
  auto low = CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kAuto, 1920, 1080, 60, 24);
  EXPECT_EQ(low.current_framerate, 24);
}

TEST(CapabilityModelTest, GameAndCinemaPresetsAreDistinctFromAuto) {
  CastDevice dev;
  dev.model_name = "Chromecast Ultra";
  auto game = CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kGame, 1920, 1080, 60, 0);
  auto cinema =
      CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kCinema, 1920, 1080, 60, 0);
  EXPECT_EQ(game.target_delay_ms, 150);
  EXPECT_EQ(cinema.target_delay_ms, 400);
  EXPECT_GT(cinema.bitrate_kbps, game.bitrate_kbps);
}

TEST(CapabilityModelTest, GoogleTvStreamerAndUltraCodec4KGating) {
  CastDevice streamer;
  streamer.model_name = "Google TV Streamer";
  auto caps_streamer = CapabilityModel::Evaluate(streamer);
  EXPECT_EQ(caps_streamer.max_resolution.width, 3840);
  EXPECT_TRUE(caps_streamer.supports_hevc);
  EXPECT_TRUE(caps_streamer.supports_vp9);

  CastDevice ultra;
  ultra.model_name = "Chromecast Ultra";
  auto caps_ultra = CapabilityModel::Evaluate(ultra);
  EXPECT_EQ(caps_ultra.max_resolution.width, 3840);
  EXPECT_TRUE(caps_ultra.supports_vp9);

  CastDevice gen1;
  gen1.model_name = "H2G2-42";
  auto caps_gen1 = CapabilityModel::Evaluate(gen1);
  EXPECT_EQ(caps_gen1.max_resolution.width, 1920);
  EXPECT_FALSE(caps_gen1.supports_hevc);
}
