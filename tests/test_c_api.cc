#include <gtest/gtest.h>
#include "castcore/c_api.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST(CApiTest, LifecycleAndStateTransitions) {
  EXPECT_TRUE(castmirror_init());
  EXPECT_EQ(castmirror_get_state(), CASTMIRROR_STATE_IDLE);

  // Stats retrieval in idle state
  CastMirrorStreamStats stats{};
  EXPECT_TRUE(castmirror_get_stats(&stats));

  // Bitrate and delay runtime adjustments
  castmirror_set_bitrate(5000);
  castmirror_set_playout_delay(150);
  castmirror_set_freeze(true);
  castmirror_set_freeze(false);
  castmirror_set_muted(true);
  castmirror_set_muted(false);

  // Discovery controls
  castmirror_start_discovery();
  int dev_count = castmirror_get_device_count();
  EXPECT_GE(dev_count, 0);

  CastMirrorDeviceInfo dev_info{};
  EXPECT_FALSE(castmirror_get_device_info(-1, &dev_info));
  EXPECT_FALSE(castmirror_get_device_info(999, &dev_info));

  castmirror_stop_discovery();
  castmirror_shutdown();
}

TEST(CApiTest, DisplayAndWindowEnumeration) {
  ASSERT_TRUE(castmirror_init());

  int display_count = castmirror_get_display_count();
  EXPECT_GT(display_count, 0);

  CastMirrorDisplayInfo display{};
  EXPECT_TRUE(castmirror_get_display_info(0, &display));
  EXPECT_GT(display.width, 0);
  EXPECT_GT(display.height, 0);
  EXPECT_FALSE(castmirror_get_display_info(-1, &display));
  EXPECT_FALSE(castmirror_get_display_info(display_count, &display));
  EXPECT_FALSE(castmirror_get_display_info(0, nullptr));

  int window_count = castmirror_get_window_count();
  EXPECT_GE(window_count, 0);
  if (window_count > 0) {
    CastMirrorWindowInfo window{};
    EXPECT_TRUE(castmirror_get_window_info(0, &window));
    EXPECT_FALSE(castmirror_get_window_info(window_count, &window));
  }
  EXPECT_FALSE(castmirror_get_window_info(0, nullptr));
  (void)castmirror_window_capture_supported();

  castmirror_shutdown();
}

TEST(CApiTest, LastErrorBufferContract) {
  ASSERT_TRUE(castmirror_init());

  char buffer[64];
  EXPECT_EQ(castmirror_get_last_error(nullptr, sizeof(buffer)), -1);
  EXPECT_EQ(castmirror_get_last_error(buffer, 0), -1);

  int len = castmirror_get_last_error(buffer, sizeof(buffer));
  EXPECT_GE(len, 0);
  EXPECT_EQ(buffer[len], '\0');

  // Tiny buffer must still be NUL-terminated.
  char tiny[4] = {'x', 'x', 'x', 'x'};
  int tiny_len = castmirror_get_last_error(tiny, sizeof(tiny));
  EXPECT_GE(tiny_len, 0);
  EXPECT_LE(tiny_len, 3);
  EXPECT_EQ(tiny[tiny_len], '\0');

  castmirror_shutdown();
}

