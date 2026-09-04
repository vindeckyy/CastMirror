#include "castcore/display_capture.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include "castcore/latency_hud.h"
#include "castcore/display_capture_wgc.h"
#if !defined(_WIN32)
#include "i3_integration.h"
#endif
#include <chrono>
#include <cstring>
#include <cmath>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <thread>
#include <unordered_set>

#if !defined(_WIN32)
  #include <X11/Xlib.h>
  #include <X11/Xatom.h>
  #include <X11/Xutil.h>
  #include <X11/extensions/XShm.h>
  #include <X11/extensions/Xrandr.h>
  #include <X11/extensions/Xfixes.h>
#if defined(CASTCORE_HAVE_XDAMAGE)
  #include <X11/extensions/Xdamage.h>
#endif
#if defined(CASTCORE_HAVE_XCOMPOSITE)
  #include <X11/extensions/Xcomposite.h>
#endif
  #include <sys/ipc.h>
  #include <sys/shm.h>
  #include <unistd.h>
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
    return Start(CaptureSource{CaptureSourceKind::kMonitor, display_id, "Synthetic Primary Display"}, target_fps);
  }

  bool Start(const CaptureSource& source, int target_fps) override {
    Stop();
    target_fps_.store(target_fps > 0 ? target_fps : 60);
    active_source_ = source;
    if (source.IsWindow() && source.width > 0 && source.height > 0) {
      win_w_ = source.width & ~1;
      win_h_ = source.height & ~1;
    } else {
      // Monitor (or window with no geometry): use the full synthetic frame.
      win_w_ = 0;
      win_h_ = 0;
    }
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

  uint64_t GetCaptureSkipped() const override {
    return 0;
  }

  // Synthetic window support: used by tests and the GUI fallback path to
  // exercise the window-selector flow without a real X11/portal backend.
  bool SupportsWindowCapture() const override { return true; }

  std::vector<WindowInfo> EnumerateWindows() override {
    std::vector<WindowInfo> list;
    WindowInfo w;
    w.id = 1;
    w.title = "Synthetic Window";
    w.app_class = "Synthetic";
    w.x = 0;
    w.y = 0;
    w.width = width_;
    w.height = height_;
    w.visible = true;
    list.push_back(w);
    return list;
  }

  CaptureSource ActiveSource() const override { return active_source_; }

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

      // Effective output dimensions: window geometry when a window source is
      // active, else the full synthetic frame.
      const int out_w = (win_w_ > 0) ? win_w_ : width_;
      const int out_h = (win_h_ > 0) ? win_h_ : height_;

      // Render test pattern
      // Dark gradient background
      for (int y = 0; y < out_h; ++y) {
        uint8_t bg_val = static_cast<uint8_t>(20 + (y * 40 / out_h));
        uint8_t* row = &frame_buffer[y * width_ * 4];
        for (int x = 0; x < out_w; ++x) {
          row[x * 4 + 0] = bg_val + 10; // B
          row[x * 4 + 1] = bg_val;      // G
          row[x * 4 + 2] = bg_val;      // R
          row[x * 4 + 3] = 255;         // A
        }
      }

      // Draw bouncing glowing square / ball
      ball_x += dx;
      ball_y += dy;
      if (ball_x <= 20 || ball_x + ball_size >= out_w - 20) dx = -dx;
      if (ball_y <= 20 || ball_y + ball_size >= out_h - 20) dy = -dy;

      for (int by = 0; by < ball_size; ++by) {
        int py = ball_y + by;
        if (py < 0 || py >= out_h) continue;
        uint8_t* row = &frame_buffer[py * width_ * 4];
        for (int bx = 0; bx < ball_size; ++bx) {
          int px = ball_x + bx;
          if (px < 0 || px >= out_w) continue;
          row[px * 4 + 0] = 240; // B
          row[px * 4 + 1] = 180; // G
          row[px * 4 + 2] = 40;  // R (Amber/Cyan)
          row[px * 4 + 3] = 255;
        }
      }

      // Draw top header bar
      for (int y = 0; y < 40 && y < out_h; ++y) {
        uint8_t* row = &frame_buffer[y * width_ * 4];
        for (int x = 0; x < out_w; ++x) {
          row[x * 4 + 0] = 60;
          row[x * 4 + 1] = 40;
          row[x * 4 + 2] = 200;
          row[x * 4 + 3] = 255;
        }
      }

      CapturedVideoFrame vf;
      vf.width = out_w;
      vf.height = out_h;
      vf.stride = width_ * 4;
      vf.timestamp = start_time;
      vf.data = frame_buffer;

      if (ConfigStore::Instance().Get().latency_hud_enabled && !vf.data.empty()) {
        LatencyHud::Render(vf);
      }

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
  // Effective output dimensions for the active source (window geometry or
  // full frame). Set by Start(CaptureSource); defaults to width_/height_.
  int win_w_ = 0;
  int win_h_ = 0;
  CaptureSource active_source_{CaptureSourceKind::kMonitor, 0, ""};
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

bool GetWindowGeometry(Display* d, Window window, XWindowAttributes* attrs,
                       int* root_x, int* root_y) {
  if (!d || !attrs || !XGetWindowAttributes(d, window, attrs)) return false;

  int translated_x = attrs->x;
  int translated_y = attrs->y;
  Window child = None;
  if (XTranslateCoordinates(d, window, DefaultRootWindow(d), 0, 0,
                            &translated_x, &translated_y, &child)) {
    if (root_x) *root_x = translated_x;
    if (root_y) *root_y = translated_y;
  } else {
    if (root_x) *root_x = attrs->x;
    if (root_y) *root_y = attrs->y;
  }
  return true;
}

}  // namespace

