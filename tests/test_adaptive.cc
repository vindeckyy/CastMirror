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

  // NACKs with actual packet loss should trigger downshift
  RtcpFeedback fb;
  fb.fraction_lost = 0.05;
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

TEST(AdaptiveTest, NacksWithZeroLossDoNotDownshift) {
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 200;
  adaptive.Initialize(initial, QualityPreset::kAuto);
  const int initial_rung = adaptive.GetCurrentLadderIndex();

  // 12 NACKs with 0% loss — retransmissions are working, no downshift needed
  RtcpFeedback fb;
  fb.fraction_lost = 0.0;
  for (uint16_t i = 0; i < 12; ++i) {
    fb.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  StreamStats updated;
  EXPECT_FALSE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetCurrentLadderIndex(), initial_rung);
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
  burst.fraction_lost = 0.05;
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

TEST(AdaptiveTest, DynamicPlayoutDelayScaling) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = AdaptiveController::kMinPlayoutDelayMs; // 150ms

  adaptive.Initialize(initial, QualityPreset::kAuto);
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), AdaptiveController::kMinPlayoutDelayMs);
  EXPECT_EQ(adaptive.GetCurrentTargetDelayMs(), 150);

  adaptive.SetEvaluationIntervalMsForTest(10);

  // 1. Step up from 150ms to 200ms with packet loss (> 3%)
  RtcpFeedback loss_fb;
  loss_fb.fraction_lost = 0.05;
  loss_fb.rtt_ms = 15.0;
  adaptive.OnFeedback(loss_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));

  StreamStats updated;
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 200);
  EXPECT_EQ(updated.target_delay_ms, 200);

  // 2. Step up from 200ms to 300ms on next loss interval
  adaptive.ResetEvalTimeForTest();
  adaptive.OnFeedback(loss_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 300);
  EXPECT_EQ(updated.target_delay_ms, 300);

  // 3. Step up from 300ms to 400ms on next loss interval
  adaptive.ResetEvalTimeForTest();
  adaptive.OnFeedback(loss_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 400);
  EXPECT_EQ(updated.target_delay_ms, 400);

  // 4. Stays at max playout delay (400ms) on further loss
  adaptive.ResetEvalTimeForTest();
  adaptive.OnFeedback(loss_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  adaptive.CheckAdaptation(updated);
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), AdaptiveController::kMaxPlayoutDelayMs);

  // 5. Clean network: 14 intervals do not step down yet
  RtcpFeedback clean_fb;
  clean_fb.fraction_lost = 0.0;
  clean_fb.rtt_ms = 10.0;
  clean_fb.jitter = 2;

  for (int i = 0; i < 14; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    adaptive.CheckAdaptation(updated);
    EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 400);
  }

  // 15th clean interval triggers step down: 400ms -> 300ms
  adaptive.OnFeedback(clean_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 300);
  EXPECT_EQ(updated.target_delay_ms, 300);

  // 15 more clean intervals triggers step down: 300ms -> 200ms
  for (int i = 0; i < 14; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    adaptive.CheckAdaptation(updated);
    EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 300);
  }
  adaptive.OnFeedback(clean_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 200);

  // 15 more clean intervals triggers step down: 200ms -> 150ms
  for (int i = 0; i < 14; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    adaptive.CheckAdaptation(updated);
    EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 200);
  }
  adaptive.OnFeedback(clean_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 150);

  // Further clean intervals stay at 150ms floor
  for (int i = 0; i < 15; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    adaptive.CheckAdaptation(updated);
  }
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), AdaptiveController::kMinPlayoutDelayMs);

  // 6. Test manual / dynamic playout delay update and clamping
  adaptive.SetPlayoutDelayMs(250);
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 250);
  adaptive.SetPlayoutDelayMs(50); // below min
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 150);
  adaptive.SetPlayoutDelayMs(800); // above max
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 400);
}

