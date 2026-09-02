#ifndef CASTCORE_DISPLAY_CAPTURE_H_
#define CASTCORE_DISPLAY_CAPTURE_H_

#include "castcore/types.h"
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace castcore {

class IDisplayCapture {
 public:
  using FrameCallback = std::function<void(const CapturedVideoFrame& frame)>;

  virtual ~IDisplayCapture() = default;

  // Legacy entry point: capture a monitor by its DisplayInfo::id.
  // Implementations should treat this as Start(CaptureSource{Monitor, display_id}, fps).
  virtual bool Start(int display_id, int target_fps = 60) = 0;

  // Source-aware entry point: capture a monitor or a single window. The
  // default implementation forwards to the legacy Start(int,int) so existing
  // backends keep working until they override this. Backends that support
  // window capture must override and honor source.kind == Window.
  virtual bool Start(const CaptureSource& source, int target_fps) {
    return Start(source.id, target_fps);
  }

  virtual void Stop() = 0;
  virtual bool IsCapturing() const = 0;
  virtual void SetTargetFps(int fps) { (void)fps; }
  virtual bool SizeKnownBeforeStart() const { return true; }
  virtual std::string BackendName() const { return "unknown"; }

  virtual std::vector<DisplayInfo> EnumerateDisplays() = 0;

  // Enumerate toplevel windows available for window sharing. Returns empty
  // when the backend does not support window capture (check
  // SupportsWindowCapture()).
  virtual std::vector<WindowInfo> EnumerateWindows() { return {}; }

  // True when this backend can capture individual windows (not just monitors).
  virtual bool SupportsWindowCapture() const { return false; }

  // The source currently being captured, or a default Monitor source when
  // not capturing / unsupported. Used by the session for stats and geometry.
  virtual CaptureSource ActiveSource() const { return {CaptureSourceKind::kMonitor, 0, ""}; }

  virtual void SetFrameCallback(FrameCallback callback) = 0;
  // Control whether the hardware cursor is composited into captured frames.
  // Default is false (cursor hidden). Must be called before Start().
  virtual void SetShowCursor(bool show) { (void)show; }

  // Phase 1.1 metric: frames skipped due to XDamage no-damage optimization
  virtual uint64_t GetCaptureSkipped() const { return 0; }
};

// Factory for creating platform or synthetic display capturers
class DisplayCaptureFactory {
 public:
  static std::unique_ptr<IDisplayCapture> Create();
  static std::unique_ptr<IDisplayCapture> CreateSynthetic(int width = 1920, int height = 1080);
};

} // namespace castcore

#endif // CASTCORE_DISPLAY_CAPTURE_H_
