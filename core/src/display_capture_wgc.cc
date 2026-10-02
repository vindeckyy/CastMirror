#include "castcore/display_capture_wgc.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include "castcore/latency_hud.h"
#include "castcore/win_time.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <algorithm>
#include <timeapi.h>
#include "castcore/frame_pacer.h"
#endif

namespace castcore {

#if defined(_WIN32)
namespace {

// How many times the capture loop will try to rebuild a lost duplication
// before failing the session, and how long to wait between attempts. Shared by
// every recreate path: a permanently unavailable output (unplugged monitor,
// wedged driver) must end the session rather than spin the loop.
constexpr int kMaxDuplicationRetries = 10;
constexpr int kDuplicationRetryMs = 500;
// Window-picker threshold: ignore windows too small to be a capture target.
constexpr int kMinShareableWindowPx = 100;
// Size of the placeholder frame that signals "source is gone" to the session.
constexpr int kSourceLostFrameSize = 2;

// Human-readable name for the DXGI failures that actually stop enumeration, so
// a log line names the condition instead of only a number.
const char* DxgiErrorName(HRESULT hr) {
  switch (hr) {
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    case E_ACCESSDENIED: return "E_ACCESSDENIED";
    default: return nullptr;
  }
}

// Renders a failure as "0x887A0022 (DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)".
std::string DescribeHresult(HRESULT hr) {
  char buffer[96];
  const char* name = DxgiErrorName(hr);
  if (name) {
    std::snprintf(buffer, sizeof(buffer), "0x%08lX (%s)", static_cast<unsigned long>(hr), name);
  } else {
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  }
  return buffer;
}

// Enumerate all DXGI outputs across adapters as a flat list of
// (adapter, output) pairs, in the same order EnumerateDisplays() uses so a
// DisplayInfo::id maps to exactly one entry.
struct OutputEntry {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  DXGI_OUTPUT_DESC desc{};
};

// first_error receives the first failure other than DXGI_ERROR_NOT_FOUND, which
// is the normal end-of-enumeration signal. An empty result with S_OK means the
// machine genuinely has no outputs; an empty result with a failure means the
// environment refused to enumerate (session 0, remote logon, restricted GPU)
// and the caller should say so rather than report "no displays".
std::vector<OutputEntry> EnumerateOutputs(HRESULT* first_error = nullptr) {
  std::vector<OutputEntry> out;
  HRESULT failure = S_OK;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if (FAILED(factory_hr)) {
    if (first_error) *first_error = factory_hr;
    return out;
  }

  for (UINT ai = 0;; ++ai) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    const HRESULT adapter_hr = factory->EnumAdapters(ai, &adapter);
    if (adapter_hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(adapter_hr)) {
      if (failure == S_OK) failure = adapter_hr;
      break;
    }
    for (UINT oi = 0;; ++oi) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      const HRESULT output_hr = adapter->EnumOutputs(oi, &output);
      if (output_hr == DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(output_hr)) {
        if (failure == S_OK) failure = output_hr;
        break;
      }
      OutputEntry e;
      e.adapter = adapter;
      e.output = output;
      output->GetDesc(&e.desc);
      out.push_back(std::move(e));
    }
  }
  if (first_error) *first_error = failure;
  return out;
}

}  // namespace
#endif

DisplayCaptureWgc::DisplayCaptureWgc() = default;

DisplayCaptureWgc::~DisplayCaptureWgc() {
  Stop();
}

bool DisplayCaptureWgc::Start(int display_id, int target_fps) {
  CaptureSource src{CaptureSourceKind::kMonitor, display_id, "Primary Display"};
  return Start(src, target_fps);
}

