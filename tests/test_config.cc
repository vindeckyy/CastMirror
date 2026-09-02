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
  cfg.capture_fps = 45;
  cfg.audio_bitrate_bps = 96000;
  cfg.silence_host_speakers = false;
  cfg.adaptive_enabled = false;
  cfg.subnet_scan_enabled = true;
  cfg.first_run_complete = true;
  cfg.window_width = 1000;
  cfg.window_height = 800;
  cfg.notify_on_events = false;
  cfg.force_software_encode = true;
  cfg.force_x11_capture = true;
  cfg.close_to_tray = false;
  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kHigh), 15000u);
  EXPECT_EQ(cfg.GetPresetBitrateKbps(QualityPreset::kSmooth), 5000u);

  EXPECT_TRUE(store.Save(temp_path));
  EXPECT_TRUE(std::filesystem::exists(temp_path));

  // Reset and load back
  cfg.last_device_id = "";
  cfg.last_device_name = "";
  cfg.quality_preset = QualityPreset::kSmooth;
  cfg.bitrate_kbps_high = 0;
  cfg.capture_fps = 0;
  cfg.audio_bitrate_bps = 192000;
  cfg.silence_host_speakers = true;
  cfg.subnet_scan_enabled = false;
  cfg.first_run_complete = false;
  cfg.window_width = 920;
  cfg.window_height = 700;
  cfg.notify_on_events = true;
  cfg.force_software_encode = false;
  cfg.force_x11_capture = false;
  cfg.close_to_tray = true;
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
  EXPECT_EQ(cfg.capture_fps, 45);
  EXPECT_EQ(cfg.audio_bitrate_bps, 96000u);
  EXPECT_FALSE(cfg.silence_host_speakers);
  // adaptive_enabled is forced to true by Validate() — the toggle was
  // removed from the UI and the hold-and-ramp behavior is always on.
  EXPECT_TRUE(cfg.adaptive_enabled);
  EXPECT_TRUE(cfg.subnet_scan_enabled);
  EXPECT_TRUE(cfg.first_run_complete);
  EXPECT_EQ(cfg.window_width, 1000);
  EXPECT_EQ(cfg.window_height, 800);
  EXPECT_FALSE(cfg.notify_on_events);
  EXPECT_TRUE(cfg.force_software_encode);
  EXPECT_TRUE(cfg.force_x11_capture);
  EXPECT_FALSE(cfg.close_to_tray);
  std::filesystem::remove(temp_path);
}

TEST(ConfigTest, PerDeviceProfilesPersistenceAndPresets) {
  std::string temp_path = "/tmp/castmirror_test_device_profiles.json";
  if (std::filesystem::exists(temp_path)) {
    std::filesystem::remove(temp_path);
  }

  auto& store = ConfigStore::Instance();
  auto& cfg = store.Mutable();

  DeviceProfile living_room;
  living_room.preset = QualityPreset::kCinema;
  living_room.bitrate_kbps = 16000;
  living_room.target_delay_ms = 400;
  living_room.target_fps = 60;
  cfg.SetDeviceProfile("dev-living-room", living_room);

  DeviceProfile bedroom;
  bedroom.preset = QualityPreset::kGame;
  bedroom.bitrate_kbps = 8000;
  bedroom.target_delay_ms = 150;
  bedroom.target_fps = 60;
  cfg.SetDeviceProfile("dev-bedroom", bedroom);

  EXPECT_TRUE(store.Save(temp_path));

  // Clear profiles in memory
  cfg.device_profiles.clear();
  EXPECT_FALSE(cfg.GetDeviceProfile("dev-living-room").has_value());

  // Load back
  EXPECT_TRUE(store.Load(temp_path));

  auto lr_loaded = cfg.GetDeviceProfile("dev-living-room");
  ASSERT_TRUE(lr_loaded.has_value());
  EXPECT_EQ(lr_loaded->preset, QualityPreset::kCinema);
  EXPECT_EQ(lr_loaded->bitrate_kbps, 16000u);
  EXPECT_EQ(lr_loaded->target_delay_ms, 400);

  auto br_loaded = cfg.GetDeviceProfile("dev-bedroom");
  ASSERT_TRUE(br_loaded.has_value());
  EXPECT_EQ(br_loaded->preset, QualityPreset::kGame);
  EXPECT_EQ(br_loaded->bitrate_kbps, 8000u);
  EXPECT_EQ(br_loaded->target_delay_ms, 150);

  std::filesystem::remove(temp_path);
}