TEST(CApiTest, ConfigJsonRoundTrip) {
  ASSERT_TRUE(castmirror_init());

  int needed = castmirror_get_config_json(nullptr, 0);
  ASSERT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed));
  int written = castmirror_get_config_json(buf.data(), needed);
  ASSERT_GT(written, 0);
  EXPECT_LE(written, needed - 1);
  EXPECT_EQ(buf[static_cast<size_t>(written)], '\0');

  nlohmann::json original = nlohmann::json::parse(std::string(buf.data(), static_cast<size_t>(written)));
  for (const char* key : {"quality_preset", "capture_fps", "audio_bitrate_bps",
                          "adaptive_resolution_enabled", "ui_theme"}) {
    EXPECT_TRUE(original.contains(key)) << key;
  }

  // Malformed and non-object payloads are rejected without touching the config.
  EXPECT_FALSE(castmirror_set_config_json(nullptr));
  EXPECT_FALSE(castmirror_set_config_json("{not json"));
  EXPECT_FALSE(castmirror_set_config_json("[1,2,3]"));

  nlohmann::json patch = {
      {"capture_fps", 30},
      {"ui_theme", "dark"},
      {"adaptive_resolution_enabled", false},
      {"audio_bitrate_bps", 96000},
  };
  ASSERT_TRUE(castmirror_set_config_json(patch.dump().c_str()));

  needed = castmirror_get_config_json(nullptr, 0);
  std::vector<char> after_buf(static_cast<size_t>(needed));
  written = castmirror_get_config_json(after_buf.data(), needed);
  ASSERT_GT(written, 0);
  nlohmann::json after = nlohmann::json::parse(std::string(after_buf.data(), static_cast<size_t>(written)));
  EXPECT_EQ(after["capture_fps"].get<int>(), 30);
  EXPECT_EQ(after["ui_theme"].get<std::string>(), "dark");
  EXPECT_FALSE(after["adaptive_resolution_enabled"].get<bool>());
  EXPECT_EQ(after["audio_bitrate_bps"].get<uint32_t>(), 96000u);
  // Keys that were not part of the patch survive the merge.
  EXPECT_EQ(after["quality_preset"].get<std::string>(), original["quality_preset"].get<std::string>());
  EXPECT_EQ(after["target_delay_ms"].get<int>(), original["target_delay_ms"].get<int>());

  // Unknown keys are ignored, not fatal.
  EXPECT_TRUE(castmirror_set_config_json(R"({"not_a_real_key": 42})"));

  // Restore whatever the user (or a previous test) had.
  ASSERT_TRUE(castmirror_set_config_json(original.dump().c_str()));

  castmirror_shutdown();
}

TEST(CApiTest, SelfTestReportsSubsystems) {
  ASSERT_TRUE(castmirror_init());

  int needed = castmirror_self_test(nullptr, 0);
  ASSERT_GT(needed, 0);
  std::vector<char> buf(static_cast<size_t>(needed));
  int written = castmirror_self_test(buf.data(), needed);
  ASSERT_GT(written, 0);
  EXPECT_EQ(buf[static_cast<size_t>(written)], '\0');

  nlohmann::json result = nlohmann::json::parse(std::string(buf.data(), static_cast<size_t>(written)));
  for (const char* key : {"capture", "encoder", "audio", "network"}) {
    ASSERT_TRUE(result.contains(key)) << key;
    EXPECT_TRUE(result[key].contains("ok")) << key;
    EXPECT_TRUE(result[key]["ok"].is_boolean()) << key;
    EXPECT_TRUE(result[key]["detail"].is_string()) << key;
  }

  castmirror_shutdown();
}

TEST(CApiTest, StreamStatsCarryPipelineDetails) {
  ASSERT_TRUE(castmirror_init());

  CastMirrorStreamStats stats{};
  ASSERT_TRUE(castmirror_get_stats(&stats));
  EXPECT_GE(stats.current_framerate, 0);
  EXPECT_GE(stats.adaptive_rung_count, 0);
  // The string fields are NUL-terminated char arrays (empty while idle).
  EXPECT_EQ(stats.encoder_name[sizeof(stats.encoder_name) - 1], '\0');
  EXPECT_EQ(stats.capture_backend[sizeof(stats.capture_backend) - 1], '\0');
  EXPECT_EQ(stats.display_name[sizeof(stats.display_name) - 1], '\0');

  // The size-query contract holds for a too-small buffer too.
  char tiny[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
  int len = castmirror_get_config_json(tiny, sizeof(tiny));
  EXPECT_GE(len, 0);
  EXPECT_LE(len, static_cast<int>(sizeof(tiny)) - 1);
  EXPECT_EQ(tiny[len], '\0');

  castmirror_shutdown();
}
