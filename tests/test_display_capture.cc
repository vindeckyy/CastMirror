#include <gtest/gtest.h>
#include "castcore/display_capture.h"
#include "castcore/types.h"
#include "castcore/gpu_processor.h"

#if defined(__has_include)
#if __has_include(<libdrm/drm_fourcc.h>)
#include <libdrm/drm_fourcc.h>
#elif __has_include(<drm/drm_fourcc.h>)
#include <drm/drm_fourcc.h>
#elif __has_include(<drm_fourcc.h>)
#include <drm_fourcc.h>
#endif
#endif

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e
#endif
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

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

TEST(DisplayCaptureTest, DmaBufMetadataHandling) {
  // 1. Verify default values of CapturedVideoFrame
  CapturedVideoFrame default_frame;
  EXPECT_FALSE(default_frame.is_dmabuf);
  EXPECT_EQ(default_frame.dmabuf_fd, -1);
  EXPECT_EQ(default_frame.dmabuf_stride, 0);
  EXPECT_EQ(default_frame.dmabuf_offset_y, 0);
  EXPECT_EQ(default_frame.dmabuf_offset_uv, 0);
  EXPECT_EQ(default_frame.dmabuf_modifier, 0u);
  EXPECT_EQ(default_frame.dmabuf_format, 0u);

  // 2. Populate complete DMA-BUF NV12 metadata
  CapturedVideoFrame dmabuf_frame;
  dmabuf_frame.width = 1920;
  dmabuf_frame.height = 1080;
  dmabuf_frame.is_dmabuf = true;
  dmabuf_frame.dmabuf_fd = 42;
  dmabuf_frame.dmabuf_stride = 1920;
  dmabuf_frame.dmabuf_offset_y = 0;
  dmabuf_frame.dmabuf_offset_uv = 1920 * 1080;
  dmabuf_frame.dmabuf_modifier = DRM_FORMAT_MOD_LINEAR;
  dmabuf_frame.dmabuf_format = DRM_FORMAT_NV12;

  // Verify fields
  EXPECT_TRUE(dmabuf_frame.is_dmabuf);
  EXPECT_EQ(dmabuf_frame.dmabuf_fd, 42);
  EXPECT_EQ(dmabuf_frame.dmabuf_stride, 1920);
  EXPECT_EQ(dmabuf_frame.dmabuf_offset_y, 0);
  EXPECT_EQ(dmabuf_frame.dmabuf_offset_uv, 1920 * 1080);
  EXPECT_EQ(dmabuf_frame.dmabuf_modifier, DRM_FORMAT_MOD_LINEAR);
  EXPECT_EQ(dmabuf_frame.dmabuf_format, DRM_FORMAT_NV12);

  // Validate NV12 layout constraints
  EXPECT_GE(dmabuf_frame.dmabuf_stride, dmabuf_frame.width);
  EXPECT_GE(dmabuf_frame.dmabuf_offset_uv, dmabuf_frame.dmabuf_offset_y + dmabuf_frame.dmabuf_stride * dmabuf_frame.height);

  // 3. Cursor plane preservation on DMA-BUF frames
  dmabuf_frame.has_cursor = true;
  dmabuf_frame.cursor_x = 100;
  dmabuf_frame.cursor_y = 200;
  dmabuf_frame.cursor_width = 32;
  dmabuf_frame.cursor_height = 32;
  dmabuf_frame.cursor_stride = 128;
  dmabuf_frame.cursor_data.resize(32 * 128, 0xFF);

  EXPECT_TRUE(dmabuf_frame.has_cursor);
  EXPECT_EQ(dmabuf_frame.cursor_x, 100);
  EXPECT_EQ(dmabuf_frame.cursor_y, 200);
  EXPECT_EQ(dmabuf_frame.cursor_data.size(), 32u * 128u);

  // 4. Test GpuProcessor handling of DMA-BUF frames
  GpuProcessor gpu_proc;
  ASSERT_TRUE(gpu_proc.Initialize(640, 360, 640, 360));

  // 4a. Zero-copy hardware frame: frame.data empty, dmabuf_fd valid
  // ConvertBgraToNv12 should return false so caller uses direct VAAPI hw_frames_ctx import
  CapturedVideoFrame hw_dmabuf;
  hw_dmabuf.width = 640;
  hw_dmabuf.height = 360;
  hw_dmabuf.is_dmabuf = true;
  hw_dmabuf.dmabuf_fd = 50;
  hw_dmabuf.dmabuf_stride = 640;
  hw_dmabuf.dmabuf_format = DRM_FORMAT_NV12;

  std::vector<uint8_t> dst_y(640 * 360);
  std::vector<uint8_t> dst_uv(640 * 180);
  EXPECT_FALSE(gpu_proc.ConvertBgraToNv12(hw_dmabuf, dst_y.data(), 640, dst_uv.data(), 640));

  // 4b. NV12 direct scaling fallback: frame.data contains NV12 CPU staging
  CapturedVideoFrame staged_nv12;
  staged_nv12.width = 640;
  staged_nv12.height = 360;
  staged_nv12.is_dmabuf = true;
  staged_nv12.dmabuf_fd = 50;
  staged_nv12.dmabuf_stride = 640;
  staged_nv12.dmabuf_offset_y = 0;
  staged_nv12.dmabuf_offset_uv = 640 * 360;
  staged_nv12.dmabuf_format = DRM_FORMAT_NV12;
  staged_nv12.data.resize(640 * 360 * 3 / 2, 0x80);

  EXPECT_TRUE(gpu_proc.ConvertBgraToNv12(staged_nv12, dst_y.data(), 640, dst_uv.data(), 640));
}