// X11 window enumeration: uses the EWMH _NET_CLIENT_LIST property to get
// the list of managed client windows (correct under reparenting WMs where
// XQueryTree on root returns WM frames, not clients). Falls back to
// XQueryTree + WM_STATE filtering if _NET_CLIENT_LIST is unavailable.
// Override-redirect windows (popups, tooltips, docks) are excluded by
// definition since they're not in _NET_CLIENT_LIST. Returns WindowInfo
// with id = XID cast to int, title from _NET_WM_NAME or WM_NAME, and
// geometry from XGetWindowAttributes.
std::vector<WindowInfo> EnumerateX11Windows(Display* d) {
  std::vector<WindowInfo> list;
  if (!d) return list;

  Window root = DefaultRootWindow(d);
  Atom net_wm_name = XInternAtom(d, "_NET_WM_NAME", True);
  Atom utf8_string = XInternAtom(d, "UTF8_STRING", True);
  Atom net_wm_pid = XInternAtom(d, "_NET_WM_PID", True);
  Atom wm_state = XInternAtom(d, "WM_STATE", True);
  Atom net_client_list = XInternAtom(d, "_NET_CLIENT_LIST", True);
  Atom net_wm_window_type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", True);
  Atom win_type_desktop = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DESKTOP", True);
  Atom win_type_dock = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", True);
  Atom win_type_splash = XInternAtom(d, "_NET_WM_WINDOW_TYPE_SPLASH", True);
  Atom win_type_tooltip = XInternAtom(d, "_NET_WM_WINDOW_TYPE_TOOLTIP", True);
  Atom win_type_notification = XInternAtom(d, "_NET_WM_WINDOW_TYPE_NOTIFICATION", True);

  // Helper lambda to build a WindowInfo from a window XID.
  auto build_info = [&](Window w, bool include_hidden) -> bool {
    XWindowAttributes attrs{};
    int root_x = 0;
    int root_y = 0;
    if (!GetWindowGeometry(d, w, &attrs, &root_x, &root_y)) return false;
    if (attrs.override_redirect) return false;
    if (!include_hidden && attrs.map_state != IsViewable) return false;
    if (attrs.width <= 1 || attrs.height <= 1) return false;

    // Sharing CastMirror itself creates a recursive hall-of-mirrors and makes
    // it appear to be the only choice on sparse i3 workspaces.
    if (net_wm_pid != None) {
      Atom pid_type = None;
      int pid_format = 0;
      unsigned long pid_items = 0, pid_after = 0;
      unsigned char* pid_data = nullptr;
      if (XGetWindowProperty(d, w, net_wm_pid, 0, 1, False, XA_CARDINAL,
                             &pid_type, &pid_format, &pid_items, &pid_after,
                             &pid_data) == Success) {
        if (pid_data && pid_format == 32 && pid_items == 1 &&
            *reinterpret_cast<unsigned long*>(pid_data) ==
                static_cast<unsigned long>(getpid())) {
          XFree(pid_data);
          return false;
        }
        if (pid_data) XFree(pid_data);
      }
    }

    // Skip desktop chrome: panels/docks, desktop background, splash screens,
    // tooltips, and notifications. These are never useful to share.
    if (net_wm_window_type != None) {
      Atom actual_type = None;
      int actual_format = 0;
      unsigned long nitems = 0, bytes_after = 0;
      unsigned char* prop_data = nullptr;
      if (XGetWindowProperty(d, w, net_wm_window_type, 0, 16, False, XA_ATOM,
                             &actual_type, &actual_format, &nitems, &bytes_after,
                             &prop_data) == Success) {
        if (prop_data && nitems > 0) {
          Atom* types = reinterpret_cast<Atom*>(prop_data);
          bool skip = false;
          for (unsigned long i = 0; i < nitems; ++i) {
            if (types[i] == win_type_desktop || types[i] == win_type_dock ||
                types[i] == win_type_splash || types[i] == win_type_tooltip ||
                types[i] == win_type_notification) {
              skip = true;
              break;
            }
          }
          XFree(prop_data);
          if (skip) return false;
        }
      }
    }

    // Read _NET_WM_NAME (UTF-8) first, fall back to WM_NAME.
    std::string title;
    if (net_wm_name != None && utf8_string != None) {
      Atom type = None;
      int fmt = 0;
      unsigned long n = 0, after = 0;
      unsigned char* name_data = nullptr;
      if (XGetWindowProperty(d, w, net_wm_name, 0, 1024, False, utf8_string,
                             &type, &fmt, &n, &after, &name_data) == Success) {
        if (name_data && n > 0) {
          title = std::string(reinterpret_cast<char*>(name_data), n);
        }
        if (name_data) XFree(name_data);
      }
    }
    if (title.empty()) {
      XTextProperty tp;
      if (XGetWMName(d, w, &tp) && tp.value) {
        char** list_str = nullptr;
        int count = 0;
        if (XmbTextPropertyToTextList(d, &tp, &list_str, &count) >= 0 && count > 0 && list_str[0]) {
          title = list_str[0];
        }
        if (list_str) XFreeStringList(list_str);
        XFree(tp.value);
      }
    }
    if (title.empty()) return false;  // unnamed windows are usually not interesting

    // Read WM_CLASS. res_class is the generic app name (e.g. "Firefox"),
    // res_name is the instance (e.g. "Navigator"). Use res_class for the
    // display name, but clean it up: strip common noise like "python3",
    // "electron", etc. so the title is the primary identifier.
    std::string app_class;
    std::string res_name;
    XClassHint ch;
    if (XGetClassHint(d, w, &ch)) {
      if (ch.res_class) {
        app_class = ch.res_class;
        XFree(ch.res_class);
      }
      if (ch.res_name) {
        res_name = ch.res_name;
        XFree(ch.res_name);
      }
    }

    // If the app class is a generic runtime (python, electron, etc.), it's
    // not helpful — clear it so the display falls back to just the title.
    static const std::unordered_set<std::string> kGenericClasses = {
        "python3", "python", "python2", "electron", "node", "nw",
        "wrapper", "appimage", "gnome-terminal", "xterm", "urxvt"
    };
    std::string lower_class = app_class;
    std::transform(lower_class.begin(), lower_class.end(), lower_class.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (kGenericClasses.count(lower_class)) {
      app_class.clear();
    }

    WindowInfo info;
    info.id = static_cast<int>(w);
    info.title = std::move(title);
    info.app_class = std::move(app_class);
    info.x = root_x;
    info.y = root_y;
    info.width = attrs.width;
    info.height = attrs.height;
    info.visible = attrs.map_state == IsViewable;
    list.push_back(info);
    return true;
  };

  // Primary path: _NET_CLIENT_LIST (EWMH). This is a list of Window XIDs.
  bool used_client_list = false;
  const bool include_hidden_clients = I3CapturePin::IsAvailable(d);
  if (net_client_list != None) {
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop_data = nullptr;
    if (XGetWindowProperty(d, root, net_client_list, 0, 1024, False, XA_WINDOW,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop_data) == Success) {
      if (prop_data && nitems > 0 && actual_format == 32) {
        Window* wins = reinterpret_cast<Window*>(prop_data);
        for (unsigned long i = 0; i < nitems; ++i) {
          // i3 keeps clients from inactive workspaces in this EWMH list but
          // unmaps them. Keep those clients selectable; the capture pin maps
          // them without requiring the user to remain on that workspace.
          build_info(wins[i], include_hidden_clients);
        }
        used_client_list = true;
      }
      if (prop_data) XFree(prop_data);
    }
  }

  // Fallback: XQueryTree + WM_STATE filtering (for non-EWMH WMs).
  if (!used_client_list) {
    Window parent = 0;
    Window* children = nullptr;
    unsigned int nchildren = 0;
    if (XQueryTree(d, root, &root, &parent, &children, &nchildren) && children) {
      for (unsigned int i = 0; i < nchildren; ++i) {
        Window w = children[i];
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(d, w, &attrs)) continue;
        if (attrs.override_redirect) continue;
        if (attrs.map_state != IsViewable) continue;
        if (attrs.width <= 1 || attrs.height <= 1) continue;

        // Check WM_STATE == NormalState (managed toplevel, not iconified).
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char* prop_data = nullptr;
        bool is_normal = false;
        if (wm_state != None && XGetWindowProperty(d, w, wm_state, 0, 2, False, AnyPropertyType,
                                                   &actual_type, &actual_format, &nitems, &bytes_after,
                                                   &prop_data) == Success) {
          if (prop_data && nitems >= 1) {
            long state_val = reinterpret_cast<long*>(prop_data)[0];
            is_normal = (state_val == 1);  // NormalState
          }
          if (prop_data) XFree(prop_data);
        }
        if (!is_normal) continue;

        build_info(w, false);
      }
      XFree(children);
    }
  }

  // Current-workspace windows are the most useful defaults. Preserve the
  // window manager's ordering within the visible and hidden groups.
  std::stable_sort(list.begin(), list.end(), [](const WindowInfo& a,
                                                 const WindowInfo& b) {
    return a.visible && !b.visible;
  });
  return list;
}

