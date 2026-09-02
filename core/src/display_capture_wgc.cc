#include "castcore/display_capture_wgc.h"
#include "castcore/logger.h"
#include "castcore/config.h"
#include "castcore/latency_hud.h"

#include <chrono>
#include <thread>

namespace castcore {

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

  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
      D3D11_SDK_VERSION, &d3d_device_, nullptr, &d3d_context_);
  if (FAILED(hr)) {
    LOG_ERROR << "Failed to create D3D11 device for desktop capture: hr=" << hr;
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_dev;
  if (FAILED(d3d_device_.As(&dxgi_dev))) return false;

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_dev->GetAdapter(&adapter))) return false;

  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  if (FAILED(adapter->EnumOutputs(static_cast<UINT>(source.id), &output))) {
    // Fallback to output 0
    if (FAILED(adapter->EnumOutputs(0, &output))) return false;
  }

  Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
  if (FAILED(output.As(&output1))) return false;

  hr = output1->DuplicateOutput(d3d_device_.Get(), &desk_dupl_);
  if (FAILED(hr)) {
    LOG_ERROR << "DuplicateOutput failed: hr=" << hr;
    return false;
  }

  running_ = true;
  worker_thread_ = std::thread(&DisplayCaptureWgc::CaptureLoop, this);
  return true;
#endif
}

void DisplayCaptureWgc::Stop() {
  running_ = false;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
#if defined(_WIN32)
  desk_dupl_.Reset();
  d3d_context_.Reset();
  d3d_device_.Reset();
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
  DISPLAY_DEVICEW dd{};
  dd.cb = sizeof(dd);
  DWORD dev_num = 0;
  while (EnumDisplayDevicesW(nullptr, dev_num, &dd, 0)) {
    if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
      DEVMODEW dm{};
      dm.dmSize = sizeof(dm);
      if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        DisplayInfo info;
        info.id = static_cast<int>(dev_num);
        char name_buf[128];
        std::snprintf(name_buf, sizeof(name_buf), "Display %lu (%ux%u)",
                      dev_num + 1, static_cast<unsigned>(dm.dmPelsWidth),
                      static_cast<unsigned>(dm.dmPelsHeight));
        info.name = name_buf;
        info.width = static_cast<int>(dm.dmPelsWidth);
        info.height = static_cast<int>(dm.dmPelsHeight);
        info.refresh_rate = static_cast<int>(dm.dmDisplayFrequency);
        info.is_primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        displays.push_back(info);
      }
    }
    ++dev_num;
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

void DisplayCaptureWgc::CaptureLoop() {
#if defined(_WIN32)
  while (running_) {
    auto frame_start = std::chrono::steady_clock::now();
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> desktop_res;
    HRESULT hr = desk_dupl_->AcquireNextFrame(100, &frame_info, &desktop_res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
      continue;
    }
    if (FAILED(hr)) {
      if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARN << "DXGI duplication access lost (screen resolution or desktop switch)";
      }
      break;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_tex;
    desktop_res.As(&desktop_tex);

    D3D11_TEXTURE2D_DESC desc;
    desktop_tex->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_tex;
    d3d_device_->CreateTexture2D(&desc, nullptr, &staging_tex);
    d3d_context_->CopyResource(staging_tex.Get(), desktop_tex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(d3d_context_->Map(staging_tex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      CapturedVideoFrame vf;
      vf.width = static_cast<int>(desc.Width);
      vf.height = static_cast<int>(desc.Height);
      vf.stride = static_cast<int>(mapped.RowPitch);
      vf.timestamp = frame_start;
      size_t total_bytes = static_cast<size_t>(vf.stride) * vf.height;
      vf.data.resize(total_bytes);
      std::memcpy(vf.data.data(), mapped.pData, total_bytes);

      d3d_context_->Unmap(staging_tex.Get(), 0);

      if (ConfigStore::Instance().Get().latency_hud_enabled) {
        LatencyHud::Render(vf);
      }

      FrameCallback cb;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
      }
      if (cb) cb(vf);
    }

    desk_dupl_->ReleaseFrame();

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - frame_start);
    auto interval = std::chrono::microseconds(1000000 / target_fps_);
    if (elapsed < interval) {
      std::this_thread::sleep_for(interval - elapsed);
    }
  }
#endif
}

}  // namespace castcore
