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
  EXPECT_EQ(caps_nest.max_fps, 30);
}

TEST(CapabilityModelTest, PresetRecommendations) {
  CastDevice dev;
  dev.model_name = "Chromecast Ultra";

  auto stats_high = CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kHigh, 3840, 2160, 60);
  EXPECT_EQ(stats_high.current_resolution.width, 3840);
  EXPECT_EQ(stats_high.current_resolution.height, 2160);
  EXPECT_EQ(stats_high.current_framerate, 60);

  auto stats_smooth = CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kSmooth, 3840, 2160, 60);
  EXPECT_EQ(stats_smooth.current_resolution.width, 1280);
  EXPECT_EQ(stats_smooth.current_resolution.height, 720);
  EXPECT_EQ(stats_smooth.target_delay_ms, 200);

  auto stats_auto = CapabilityModel::GetRecommendedSettings(dev, QualityPreset::kAuto, 1920, 1080, 60);
  EXPECT_EQ(stats_auto.target_delay_ms, 200);
  EXPECT_EQ(stats_auto.current_resolution.width, 1920);
  EXPECT_GE(stats_auto.bitrate_kbps, 8000u);
}