// Custom X11 error handler that logs instead of aborting. This is critical
// for window capture: querying an invalid/destroyed window ID raises
// BadWindow, which the default handler would abort on. We store the last
// error in a thread-local so callers can check it after X calls.
thread_local int g_x11_last_error_code = 0;
thread_local unsigned long g_x11_last_error_resource = 0;

static int X11QuietErrorHandler(Display* d, XErrorEvent* ev) {
  g_x11_last_error_code = ev->error_code;
  g_x11_last_error_resource = ev->resourceid;
  return 0;
}

// Native X11 Capturer for Linux: XRandR per-monitor crop, MIT-SHM fast path,
// XFixes cursor compositing. Falls back to a cropped XGetImage when SHM is
// unavailable; never captures the full virtual desktop when a monitor exists.
class X11DisplayCapture : public IDisplayCapture {
 public:
  X11DisplayCapture() { shm_info_.shmid = -1; }
  std::string BackendName() const override { return "X11"; }
  ~X11DisplayCapture() override { Stop(); }

  void SetShowCursor(bool show) override { show_cursor_ = show; }

  bool Start(int display_id, int target_fps) override {
    Stop();
    EnsureX11Threads();
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      LOG_WARN << "Cannot open X11 Display, falling back to Synthetic Capturer";
      return false;
    }
    XSetErrorHandler(X11QuietErrorHandler);
    target_fps_.store(target_fps > 0 ? target_fps : 60);
    root_window_ = DefaultRootWindow(display_);
    target_window_ = 0;  // monitor mode: capture from root
    active_source_ = CaptureSource{CaptureSourceKind::kMonitor, display_id, ""};

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
    SetupDamage();