bool DisplayCaptureWgc::Start(const CaptureSource& source, int target_fps) {
#if !defined(_WIN32)
  (void)source;
  (void)target_fps;
  LOG_ERROR << "DisplayCaptureWgc is only available on Windows";
  return false;
#else
  Stop();
  active_source_ = source;
  target_fps_ = target_fps > 0 ? target_fps : 60;
  target_hwnd_ = nullptr;
  source_lost_emitted_ = false;
  age_sum_ms_ = 0.0;
  age_count_ = 0;
  last_age_log_ = std::chrono::steady_clock::now();
  // A new stream shares nothing with the last one's source timeline.
  have_last_present_ts_ = false;
  // A new stream must not serve an overlay composite from the previous one:
  // the pixels, crop and dimensions are unrelated to what is cached.
  overlay_cache_ = CapturedVideoFrame{};
  overlay_cursor_pos_ = POINT{};
  overlay_cache_valid_ = false;

  if (source.IsWindow()) {
    target_hwnd_ = reinterpret_cast<HWND>(static_cast<intptr_t>(source.id));
    if (!IsWindow(target_hwnd_)) {
      LOG_ERROR << "Window capture source 0x" << std::hex << source.id << std::dec
                << " is not a valid window";
      target_hwnd_ = nullptr;
      return false;
    }
    // Prefer capturing the window itself. CASTMIRROR_DISABLE_WGC=1 forces the
    // desktop-crop path (for comparing behaviour, or a driver that misbehaves).
    const char* disable = std::getenv("CASTMIRROR_DISABLE_WGC");
    const bool wgc_disabled = disable && disable[0] != '\0' && std::strcmp(disable, "0") != 0;
    if (!wgc_disabled) {
      wgc_window_ = WgcWindowSource::Create(target_hwnd_);
      if (wgc_window_) {
        running_ = true;
        worker_thread_ = std::thread(&DisplayCaptureWgc::WindowCaptureLoop, this);
        return true;
      }
    }

    RECT wr{};
    GetWindowRect(target_hwnd_, &wr);
    output_index_ = OutputIndexForRect(wr);
    if (output_index_ < 0) {
      LOG_ERROR << "No DXGI output found containing capture window";
      target_hwnd_ = nullptr;
      return false;
    }
  } else {
    output_index_ = source.id;
  }

  if (!CreateDuplication()) {
    target_hwnd_ = nullptr;
    return false;
  }

  running_ = true;
  worker_thread_ = std::thread(&DisplayCaptureWgc::CaptureLoop, this);
  return true;
#endif
}

