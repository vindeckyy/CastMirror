#include <gtest/gtest.h>
#include "castcore/display_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
extern "C" {
#include <X11/Xlib.h>
}
#endif

using namespace castcore;
using namespace std::chrono_literals;

namespace {

// Waits for the first frame up to `timeout`; returns width/height via out.
class FrameWatcher {
 public:
  castcore::IDisplayCapture::FrameCallback Callback() {
    return [this](const CapturedVideoFrame& f) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!got_) {
        last_ = f;
        got_ = true;
        cv_.notify_all();
      }
    };
  }

  bool WaitFirstFrame(int timeout_ms, CapturedVideoFrame* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return got_; })) {
      return false;
    }
    *out = last_;
    return true;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool got_ = false;
  CapturedVideoFrame last_;
};

#if !defined(_WIN32)
bool X11Available() {
  Display* d = XOpenDisplay(nullptr);
  if (!d) return false;
  XCloseDisplay(d);
  return true;
}

int EvenRound(int v) { return v & ~1; }
#endif

}  // namespace

TEST(DisplayCaptureTest, SyntheticEnumerateAndFrame) {
  auto capture = DisplayCaptureFactory::CreateSynthetic(640, 360);
  ASSERT_NE(capture, nullptr);
  EXPECT_EQ(capture->BackendName(), "Synthetic");
  auto displays = capture->EnumerateDisplays();
  ASSERT_EQ(displays.size(), 1u);
  EXPECT_EQ(displays[0].width, 640);
  EXPECT_EQ(displays[0].height, 360);
  EXPECT_TRUE(displays[0].is_primary);

  FrameWatcher watcher;
  capture->SetFrameCallback(watcher.Callback());
  ASSERT_TRUE(capture->Start(0, 10));
  EXPECT_TRUE(capture->IsCapturing());

  CapturedVideoFrame frame;
  EXPECT_TRUE(watcher.WaitFirstFrame(500, &frame));
  EXPECT_EQ(frame.width, 640);
  EXPECT_EQ(frame.height, 360);

  capture->Stop();
  EXPECT_FALSE(capture->IsCapturing());
}

TEST(DisplayCaptureTest, FactoryReturnsNonNull) {
  auto capture = DisplayCaptureFactory::Create();
  ASSERT_NE(capture, nullptr);
}

#if !defined(_WIN32)
TEST(DisplayCaptureTest, X11EnumerateHasGeometry) {
  if (!X11Available()) {
    GTEST_SKIP() << "No X11 display available";
  }
  auto capture = DisplayCaptureFactory::Create();
  auto displays = capture->EnumerateDisplays();
  if (displays.empty()) {
    GTEST_SKIP() << "X server exposes no displays";
  }
  bool any_primary = false;
  for (const auto& d : displays) {
    EXPECT_GT(d.width, 0);
    EXPECT_GT(d.height, 0);
    EXPECT_FALSE(d.name.empty());
    if (d.is_primary) any_primary = true;
  }
  EXPECT_TRUE(any_primary);
}

TEST(DisplayCaptureTest, X11StartHonorsDisplayId) {
  if (!X11Available()) {
    GTEST_SKIP() << "No X11 display available";
  }
  auto capture = DisplayCaptureFactory::Create();
  auto displays = capture->EnumerateDisplays();
  if (displays.empty()) {
    GTEST_SKIP() << "X server exposes no displays";
  }

  // Pick the last enumerated monitor (non-primary when multi-monitor).
  const DisplayInfo& target = displays.back();
  FrameWatcher watcher;
  capture->SetFrameCallback(watcher.Callback());
  ASSERT_TRUE(capture->Start(target.id, 10));

  CapturedVideoFrame frame;
  bool got = watcher.WaitFirstFrame(2000, &frame);
  capture->Stop();

  ASSERT_TRUE(got) << "No frame captured from monitor '" << target.name << "'";
  EXPECT_EQ(frame.width, EvenRound(target.width));
  EXPECT_EQ(frame.height, EvenRound(target.height));
}
#endif
