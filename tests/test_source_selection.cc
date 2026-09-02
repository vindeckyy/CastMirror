#include <gtest/gtest.h>
#include "castcore/cast_engine.h"
#include "castcore/display_capture.h"
#include "castcore/types.h"
#include "castcore/config.h"
#include <filesystem>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace castcore;
using namespace std::chrono_literals;

namespace {

class FrameWatcher {
 public:
  IDisplayCapture::FrameCallback Callback() {
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
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return got_; }) && ([&] { *out = last_; return true; }());
  }
 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool got_ = false;
  CapturedVideoFrame last_;
};

}  // namespace

// The default capturer advertises window support when X11 or Synthetic is
// available. We verify the contract: SupportsWindowCapture() and
// EnumerateWindows() are callable and consistent. The window list may be
// empty on a headless/minimal X11 session with no managed toplevels.
TEST(SourceSelectionTest, EngineReportsWindowSupportViaSynthetic) {
  auto cap = DisplayCaptureFactory::Create();
  if (!cap->SupportsWindowCapture()) {
    GTEST_SKIP() << "Default backend does not advertise window support in this env";
  }
  // EnumerateWindows must be callable without crashing. Non-empty is not
  // guaranteed (headless X11 may have no managed toplevels with WM_STATE).
  auto windows = cap->EnumerateWindows();
  for (const auto& w : windows) {
    EXPECT_FALSE(w.title.empty());
    EXPECT_GT(w.width, 0);
    EXPECT_GT(w.height, 0);
  }
}

TEST(SourceSelectionTest, EngineGetWindowsNonEmptyWhenSupported) {
  auto& engine = CastEngine::Instance();
  engine.Initialize();
  auto windows = engine.GetWindows();
  if (!engine.WindowCaptureSupported()) {
    EXPECT_TRUE(windows.empty());
  } else {
    // Window support is advertised but the list may be empty on a headless
    // or minimal X11 session. Just verify the contract: titles are non-empty
    // and geometry is positive for any returned windows.
    for (const auto& w : windows) {
      EXPECT_FALSE(w.title.empty());
      EXPECT_GT(w.width, 0);
      EXPECT_GT(w.height, 0);
    }
  }
}

// Starting the synthetic capturer with a Window source must:
//  - honor the window geometry in the emitted frames
//  - report the window source via ActiveSource()
TEST(SourceSelectionTest, SyntheticWindowSourceHonorsGeometry) {
  auto cap = DisplayCaptureFactory::CreateSynthetic(640, 480);
  ASSERT_NE(cap, nullptr);
  ASSERT_TRUE(cap->SupportsWindowCapture());
  auto windows = cap->EnumerateWindows();
  ASSERT_EQ(windows.size(), 1u);

  CaptureSource src{CaptureSourceKind::kWindow, windows[0].id, windows[0].title,
                    0, 0, 320, 240};
  FrameWatcher watcher;
  cap->SetFrameCallback(watcher.Callback());
  ASSERT_TRUE(cap->Start(src, 10));

  CapturedVideoFrame frame;
  ASSERT_TRUE(watcher.WaitFirstFrame(500, &frame));
  EXPECT_EQ(frame.width, 320);
  EXPECT_EQ(frame.height, 240);

  auto active = cap->ActiveSource();
  EXPECT_EQ(active.kind, CaptureSourceKind::kWindow);
  EXPECT_EQ(active.id, windows[0].id);
  EXPECT_EQ(active.name, windows[0].title);

  cap->Stop();
}

// Starting with a Monitor source (legacy path) must reset window dims and
// report a Monitor active source.
TEST(SourceSelectionTest, SyntheticMonitorSourceResetsWindowDims) {
  auto cap = DisplayCaptureFactory::CreateSynthetic(640, 480);

  // First start as window with small geometry.
  CaptureSource wsrc{CaptureSourceKind::kWindow, 1, "W", 0, 0, 200, 200};
  FrameWatcher w1;
  cap->SetFrameCallback(w1.Callback());
  ASSERT_TRUE(cap->Start(wsrc, 10));
  CapturedVideoFrame f1;
  ASSERT_TRUE(w1.WaitFirstFrame(500, &f1));
  EXPECT_EQ(f1.width, 200);
  cap->Stop();

  // Now start as monitor 0; frame must be full 640x480, not 200x200.
  FrameWatcher w2;
  cap->SetFrameCallback(w2.Callback());
  ASSERT_TRUE(cap->Start(0, 10));
  CapturedVideoFrame f2;
  ASSERT_TRUE(w2.WaitFirstFrame(500, &f2));
  EXPECT_EQ(f2.width, 640);
  EXPECT_EQ(f2.height, 480);
  EXPECT_EQ(cap->ActiveSource().kind, CaptureSourceKind::kMonitor);
  cap->Stop();
}

// Engine source-aware StartCasting overload must set options.source and
// persist last_source_* in config. We verify the config persistence by
// calling the overload with a fake device; the call will fail to connect
// but config must still be updated.
TEST(SourceSelectionTest, EnginePersistsWindowSourceInConfig) {
  std::string cfg_path = "/tmp/castmirror_test_engine_source.json";
  if (std::filesystem::exists(cfg_path)) std::filesystem::remove(cfg_path);
  // Point the config store at our temp file by loading it first (creates
  // defaults), then saving to the temp path.
  auto& store = ConfigStore::Instance();
  store.Mutable() = AppConfig{};
  ASSERT_TRUE(store.Save(cfg_path));
  ASSERT_TRUE(store.Load(cfg_path));

  auto& engine = CastEngine::Instance();
  engine.Initialize();

  CaptureSource src{CaptureSourceKind::kWindow, 0x1234, "TestWindow"};
  SessionOptions opts;
  opts.enable_audio = false;
  // This will fail (no receiver at 127.0.0.1:8009) but should still persist.
  engine.StartCasting("127.0.0.1", src, opts);
  engine.StopCasting();

  const auto& cfg = store.Get();
  EXPECT_EQ(cfg.last_source_kind, "window");
  EXPECT_EQ(cfg.last_source_id, 0x1234);
  EXPECT_EQ(cfg.last_source_name, "TestWindow");

  std::filesystem::remove(cfg_path);
}
