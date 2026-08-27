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

  virtual bool Start(int display_id, int target_fps = 60) = 0;
  virtual void Stop() = 0;
  virtual bool IsCapturing() const = 0;
  virtual void SetTargetFps(int fps) { (void)fps; }
  virtual bool SizeKnownBeforeStart() const { return true; }
  virtual std::string BackendName() const { return "unknown"; }

  virtual std::vector<DisplayInfo> EnumerateDisplays() = 0;
  virtual void SetFrameCallback(FrameCallback callback) = 0;
};

// Factory for creating platform or synthetic display capturers
class DisplayCaptureFactory {
 public:
  static std::unique_ptr<IDisplayCapture> Create();
  static std::unique_ptr<IDisplayCapture> CreateSynthetic(int width = 1920, int height = 1080);
};

} // namespace castcore

#endif // CASTCORE_DISPLAY_CAPTURE_H_
