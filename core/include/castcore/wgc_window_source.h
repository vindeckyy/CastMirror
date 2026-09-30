#ifndef CASTCORE_WGC_WINDOW_SOURCE_H_
#define CASTCORE_WGC_WINDOW_SOURCE_H_

#if defined(_WIN32)

#include "castcore/types.h"

#include <windows.h>
#include <memory>

namespace castcore {

// Captures one window through Windows.Graphics.Capture (Windows 10 version 1903+).
// Unlike cropping the desktop image, this delivers the window's own composed
// content: other windows in front of it, or partly off-screen, do not appear in
// the picture. It is a polling source: the capture loop asks for the newest frame
// on its own cadence.
class WgcWindowSource {
 public:
  // Null when the API is unavailable (older Windows) or this window cannot be
  // captured (protected content, elevated process, not a top-level window).
  static std::unique_ptr<WgcWindowSource> Create(HWND hwnd);
  ~WgcWindowSource();

  WgcWindowSource(const WgcWindowSource&) = delete;
  WgcWindowSource& operator=(const WgcWindowSource&) = delete;

  // The newest frame delivered since the last call, as tightly packed BGRA with an
  // even width and height. Returns false when nothing new has arrived. Follows the
  // window when it is resized.
  bool TryGetFrame(CapturedVideoFrame* out);

 private:
  struct Impl;
  explicit WgcWindowSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace castcore

#endif  // _WIN32
#endif  // CASTCORE_WGC_WINDOW_SOURCE_H_