#if defined(_WIN32)
bool DisplayCaptureWgc::CreateDuplication() {
  desk_dupl_.Reset();
  staging_tex_.Reset();
  staging_w_ = staging_h_ = 0;
  d3d_context_.Reset();
  d3d_device_.Reset();

  HRESULT enumeration_error = S_OK;
  auto outputs = EnumerateOutputs(&enumeration_error);
  if (outputs.empty()) {
    if (FAILED(enumeration_error)) {
      LOG_ERROR << "No DXGI outputs available for desktop duplication: "
                << DescribeHresult(enumeration_error)
                << ". Run CastMirror from a local desktop session with GPU access.";
    } else {
      LOG_ERROR << "No DXGI outputs available for desktop duplication";
    }
    return false;
  }
  int idx = output_index_;
  if (idx < 0 || idx >= static_cast<int>(outputs.size())) {
    // A monitor id that no longer exists means the display list the caller
    // resolved it from is stale (unplugged monitor, hotplug re-order). Mirroring
    // output 0 instead would silently cast a different screen than the one the
    // user picked, so fail loudly and let the caller re-enumerate.
    LOG_ERROR << "Display " << idx << " is not available: " << outputs.size()
              << " output(s) present. The display list is stale - re-enumerate "
                 "displays before starting capture.";
    return false;
  }

  OutputEntry& entry = outputs[idx];

  // The D3D11 device must be created on the same adapter that owns the output,
  // otherwise DuplicateOutput fails with E_INVALIDARG.
  HRESULT hr = D3D11CreateDevice(entry.adapter.Get(),
                                 D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr,
                                 0,
                                 D3D11_SDK_VERSION,
                                 &d3d_device_,
                                 nullptr,
                                 &d3d_context_);
  if (FAILED(hr)) {
    LOG_ERROR << "Failed to create D3D11 device on adapter for output " << idx << ": hr=0x"
              << std::hex << hr << std::dec;
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
  if (FAILED(entry.output.As(&output1))) {
    LOG_ERROR << "Output " << idx << " does not support DXGI1.1 duplication";
    return false;
  }

  hr = output1->DuplicateOutput(d3d_device_.Get(), &desk_dupl_);
  if (FAILED(hr)) {
    LOG_ERROR << "DuplicateOutput failed on output " << idx << ": hr=0x" << std::hex << hr
              << std::dec;
    return false;
  }

  output_rect_ = entry.desc.DesktopCoordinates;
  char dev_name[128] = {};
  WideCharToMultiByte(
      CP_UTF8, 0, entry.desc.DeviceName, -1, dev_name, sizeof(dev_name), nullptr, nullptr);
  LOG_INFO << "Duplicating DXGI output " << idx << " (" << dev_name << ") rect ("
           << output_rect_.left << "," << output_rect_.top << ")-(" << output_rect_.right << ","
           << output_rect_.bottom << ")";
  return true;
}

int DisplayCaptureWgc::OutputIndexForRect(const RECT& rect) {
  auto outputs = EnumerateOutputs();
  int best = -1;
  int64_t best_area = 0;
  for (size_t i = 0; i < outputs.size(); ++i) {
    const RECT& o = outputs[i].desc.DesktopCoordinates;
    int64_t iw = std::min<int64_t>(rect.right, o.right) - std::max<int64_t>(rect.left, o.left);
    int64_t ih = std::min<int64_t>(rect.bottom, o.bottom) - std::max<int64_t>(rect.top, o.top);
    int64_t area = (iw > 0 && ih > 0) ? iw * ih : 0;
    if (area > best_area) {
      best_area = area;
      best = static_cast<int>(i);
    }
  }
  return best;
}
#endif

void DisplayCaptureWgc::Stop() {
  running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
#if defined(_WIN32)
  wgc_window_.reset();
  DestroyCursorDib();
  desk_dupl_.Reset();
  staging_tex_.Reset();
  staging_w_ = staging_h_ = 0;
  d3d_context_.Reset();
  d3d_device_.Reset();
  target_hwnd_ = nullptr;
#endif
}

bool DisplayCaptureWgc::IsCapturing() const {
  return running_.load();
}

void DisplayCaptureWgc::SetTargetFps(int fps) {
  if (fps > 0) target_fps_ = fps;
}

std::vector<DisplayInfo> DisplayCaptureWgc::EnumerateDisplays() {
  std::vector<DisplayInfo> displays;
#if defined(_WIN32)
  // Enumerate through DXGI so DisplayInfo::id is the same flat output index
  // Start() resolves back to an adapter/output pair.
  HRESULT enumeration_error = S_OK;
  auto outputs = EnumerateOutputs(&enumeration_error);
  for (size_t i = 0; i < outputs.size(); ++i) {
    const DXGI_OUTPUT_DESC& desc = outputs[i].desc;
    if (!desc.AttachedToDesktop) continue;

    DisplayInfo info;
    info.id = static_cast<int>(i);
    const RECT& r = desc.DesktopCoordinates;
    info.x = r.left;
    info.y = r.top;
    info.width = r.right - r.left;
    info.height = r.bottom - r.top;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(desc.Monitor, &mi)) {
      info.is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
      DEVMODEW dm{};
      dm.dmSize = sizeof(dm);
      if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        info.refresh_rate = static_cast<int>(dm.dmDisplayFrequency);
      }
    }
    if (info.refresh_rate <= 0) info.refresh_rate = 60;

    char name_buf[128];
    std::snprintf(name_buf,
                  sizeof(name_buf),
                  "Display %d (%dx%d)",
                  static_cast<int>(i) + 1,
                  info.width,
                  info.height);
    info.name = name_buf;
    displays.push_back(info);
  }

  // An empty list has two very different causes and the user can only act on
  // one of them, so never leave it unexplained. Nothing is logged on the
  // success path because EnumerateDisplays() is called on every device list
  // refresh.
  if (displays.empty()) {
    if (FAILED(enumeration_error)) {
      LOG_WARN << "DXGI enumeration found no outputs: " << DescribeHresult(enumeration_error)
               << ". A process without an interactive desktop or GPU access (service or "
                  "session-0 logon, remote session, headless VM) has no capturable output.";
    } else if (!outputs.empty()) {
      LOG_WARN << "DXGI reported " << outputs.size() << " output(s), none attached to a desktop";
    } else {
      LOG_WARN << "DXGI reported no outputs; this machine has no attached display";
    }
  }
#else
  DisplayInfo dummy;
  dummy.id = 0;
  dummy.name = "Windows Display (Stub)";
  dummy.width = 1920;
  dummy.height = 1080;
  dummy.refresh_rate = 60;
  dummy.is_primary = true;
  displays.push_back(dummy);
#endif
  return displays;
}

