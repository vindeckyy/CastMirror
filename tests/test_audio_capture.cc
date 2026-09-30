// Coverage for the synthetic audio backend — the fallback every session uses
// when PulseAudio/WASAPI is unavailable — and for JoinOrDetach, the helper that
// stops capture threads without Stop() deadlocking on its own worker.

#include <gtest/gtest.h>
#include "castcore/audio_capture.h"
#include "castcore/thread_util.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

using namespace castcore;

namespace {

struct FrameCollector {
  std::mutex m;
  std::condition_variable cv;
  std::vector<CapturedAudioFrame> frames;

  void operator()(const CapturedAudioFrame& frame) {
    std::lock_guard<std::mutex> lock(m);
    frames.push_back(frame);
    cv.notify_all();
  }

  bool WaitFor(size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, timeout, [&] { return frames.size() >= count; });
  }

  std::vector<CapturedAudioFrame> Snapshot() {
    std::lock_guard<std::mutex> lock(m);
    return frames;
  }
};

}  // namespace

TEST(AudioCaptureTest, SyntheticFramesAreExactlyTenMilliseconds) {
  auto capture = AudioCaptureFactory::CreateSynthetic();
  ASSERT_NE(capture, nullptr);

  FrameCollector collector;
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) { collector(f); });
  ASSERT_TRUE(capture->Start(48000, 2));
  EXPECT_TRUE(capture->IsCapturing());

  ASSERT_TRUE(collector.WaitFor(3, std::chrono::seconds(5)))
      << "synthetic capture produced no frames within 5 s";
  capture->Stop();
  EXPECT_FALSE(capture->IsCapturing());

  const auto frames = collector.Snapshot();
  ASSERT_GE(frames.size(), 3u);
  for (const auto& f : frames) {
    EXPECT_EQ(f.sample_rate, 48000);
    EXPECT_EQ(f.channels, 2);
    EXPECT_EQ(f.samples_per_channel, 48000 / 100);  // 10 ms frame
    EXPECT_EQ(f.pcm_data.size(), static_cast<size_t>(480) * 2 * sizeof(int16_t));
  }
}

TEST(AudioCaptureTest, FramesAreTenMillisecondPacedWithStrictlyMonotonicTimestamps) {
  auto capture = AudioCaptureFactory::CreateSynthetic();
  FrameCollector collector;
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) { collector(f); });
  ASSERT_TRUE(capture->Start(48000, 2));
  ASSERT_TRUE(collector.WaitFor(12, std::chrono::seconds(5)));
  capture->Stop();

  const auto frames = collector.Snapshot();
  ASSERT_GE(frames.size(), 12u);

  for (size_t i = 1; i < frames.size(); ++i) {
    EXPECT_GT(frames[i].timestamp, frames[i - 1].timestamp)
        << "capture timestamps must be strictly monotonic (frame " << i << ")";
  }

  const int64_t span_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              frames.back().timestamp - frames.front().timestamp)
                              .count();
  const double mean_ms =
      static_cast<double>(span_us) / 1000.0 / static_cast<double>(frames.size() - 1);
  EXPECT_GE(mean_ms, 4.0) << "frames are not paced to ~10 ms (got " << mean_ms << " ms)";
  EXPECT_LE(mean_ms, 30.0) << "frames are not paced to ~10 ms (got " << mean_ms << " ms)";
}

TEST(AudioCaptureTest, FrameSizeFollowsRequestedSampleRateAndChannelCount) {
  auto capture = AudioCaptureFactory::CreateSynthetic();
  FrameCollector collector;
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) { collector(f); });
  ASSERT_TRUE(capture->Start(16000, 1));
  ASSERT_TRUE(collector.WaitFor(1, std::chrono::seconds(5)));
  capture->Stop();

  const auto frames = collector.Snapshot();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().sample_rate, 16000);
  EXPECT_EQ(frames.front().channels, 1);
  EXPECT_EQ(frames.front().samples_per_channel, 16000 / 100);  // 160
  EXPECT_EQ(frames.front().pcm_data.size(), static_cast<size_t>(160) * sizeof(int16_t));
}