    running_ = true;
    capture_thread_ = std::thread(&X11DisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started X11 Display Capture (" << crop_w_ << "x" << crop_h_ << " @ "
             << target_fps_.load() << "fps, " << (shm_ok_ ? "MIT-SHM" : "XGetImage")
             << ", cursor " << ((xfixes_ok_ && show_cursor_) ? "on" : "off")
             << ", damage " << (xdamage_ok_ ? "on" : "off") << ")";
    return true;
  }

  void Stop() override {
    running_.store(false);
    if (capture_thread_.joinable()) {
      if (capture_thread_.get_id() == std::this_thread::get_id()) {
        capture_thread_.detach();
      } else {
        capture_thread_.join();
      }
    }
    TeardownDamage();
    TeardownComposite();
    TeardownShm();
    i3_capture_pin_.Restore(display_);
    if (display_) {
      XCloseDisplay(display_);
      display_ = nullptr;
    }
    xfixes_ok_ = false;
    target_window_ = 0;
    window_visible_ = false;
    window_root_x_ = 0;
    window_root_y_ = 0;
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

  uint64_t GetCaptureSkipped() const override {
    return capture_skipped_.load(std::memory_order_relaxed);
  }

  // --- Window capture support ---

  bool SupportsWindowCapture() const override {
#if !defined(_WIN32)
    EnsureX11Threads();
    Display* d = XOpenDisplay(nullptr);
    if (!d) return false;
    XCloseDisplay(d);
    return true;
#else
    return false;
#endif
  }

  std::vector<WindowInfo> EnumerateWindows() override {
    std::vector<WindowInfo> list;
    EnsureX11Threads();
    Display* d = XOpenDisplay(nullptr);
    if (!d) return list;
    XSetErrorHandler(X11QuietErrorHandler);
    list = EnumerateX11Windows(d);
    XCloseDisplay(d);
    return list;
  }

  bool Start(const CaptureSource& source, int target_fps) override {
    if (source.IsWindow()) {
      return StartWindow(source, target_fps);
    }
    return Start(source.id, target_fps);
  }

  CaptureSource ActiveSource() const override { return active_source_; }

 private:

  bool StartWindow(const CaptureSource& source, int target_fps) {
    Stop();
    EnsureX11Threads();
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
      LOG_WARN << "Cannot open X11 Display for window capture";
      return false;
    }
    // Install quiet error handler so BadWindow on invalid IDs doesn't abort.
    XSetErrorHandler(X11QuietErrorHandler);
    g_x11_last_error_code = 0;
    target_fps_.store(target_fps > 0 ? target_fps : 60);
    root_window_ = DefaultRootWindow(display_);
    target_window_ = static_cast<Window>(source.id);
    active_source_ = source;

    // Query initial geometry. XGetWindowAttributes returns 0 on failure
    // (including BadWindow), and our error handler prevents abort.
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(display_, target_window_, &attrs) ||
        g_x11_last_error_code != 0) {
      LOG_ERROR << "Target window 0x" << std::hex << target_window_ << std::dec
                << " does not exist";
      XCloseDisplay(display_);
      display_ = nullptr;
      target_window_ = 0;
      return false;
    }

    // i3 unmaps windows on inactive workspaces. Pin the source as a
    // transparent, click-through sticky window so it keeps rendering while
    // the user works elsewhere. Its original i3 state is restored in Stop().
    if (I3CapturePin::IsAvailable(display_)) {
      if (!i3_capture_pin_.Pin(display_, target_window_)) {
        LOG_ERROR << "Could not prepare the selected i3 window for background capture";
        XCloseDisplay(display_);
        display_ = nullptr;
        target_window_ = 0;
        return false;
      }
    } else if (attrs.map_state != IsViewable) {
      LOG_ERROR << "Target window 0x" << std::hex << target_window_ << std::dec
                << " is not currently viewable";
      XCloseDisplay(display_);
      display_ = nullptr;
      target_window_ = 0;
      return false;
    }

    int root_x = 0;
    int root_y = 0;
    g_x11_last_error_code = 0;
    if (!GetWindowGeometry(display_, target_window_, &attrs, &root_x, &root_y) ||
        g_x11_last_error_code != 0 || attrs.map_state != IsViewable) {
      LOG_ERROR << "Target window could not be mapped for capture";
      i3_capture_pin_.Restore(display_);
      XCloseDisplay(display_);
      display_ = nullptr;
      target_window_ = 0;
      return false;
    }
    crop_x_ = 0;
    crop_y_ = 0;
    crop_w_ = attrs.width & ~1;
    crop_h_ = attrs.height & ~1;
    window_root_x_ = root_x;
    window_root_y_ = root_y;
    window_visible_ = true;
    active_source_.x = root_x;
    active_source_.y = root_y;
    active_source_.width = crop_w_;
    active_source_.height = crop_h_;

    // XComposite redirect so occluded windows still capture correctly.
    SetupComposite();

    xfixes_ok_ = XFixesQueryExtension(display_, &xfixes_event_base_, &xfixes_error_base_);
    shm_ok_ = SetupShm();
    SetupDamage();

    // Select input events for lifecycle tracking (resize, destroy, unmap).
    XSelectInput(display_, target_window_,
                 StructureNotifyMask | SubstructureNotifyMask);

    running_ = true;
    capture_thread_ = std::thread(&X11DisplayCapture::CaptureLoop, this);
    LOG_INFO << "Started X11 Window Capture (0x" << std::hex << target_window_ << std::dec
             << ", " << crop_w_ << "x" << crop_h_ << " @ " << target_fps_.load() << "fps, "
             << (shm_ok_ ? "MIT-SHM" : "XGetImage")
             << ", cursor " << ((xfixes_ok_ && show_cursor_) ? "on" : "off")
             << ", damage " << (xdamage_ok_ ? "on" : "off")
             << ", composite " << (xcomposite_ok_ ? "on" : "off") << ")";
    return true;
  }

