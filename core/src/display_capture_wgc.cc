#include "castcore/display_capture_wgc.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include "castcore/latency_hud.h"
#include "castcore/win_time.h"

#include <chrono>
#include <thread>
#include <cstring>

#if defined(_WIN32)
#include <algorithm>
#include <timeapi.h>
#include "castcore/frame_pacer.h"
#endif

namespace castcore {

#if defined(_WIN32)
namespace {

// Enumerate all DXGI outputs across adapters as a flat list of
// (adapter, output) pairs, in the same order EnumerateDisplays() uses so a
// DisplayInfo::id maps to exactly one entry.
struct OutputEntry {
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  DXGI_OUTPUT_DESC desc{};
};

std::vector<OutputEntry> EnumerateOutputs() {
  std::vector<OutputEntry> out;
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;

  for (UINT ai = 0;; ++ai) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(factory->EnumAdapters(ai, &adapter))) break;
    for (UINT oi = 0;; ++oi) {
      Microsoft::WRL::ComPtr<IDXGIOutput> output;
      if (FAILED(adapter->EnumOutputs(oi, &output))) break;
      OutputEntry e;
      e.adapter = adapter;
      e.output = output;
      output->GetDesc(&e.desc);
      out.push_back(std::move(e));
    }
  }
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

  if (source.IsWindow()) {
    target_hwnd_ = reinterpret_cast<HWND>(static_cast<intptr_t>(source.id));
    if (!IsWindow(target_hwnd_)) {
      LOG_ERROR << "Window capture source 0x" << std::hex << source.id << std::dec
                << " is not a valid window";
      target_hwnd_ = nullptr;
      return false;
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

  auto outputs = EnumerateOutputs();
  if (outputs.empty()) {
    LOG_ERROR << "No DXGI outputs available for desktop duplication";
    return false;
  }
  int idx = output_index_;
  if (idx < 0 || idx >= static_cast<int>(outputs.size())) {
    LOG_WARN << "Requested output index " << idx << " out of range ("
             << outputs.size() << " outputs); falling back to output 0";
    idx = 0;
    output_index_ = 0;
  }

  OutputEntry& entry = outputs[idx];

  // The D3D11 device must be created on the same adapter that owns the output,
  // otherwise DuplicateOutput fails with E_INVALIDARG.
  HRESULT hr = D3D11CreateDevice(
      entry.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, &d3d_device_, nullptr, &d3d_context_);
  if (FAILED(hr)) {
    LOG_ERROR << "Failed to create D3D11 device on adapter for output " << idx
              << ": hr=0x" << std::hex << hr << std::dec;
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
  if (FAILED(entry.output.As(&output1))) {
    LOG_ERROR << "Output " << idx << " does not support DXGI1.1 duplication";
    return false;
  }

  hr = output1->DuplicateOutput(d3d_device_.Get(), &desk_dupl_);
  if (FAILED(hr)) {
    LOG_ERROR << "DuplicateOutput failed on output " << idx << ": hr=0x"
              << std::hex << hr << std::dec;
    return false;
  }

  output_rect_ = entry.desc.DesktopCoordinates;
  char dev_name[64];
  WideCharToMultiByte(CP_UTF8, 0, entry.desc.DeviceName, -1, dev_name, sizeof(dev_name), nullptr, nullptr);
  LOG_INFO << "Duplicating DXGI output " << idx << " (" << dev_name << ") rect ("
           << output_rect_.left << "," << output_rect_.top << ")-("
           << output_rect_.right << "," << output_rect_.bottom << ")";
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
  auto outputs = EnumerateOutputs();
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
    char dev_name[64];
    WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, dev_name, sizeof(dev_name), nullptr, nullptr);
    std::snprintf(name_buf, sizeof(name_buf), "Display %d (%dx%d)",
                  static_cast<int>(i) + 1, info.width, info.height);
    info.name = name_buf;
    displays.push_back(info);
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

  EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
    if (!IsWindowVisible(hwnd)) return TRUE;
    auto* c = reinterpret_cast<EnumContext*>(lparam);
    wchar_t title[256];
    if (GetWindowTextW(hwnd, title, 256) > 0) {
      RECT r;
      GetWindowRect(hwnd, &r);
      int w = r.right - r.left;
      int h = r.bottom - r.top;
      if (w > 100 && h > 100) {
        WindowInfo wi;
        wi.id = static_cast<int>(reinterpret_cast<intptr_t>(hwnd));
        char title_utf8[512];
        WideCharToMultiByte(CP_UTF8, 0, title, -1, title_utf8, sizeof(title_utf8), nullptr, nullptr);
        wi.title = title_utf8;
        wi.x = r.left;
        wi.y = r.top;
        wi.width = w;
        wi.height = h;
        c->list->push_back(wi);
      }
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&ctx));
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

void DisplayCaptureWgc::CompositeCursor(std::vector<uint8_t>* bgra, int w, int h,
                                        int origin_x, int origin_y) {
  CURSORINFO ci{};
  ci.cbSize = sizeof(ci);
  if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

  ICONINFO ii{};
  if (!GetIconInfo(ci.hCursor, &ii)) return;
  int hot_x = ii.xHotspot;
  int hot_y = ii.yHotspot;
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  if (ii.hbmColor) DeleteObject(ii.hbmColor);

  int px = ci.ptScreenPos.x - hot_x - origin_x;
  int py = ci.ptScreenPos.y - hot_y - origin_y;

  // Fully off-screen cursors still need a cheap early-out.
  if (px >= w || py >= h || px + GetSystemMetrics(SM_CXICON) <= 0 ||
      py + GetSystemMetrics(SM_CYICON) <= 0) {
    return;
  }

  if (!EnsureCursorDib(w, h)) return;

  std::memcpy(cursor_bits_, bgra->data(), static_cast<size_t>(w) * h * 4);
  DrawIconEx(cursor_dc_, px, py, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
  std::memcpy(bgra->data(), cursor_bits_, static_cast<size_t>(w) * h * 4);
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

  int dupl_failures = 0;
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
    if (emit.emit && emit.frame != nullptr) {
      const AppConfig& cfg = ConfigStore::Instance().Get();
      const bool draw_overlays = (show_cursor_ || cfg.latency_hud_enabled) && !emit.frame->data.empty();
      CapturedVideoFrame overlay_copy;
      const CapturedVideoFrame* frame = emit.frame;
      if (draw_overlays) {
        overlay_copy = *emit.frame;
        if (show_cursor_) {
          CompositeCursor(&overlay_copy.data, overlay_copy.width, overlay_copy.height, emit.crop_x,
                          emit.crop_y);
        }
        if (cfg.latency_hud_enabled) {
          LatencyHud::Render(overlay_copy);
        }
        frame = &overlay_copy;
      }

      FrameCallback cb;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
      }
      if (cb) cb(*frame);
    }

    // Window mode: keep the crop rect fresh (moves/resizes) and detect the
    // window closing or sliding onto a different output.
    RECT crop{};
    if (target_hwnd_) {
      if (!IsWindow(target_hwnd_)) {
        // Emit a single source_lost frame so the session fails gracefully
        // instead of stalling on the last captured frame.
        if (!source_lost_emitted_) {
          source_lost_emitted_ = true;
          CapturedVideoFrame vf;
          vf.width = 2;
          vf.height = 2;
          vf.stride = 8;
          vf.timestamp = frame_start;
          vf.data.assign(2 * 8, 0);
          vf.source_lost = true;
          FrameCallback cb;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = callback_;
          }
          if (cb) cb(vf);
        }
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
        if (!CreateDuplication()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
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

    // Wait budget for the acquire: measured after the cadence tick so the call
    // blocks until the next tick instead of returning immediately on a tick
    // iteration and needing a second loop pass to do the real wait.
    const int wait_ms = pacer.WaitSliceMs(std::chrono::steady_clock::now());

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktop_res;
    HRESULT hr = desk_dupl_->AcquireNextFrame(static_cast<UINT>(wait_ms), &frame_info, &desktop_res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      // Desktop unchanged: the cadence emit block re-sends the last frame.
      continue;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
      // Mode change / desktop switch / TDR: rebuild the duplication instead of
      // killing the capture session.
      LOG_WARN << "DXGI duplication lost (hr=0x" << std::hex << hr << std::dec
               << "); re-creating";
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!CreateDuplication()) {
        if (++dupl_failures >= 10) {
          LOG_ERROR << "Could not re-create desktop duplication; stopping capture";
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      } else {
        dupl_failures = 0;
      }
      continue;
    }
    if (FAILED(hr)) {
      LOG_ERROR << "AcquireNextFrame failed: hr=0x" << std::hex << hr << std::dec;
      break;
    }

    // Content timestamp: DXGI reports the QPC time of the desktop present in
    // LastPresentTime. Mapping it (rather than stamping "now") puts video on
    // the same source timeline as WASAPI's QPC-based audio timestamps, which
    // is what keeps A/V in sync. Cursor-only updates have no present time and
    // fall back to now.
    auto frame_ts = frame_start;
    if (frame_info.AccumulatedFrames > 0 && frame_info.LastPresentTime.QuadPart != 0) {
      frame_ts = QpcTicksToSteadyClock(
          static_cast<uint64_t>(frame_info.LastPresentTime.QuadPart));
    }

    // Periodic diagnostic: how old is the desktop image when we acquire it?
    // Video and audio (WASAPI QPC position) timestamps both reference the
    // source; this makes pipeline asymmetry visible without affecting sync.
    {
      static double s_age_sum_ms = 0.0;
      static int s_age_count = 0;
      static auto s_last_age_log = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      s_age_sum_ms +=
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
              now - frame_ts)
              .count();
      ++s_age_count;
      if (now - s_last_age_log >= std::chrono::seconds(5) && s_age_count > 0) {
        LOG_DEBUG << "DXGI frame present age avg: " << (s_age_sum_ms / s_age_count)
                  << " ms (" << s_age_count << " frames)";
        s_last_age_log = now;
        s_age_sum_ms = 0.0;
        s_age_count = 0;
      }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_tex;
    desktop_res.As(&desktop_tex);

    D3D11_TEXTURE2D_DESC desc;
    desktop_tex->GetDesc(&desc);

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
      // Crop rect in texture coordinates (output-local).
      int cx = crop.left - output_rect_.left;
      int cy = crop.top - output_rect_.top;
      int cw = (crop.right - crop.left) & ~1;
      int ch = (crop.bottom - crop.top) & ~1;
      cw = std::min(cw, staging_w_ - (cx & ~1));
      ch = std::min(ch, staging_h_ - cy);
      cx &= ~1;

      if (cw > 0 && ch > 0) {
        CapturedVideoFrame vf;
        vf.width = cw;
        vf.height = ch;
        vf.stride = cw * 4;
        vf.timestamp = frame_ts;
        vf.data.resize(static_cast<size_t>(vf.stride) * ch);
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) +
                             static_cast<size_t>(cy) * mapped.RowPitch +
                             static_cast<size_t>(cx) * 4;
        for (int row = 0; row < ch; ++row) {
          std::memcpy(vf.data.data() + static_cast<size_t>(row) * vf.stride,
                      src + static_cast<size_t>(row) * mapped.RowPitch,
                      static_cast<size_t>(vf.stride));
        }

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
