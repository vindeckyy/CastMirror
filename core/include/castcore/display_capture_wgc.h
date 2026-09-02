#ifndef CASTCORE_DISPLAY_CAPTURE_WGC_H_
#define CASTCORE_DISPLAY_CAPTURE_WGC_H_

#include "castcore/display_capture.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace castcore {

class DisplayCaptureWgc : public IDisplayCapture {
 public:
  DisplayCaptureWgc();
  ~DisplayCaptureWgc() override;

  bool Start(int display_id, int target_fps = 60) override;
  bool Start(const CaptureSource& source, int target_fps) override;
  void Stop() override;
  bool IsCapturing() const override;
  void SetTargetFps(int fps) override;
  std::string BackendName() const override { return "windows_graphics_capture"; }

  std::vector<DisplayInfo> EnumerateDisplays() override;
  std::vector<WindowInfo> EnumerateWindows() override;
  bool SupportsWindowCapture() const override { return true; }
  CaptureSource ActiveSource() const override { return active_source_; }

  void SetFrameCallback(FrameCallback callback) override;
  void SetShowCursor(bool show) override;

 private:
  void CaptureLoop();

  std::atomic<bool> running_{false};
  std::thread worker_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
  bool show_cursor_ = false;
  int target_fps_ = 60;
  CaptureSource active_source_{CaptureSourceKind::kMonitor, 0, ""};

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> desk_dupl_;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_DISPLAY_CAPTURE_WGC_H_