  void SetupComposite() {
    xcomposite_ok_ = false;
#if defined(CASTCORE_HAVE_XCOMPOSITE)
    if (!display_ || target_window_ == 0) return;
    int event_base = 0, error_base = 0;
    if (!XCompositeQueryExtension(display_, &event_base, &error_base)) {
      LOG_WARN << "XComposite not available; occluded windows may tear";
      return;
    }
    int major = 0, minor = 0;
    XCompositeQueryVersion(display_, &major, &minor);
    // Use CompositeRedirectAutomatic so the window drawable stays updated
    // by the application. With CompositeRedirectManual, rendering is diverted
    // to an offscreen pixmap and the window drawable becomes stale, so
    // XShmGetImage returns empty frames and XDamage never fires.
    // Automatic redirection still allows capturing occluded windows because
    // the application keeps drawing to its window drawable regardless of
    // whether the compositor shows it on screen.
    XCompositeRedirectWindow(display_, target_window_, CompositeRedirectAutomatic);
    xcomposite_ok_ = true;
#endif
  }

  void TeardownComposite() {
#if defined(CASTCORE_HAVE_XCOMPOSITE)
    if (xcomposite_ok_ && display_ && target_window_ != 0) {
      XCompositeUnredirectWindow(display_, target_window_, CompositeRedirectAutomatic);
    }
#endif
    xcomposite_ok_ = false;
  }

