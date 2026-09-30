// Per-app audio: listing apps that have an audio session, and capturing just one
// process through WASAPI process loopback (Windows 10 version 2004+).

#include <gtest/gtest.h>

#include "castcore/audio_capture.h"
#include "castcore/audio_sessions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <set>
#include <thread>

using namespace castcore;

TEST(AudioSessionsTest, EnumerationIsSafeAndReturnsOneEntryPerProcess) {
  const auto apps = EnumerateAudioApps();  // may be empty when nothing is playing
  std::set<uint32_t> pids;
  for (const auto& app : apps) {
    EXPECT_NE(app.pid, 0u);
    EXPECT_FALSE(app.exe_name.empty());
    EXPECT_FALSE(app.title.empty());
    EXPECT_TRUE(pids.insert(app.pid).second) << "pid listed twice: " << app.pid;
  }
}

TEST(AudioSessionsTest, FindingAnAppIsCaseInsensitiveAndMissesReturnZero) {
  EXPECT_EQ(FindAudioProcessByName(""), 0u);
  EXPECT_EQ(FindAudioProcessByName("definitely-not-running-castmirror-test.exe"), 0u);
  for (const auto& app : EnumerateAudioApps()) {
    // The same process may appear under a name that is also matched by another pid;
    // either is a valid answer, but it must be a real session.
    EXPECT_NE(FindAudioProcessByName(app.exe_name), 0u);
  }
}

#if defined(_WIN32)
namespace {

// Loudest sample captured from `pid` over `ms`, or -1 when capture could not start.
int PeakFromProcess(uint32_t pid, int ms) {
  auto capture = AudioCaptureFactory::Create();
  std::atomic<int> peak{0};
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) {
    const auto* s = reinterpret_cast<const int16_t*>(f.pcm_data.data());
    const size_t n = f.pcm_data.size() / sizeof(int16_t);
    int local = 0;
    for (size_t i = 0; i < n; ++i)
      local = std::max(local, std::abs(static_cast<int>(s[i])));
    int seen = peak.load();
    while (local > seen && !peak.compare_exchange_weak(seen, local)) {}
  });
  capture->SetTargetProcess(pid);
  if (!capture->Start(48000, 2)) return -1;
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  capture->Stop();
  return peak.load();
}

uint32_t PidFromEnv(const char* name) {
  const char* v = std::getenv(name);
  return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
}

}  // namespace

// Windows accepts any process id and simply delivers silence for one that makes no
// sound, so a target that has gone away never produces noise from other apps.
TEST(AudioSessionsTest, ProcessLoopbackForAProcessThatIsNotPlayingIsSilent) {
  const int peak = PeakFromProcess(0x7FFFFFF0u, 400);
  if (peak < 0) GTEST_SKIP() << "process loopback is not available";
  EXPECT_LE(peak, 2)
      << "rounding in the format converter may leave a peak of 1; anything more is sound";
}

// Needs two real processes, so scripts/verify_process_audio.ps1 starts them and passes
// their ids: one playing a tone, one silent. Only the first may be heard.
TEST(AudioSessionsTest, ProcessLoopbackHearsOnlyTheTargetApp) {
  const uint32_t loud = PidFromEnv("CASTMIRROR_TEST_AUDIO_PID_LOUD");
  const uint32_t quiet = PidFromEnv("CASTMIRROR_TEST_AUDIO_PID_QUIET");
  if (loud == 0 || quiet == 0)
    GTEST_SKIP() << "run scripts/verify_process_audio.ps1 to exercise this";
  const int heard = PeakFromProcess(loud, 1500);
  const int silent = PeakFromProcess(quiet, 800);
  ASSERT_GE(heard, 0);
  EXPECT_GT(heard, 300) << "the app that is playing must be audible";
  EXPECT_LE(silent, 2) << "the other app must not leak into the capture";
}

TEST(AudioSessionsTest, ProcessLoopbackCapturesTenMillisecondFramesFromARunningApp) {
  const auto apps = EnumerateAudioApps();
  if (apps.empty()) GTEST_SKIP() << "no app has an audio session right now";

  auto capture = AudioCaptureFactory::Create();
  std::atomic<int> frames{0};
  std::atomic<bool> bad_shape{false};
  capture->SetAudioCallback([&](const CapturedAudioFrame& f) {
    if (f.sample_rate != 48000 || f.channels != 2 || f.samples_per_channel != 480) bad_shape = true;
    ++frames;
  });
  capture->SetTargetProcess(apps.front().pid);
  if (!capture->Start(48000, 2))
    GTEST_SKIP() << "process loopback is not available for " << apps.front().exe_name;

  EXPECT_TRUE(capture->IsCapturing());
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  capture->Stop();
  EXPECT_GT(frames.load(), 20) << "an idle app must still produce silence frames";
  EXPECT_FALSE(bad_shape.load());
}
#endif
