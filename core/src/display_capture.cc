#include "castcore/display_capture.h"
#include "castcore/logger.h"
#include <chrono>
#include <cstring>
#include <cmath>

#if !defined(_WIN32)
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/extensions/Xinerama.h>
#endif

namespace castcore {

// Synthetic Capturer: produces clean 60fps BGRA frames with smooth animation
class SyntheticDisplayCapture : public IDisplayCapture {
 public:
  SyntheticDisplayCapture(int width = 1920, int height = 1080)
      : width_(width), height_(height) {}

  ~SyntheticDisplayCapture() override {
    Stop();
  }

  bool Start(int display_id, int target_fps) override {
    Stop();
    target_fps_ = target_fps > 0 ? target_fps : 60;
    running_ = true;
    capture_thread_ = std::thread(&SyntheticDisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started Synthetic Display Capture (" << width_ << "x" << height_ << " @ " << target_fps_ << "fps)";
    return true;
  }

  void Stop() override {
    if (!running_.exchange(false)) return;
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  std::vector<DisplayInfo> EnumerateDisplays() override {
    std::vector<DisplayInfo> list;
    DisplayInfo d;
    d.id = 0;
    d.name = "Synthetic Primary Display";
    d.width = width_;
    d.height = height_;
    d.refresh_rate = 60;
    d.is_primary = true;
    list.push_back(d);
    return list;
  }

  void SetFrameCallback(FrameCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

 private:
  void CaptureLoop() {
    auto frame_interval = std::chrono::microseconds(1000000 / target_fps_);
    int frame_counter = 0;
    int ball_x = 100, ball_y = 100;
    int dx = 6, dy = 4;
    int ball_size = 80;

    std::vector<uint8_t> frame_buffer(width_ * height_ * 4);

    while (running_.load()) {
      auto start_time = std::chrono::steady_clock::now();

      // Render test pattern
      // Dark gradient background
      for (int y = 0; y < height_; ++y) {
        uint8_t bg_val = static_cast<uint8_t>(20 + (y * 40 / height_));
        uint8_t* row = &frame_buffer[y * width_ * 4];
        for (int x = 0; x < width_; ++x) {
          row[x * 4 + 0] = bg_val + 10; // B
          row[x * 4 + 1] = bg_val;      // G
          row[x * 4 + 2] = bg_val;      // R
          row[x * 4 + 3] = 255;         // A
        }
      }

      // Draw bouncing glowing square / ball
      ball_x += dx;
      ball_y += dy;
      if (ball_x <= 20 || ball_x + ball_size >= width_ - 20) dx = -dx;
      if (ball_y <= 20 || ball_y + ball_size >= height_ - 20) dy = -dy;

      for (int by = 0; by < ball_size; ++by) {
        int py = ball_y + by;
        if (py < 0 || py >= height_) continue;
        uint8_t* row = &frame_buffer[py * width_ * 4];
        for (int bx = 0; bx < ball_size; ++bx) {
          int px = ball_x + bx;
          if (px < 0 || px >= width_) continue;
          row[px * 4 + 0] = 240; // B
          row[px * 4 + 1] = 180; // G
          row[px * 4 + 2] = 40;  // R (Amber/Cyan)
          row[px * 4 + 3] = 255;
        }
      }

      // Draw top header bar
      for (int y = 0; y < 40; ++y) {
        uint8_t* row = &frame_buffer[y * width_ * 4];
        for (int x = 0; x < width_; ++x) {
          row[x * 4 + 0] = 60;
          row[x * 4 + 1] = 40;
          row[x * 4 + 2] = 200;
          row[x * 4 + 3] = 255;
        }
      }

      CapturedVideoFrame vf;
      vf.width = width_;
      vf.height = height_;
      vf.stride = width_ * 4;
      vf.timestamp = start_time;
      vf.data = frame_buffer;

      FrameCallback cb;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
      }
      if (cb) {
        cb(vf);
      }

      frame_counter++;
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time);
      if (elapsed < frame_interval) {
        std::this_thread::sleep_for(frame_interval - elapsed);
      }
    }
  }

  int width_ = 1920;
  int height_ = 1080;
  int target_fps_ = 60;
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
};

#if !defined(_WIN32)
// Native X11 Capturer for Linux
class X11DisplayCapture : public IDisplayCapture {
 public:
  X11DisplayCapture() = default;
  ~X11DisplayCapture() override { Stop(); }

  bool Start(int display_id, int target_fps) override {
    Stop();
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      LOG_WARN << "Cannot open X11 Display, falling back to Synthetic Capturer";
      return false;
    }

    target_fps_ = target_fps > 0 ? target_fps : 60;
    root_window_ = DefaultRootWindow(display_);

    XWindowAttributes attrs{};
    XGetWindowAttributes(display_, root_window_, &attrs);
    width_ = attrs.width;
    height_ = attrs.height;

    running_ = true;
    capture_thread_ = std::thread(&X11DisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started X11 Display Capture (" << width_ << "x" << height_ << " @ " << target_fps_ << "fps)";
    return true;
  }

  void Stop() override {
    if (!running_.exchange(false)) return;
    if (capture_thread_.joinable()) {
      capture_thread_.join();
    }
    if (display_) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  std::vector<DisplayInfo> EnumerateDisplays() override {
    std::vector<DisplayInfo> list;
    Display* d = XOpenDisplay(nullptr);
    if (!d) return list;

    int screen_count = ScreenCount(d);
    for (int i = 0; i < screen_count; ++i) {
      DisplayInfo info;
      info.id = i;
      info.name = "X11 Screen " + std::to_string(i);
      info.width = DisplayWidth(d, i);
      info.height = DisplayHeight(d, i);
      info.refresh_rate = 60;
      info.is_primary = (i == DefaultScreen(d));
      list.push_back(info);
    }
    XCloseDisplay(d);
    return list;
  }

  void SetFrameCallback(FrameCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

 private:
  void CaptureLoop() {
    auto frame_interval = std::chrono::microseconds(1000000 / target_fps_);

    while (running_.load()) {
      auto start_time = std::chrono::steady_clock::now();

      XImage* image = XGetImage(display_, root_window_, 0, 0, width_, height_, AllPlanes, ZPixmap);
      if (image) {
        CapturedVideoFrame vf;
        vf.width = width_;
        vf.height = height_;
        vf.stride = image->bytes_per_line;
        vf.timestamp = start_time;
        vf.data.resize(image->bytes_per_line * height_);
        std::memcpy(vf.data.data(), image->data, vf.data.size());

        XDestroyImage(image);

        FrameCallback cb;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          cb = callback_;
        }
        if (cb) {
          cb(vf);
        }
      }

      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time);
      if (elapsed < frame_interval) {
        std::this_thread::sleep_for(frame_interval - elapsed);
      }
    }
  }

  Display* display_ = nullptr;
  Window root_window_ = 0;
  int width_ = 1920;
  int height_ = 1080;
  int target_fps_ = 60;
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
};
#endif

std::unique_ptr<IDisplayCapture> DisplayCaptureFactory::Create() {
#if !defined(_WIN32)
  auto x11 = std::make_unique<X11DisplayCapture>();
  // Check if X11 is accessible
  Display* d = XOpenDisplay(nullptr);
  if (d) {
    XCloseDisplay(d);
    return x11;
  }
#endif
  return std::make_unique<SyntheticDisplayCapture>();
}

std::unique_ptr<IDisplayCapture> DisplayCaptureFactory::CreateSynthetic(int width, int height) {
  return std::make_unique<SyntheticDisplayCapture>(width, height);
}

} // namespace castcore
