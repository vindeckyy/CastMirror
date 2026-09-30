// Window capture through Windows.Graphics.Capture, against real top-level windows.
//
// The point of capturing the window itself is that things in front of it stay out
// of the picture. The old approach cropped the desktop image, so an overlapping
// window appeared in the stream. These tests create real windows, capture one,
// cover it with another, and check the picture is still the original window.
// They need an interactive desktop and skip on a runner without one.

#include <gtest/gtest.h>

#if defined(_WIN32)

#include "castcore/display_capture.h"

#include <windows.h>
#include <cstdio>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace castcore;
using namespace std::chrono_literals;

namespace {

// A plain popup window filled with one colour, on its own message-loop thread.
class ColourWindow {
 public:
  ColourWindow(int x, int y, int w, int h, COLORREF colour, bool topmost) : colour_(colour) {
    thread_ = std::thread([=] { Run(x, y, w, h, topmost); });
    std::unique_lock<std::mutex> lock(mutex_);
    ready_cv_.wait_for(lock, 5s, [&] { return ready_; });
  }

  ~ColourWindow() {
    Close();
    if (thread_.joinable()) thread_.join();
  }

  HWND handle() const { return hwnd_; }

  void Close() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  void Run(int x, int y, int w, int h, bool topmost) {
    static std::atomic<int> counter{0};
    wchar_t cls[64];
    swprintf(cls, 64, L"CastMirrorTestWindow%d", counter++);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, cls, L"CastMirror test window", WS_POPUP, x, y, w, h,
                            nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd_) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, static_cast<LONG_PTR>(colour_));
      ShowWindow(hwnd_, SW_SHOWNA);
      UpdateWindow(hwnd_);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_ = true;
    }
    ready_cv_.notify_all();
    MSG msg;
    while (hwnd_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    UnregisterClassW(cls, wc.hInstance);
  }

  COLORREF colour_;
  HWND hwnd_ = nullptr;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  bool ready_ = false;
};

struct Capturer {
  std::unique_ptr<IDisplayCapture> capture = DisplayCaptureFactory::Create();
  std::mutex mutex;
  CapturedVideoFrame latest;
  int frames = 0;
  std::atomic<bool> lost{false};

  bool Start(HWND hwnd) {
    capture->SetFrameCallback([this](const CapturedVideoFrame& f) {
      std::lock_guard<std::mutex> lock(mutex);
      if (f.source_lost) lost = true;
      else latest = f;
      ++frames;
    });
    return capture->Start(CaptureSource{CaptureSourceKind::kWindow, static_cast<int>(reinterpret_cast<intptr_t>(hwnd)), "test"}, 30);
  }

  bool WaitForFrames(int count, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (frames >= count && !latest.data.empty()) return true;
      }
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

  // BGRA of the pixel at the middle of the newest frame.
  bool CentrePixel(uint8_t out[4]) {
    std::lock_guard<std::mutex> lock(mutex);
    if (latest.data.empty()) return false;
    const size_t offset = (static_cast<size_t>(latest.height / 2) * latest.stride) + (latest.width / 2) * 4;
    for (int i = 0; i < 4; ++i) out[i] = latest.data[offset + i];
    return true;
  }
};

bool IsColour(const uint8_t bgra[4], int r, int g, int b);

// Waits for the newest frame's centre to become the given colour. WGC only delivers a
// frame when the window changes, so right after starting the first frame can be the
// window as it was a moment ago; a real app repaints and settles almost at once.
bool WaitForCentre(Capturer& c, int r, int g, int b, std::chrono::milliseconds timeout, uint8_t out[4]) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (c.CentrePixel(out) && IsColour(out, r, g, b)) return true;
    std::this_thread::sleep_for(25ms);
  }
  return c.CentrePixel(out) && IsColour(out, r, g, b);
}

constexpr COLORREF kRed = RGB(255, 0, 0);
constexpr COLORREF kGreen = RGB(0, 255, 0);

bool IsColour(const uint8_t bgra[4], int r, int g, int b) {
  return std::abs(bgra[2] - r) <= 12 && std::abs(bgra[1] - g) <= 12 && std::abs(bgra[0] - b) <= 12;
}

}  // namespace