TEST(AdaptiveTest, JitterAwareDelayAdaptation) {
  AdaptiveController adaptive;

  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 150;

  adaptive.Initialize(initial, QualityPreset::kAuto);
  adaptive.SetEvaluationIntervalMsForTest(10);

  // 1. Verify EWMA RTT & Jitter smoothing formulas:
  // Sample 1: RTT = 50ms, Jitter = 20ms
  RtcpFeedback fb1;
  fb1.rtt_ms = 50.0;
  fb1.jitter = 20;
  fb1.fraction_lost = 0.0;
  adaptive.OnFeedback(fb1);
  EXPECT_DOUBLE_EQ(adaptive.GetEwmaRttMs(), 50.0);
  EXPECT_DOUBLE_EQ(adaptive.GetEwmaJitterMs(), 20.0);

  // Sample 2: RTT = 100ms, Jitter = 40ms
  // EWMA_RTT = 0.8 * 50.0 + 0.2 * 100.0 = 60.0 ms
  // EWMA_Jitter = 0.9 * 20.0 + 0.1 * 40.0 = 22.0 ms
  RtcpFeedback fb2;
  fb2.rtt_ms = 100.0;
  fb2.jitter = 40;
  fb2.fraction_lost = 0.0;
  adaptive.OnFeedback(fb2);
  EXPECT_NEAR(adaptive.GetEwmaRttMs(), 60.0, 0.001);
  EXPECT_NEAR(adaptive.GetEwmaJitterMs(), 22.0, 0.001);

  // 2. High Jitter without packet loss (> 30ms EWMA Jitter) causes playout delay step up:
  // Inject higher jitter sample to push EWMA_Jitter > 30ms (sample = 120ms)
  // EWMA_Jitter = 0.9 * 22.0 + 0.1 * 120.0 = 19.8 + 12.0 = 31.8 ms
  RtcpFeedback high_jitter_fb;
  high_jitter_fb.rtt_ms = 20.0;
  high_jitter_fb.jitter = 120;
  high_jitter_fb.fraction_lost = 0.0;
  adaptive.OnFeedback(high_jitter_fb);
  EXPECT_GT(adaptive.GetEwmaJitterMs(), 30.0);

  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  StreamStats updated;
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  // Delay stepped up from 150ms -> 200ms solely due to high jitter
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 200);
  EXPECT_EQ(updated.target_delay_ms, 200);

  // Second tick with high jitter steps up delay from 200ms -> 300ms
  adaptive.ResetEvalTimeForTest();
  adaptive.OnFeedback(high_jitter_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 300);
  EXPECT_EQ(updated.target_delay_ms, 300);

  // Third tick with high jitter steps up delay from 300ms -> 400ms
  adaptive.ResetEvalTimeForTest();
  adaptive.OnFeedback(high_jitter_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 400);
  EXPECT_EQ(updated.target_delay_ms, 400);

  // 3. Jitter recovery: jitter drops to 2ms, RTT drops to 10ms, loss is 0
  RtcpFeedback clean_fb;
  clean_fb.rtt_ms = 10.0;
  clean_fb.jitter = 2;
  clean_fb.fraction_lost = 0.0;

  // Run iterations to decay EWMA Jitter below 8ms and accumulate clean intervals
  for (int i = 0; i < 30; ++i) {
    adaptive.OnFeedback(clean_fb);
  }
  EXPECT_LT(adaptive.GetEwmaJitterMs(), 8.0);
  EXPECT_LT(adaptive.GetEwmaRttMs(), 25.0);

  // Evaluate clean intervals to step down 400ms -> 300ms
  for (int i = 0; i < 14; ++i) {
    adaptive.OnFeedback(clean_fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    adaptive.CheckAdaptation(updated);
    EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 400);
  }
  adaptive.OnFeedback(clean_fb);
  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  EXPECT_TRUE(adaptive.CheckAdaptation(updated));
  EXPECT_EQ(adaptive.GetPlayoutDelayMs(), 300);
}

