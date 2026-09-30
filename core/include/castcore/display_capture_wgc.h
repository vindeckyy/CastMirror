#ifndef CASTCORE_DISPLAY_CAPTURE_WGC_H_
#define CASTCORE_DISPLAY_CAPTURE_WGC_H_

#include "castcore/display_capture.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace castcore {

// Windows desktop capture via DXGI Desktop Duplication. Supports whole-monitor
// capture and single-window capture (implemented as a per-frame crop of the
// output the window lives on, so it also works on multi-GPU/multi-monitor
// setups). Recreates the duplication when the desktop mode changes
// (DXGI_ERROR_ACCESS_LOST) instead of tearing the session down.
class DisplayCaptureWgc : public IDisplayCapture {
 public:
  DisplayCaptureWgc();
  ~DisplayCaptureWgc() override;

  bool Start(int display_id, int target_fps = 60) override;
  bool Start(const CaptureSource& source, int target_fps) override;
  void Stop() override;
  bool IsCapturing() const override;
  void SetTargetFps(int fps) override;
  // The backend is DXGI Desktop Duplication; the string is what the UI shows
  // as capture detail, so it must not name Windows.Graphics.Capture.
  std::string BackendName() const override { return "windows_desktop_duplication"; }

  std::vector<DisplayInfo> EnumerateDisplays() override;
  std::vector<WindowInfo> EnumerateWindows() override;
  bool SupportsWindowCapture() const override { return true; }
  CaptureSource ActiveSource() const override { return active_source_; }

  void SetFrameCallback(FrameCallback callback) override;
  void SetShowCursor(bool show) override;

 private:
  void CaptureLoop();
#if defined(_WIN32)
  // (Re)creates the D3D11 device + output duplication for the currently
  // selected output index. On failure the duplication is left null, and the
  // capture loop rebuilds it before the next acquire. Safe to call again after
  // ACCESS_LOST.
  bool CreateDuplication();
  // Delivers the one-shot source_lost frame that tells the session the capture
  // source is gone (window closed, duplication unrecoverable) so it fails
  // gracefully instead of stalling on the last captured frame.
  void EmitSourceLost(std::chrono::steady_clock::time_point timestamp);
  // Returns the flat output index (as produced by EnumerateDisplays) that has
  // the largest intersection with rect, in virtual-screen coordinates.
  int OutputIndexForRect(const RECT& rect);
  // Ensures the GDI DIB used for cursor compositing matches w x h.
  bool EnsureCursorDib(int w, int h);
  void DestroyCursorDib();
  void CompositeCursor(std::vector<uint8_t>* bgra, int w, int h, int origin_x, int origin_y);
#endif

  std::atomic<bool> running_{false};
  std::thread worker_thread_;
  std::mutex mutex_;
  FrameCallback callback_;
  // Written by SetShowCursor() on the session thread, read by the capture
  // worker on every frame it composites.
  std::atomic<bool> show_cursor_{false};
  std::atomic<int> target_fps_{60};
  CaptureSource active_source_{CaptureSourceKind::kMonitor, 0, ""};

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> desk_dupl_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_tex_;
  int staging_w_ = 0;
  int staging_h_ = 0;

  int output_index_ = 0;      // flat DXGI output index being duplicated
  RECT output_rect_{};        // duplicated output's virtual-screen rect
  HWND target_hwnd_ = nullptr; // non-null in window-capture mode
  bool source_lost_emitted_ = false;

  // Present-age diagnostic accumulator, touched only by the capture worker.
  // Per-instance so two concurrent sessions neither race the counters nor
  // attribute one session's frames to the other.
  double age_sum_ms_ = 0.0;
  int age_count_ = 0;
  std::chrono::steady_clock::time_point last_age_log_{};

  // Overlay reuse cache. Compositing the cursor needs a private copy of the
  // frame, because the pacer owns the pixels it re-sends. On a static desktop
  // with a motionless cursor that copy is pure overhead - 8.3 MB per tick at
  // 1080p, 60 times a second - and produces a byte-identical result. This holds
  // the last composited frame so an unchanged re-send is handed straight to the
  // callback. Only valid while the cursor is the only overlay: the latency HUD
  // redraws a live millisecond counter every frame, so it never caches.
  CapturedVideoFrame overlay_cache_;
  POINT overlay_cursor_pos_{};
  bool overlay_cache_valid_ = false;

  HDC cursor_dc_ = nullptr;
  HBITMAP cursor_bitmap_ = nullptr;
  void* cursor_bits_ = nullptr;
  int cursor_dib_w_ = 0;
  int cursor_dib_h_ = 0;
#endif
};

}  // namespace castcore

#endif  // CASTCORE_DISPLAY_CAPTURE_WGC_H_