TEST(WindowCaptureTest, CapturesTheWindowsOwnContent) {
  ColourWindow window(200, 200, 400, 300, kRed, /*topmost=*/false);
  if (!window.handle()) GTEST_SKIP() << "no interactive desktop to create a window on";

  Capturer c;
  if (!c.Start(window.handle())) GTEST_SKIP() << "window capture could not start here";
  if (c.capture->BackendName() != "windows_graphics_capture") {
    GTEST_SKIP() << "Windows.Graphics.Capture is not available; the crop fallback is in use";
  }
  ASSERT_TRUE(c.WaitForFrames(3, 4s));

  uint8_t px[4] = {};
  EXPECT_TRUE(WaitForCentre(c, 255, 0, 0, 3s, px)) << "got BGRA " << int(px[0]) << "," << int(px[1]) << "," << int(px[2]);
  {
    std::lock_guard<std::mutex> lock(c.mutex);
    EXPECT_EQ(c.latest.width, 400);
    EXPECT_EQ(c.latest.height, 300);
    EXPECT_EQ(c.latest.stride, 400 * 4);
  }
  c.capture->Stop();
}

TEST(WindowCaptureTest, AWindowInFrontDoesNotAppearInThePicture) {
  ColourWindow target(200, 200, 400, 300, kRed, false);
  if (!target.handle()) GTEST_SKIP() << "no interactive desktop to create a window on";

  Capturer c;
  if (!c.Start(target.handle())) GTEST_SKIP() << "window capture could not start here";
  if (c.capture->BackendName() != "windows_graphics_capture") GTEST_SKIP() << "crop fallback in use";
  ASSERT_TRUE(c.WaitForFrames(3, 4s));

  // Cover the whole target with a topmost window of another colour.
  ColourWindow cover(150, 150, 500, 400, kGreen, /*topmost=*/true);
  ASSERT_NE(cover.handle(), nullptr);
  std::this_thread::sleep_for(1200ms);

  // Sanity check on the test itself: the cover must really be above the target in the
  // z-order. Walk up from the target; the cover has to be on the way.
  bool cover_above = false;
  for (HWND w = GetWindow(target.handle(), GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
    if (w == cover.handle()) { cover_above = true; break; }
  }
  EXPECT_TRUE(cover_above) << "the covering window is not above the target, so this test proves nothing";
  uint8_t px[4] = {};
  ASSERT_TRUE(c.WaitForFrames(3, 4s));
  EXPECT_TRUE(WaitForCentre(c, 255, 0, 0, 3s, px))
      << "the picture shows the window in front instead of the target: BGRA "
      << int(px[0]) << "," << int(px[1]) << "," << int(px[2]);
  c.capture->Stop();
}

TEST(WindowCaptureTest, TheDesktopCropFallbackDoesShowTheWindowInFront) {
  // Documents why WGC matters: the old crop path captures whatever is on screen.
  _putenv_s("CASTMIRROR_DISABLE_WGC", "1");
  ColourWindow target(200, 200, 400, 300, kRed, false);
  ColourWindow cover(150, 150, 500, 400, kGreen, true);
  if (!target.handle() || !cover.handle()) {
    _putenv_s("CASTMIRROR_DISABLE_WGC", "");
    GTEST_SKIP() << "no interactive desktop to create windows on";
  }
  if (WindowFromPoint(POINT{400, 350}) != cover.handle()) {
    _putenv_s("CASTMIRROR_DISABLE_WGC", "");
    GTEST_SKIP() << "another window (a system dialog, say) is covering the screen";
  }
  Capturer c;
  const bool started = c.Start(target.handle());
  _putenv_s("CASTMIRROR_DISABLE_WGC", "");
  if (!started) GTEST_SKIP() << "desktop capture could not start here";
  EXPECT_EQ(c.capture->BackendName(), "windows_desktop_duplication");
  ASSERT_TRUE(c.WaitForFrames(3, 4s));
  uint8_t px[4] = {};
  EXPECT_TRUE(WaitForCentre(c, 0, 255, 0, 3s, px)) << "the crop path is expected to show the covering window";
  c.capture->Stop();
}

TEST(WindowCaptureTest, ClosingTheWindowEndsTheCapture) {
  auto window = std::make_unique<ColourWindow>(200, 200, 400, 300, kRed, false);
  if (!window->handle()) GTEST_SKIP() << "no interactive desktop to create a window on";
  Capturer c;
  if (!c.Start(window->handle())) GTEST_SKIP() << "window capture could not start here";
  ASSERT_TRUE(c.WaitForFrames(2, 4s));

  window.reset();  // closes and destroys the window
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!c.lost && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
  EXPECT_TRUE(c.lost.load()) << "the session must be told the source is gone";
  c.capture->Stop();
}

#else

TEST(WindowCaptureTest, WindowsOnly) { GTEST_SKIP() << "Windows.Graphics.Capture is Windows-only"; }

#endif
