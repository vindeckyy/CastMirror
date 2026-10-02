#ifndef CASTCORE_DISPLAY_CAPTURE_WGC_H_
#define CASTCORE_DISPLAY_CAPTURE_WGC_H_

#include "castcore/display_capture.h"
#include "castcore/frame_pacer.h"
#include "castcore/pixel_convert.h"
#include "castcore/wgc_window_source.h"

#if defined(_WIN32)
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace castcore {

// Windows capture. Monitors use DXGI Desktop Duplication. A single window uses
// Windows.Graphics.Capture when it is available (Windows 10 1903+), which delivers
// the window's own content, so windows in front of it stay out of the picture. If
// that cannot start, a window is captured as a per-frame crop of the output it lives
// on. Recreates the duplication when the desktop mode changes
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
  // Shown in the UI as capture detail, so it names what is really capturing.
  std::string BackendName() const override {
#if defined(_WIN32)
    if (wgc_window_) return "windows_graphics_capture";
#endif
    return "windows_desktop_duplication";
  }

  std::vector<DisplayInfo> EnumerateDisplays() override;
  std::vector<WindowInfo> EnumerateWindows() override;
  bool SupportsWindowCapture() const override { return true; }
  CaptureSource ActiveSource() const override { return active_source_; }

  void SetFrameCallback(FrameCallback callback) override;
  void SetShowCursor(bool show) override;

 private:
  void CaptureLoop();
#if defined(_WIN32)
  void WindowCaptureLoop();
  void EmitPacedFrame(const FramePacer::Decision& emit);
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
  // Last surface format logged, so a switch to HDR is reported once, not per frame.
  SourcePixelFormat last_logged_format_ = SourcePixelFormat::kBgra8;
  CaptureSource active_source_{CaptureSourceKind::kMonitor, 0, ""};

#if defined(_WIN32)
  Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> desk_dupl_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_tex_;
  int staging_w_ = 0;
  int staging_h_ = 0;

  int output_index_ = 0;  // flat DXGI output index being duplicated
  RECT output_rect_{};  // duplicated output's virtual-screen rect
  HWND target_hwnd_ = nullptr;  // non-null in window-capture mode
  std::unique_ptr<WgcWindowSource> wgc_window_;  // set when the window is captured through WGC
  bool source_lost_emitted_ = false;

  // Present-age diagnostic accumulator, touched only by the capture worker.
  // Per-instance so two concurrent sessions neither race the counters nor
  // attribute one session's frames to the other.
  double age_sum_ms_ = 0.0;
  int age_count_ = 0;
  std::chrono::steady_clock::time_point last_age_log_{};
  // Content instant of the last frame that carried a real present time. A
  // cursor-only update has none, and the desktop image genuinely did not
  // change then, so the previous present time is the honest content instant;
  // wall clock would be ~20 ms ahead of the source timeline.
  std::chrono::steady_clock::time_point last_present_ts_{};
  bool have_last_present_ts_ = false;

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