TEST(AudioCaptureTest, StopIsIdempotentAndRestartWorks) {
  auto capture = AudioCaptureFactory::CreateSynthetic();
  EXPECT_FALSE(capture->IsCapturing());
  capture->Stop();  // no-op before Start
  EXPECT_FALSE(capture->IsCapturing());

  ASSERT_TRUE(capture->Start(48000, 2));
  EXPECT_TRUE(capture->IsCapturing());
  capture->Stop();
  EXPECT_FALSE(capture->IsCapturing());
  capture->Stop();  // a second Stop must return, not hang

  ASSERT_TRUE(capture->Start(48000, 2));
  EXPECT_TRUE(capture->IsCapturing());
  capture->Stop();
}

TEST(AudioCaptureTest, DefaultStartUsesFortyEightKiloHertzStereo) {
  auto capture = AudioCaptureFactory::CreateSynthetic();
  FrameCollector collector;
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) { collector(f); });
  ASSERT_TRUE(capture->Start());
  ASSERT_TRUE(collector.WaitFor(1, std::chrono::seconds(5)));
  capture->Stop();

  const auto frames = collector.Snapshot();
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().sample_rate, 48000);
  EXPECT_EQ(frames.front().channels, 2);
  EXPECT_EQ(frames.front().samples_per_channel, 480);
}

// ---------------------------------------------------------------------------
// JoinOrDetach
// ---------------------------------------------------------------------------

TEST(ThreadUtilTest, JoinOrDetachIgnoresAJoinlessThread) {
  std::thread worker;
  ASSERT_FALSE(worker.joinable());
  JoinOrDetach(worker, 500, "joinless-worker");
  EXPECT_FALSE(worker.joinable());
}

TEST(ThreadUtilTest, JoinOrDetachReturnsWhenWorkerExitsQuickly) {
  std::thread worker([] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); });
  ASSERT_TRUE(worker.joinable());
  JoinOrDetach(worker, 500, "quick-worker");
  EXPECT_FALSE(worker.joinable()) << "an idle worker must be joined, not detached";
}

TEST(ThreadUtilTest, JoinOrDetachJoinsEvenWhenTheBudgetIsExceeded) {
  // The timeout is advisory: the helper still joins (and only warns). Documented
  // here so nobody "optimises" it into a detach that leaks the thread.
  std::thread worker([] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); });
  const auto start = std::chrono::steady_clock::now();
  JoinOrDetach(worker, 1, "slow-worker");
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  EXPECT_FALSE(worker.joinable());
  EXPECT_GE(elapsed_ms, 20);
}

TEST(ThreadUtilTest, JoinOrDetachDetachesWhenCalledFromTheWorkerItself) {
  std::atomic<bool> go{false};
  std::atomic<bool> returned{false};
  std::atomic<bool> done{false};

  std::thread worker;
  worker = std::thread([&] {
    while (!go.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // Invoked from the worker, on the worker's own handle: joining would be a
    // self-deadlock, so this must detach and return.
    JoinOrDetach(worker, 500, "self-worker");
    returned.store(true, std::memory_order_release);
    done.store(true, std::memory_order_release);
  });

  go.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(returned.load()) << "JoinOrDetach did not return when invoked from the worker";
  EXPECT_FALSE(worker.joinable()) << "the worker must have detached itself";
}

#if defined(_WIN32)
// Smoke test for the real loopback backend. Machines without a playback
// endpoint (CI runners) cannot open it, so a failed Start() skips.
TEST(AudioCaptureTest, WasapiLoopbackStartsDeliversTenMsFramesAndStops) {
  auto capture = AudioCaptureFactory::Create();
  ASSERT_NE(capture, nullptr);
  std::atomic<int> frames{0};
  std::atomic<bool> bad_shape{false};
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) {
    if (f.sample_rate != 48000 || f.channels != 2 || f.samples_per_channel != 480 ||
        f.pcm_data.size() != 480u * 2u * sizeof(int16_t)) {
      bad_shape = true;
    }
    ++frames;
  });
  if (!capture->Start(48000, 2)) GTEST_SKIP() << "no loopback endpoint on this machine";
  EXPECT_TRUE(capture->IsCapturing());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  capture->Stop();
  EXPECT_FALSE(capture->IsCapturing());
  EXPECT_GT(frames.load(), 20) << "an idle endpoint must still produce silence frames";
  EXPECT_FALSE(bad_shape.load());
}
#endif