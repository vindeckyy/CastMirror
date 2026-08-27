#include "castcore/display_capture.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include <chrono>
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <thread>

#if !defined(_WIN32)
  #include <X11/Xlib.h>
  #include <X11/Xutil.h>
  #include <X11/extensions/XShm.h>
  #include <X11/extensions/Xrandr.h>
  #include <X11/extensions/Xfixes.h>
  #include <sys/ipc.h>
  #include <sys/shm.h>
#endif

namespace castcore {

#if !defined(_WIN32)
namespace {
void EnsureX11Threads() {
  static std::once_flag once;
  std::call_once(once, [] { XInitThreads(); });
}
}  // namespace
#endif

// Synthetic Capturer: produces clean 60fps BGRA frames with smooth animation
class SyntheticDisplayCapture : public IDisplayCapture {
 public:
  SyntheticDisplayCapture(int width = 1920, int height = 1080)
      : width_(width), height_(height) {}

  std::string BackendName() const override { return "Synthetic"; }

  ~SyntheticDisplayCapture() override {
    Stop();
  }

  bool Start(int display_id, int target_fps) override {
    Stop();
    target_fps_.store(target_fps > 0 ? target_fps : 60);
    running_ = true;
    capture_thread_ = std::thread(&SyntheticDisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started Synthetic Display Capture (" << width_ << "x" << height_ << " @ " << target_fps_.load() << "fps)";
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

  void SetTargetFps(int fps) override {
    if (fps > 0) {
      target_fps_.store(fps);
    }
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
    int frame_counter = 0;
    int ball_x = 100, ball_y = 100;
    int dx = 6, dy = 4;
    int ball_size = 80;

    std::vector<uint8_t> frame_buffer(width_ * height_ * 4);

    while (running_.load()) {
      int fps = std::max(target_fps_.load(), 1);
      auto frame_interval = std::chrono::microseconds(1000000 / fps);
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
  std::atomic<int> target_fps_{60};
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
};

#if !defined(_WIN32)
namespace {

// Enumerate active monitors via XRandR: one DisplayInfo per connected output
// with a live CRTC. Returns empty when XRandR is unavailable or no output is
// active (caller falls back to root-window geometry).
std::vector<DisplayInfo> EnumerateRandrDisplays(Display* d) {
  std::vector<DisplayInfo> list;
  int event_base = 0, error_base = 0;
  if (!XRRQueryExtension(d, &event_base, &error_base)) {
    return list;
  }
  Window root = DefaultRootWindow(d);
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(d, root);
  if (!res) return list;
  RROutput primary_out = XRRGetOutputPrimary(d, root);

  int id = 0;
  for (int i = 0; i < res->noutput; ++i) {
    XRROutputInfo* out = XRRGetOutputInfo(d, res, res->outputs[i]);
    if (!out) continue;
    if (out->crtc == None || out->connection != RR_Connected || out->ncrtc == 0) {
      XRRFreeOutputInfo(out);
      continue;
    }
    XRRCrtcInfo* crtc = XRRGetCrtcInfo(d, res, out->crtc);
    if (!crtc || crtc->mode == None) {
      if (crtc) XRRFreeCrtcInfo(crtc);
      XRRFreeOutputInfo(out);
      continue;
    }

    DisplayInfo info;
    info.id = id++;
    info.name = out->name ? out->name : ("Output " + std::to_string(i));
    info.x = crtc->x;
    info.y = crtc->y;
    info.width = static_cast<int>(crtc->width);
    info.height = static_cast<int>(crtc->height);
    info.refresh_rate = 60;
    for (int m = 0; m < res->nmode; ++m) {
      const XRRModeInfo& mi = res->modes[m];
      if (mi.id != crtc->mode) continue;
      unsigned long long denom =
          static_cast<unsigned long long>(mi.hTotal) * static_cast<unsigned long long>(mi.vTotal);
      if (denom > 0 && mi.dotClock > 0) {
        info.refresh_rate = static_cast<int>((mi.dotClock + denom / 2) / denom);
      }
      break;
    }
    info.is_primary = (primary_out != None)
                          ? (res->outputs[i] == primary_out)
                          : (info.x == 0 && info.y == 0);
    list.push_back(info);

    XRRFreeCrtcInfo(crtc);
    XRRFreeOutputInfo(out);
  }
  XRRFreeScreenResources(res);
  return list;
}

}  // namespace

// Native X11 Capturer for Linux: XRandR per-monitor crop, MIT-SHM fast path,
// XFixes cursor compositing. Falls back to a cropped XGetImage when SHM is
// unavailable; never captures the full virtual desktop when a monitor exists.
class X11DisplayCapture : public IDisplayCapture {
 public:
  X11DisplayCapture() { shm_info_.shmid = -1; }
  std::string BackendName() const override { return "X11"; }
  ~X11DisplayCapture() override { Stop(); }

  bool Start(int display_id, int target_fps) override {
    Stop();
    EnsureX11Threads();
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      LOG_WARN << "Cannot open X11 Display, falling back to Synthetic Capturer";
      return false;
    }

    target_fps_.store(target_fps > 0 ? target_fps : 60);
    root_window_ = DefaultRootWindow(display_);

    auto displays = EnumerateRandrDisplays(display_);
    if (displays.empty()) {
      XWindowAttributes attrs{};
      XGetWindowAttributes(display_, root_window_, &attrs);
      crop_x_ = 0;
      crop_y_ = 0;
      crop_w_ = attrs.width & ~1;
      crop_h_ = attrs.height & ~1;
      LOG_INFO << "XRandR unavailable; capturing full root " << crop_w_ << "x" << crop_h_;
    } else {
      const DisplayInfo* sel = nullptr;
      for (const auto& di : displays) {
        if (di.id == display_id) { sel = &di; break; }
      }
      if (!sel) {
        for (const auto& di : displays) {
          if (di.is_primary) { sel = &di; break; }
        }
      }
      if (!sel) sel = &displays.front();
      display_id_ = sel->id;
      crop_x_ = sel->x;
      crop_y_ = sel->y;
      crop_w_ = sel->width & ~1;
      crop_h_ = sel->height & ~1;
      LOG_INFO << "Capturing monitor '" << sel->name << "' " << crop_w_ << "x" << crop_h_
               << " at (" << crop_x_ << "," << crop_y_ << ")";
    }

    xfixes_ok_ = XFixesQueryExtension(display_, &xfixes_event_base_, &xfixes_error_base_);
    shm_ok_ = SetupShm();

    running_ = true;
    capture_thread_ = std::thread(&X11DisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started X11 Display Capture (" << crop_w_ << "x" << crop_h_ << " @ "
             << target_fps_.load() << "fps, " << (shm_ok_ ? "MIT-SHM" : "XGetImage")
             << ", cursor " << (xfixes_ok_ ? "on" : "off") << ")";
    return true;
  }

  void Stop() override {
    const bool was_running = running_.exchange(false);
    if (was_running && capture_thread_.joinable()) {
      capture_thread_.join();
    }
    TeardownShm();
    if (display_) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
    xfixes_ok_ = false;
  }

  bool IsCapturing() const override {
    return running_.load();
  }

  void SetTargetFps(int fps) override {
    if (fps > 0) {
      target_fps_.store(fps);
    }
  }

  std::vector<DisplayInfo> EnumerateDisplays() override {
    std::vector<DisplayInfo> list;
    EnsureX11Threads();
    Display* d = XOpenDisplay(nullptr);
    if (!d) return list;

    list = EnumerateRandrDisplays(d);
    if (list.empty()) {
      XWindowAttributes attrs{};
      XGetWindowAttributes(d, DefaultRootWindow(d), &attrs);
      DisplayInfo info;
      info.id = 0;
      info.name = "X11 Display";
      info.width = attrs.width;
      info.height = attrs.height;
      info.refresh_rate = 60;
      info.is_primary = true;
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
  bool SetupShm() {
    int major = 0, minor = 0;
    Bool shared_pixel_format = False;
    if (!XShmQueryVersion(display_, &major, &minor, &shared_pixel_format) ||
        !shared_pixel_format) {
      return false;
    }
    const int screen = DefaultScreen(display_);
    shm_image_ = XShmCreateImage(display_, DefaultVisual(display_, screen),
                                 DefaultDepth(display_, screen), ZPixmap, nullptr,
                                 &shm_info_, crop_w_, crop_h_);
    if (!shm_image_) return false;

    size_t seg_size = static_cast<size_t>(shm_image_->bytes_per_line) * shm_image_->height;
    shm_info_.shmid = shmget(IPC_PRIVATE, seg_size, IPC_CREAT | 0600);
    if (shm_info_.shmid < 0) {
      XDestroyImage(shm_image_);
      shm_image_ = nullptr;
      return false;
    }
    char* addr = static_cast<char*>(shmat(shm_info_.shmid, nullptr, 0));
    if (addr == reinterpret_cast<char*>(-1)) {
      shmctl(shm_info_.shmid, IPC_RMID, nullptr);
      shm_info_.shmid = -1;
      XDestroyImage(shm_image_);
      shm_image_ = nullptr;
      return false;
    }
    shm_info_.shmaddr = addr;
    shm_info_.readOnly = False;
    shm_image_->data = addr;
    if (!XShmAttach(display_, &shm_info_)) {
      shmdt(addr);
      shmctl(shm_info_.shmid, IPC_RMID, nullptr);
      shm_info_.shmid = -1;
      shm_info_.shmaddr = nullptr;
      shm_image_->data = nullptr;
      XDestroyImage(shm_image_);
      shm_image_ = nullptr;
      return false;
    }
    return true;
  }

  void TeardownShm() {
    if (shm_image_) {
      if (display_) XShmDetach(display_, &shm_info_);
      shm_image_->data = nullptr;  // pixel store owned by the shm segment
      XDestroyImage(shm_image_);
      shm_image_ = nullptr;
    }
    if (shm_info_.shmid >= 0) {
      if (shm_info_.shmaddr) shmdt(shm_info_.shmaddr);
      shmctl(shm_info_.shmid, IPC_RMID, nullptr);
      shm_info_.shmid = -1;
      shm_info_.shmaddr = nullptr;
    }
  }

  // Blend the hardware cursor (XFixes) over the cropped BGRA frame.
  void CompositeCursor(CapturedVideoFrame& vf) {
    if (!xfixes_ok_ || !display_) return;
    XFixesCursorImage* ci = XFixesGetCursorImage(display_);
    if (!ci) return;
    const int ox = ci->x - ci->xhot - crop_x_;
    const int oy = ci->y - ci->yhot - crop_y_;
    for (int y = 0; y < ci->height; ++y) {
      const int py = oy + y;
      if (py < 0 || py >= vf.height) continue;
      uint8_t* row = vf.data.data() + static_cast<size_t>(py) * vf.stride;
      for (int x = 0; x < ci->width; ++x) {
        const int px = ox + x;
        if (px < 0 || px >= vf.width) continue;
        const uint32_t c = static_cast<uint32_t>(ci->pixels[y * ci->width + x]);
        const uint32_t a = (c >> 24) & 0xFF;
        if (a == 0) continue;
        uint8_t* d = row + px * 4;  // BGRA pixel
        if (a == 255) {
          d[0] = c & 0xFF;
          d[1] = (c >> 8) & 0xFF;
          d[2] = (c >> 16) & 0xFF;
          d[3] = 255;
        } else {
          const uint32_t inv = 255 - a;
          d[0] = static_cast<uint8_t>((( c        & 0xFF) * a + d[0] * inv) / 255);
          d[1] = static_cast<uint8_t>((((c >> 8)  & 0xFF) * a + d[1] * inv) / 255);
          d[2] = static_cast<uint8_t>((((c >> 16) & 0xFF) * a + d[2] * inv) / 255);
          d[3] = 255;
        }
      }
    }
    XFree(ci);
  }

  void CaptureLoop() {
    while (running_.load()) {
      int fps = std::max(target_fps_.load(), 1);
      auto frame_interval = std::chrono::microseconds(1000000 / fps);
      auto start_time = std::chrono::steady_clock::now();

      XImage* image = nullptr;
      bool image_owned = false;
      if (shm_ok_) {
        if (XShmGetImage(display_, root_window_, shm_image_, crop_x_, crop_y_, AllPlanes)) {
          image = shm_image_;
        }
      }
      if (!image) {
        image = XGetImage(display_, root_window_, crop_x_, crop_y_, crop_w_, crop_h_,
                          AllPlanes, ZPixmap);
        image_owned = (image != nullptr);
      }

      if (image) {
        CapturedVideoFrame vf;
        vf.width = crop_w_;
        vf.height = crop_h_;
        vf.stride = image->bytes_per_line;
        vf.timestamp = start_time;
        vf.data.resize(static_cast<size_t>(image->bytes_per_line) * crop_h_);
        std::memcpy(vf.data.data(), image->data, vf.data.size());
        if (image_owned) {
          XDestroyImage(image);
        }

        CompositeCursor(vf);

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
  int display_id_ = 0;
  int crop_x_ = 0;
  int crop_y_ = 0;
  int crop_w_ = 0;
  int crop_h_ = 0;
  XShmSegmentInfo shm_info_{};
  XImage* shm_image_ = nullptr;
  bool shm_ok_ = false;
  bool xfixes_ok_ = false;
  int xfixes_event_base_ = 0;
  int xfixes_error_base_ = 0;
  std::atomic<int> target_fps_{60};
  std::atomic<bool> running_{false};
  std::thread capture_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
};
#endif

#if defined(CASTCORE_HAVE_PIPEWIRE)
std::unique_ptr<IDisplayCapture> CreateWaylandPortalCapture();
#endif

std::unique_ptr<IDisplayCapture> DisplayCaptureFactory::Create() {
  const char* force_x11 = std::getenv("CASTMIRROR_FORCE_X11");
  bool env_force = force_x11 && force_x11[0] != '\0' && std::strcmp(force_x11, "0") != 0;
  bool cfg_force = ConfigStore::Instance().Get().force_x11_capture;
  if (env_force || cfg_force) {
#if !defined(_WIN32)
    EnsureX11Threads();
    Display* d = XOpenDisplay(nullptr);
    if (d) {
      XCloseDisplay(d);
      return std::make_unique<X11DisplayCapture>();
    }
#endif
    return std::make_unique<SyntheticDisplayCapture>();
  }

#if defined(CASTCORE_HAVE_PIPEWIRE)
  const char* wayland_display = std::getenv("WAYLAND_DISPLAY");
  if (wayland_display && wayland_display[0] != '\0') {
    return CreateWaylandPortalCapture();
  }
#endif

#if !defined(_WIN32)
  EnsureX11Threads();
  Display* d = XOpenDisplay(nullptr);
  if (d) {
    XCloseDisplay(d);
    return std::make_unique<X11DisplayCapture>();
  }
#endif

  return std::make_unique<SyntheticDisplayCapture>();
}

std::unique_ptr<IDisplayCapture> DisplayCaptureFactory::CreateSynthetic(int width, int height) {
  return std::make_unique<SyntheticDisplayCapture>(width, height);
}

} // namespace castcore