  // Poll lifecycle events for the target window. i3 may briefly unmap a
  // sticky window while moving it between workspaces, so UnmapNotify pauses
  // capture instead of being mistaken for source destruction.
  bool PollWindowLifecycle() {
    if (!display_ || target_window_ == 0) return true;
    while (XPending(display_)) {
      XEvent ev{};
      XNextEvent(display_, &ev);
      if (ev.type == ConfigureNotify && ev.xconfigure.window == target_window_) {
        // Geometry is refreshed below. ConfigureNotify coordinates are often
        // relative to i3's frame window rather than to the X root.
      } else if (ev.type == DestroyNotify && ev.xdestroywindow.window == target_window_) {
        LOG_WARN << "Target window destroyed";
        return false;
      } else if (ev.type == UnmapNotify && ev.xunmap.window == target_window_) {
        if (window_visible_) {
          LOG_INFO << "Target window temporarily unmapped; pausing capture";
        }
        window_visible_ = false;
      } else if (ev.type == MapNotify && ev.xmap.window == target_window_) {
        if (!window_visible_) {
          LOG_INFO << "Target window mapped again; resuming capture";
        }
        window_visible_ = true;
      }
    }

    XWindowAttributes attrs{};
    int root_x = window_root_x_;
    int root_y = window_root_y_;
    g_x11_last_error_code = 0;
    if (!GetWindowGeometry(display_, target_window_, &attrs, &root_x, &root_y) ||
        g_x11_last_error_code != 0) {
      LOG_WARN << "Target window no longer exists";
      return false;
    }

    window_visible_ = attrs.map_state == IsViewable;
    window_root_x_ = root_x;
    window_root_y_ = root_y;
    active_source_.x = root_x;
    active_source_.y = root_y;

    const int new_w = attrs.width & ~1;
    const int new_h = attrs.height & ~1;
    if (new_w > 0 && new_h > 0 && (new_w != crop_w_ || new_h != crop_h_)) {
      ResizeShm(new_w, new_h);
      active_source_.width = new_w;
      active_source_.height = new_h;
      LOG_INFO << "Window resized to " << crop_w_ << "x" << crop_h_;
    }
    return true;
  }