TEST(AdaptiveTest, UpshiftAfterRecoveryForAllPresets) {
  // Verify that upshift works for Balanced preset (previously only Auto/High).
  // With the hold-and-ramp behavior, bitrate ramps back aggressively (~3 clean
  // intervals per step) and resolution upshifts on the slow cadence (10 clean
  // + 15s cooldown) after bitrate has recovered.
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 200;
  adaptive.Initialize(initial, QualityPreset::kBalanced);

  // Force a downshift with loss
  RtcpFeedback lossy;
  lossy.fraction_lost = 0.05;
  for (uint16_t i = 0; i < 12; ++i) {
    lossy.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(lossy);
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  StreamStats updated;
  ASSERT_TRUE(adaptive.CheckAdaptation(updated));
  ASSERT_GT(adaptive.GetCurrentLadderIndex(), 0);
  ASSERT_LT(adaptive.GetCurrentBitrateKbps(), 8000u);

  // Feed clean feedback. Bitrate ramps back in ~12 clean intervals (4 ramp
  // steps × 3 clean each), then resolution upshifts after 10 more clean
  // intervals + 15s since downshift. 28 iterations gives ample margin.
  RtcpFeedback clean;
  clean.fraction_lost = 0.0;
  clean.rtt_ms = 10.0;
  for (int i = 0; i < 28; ++i) {
    adaptive.OnFeedback(clean);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    adaptive.CheckAdaptation(updated);
  }
  // Bitrate should have ramped back to the custom target (8000)
  EXPECT_EQ(adaptive.GetCurrentBitrateKbps(), 8000u);
  // Resolution should have upshifted back to the original rung (3 = 1080p60)
  EXPECT_EQ(adaptive.GetCurrentLadderIndex(), 3);
}

TEST(AdaptiveTest, CustomBitrateHoldsAndRampsBackUp) {
  // Verify that a user-selected custom bitrate is held on a clean link,
  // drops on congestion, and aggressively ramps back to the target.
  AdaptiveController adaptive;
  StreamStats initial;
  initial.current_resolution = {1920, 1080};
  initial.current_framerate = 60;
  initial.bitrate_kbps = 8000;
  initial.target_delay_ms = 200;
  adaptive.Initialize(initial, QualityPreset::kAuto);
  adaptive.SetEvaluationIntervalMsForTest(50);

  // Confirm bitrate holds at 8000 on clean feedback
  RtcpFeedback clean;
  clean.fraction_lost = 0.0;
  clean.rtt_ms = 10.0;
  clean.jitter = 2;
  for (int i = 0; i < 5; ++i) {
    adaptive.OnFeedback(clean);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    StreamStats updated;
    adaptive.CheckAdaptation(updated);
  }
  EXPECT_EQ(adaptive.GetCurrentBitrateKbps(), 8000u);

  // Force a downshift with severe congestion
  RtcpFeedback severe;
  severe.fraction_lost = 0.20;
  for (uint16_t i = 0; i < 60; ++i) {
    severe.nacks.push_back(PacketNack{10, i});
  }
  adaptive.OnFeedback(severe);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  StreamStats updated;
  ASSERT_TRUE(adaptive.CheckAdaptation(updated));
  ASSERT_LT(adaptive.GetCurrentBitrateKbps(), 8000u);

  uint32_t after_drop = adaptive.GetCurrentBitrateKbps();

  // Feed clean feedback — bitrate should ramp back aggressively
  // (3 clean intervals per step, ~50% of gap each step)
  for (int i = 0; i < 20; ++i) {
    adaptive.OnFeedback(clean);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    adaptive.CheckAdaptation(updated);
  }

  // Bitrate should have ramped back to the custom target (8000)
  EXPECT_EQ(adaptive.GetCurrentBitrateKbps(), 8000u);
  EXPECT_GT(adaptive.GetCurrentBitrateKbps(), after_drop);
}
