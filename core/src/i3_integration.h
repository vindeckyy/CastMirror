#ifndef CASTCORE_I3_INTEGRATION_H_
#define CASTCORE_I3_INTEGRATION_H_

#if !defined(_WIN32)

#include <X11/Xlib.h>

#include <memory>

namespace castcore {

// Keeps an i3-managed X11 window mapped while it is being captured. i3
// normally unmaps every client on an inactive workspace, which freezes direct
// per-window capture. A capture pin temporarily makes the source floating and
// sticky, parks its i3 frame outside the visible root window, and gives it an
// empty input shape. Restore() returns all state that CastMirror changed.
class I3CapturePin {
 public:
  I3CapturePin();
  ~I3CapturePin();

  I3CapturePin(const I3CapturePin&) = delete;
  I3CapturePin& operator=(const I3CapturePin&) = delete;

  static bool IsAvailable(Display* display);

  bool Pin(Display* display, Window window);
  void Restore(Display* display);
  bool IsPinned() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace castcore

#endif  // !defined(_WIN32)

#endif  // CASTCORE_I3_INTEGRATION_H_