// --- Window capture tests (Phase C) ---

// Synthetic capturer advertises window support and returns at least one
// window with a non-empty title.
TEST(DisplayCaptureTest, SyntheticEnumerateWindows) {
  auto cap = DisplayCaptureFactory::CreateSynthetic(640, 480);
  ASSERT_TRUE(cap->SupportsWindowCapture());
  auto windows = cap->EnumerateWindows();
  ASSERT_EQ(windows.size(), 1u);
  EXPECT_EQ(windows[0].id, 1);
  EXPECT_FALSE(windows[0].title.empty());
  EXPECT_GT(windows[0].width, 0);
  EXPECT_GT(windows[0].height, 0);
}

// Starting the synthetic capturer with a window source must produce frames
// matching the window geometry and report the window via ActiveSource().
TEST(DisplayCaptureTest, SyntheticWindowStartProducesWindowFrames) {
  auto cap = DisplayCaptureFactory::CreateSynthetic(640, 480);
  auto windows = cap->EnumerateWindows();
  ASSERT_FALSE(windows.empty());

  CaptureSource src{CaptureSourceKind::kWindow, windows[0].id, windows[0].title,
                    0, 0, 320, 240};
  FrameWatcher watcher;
  cap->SetFrameCallback(watcher.Callback());
  ASSERT_TRUE(cap->Start(src, 10));

  CapturedVideoFrame frame;
  ASSERT_TRUE(watcher.WaitFirstFrame(500, &frame));
  EXPECT_EQ(frame.width, 320);
  EXPECT_EQ(frame.height, 240);
  EXPECT_FALSE(frame.source_lost);

  auto active = cap->ActiveSource();
  EXPECT_EQ(active.kind, CaptureSourceKind::kWindow);
  EXPECT_EQ(active.id, windows[0].id);

  cap->Stop();
}

// The synthetic capturer must not set source_lost during normal window
// capture (only on actual window destruction, which we can't simulate
// here). This guards against false-positive session failures.
TEST(DisplayCaptureTest, SyntheticWindowFrameNotLostDuringCapture) {
  auto cap = DisplayCaptureFactory::CreateSynthetic(640, 480);
  auto windows = cap->EnumerateWindows();
  ASSERT_FALSE(windows.empty());

  CaptureSource src{CaptureSourceKind::kWindow, windows[0].id, windows[0].title};
  FrameWatcher watcher;
  cap->SetFrameCallback(watcher.Callback());
  ASSERT_TRUE(cap->Start(src, 10));

  CapturedVideoFrame frame;
  ASSERT_TRUE(watcher.WaitFirstFrame(500, &frame));
  EXPECT_FALSE(frame.source_lost);

  cap->Stop();
}

// X11 window enumeration: only runs when an X server is available. On
// headless CI this test is skipped.
TEST(DisplayCaptureTest, X11EnumerateWindowsWhenAvailable) {
#if !defined(_WIN32)
  Display* d = XOpenDisplay(nullptr);
  if (!d) {
    GTEST_SKIP() << "No X11 display available";
  }
  XCloseDisplay(d);

  auto cap = DisplayCaptureFactory::Create();
  if (!cap->SupportsWindowCapture()) {
    GTEST_SKIP() << "Default backend does not support window capture";
  }
  // Enumeration should not crash even if there are no suitable windows.
  auto windows = cap->EnumerateWindows();
  // We don't assert non-empty: a minimal Xvfb may have no managed toplevels
  // with WM_STATE. The contract is just that it returns without error.
  for (const auto& w : windows) {
    EXPECT_FALSE(w.title.empty());
    EXPECT_GT(w.width, 0);
    EXPECT_GT(w.height, 0);
  }
#else
  GTEST_SKIP() << "X11 window tests not applicable on Windows";
#endif
}

// Starting the X11 capturer with an invalid window ID must fail cleanly
// (return false) rather than crashing.
TEST(DisplayCaptureTest, X11WindowStartInvalidIdFailsCleanly) {
#if !defined(_WIN32)
  Display* d = XOpenDisplay(nullptr);
  if (!d) {
    GTEST_SKIP() << "No X11 display available";
  }
  XCloseDisplay(d);

  auto cap = DisplayCaptureFactory::Create();
  // Use a window ID that is extremely unlikely to exist.
  CaptureSource src{CaptureSourceKind::kWindow, 0x7fffffff, "Nonexistent"};
  EXPECT_FALSE(cap->Start(src, 10));
  EXPECT_FALSE(cap->IsCapturing());
#else
  GTEST_SKIP() << "X11 window tests not applicable on Windows";
#endif
}

#include "castcore/latency_hud.h"

TEST(DisplayCaptureTest, LatencyHudRendersOntoFrameWhenEnabled) {
  CapturedVideoFrame frame;
  frame.width = 640;
  frame.height = 480;
  frame.stride = 640 * 4;
  frame.data.assign(static_cast<size_t>(frame.stride) * frame.height, 0);

  EXPECT_EQ(frame.data[25 * frame.stride + 25 * 4 + 0], 0);

  LatencyHud::Render(frame);

  uint8_t b = frame.data[25 * frame.stride + 25 * 4 + 0];
  uint8_t a = frame.data[25 * frame.stride + 25 * 4 + 3];
  EXPECT_EQ(a, 255);
  EXPECT_GT(b, 0);
}


