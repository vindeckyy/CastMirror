#include <gtest/gtest.h>
#include "castcore/adaptive_controller.h"
#include <thread>

using namespace castcore;

TEST(AdaptiveTest, DownshiftsOnPacketLoss) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 400;

  adaptive.Initialize(initial, QualityPreset::kAuto);
  int initial_rung = adaptive.GetCurrentLadderIndex();

  // Simulate lossy network: 8% packet loss
  RtcpFeedback fb;
  fb.fraction_lost = 0.08;
  fb.rtt_ms = 45.0;

  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  adaptive.CheckAdaptation(updated);

  // Second tick of loss triggers downshift
  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  bool changed = adaptive.CheckAdaptation(updated);
  EXPECT_TRUE(changed);
  EXPECT_GT(adaptive.GetCurrentLadderIndex(), initial_rung);
  EXPECT_LT(updated.bitrate_kbps, 8000u);
}

TEST(AdaptiveTest, DownshiftChangesFramerate) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 400;

  adaptive.Initialize(initial, QualityPreset::kAuto);

  RtcpFeedback fb;
  fb.fraction_lost = 0.08;
  fb.rtt_ms = 45.0;

  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  adaptive.CheckAdaptation(updated);

  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  bool changed = adaptive.CheckAdaptation(updated);
  EXPECT_TRUE(changed);
  // First downshift from 1080p60 (rung 3) is 1080p30 (rung 4)
  EXPECT_EQ(updated.current_resolution.width, 1920);
  EXPECT_EQ(updated.current_resolution.height, 1080);
  EXPECT_EQ(updated.current_framerate, 30);
}

TEST(AdaptiveTest, SecondDownshiftDropsTo720p) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 400;

  adaptive.Initialize(initial, QualityPreset::kAuto);

  RtcpFeedback fb;
  fb.fraction_lost = 0.08;
  fb.rtt_ms = 45.0;

  StreamStats updated;
  // Feed loss until we reach rung 5 (720p60)
  for (int i = 0; i < 6; ++i) {
    adaptive.OnFeedback(fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    adaptive.CheckAdaptation(updated);
    if (adaptive.GetCurrentLadderIndex() >= 5) break;
  }

  EXPECT_GE(adaptive.GetCurrentLadderIndex(), 5);
  EXPECT_LE(updated.current_resolution.width, 1280);
  EXPECT_LE(updated.current_resolution.height, 720);
}

TEST(AdaptiveTest, NeverExceedsInitialMax) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1280, 720};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 5000;
  initial.target_delay_ms = 200;

  adaptive.Initialize(initial, QualityPreset::kSmooth);

  RtcpFeedback clean_fb;
  clean_fb.fraction_lost = 0.0;
  clean_fb.rtt_ms = 10.0;

  StreamStats updated;
  for (int i = 0; i < 20; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Force time advance by setting last_eval_time_ through CheckAdaptation interval
  }

  EXPECT_LE(adaptive.GetCurrentLadderIndex(), 7);
  // Rung index should never go below 5 (720p60) since max is 720p
  EXPECT_GE(adaptive.GetCurrentLadderIndex(), 5);
}

TEST(AdaptiveTest, NackBurstTriggersImmediateDownshift) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 200;
  adaptive.Initialize(initial, QualityPreset::kAuto);
  const int initial_rung = adaptive.GetCurrentLadderIndex();

  RtcpFeedback fb;
  for (uint16_t i = 0; i < 12; ++i) {
    fb.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_GT(adaptive.GetCurrentLadderIndex(), initial_rung);
  EXPECT_LT(updated.bitrate_kbps, initial.bitrate_kbps);
}

TEST(AdaptiveTest, RaisingBitrateCapDoesNotRaiseAdaptiveTarget) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  adaptive.Initialize(initial, QualityPreset::kBalanced);

  adaptive.SetBitrateCapKbps(25000);
  EXPECT_EQ(adaptive.GetCurrentBitrateKbps(), 8000u);

  adaptive.SetBitrateCapKbps(5000);
  EXPECT_EQ(adaptive.GetCurrentBitrateKbps(), 5000u);
}

TEST(AdaptiveTest, DuplicateNacksCountOncePerFeedbackWindow) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  adaptive.Initialize(initial, QualityPreset::kAuto);
  const int initial_rung = adaptive.GetCurrentLadderIndex();

  RtcpFeedback fb;
  for (int i = 0; i < 20; ++i) {
    fb.nacks.push_back(PacketNack{42, 7});
  }
  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  EXPECT_FALSE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetCurrentLadderIndex(), initial_rung);
}

TEST(AdaptiveTest, DownshiftCooldownPreventsRungCascade) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  adaptive.Initialize(initial, QualityPreset::kAuto);

  RtcpFeedback burst;
  for (uint16_t i = 0; i < 12; ++i) {
    burst.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(burst);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  StreamStats updated;
  ASSERT_TRUE(adaptive.CheckAdaptation(updated));
  const int first_downshift = adaptive.GetCurrentLadderIndex();

  adaptive.OnFeedback(burst);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_FALSE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetCurrentLadderIndex(), first_downshift);
}

TEST(AdaptiveTest, ResetFeedbackWindowDropsPreReconnectNacks) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  adaptive.Initialize(initial, QualityPreset::kAuto);
  const int initial_rung = adaptive.GetCurrentLadderIndex();

  RtcpFeedback burst;
  for (uint16_t i = 0; i < 12; ++i) {
    burst.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(burst);
  adaptive.ResetFeedbackWindow();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  EXPECT_FALSE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetCurrentLadderIndex(), initial_rung);
}