std::vector<WindowInfo> DisplayCaptureWgc::EnumerateWindows() {
  std::vector<WindowInfo> windows;
#if defined(_WIN32)
  struct EnumContext {
    std::vector<WindowInfo>* list;
  } ctx{&windows};

  EnumWindows(
      [](HWND hwnd, LPARAM lparam) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        auto* c = reinterpret_cast<EnumContext*>(lparam);
        wchar_t title[256];
        if (GetWindowTextW(hwnd, title, 256) > 0) {
          RECT r;
          GetWindowRect(hwnd, &r);
          int w = r.right - r.left;
          int h = r.bottom - r.top;
          if (w > kMinShareableWindowPx && h > kMinShareableWindowPx) {
            WindowInfo wi;
            wi.id = static_cast<int>(reinterpret_cast<intptr_t>(hwnd));
            // 255 UTF-16 units can need up to 3 bytes each in UTF-8; a smaller buffer makes the
            // conversion fail and leaves it unterminated.
            char title_utf8[1024] = {};
            if (WideCharToMultiByte(
                    CP_UTF8, 0, title, -1, title_utf8, sizeof(title_utf8), nullptr, nullptr) <= 0) {
              return TRUE;
            }
            wi.title = title_utf8;
            wi.x = r.left;
            wi.y = r.top;
            wi.width = w;
            wi.height = h;
            c->list->push_back(wi);
          }
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
#endif
  return windows;
}

void DisplayCaptureWgc::SetFrameCallback(FrameCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void DisplayCaptureWgc::SetShowCursor(bool show) {
  show_cursor_ = show;
}

#if defined(_WIN32)
void DisplayCaptureWgc::EmitSourceLost(std::chrono::steady_clock::time_point timestamp) {
  if (source_lost_emitted_) return;
  source_lost_emitted_ = true;

  CapturedVideoFrame vf;
  vf.width = kSourceLostFrameSize;
  vf.height = kSourceLostFrameSize;
  vf.stride = kSourceLostFrameSize * 4;
  vf.timestamp = timestamp;
  vf.data.assign(static_cast<size_t>(vf.stride) * kSourceLostFrameSize, 0);
  vf.source_lost = true;
  FrameCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = callback_;
  }
  if (cb) cb(vf);
}

bool DisplayCaptureWgc::EnsureCursorDib(int w, int h) {
  if (cursor_dc_ && cursor_dib_w_ == w && cursor_dib_h_ == h) return true;
  DestroyCursorDib();

  cursor_dc_ = CreateCompatibleDC(nullptr);
  if (!cursor_dc_) return false;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  cursor_bitmap_ = CreateDIBSection(cursor_dc_, &bmi, DIB_RGB_COLORS, &cursor_bits_, nullptr, 0);
  if (!cursor_bitmap_) {
    DeleteDC(cursor_dc_);
    cursor_dc_ = nullptr;
    return false;
  }
  SelectObject(cursor_dc_, cursor_bitmap_);
  cursor_dib_w_ = w;
  cursor_dib_h_ = h;
  return true;
}

void DisplayCaptureWgc::DestroyCursorDib() {
  if (cursor_bitmap_) {
    DeleteObject(cursor_bitmap_);
    cursor_bitmap_ = nullptr;
  }
  if (cursor_dc_) {
    DeleteDC(cursor_dc_);
    cursor_dc_ = nullptr;
  }
  cursor_bits_ = nullptr;
  cursor_dib_w_ = cursor_dib_h_ = 0;
}

void DisplayCaptureWgc::CompositeCursor(std::vector<uint8_t>* bgra, int w, int h, int origin_x,
                                        int origin_y) {
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

  ICONINFO ii{};
  if (!GetIconInfo(ci.hCursor, &ii)) return;
  int hot_x = ii.xHotspot;
  int hot_y = ii.yHotspot;
  // The sprite's real extent, not the SM_CXICON metric: a large cursor
  // (accessibility setting, high DPI) is visible while its top-left corner is
  // still off-frame, so a fixed 32 px bound drops it near the edges.
  int sprite_w = 0;
  int sprite_h = 0;
  BITMAP bitmap{};
  if (ii.hbmColor ? GetObject(ii.hbmColor, sizeof(bitmap), &bitmap) != 0
                  : GetObject(ii.hbmMask, sizeof(bitmap), &bitmap) != 0) {
    sprite_w = bitmap.bmWidth;
    // A monochrome cursor has no color bitmap; its mask stacks the AND and XOR
    // masks, so it is twice as tall as the sprite.
    sprite_h = ii.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
  }
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (sprite_w <= 0 || sprite_h <= 0) {
    sprite_w = GetSystemMetrics(SM_CXICON);
    sprite_h = GetSystemMetrics(SM_CYICON);
  }

  int px = ci.ptScreenPos.x - hot_x - origin_x;
  int py = ci.ptScreenPos.y - hot_y - origin_y;

  // Intersect the sprite with the frame. Everything below works on that box
  // only: compositing a <=64x64 sprite through a frame-sized DIB cost a 33 MB
  // allocation plus two full-frame memcpys (about 100 MB/s at 4K60).
  const int x0 = std::max(0, px);
  const int y0 = std::max(0, py);
  const int x1 = std::min(w, px + sprite_w);
  const int y1 = std::min(h, py + sprite_h);
  const int copy_w = x1 - x0;
  const int copy_h = y1 - y0;
  if (copy_w <= 0 || copy_h <= 0) return;  // fully off-frame

  if (!EnsureCursorDib(sprite_w, sprite_h)) return;

  const int frame_stride = w * 4;
  const int dib_stride = sprite_w * 4;
  // Local offset of the covered box inside the sprite-sized DIB; non-negative
  // because the box was clamped to the frame.
  const int local_x = x0 - px;
  const int local_y = y0 - py;
  // CreateDIBSection hands back a void*: address it as bytes once rather than
  // doing arithmetic on void* (a GCC extension that warns under -Wpointer-arith).
  BYTE* const dib = static_cast<BYTE*>(cursor_bits_);
  for (int row = 0; row < copy_h; ++row) {
    std::memcpy(
        dib + (local_y + row) * dib_stride + local_x * 4,
        bgra->data() + static_cast<size_t>(y0 + row) * frame_stride + static_cast<size_t>(x0) * 4,
        static_cast<size_t>(copy_w) * 4);
  }

  DrawIconEx(cursor_dc_, local_x, local_y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);

  for (int row = 0; row < copy_h; ++row) {
    std::memcpy(
        bgra->data() + static_cast<size_t>(y0 + row) * frame_stride + static_cast<size_t>(x0) * 4,
        dib + (local_y + row) * dib_stride + local_x * 4,
        static_cast<size_t>(copy_w) * 4);
  }
}
#endif

#if defined(_WIN32)
// Sends the pacer's chosen frame to the session, drawing the pointer and the latency HUD
// first when they are enabled. Shared by the desktop-duplication loop and the window loop.
void DisplayCaptureWgc::EmitPacedFrame(const FramePacer::Decision& emit) {
  if (!(emit.emit && emit.frame != nullptr)) return;
  const AppConfig& cfg = ConfigStore::Instance().Get();
  const bool want_cursor = show_cursor_;
  const bool draw_overlays = (want_cursor || cfg.latency_hud_enabled) && !emit.frame->data.empty();
  CapturedVideoFrame overlay_copy;
  const CapturedVideoFrame* frame = emit.frame;

  // Cursor-only overlay on a re-send whose cursor has not moved since the
  // cached composite: the result would be byte-identical to the frame the
  // callback already received, so reuse it instead of copying the whole
  // frame again. A fresh capture, a moved/hidden cursor, a toggled setting
  // or a different crop all fall through to the normal composite path.
  CURSORINFO cursor_state{};
  const bool cursor_visible =
      want_cursor && GetCursorInfo(&cursor_state) && (cursor_state.flags & CURSOR_SHOWING) != 0;
  const bool can_reuse =
      draw_overlays && !cfg.latency_hud_enabled && !emit.fresh && overlay_cache_valid_ &&
      cursor_visible && overlay_cursor_pos_.x == cursor_state.ptScreenPos.x &&
      overlay_cursor_pos_.y == cursor_state.ptScreenPos.y &&
      overlay_cache_.width == emit.frame->width && overlay_cache_.height == emit.frame->height;

  if (can_reuse) {
    frame = &overlay_cache_;
  } else if (draw_overlays) {
    overlay_copy = *emit.frame;
    if (want_cursor) {
      CompositeCursor(
          &overlay_copy.data, overlay_copy.width, overlay_copy.height, emit.crop_x, emit.crop_y);
      if (cursor_visible) {
        overlay_cursor_pos_ = cursor_state.ptScreenPos;
        // Keep this composite for the next unchanged re-send. Only the
        // cursor is drawn, so the bytes stay valid for identical pixels.
        overlay_cache_ = overlay_copy;
        overlay_cache_valid_ = true;
      } else {
        overlay_cache_valid_ = false;
      }
    } else {
      // HUD path: content changes every tick, so nothing is cacheable.
      overlay_cache_valid_ = false;
    }
    if (cfg.latency_hud_enabled) {
      LatencyHud::Render(overlay_copy);
    }
    frame = &overlay_copy;
  } else {
    // No overlay at all: drop any stale cache so toggling the cursor back
    // on cannot serve a composite from before the setting changed.
    overlay_cache_valid_ = false;
  }

  FrameCallback cb;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cb = callback_;
  }
  if (cb) cb(*frame);
}

#endif

// Window capture through Windows.Graphics.Capture. It has no desktop-duplication
// step: frames come from the window itself, and the pacer keeps the cadence steady
// while the window is idle.
#if defined(_WIN32)
void DisplayCaptureWgc::WindowCaptureLoop() {
  timeBeginPeriod(1);
  struct TimerPeriodGuard {
    ~TimerPeriodGuard() { timeEndPeriod(1); }
  } timer_period_guard;
  // WinRT objects are used on this thread; it must be in a COM apartment.
  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct ComGuard {
    bool owned;
    ~ComGuard() {
      if (owned) CoUninitialize();
    }
  } com_guard{SUCCEEDED(com_hr)};

  FramePacer pacer;
  pacer.SetTargetFps(target_fps_);

  // Where the window sits on screen, for pointer compositing: the visible frame,
  // not GetWindowRect, which includes the invisible resize border on Windows 10+.
  auto window_origin = [this]() {
    RECT r{};
    using DwmGetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static const auto dwm_get = reinterpret_cast<DwmGetWindowAttributeFn>(
        GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute"));
    constexpr DWORD kExtendedFrameBounds = 9;
    if (!dwm_get || FAILED(dwm_get(target_hwnd_, kExtendedFrameBounds, &r, sizeof(r)))) {
      GetWindowRect(target_hwnd_, &r);
    }
    return POINT{r.left, r.top};
  };

  while (running_) {
    const auto frame_start = std::chrono::steady_clock::now();
    pacer.SetTargetFps(target_fps_.load());
    const auto interval = std::chrono::microseconds(1000000 / pacer.TargetFps());

    EmitPacedFrame(pacer.Tick(frame_start));

    if (!IsWindow(target_hwnd_)) {
      EmitSourceLost(frame_start);
      LOG_WARN << "Capture window closed; stopping window capture";
      break;
    }
    if (IsIconic(target_hwnd_)) {
      // A minimised window produces no frames; keep re-sending the last one.
      std::this_thread::sleep_for(interval);
      continue;
    }

    CapturedVideoFrame frame;
    if (wgc_window_->TryGetFrame(&frame)) {
      const POINT origin = window_origin();
      pacer.Submit(std::move(frame), origin.x, origin.y);
    } else {
      // Nothing new: wait a short slice so the next pacer tick lands on time.
      const int wait_ms = pacer.WaitSliceMs(std::chrono::steady_clock::now());
      std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(wait_ms, 1, 8)));
    }
  }
  running_ = false;
}
#endif

