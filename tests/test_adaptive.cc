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