  void ResizeShm(int w, int h) {
    TeardownShm();
    crop_w_ = w;
    crop_h_ = h;
    shm_ok_ = SetupShm();
  }

  // --- End window capture support ---

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

#if defined(CASTCORE_HAVE_XDAMAGE)
  bool SetupDamage() {
    TeardownDamage();
    if (!display_) return false;
    if (!XDamageQueryExtension(display_, &damage_event_base_, &damage_error_base_)) {
      return false;
    }
    int major = 0, minor = 0;
    XDamageQueryVersion(display_, &major, &minor);
    // In window mode, subscribe to damage on the target window (no crop
    // filtering needed — damage is window-relative). In monitor mode,
    // subscribe to the root window and filter by the monitor crop rect.
    Drawable damage_drawable = (target_window_ != 0) ? target_window_ : root_window_;
    damage_ = XDamageCreate(display_, damage_drawable, XDamageReportNonEmpty);
    if (!damage_) return false;
    xdamage_ok_ = true;
    undamaged_frames_ = 0;
    damage_pending_ = false;
    return true;
  }

  void TeardownDamage() {
    if (damage_) {
      if (display_) XDamageDestroy(display_, damage_);
      damage_ = 0;
    }
    xdamage_ok_ = false;
    damage_event_base_ = 0;
    damage_error_base_ = 0;
    undamaged_frames_ = 0;
    damage_pending_ = false;
  }

  // Poll XDamage events without blocking. Returns true if damage intersected
  // the selected monitor crop since last call. Uses per-monitor filtering:
  // only damage rects overlapping [crop_x_,crop_y_,crop_w_,crop_h_] count.
  bool PollDamage() {
    if (!xdamage_ok_ || !display_ || !damage_) return true; // fallback: assume damaged
    bool has_damage = damage_pending_;
    const bool window_mode = (target_window_ != 0);
    // Drain all pending X events
    while (XPending(display_)) {
      XEvent ev{};
      XNextEvent(display_, &ev);
      if (ev.type == damage_event_base_ + XDamageNotify) {
        auto* de = reinterpret_cast<XDamageNotifyEvent*>(&ev);
        if (window_mode) {
          // In window mode, all damage on the target window counts.
          has_damage = true;
          damage_pending_ = true;
        } else {
          // Monitor mode: check intersection with our monitor crop
          int ax = de->area.x;
          int ay = de->area.y;
          int aw = de->area.width;
          int ah = de->area.height;
          // de->area is relative to drawable (root). If empty, treat as full.
          bool intersects = true;
          if (aw > 0 && ah > 0) {
            intersects = !(ax + aw <= crop_x_ || ax >= crop_x_ + crop_w_ ||
                           ay + ah <= crop_y_ || ay >= crop_y_ + crop_h_);
          }
          if (intersects) {
            has_damage = true;
            damage_pending_ = true;
          }
        }
      }
    }
    if (has_damage) {
      // Reset damage region so next change generates a new notify
      XDamageSubtract(display_, damage_, None, None);
      damage_pending_ = false;
      undamaged_frames_ = 0;
      return true;
    }
    // No damage this vsync
    return false;
  }
#else
  bool SetupDamage() { return false; }
  void TeardownDamage() {}
  bool PollDamage() { return true; }
#endif

  uint64_t GetCaptureSkippedCount() const { return capture_skipped_.load(); }

