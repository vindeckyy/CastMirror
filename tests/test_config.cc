#include <gtest/gtest.h>
#include "castcore/config.h"
#include <filesystem>

using namespace castcore;

TEST(ConfigTest, DefaultsAndSaveLoad) {
  std::string temp_path = "/tmp/castmirror_test_config.json";
  if (std::filesystem::exists(temp_path)) {
    std::filesystem::remove(temp_path);
  }

  auto& store = ConfigStore::Instance();
  auto& cfg = store.Mutable();

  cfg.last_device_id = "test-device-uuid-1234";
  cfg.last_device_name = "Living Room TV";
  cfg.last_device_ip = "192.168.1.150";
  cfg.last_display_id = 1;
  cfg.audio_enabled = true;
  cfg.quality_preset = QualityPreset::kHigh;
  cfg.target_delay_ms = 200;
  cfg.max_bitrate_kbps = 12000;
  cfg.bitrate_kbps_high = 15000;
  cfg.bitrate_kbps_smooth = 0;

  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kHigh), 15000u);
  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kSmooth), 5000u);

  EXPECT_TRUE(store.Save(temp_path));
  EXPECT_TRUE(std::filesystem::exists(temp_path));

  // Reset and load back
  cfg.last_device_id = "";
  cfg.last_device_name = "";
  cfg.quality_preset = QualityPreset::kSmooth;
  cfg.bitrate_kbps_high = 0;

  EXPECT_TRUE(store.Load(temp_path));
  EXPECT_EQ(cfg.last_device_id, "test-device-uuid-1234");
  EXPECT_EQ(cfg.last_device_name, "Living Room TV");
  EXPECT_EQ(cfg.last_device_ip, "192.168.1.150");
  EXPECT_EQ(cfg.last_display_id, 1);
  EXPECT_EQ(cfg.quality_preset, QualityPreset::kHigh);
  EXPECT_EQ(cfg.target_delay_ms, 200);
  EXPECT_EQ(cfg.max_bitrate_kbps, 12000u);
  EXPECT_EQ(cfg.bitrate_kbps_high, 15000u);
  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kHigh), 15000u);
  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kSmooth), 5000u);

  std::filesystem::remove(temp_path);
}