void DisplayCaptureWgc::CaptureLoop() {
#if defined(_WIN32)
  // Frame pacing: DXGI only signals when the desktop changes, so the pacer
  // re-sends the newest frame on a fixed cadence to keep the stream steady.
  // 1 ms timer resolution keeps sleep and AcquireNextFrame waits from rounding
  // up to the default ~15.6 ms scheduler tick.
  timeBeginPeriod(1);
  struct TimerPeriodGuard {
    ~TimerPeriodGuard() { timeEndPeriod(1); }
  } timer_period_guard;

  // Consecutive duplication rebuild failures; reset on success. Owned by the
  // rebuild guard in the loop below.
  int dupl_retries = 0;
  FramePacer pacer;
  pacer.SetTargetFps(target_fps_);

  while (running_) {
    auto frame_start = std::chrono::steady_clock::now();

    pacer.SetTargetFps(target_fps_.load());
    const auto interval = std::chrono::microseconds(1000000 / pacer.TargetFps());

    // Cadence emit: newest frame if one arrived since the last tick, otherwise
    // the previous frame again. The pacer owns the pixels, so a re-send reaches
    // the session without a buffer copy; only the cursor and latency HUD need a
    // private copy to draw into.
    FramePacer::Decision emit = pacer.Tick(frame_start);
    EmitPacedFrame(emit);

    // Window mode: keep the crop rect fresh (moves/resizes) and detect the
    // window closing or sliding onto a different output.
    RECT crop{};
    if (target_hwnd_) {
      if (!IsWindow(target_hwnd_)) {
        EmitSourceLost(frame_start);
        LOG_WARN << "Capture window closed; stopping window capture";
        break;
      }
      if (IsIconic(target_hwnd_)) {
        // Minimized windows have no composited image to capture. Hold the
        // cadence (the emit block re-sends the last frame) instead of stalling
        // the session with a long sleep.
        std::this_thread::sleep_for(interval);
        continue;
      }
      RECT wr{};
      GetWindowRect(target_hwnd_, &wr);
      int best = OutputIndexForRect(wr);
      if (best >= 0 && best != output_index_) {
        LOG_INFO << "Capture window moved to output " << best << "; switching duplication";
        output_index_ = best;
        // Rebuilt by the retry guard before the next acquire, so a failed
        // switch never reaches AcquireNextFrame with a null duplication.
        desk_dupl_.Reset();
        continue;
      }
      crop.left = std::max(wr.left, output_rect_.left);
      crop.top = std::max(wr.top, output_rect_.top);
      crop.right = std::min(wr.right, output_rect_.right);
      crop.bottom = std::min(wr.bottom, output_rect_.bottom);
      if (crop.right - crop.left < 2 || crop.bottom - crop.top < 2) {
        // Window entirely off this output (mid-drag); wait for it to settle.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        continue;
      }
    } else {
      crop = output_rect_;
    }

    // Desktop duplication only signals on change, so a desktop that is already
    // static when capture starts would deliver nothing at all — not even one
    // frame to re-send. Seed a black placeholder of the crop size so the stream
    // starts on cadence; the first real capture replaces it.
    if (!pacer.HasFrame()) {
      const int seed_w = (crop.right - crop.left) & ~1;
      const int seed_h = (crop.bottom - crop.top) & ~1;
      if (seed_w > 0 && seed_h > 0) {
        CapturedVideoFrame seed;
        seed.width = seed_w;
        seed.height = seed_h;
        seed.stride = seed_w * 4;
        seed.timestamp = frame_start;
        seed.data.assign(static_cast<size_t>(seed.stride) * seed_h, 0);
        pacer.Submit(std::move(seed), crop.left, crop.top);
      }
    }

    // Loop invariant: AcquireNextFrame requires a live duplication, and every
    // path that can lose one (mode change, TDR, window moved outputs) drops it
    // and jumps back here. This is the single rebuild site, so a failed
    // rebuild can never fall through to the acquire below and dereference a
    // null duplication, and the retry budget covers every path.
    if (!desk_dupl_) {
      if (dupl_retries >= kMaxDuplicationRetries) {
        LOG_ERROR << "Desktop duplication unavailable after " << dupl_retries
                  << " attempts; stopping capture";
        if (target_hwnd_) EmitSourceLost(frame_start);
        break;
      }
      ++dupl_retries;
      if (CreateDuplication()) {
        dupl_retries = 0;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(kDuplicationRetryMs));
      }
      continue;
    }

    // Wait budget for the acquire: measured after the cadence tick so the call
    // blocks until the next tick instead of returning immediately on a tick
    // iteration and needing a second loop pass to do the real wait.
    const int wait_ms = pacer.WaitSliceMs(std::chrono::steady_clock::now());

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktop_res;
    HRESULT hr =
        desk_dupl_->AcquireNextFrame(static_cast<UINT>(wait_ms), &frame_info, &desktop_res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // Desktop unchanged: the cadence emit block re-sends the last frame.
      continue;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
      // Mode change / desktop switch / TDR: drop the duplication and let the
      // retry guard below rebuild it. One recreate site means one failure
      // budget and one place that guarantees a live pointer before the acquire.
      LOG_WARN << "DXGI duplication lost (hr=0x" << std::hex << hr << std::dec << "); re-creating";
      desk_dupl_.Reset();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    if (FAILED(hr)) {
      LOG_ERROR << "AcquireNextFrame failed: hr=0x" << std::hex << hr << std::dec;
      break;
    }

    // Content timestamp: DXGI reports the QPC time of the desktop present in
    // LastPresentTime. Mapping it (rather than stamping "now") puts video on
    // the same source timeline as WASAPI's QPC-based audio timestamps, which
    // is what keeps A/V in sync.
    //
    // A cursor-only update has no present time. Its pixels differ from the last
    // frame only in the cursor, so the desktop content instant is unchanged and
    // the previous present time is the honest value. Stamping wall clock here
    // would inject the present lag (~20 ms ahead of the source timeline) back
    // into the video clock on every mouse move.
    if (frame_info.AccumulatedFrames > 0 && frame_info.LastPresentTime.QuadPart != 0) {
      last_present_ts_ =
          QpcTicksToSteadyClock(static_cast<uint64_t>(frame_info.LastPresentTime.QuadPart));
      have_last_present_ts_ = true;
    }
    const auto frame_ts = have_last_present_ts_ ? last_present_ts_ : frame_start;

    // Periodic diagnostic: how old is the desktop image when we acquire it?
    // Video and audio (WASAPI QPC position) timestamps both reference the
    // source; this makes pipeline asymmetry visible without affecting sync.
    {
      const auto now = std::chrono::steady_clock::now();
      age_sum_ms_ +=
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(now - frame_ts)
              .count();
      ++age_count_;
      if (now - last_age_log_ >= std::chrono::seconds(5) && age_count_ > 0) {
        LOG_DEBUG << "DXGI frame present age avg: " << (age_sum_ms_ / age_count_) << " ms ("
                  << age_count_ << " frames)";
        last_age_log_ = now;
        age_sum_ms_ = 0.0;
        age_count_ = 0;
      }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_tex;
    if (FAILED(desktop_res.As(&desktop_tex)) || !desktop_tex) {
      desk_dupl_->ReleaseFrame();
      continue;
    }

    D3D11_TEXTURE2D_DESC desc;
    desktop_tex->GetDesc(&desc);

    // What the surface actually holds decides how it is read. With HDR on, Windows
    // hands out 16-bit float; reading that as 8-bit BGRA (the old behaviour) sends
    // the TV noise.
    SourcePixelFormat source_format = SourcePixelFormat::kBgra8;
    size_t source_bytes_per_pixel = 4;
    switch (desc.Format) {
      case DXGI_FORMAT_B8G8R8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: break;
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: source_format = SourcePixelFormat::kRgba8; break;
      case DXGI_FORMAT_R10G10B10A2_UNORM: source_format = SourcePixelFormat::kRgb10A2; break;
      case DXGI_FORMAT_R16G16B16A16_FLOAT:
        source_format = SourcePixelFormat::kRgbaF16;
        source_bytes_per_pixel = 8;
        break;
      default:
        LOG_ERROR << "Unsupported desktop surface format " << static_cast<int>(desc.Format)
                  << "; stopping capture instead of sending a corrupted picture";
        desk_dupl_->ReleaseFrame();
        running_ = false;
        continue;
    }
    if (source_format != last_logged_format_) {
      LOG_INFO << "Desktop surface format " << static_cast<int>(desc.Format)
               << (source_format == SourcePixelFormat::kRgbaF16 ? " (HDR, tone-mapped to SDR)"
                                                                : "");
      last_logged_format_ = source_format;
    }

    // Cache the staging texture; recreate only when the output size changes.
    if (!staging_tex_ || staging_w_ != static_cast<int>(desc.Width) ||
        staging_h_ != static_cast<int>(desc.Height)) {
      D3D11_TEXTURE2D_DESC sd = desc;
      sd.Usage = D3D11_USAGE_STAGING;
      sd.BindFlags = 0;
      sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      sd.MiscFlags = 0;
      staging_tex_.Reset();
      if (FAILED(d3d_device_->CreateTexture2D(&sd, nullptr, &staging_tex_))) {
        desk_dupl_->ReleaseFrame();
        continue;
      }
      staging_w_ = desc.Width;
      staging_h_ = desc.Height;
    }
    d3d_context_->CopyResource(staging_tex_.Get(), desktop_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      // Crop rect in texture coordinates (output-local). Every edge is snapped
      // to even: 4:2:0 chroma is subsampled 2x2, so an odd origin or size would
      // hand the encoder chroma planes that do not line up with the luma rows it
      // was given (a one-pixel skew, and for odd `cy` an out-of-bounds read on
      // the final row). RECT members are LONG, so each extent is narrowed to
      // int before meeting the staging dimensions: std::min cannot deduce one
      // type across (long, int).
      int cx = static_cast<int>(crop.left - output_rect_.left) & ~1;
      int cy = static_cast<int>(crop.top - output_rect_.top) & ~1;
      int cw = std::min(static_cast<int>(crop.right - crop.left), staging_w_ - cx) & ~1;
      int ch = std::min(static_cast<int>(crop.bottom - crop.top), staging_h_ - cy) & ~1;

      if (cw > 0 && ch > 0) {
        // Reuse the buffer of a frame the pacer has already superseded.
        // Without this, every capture allocated and zero-filled a full BGRA
        // frame (8.3 MB at 1080p) only for the row loop below to overwrite all
        // of it: a third full-frame pass, ~0.5 GB/s on a 1080p60 stream. In the
        // steady state two buffers alternate and nothing is allocated at all.
        CapturedVideoFrame vf;
        vf.data = pacer.TakeSupersededBuffer();
        vf.width = cw;
        vf.height = ch;
        vf.stride = cw * 4;
        // resize() runs only when the recycled buffer is the wrong size (a mode
        // or resolution change), so it costs nothing on the steady-state path.
        const size_t needed = static_cast<size_t>(vf.stride) * ch;
        if (vf.data.size() != needed) vf.data.resize(needed);
        vf.timestamp = frame_ts;
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(cy) * mapped.RowPitch +
                             static_cast<size_t>(cx) * source_bytes_per_pixel;
        ConvertToBgra8(src,
                       mapped.RowPitch,
                       source_format,
                       cw,
                       ch,
                       vf.data.data(),
                       static_cast<size_t>(vf.stride));

        d3d_context_->Unmap(staging_tex_.Get(), 0);

        // Stage for the pacer: it decides on which tick this frame goes out.
        pacer.Submit(std::move(vf), crop.left, crop.top);
      } else {
        d3d_context_->Unmap(staging_tex_.Get(), 0);
      }
    }

    desk_dupl_->ReleaseFrame();

    // Pace the tail of the tick towards the cadence; the pacer's deadline is
    // authoritative so a fast capture never drifts ahead of it.
    const auto now = std::chrono::steady_clock::now();
    if (now < pacer.NextTick()) {
      std::this_thread::sleep_for(pacer.NextTick() - now);
    }
  }
  running_ = false;
#endif
}

}  // namespace castcore