  // Blend the hardware cursor (XFixes) over the cropped BGRA frame.
  // In monitor mode, the cursor position is screen-relative and we offset
  // by crop_x_/crop_y_. In window mode, the cursor position is still
  // screen-relative, so we offset by the window's root-relative position.
  void CompositeCursor(CapturedVideoFrame& vf) {
    if (!xfixes_ok_ || !display_) return;
    XFixesCursorImage* ci = XFixesGetCursorImage(display_);
    if (!ci) return;
    // Effective origin: in window mode use the window's root position; in
    // monitor mode use the crop rect origin.
    const int origin_x = (target_window_ != 0) ? window_root_x_ : crop_x_;
    const int origin_y = (target_window_ != 0) ? window_root_y_ : crop_y_;
    const int ox = ci->x - ci->xhot - origin_x;
    const int oy = ci->y - ci->yhot - origin_y;
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
    constexpr int kDamageSkipThreshold = 3; // N vsyncs without damage => skip capture
    while (running_.load()) {
      int fps = std::max(target_fps_.load(), 1);
      auto frame_interval = std::chrono::microseconds(1000000 / fps);
      auto start_time = std::chrono::steady_clock::now();

      // Window mode: only destruction emits source_lost. A short unmap while
      // i3 carries the sticky capture window between workspaces is skipped.
      if (target_window_ != 0) {
        if (!PollWindowLifecycle()) {
          CapturedVideoFrame vf;
          vf.width = crop_w_;
          vf.height = crop_h_;
          vf.stride = crop_w_ * 4;
          vf.timestamp = start_time;
          vf.source_lost = true;
          FrameCallback cb;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = callback_;
          }
          if (cb) cb(vf);
          running_ = false;
          break;
        }
        if (!window_visible_) {
          capture_skipped_.fetch_add(1, std::memory_order_relaxed);
          auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start_time);
          if (elapsed < frame_interval) {
            std::this_thread::sleep_for(frame_interval - elapsed);
          }
          continue;
        }
      }

#if defined(CASTCORE_HAVE_XDAMAGE)
      // Damage-based frame skipping is only used in monitor mode (root window).
      // In window mode, the selected window may be static (not actively redrawn),
      // so damage may never fire. We must always capture to ensure frames are
      // delivered — the user explicitly chose this window to share.
      if (xdamage_ok_ && target_window_ == 0) {
        bool damaged = PollDamage();
        if (!damaged) {
          undamaged_frames_++;
          if (undamaged_frames_ >= kDamageSkipThreshold) {
            capture_skipped_.fetch_add(1, std::memory_order_relaxed);
            // Still sleep to maintain vsync cadence, but skip XShmGetImage
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_time);
            if (elapsed < frame_interval) {
              std::this_thread::sleep_for(frame_interval - elapsed);
            }
            continue;
          }
        } else {
          // damage consumed in PollDamage()
        }
      } else if (xdamage_ok_ && target_window_ != 0) {
        // Window mode: drain damage events but always capture.
        PollDamage();
      }
#endif

      // In window mode, capture from target_window_ at origin (0,0).
      // In monitor mode, capture from root_window_ at (crop_x_, crop_y_).
      Drawable capture_drawable = (target_window_ != 0) ? target_window_ : root_window_;
      int capture_x = (target_window_ != 0) ? 0 : crop_x_;
      int capture_y = (target_window_ != 0) ? 0 : crop_y_;

      XImage* image = nullptr;
      bool image_owned = false;
      if (shm_ok_) {
        if (XShmGetImage(display_, capture_drawable, shm_image_, capture_x, capture_y, AllPlanes)) {
          image = shm_image_;
        }
      }
      if (!image) {
        // Fallback to XGetImage – must not break existing X11 capture path
        image = XGetImage(display_, capture_drawable, capture_x, capture_y, crop_w_, crop_h_,
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

        if (show_cursor_) CompositeCursor(vf);
        if (ConfigStore::Instance().Get().latency_hud_enabled && !vf.data.empty()) {
          LatencyHud::Render(vf);
        }

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
  // Window capture state: target_window_ != 0 means window mode.
  Window target_window_ = 0;
  bool window_visible_ = false;
  int window_root_x_ = 0;  // window's root-relative position (for cursor offset)
  int window_root_y_ = 0;
  bool xcomposite_ok_ = false;
  I3CapturePin i3_capture_pin_;
  bool show_cursor_ = false;
  CaptureSource active_source_{CaptureSourceKind::kMonitor, 0, ""};
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
#if defined(CASTCORE_HAVE_XDAMAGE)
  bool xdamage_ok_ = false;
  int damage_event_base_ = 0;
  int damage_error_base_ = 0;
  Damage damage_ = 0;
  int undamaged_frames_ = 0;
  bool damage_pending_ = false;
#else
  bool xdamage_ok_ = false;
#endif
  std::atomic<uint64_t> capture_skipped_{0};
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

#if defined(_WIN32)
  return std::make_unique<DisplayCaptureWgc>();
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
