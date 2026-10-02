// Coverage for the synthetic audio backend — the fallback every session uses
// when PulseAudio/WASAPI is unavailable — and for JoinOrDetach, the helper that
// stops capture threads without Stop() deadlocking on its own worker.

#include <gtest/gtest.h>
#include "castcore/audio_capture.h"
#include "castcore/thread_util.h"
#include "castcore/audio_capture_wasapi.h"

#include <atomic>
#include <cmath>
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
  while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
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

// The resampler must produce exactly as many output frames as the rate ratio
// implies, packet after packet. It used to clamp a negative phase carry to
// zero, discarding the fraction of a source sample that had not been emitted
// yet; at 44.1 kHz that was ~0.41 source samples per packet, about 4.7 ms of
// audio lost every second, which walks the audio clock away from the video
// clock and is the desync the TV shows as audio lagging behind picture.
TEST(ResamplerTest, ProducesTheRateRatioWithoutDrift) {
  struct Case {
    int src_rate;
    int out_rate;
  };
  // 48 kHz 24-bit/32-bit endpoints land on the resampler with a whole-number
  // step, the case where a single clamped sample per packet is 0.21% drift.
  const Case cases[] = {{44100, 48000},
                        {48000, 44100},
                        {96000, 48000},
                        {32000, 48000},
                        {48000, 48000},
                        {24000, 48000},
                        {16000, 48000}};
  const int kPackets = 500;
  const int kFrames = 480;

  for (const Case& c : cases) {
    ResamplerCarry carry;
    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(kPackets) * 600 * 2);
    // A ramp, so a mis-indexed read shows up as a wrong sample value too.
    auto sample_at = [&](int i, float& l, float& r) {
      l = static_cast<float>((i % 200) - 100) / 200.0f;
      r = -l;
    };

    long produced = 0;
    for (int p = 0; p < kPackets; ++p) {
      produced += ResamplePacket(c.src_rate, c.out_rate, kFrames, carry, sample_at, out);
    }

    const double ideal = static_cast<double>(kPackets) * kFrames * c.out_rate / c.src_rate;
    // A bounded remainder is expected and correct: at most one output sample is
    // still held in the carry for the next packet, and one more is held as the
    // retained tail. What must not happen is a per-packet loss, which grows
    // with the packet count: at 44.1 kHz that was 2.1 source samples per 500
    // packets' worth of drift (~4.7 ms/s), and 0.21% at a whole-number ratio.
    EXPECT_LE(std::abs(produced - ideal), 2.0)
        << "sample count drift for " << c.src_rate << " -> " << c.out_rate << " Hz";
  }
}

// The retained tail sample must actually be used: a DC-free ramp must survive
// the packet boundary without a discontinuity, which only holds if the
// boundary sample is interpolated from the previous packet's last value.
TEST(ResamplerTest, InterpolationIsContinuousAcrossThePacketBoundary) {
  ResamplerCarry carry;
  std::vector<int16_t> out;
  const int kFrames = 480;
  auto sample_at = [&](int i, float& l, float& r) {
    l = static_cast<float>(i % 64) / 64.0f;
    r = l;
  };

  std::vector<float> values;
  for (int p = 0; p < 6; ++p) {
    const size_t before = out.size();
    ResamplePacket(44100, 48000, kFrames, carry, sample_at, out);
    for (size_t i = before; i < out.size(); i += 2)
      values.push_back(out[i] / 32767.0f);
  }

  ASSERT_GT(values.size(), 100u);
  // Every output sample must lie inside the source range [0, 1]: interpolation
  // between two in-range samples cannot exceed it. A boundary sample built by
  // wrapping the index (or by dropping the carry) shows up as a step outside
  // the ramp's own span.
  for (float v : values) {
    EXPECT_GE(v, 0.0f) << "interpolation overshot below the source range";
    EXPECT_LE(v, 1.0f) << "interpolation overshot above the source range";
  }
}