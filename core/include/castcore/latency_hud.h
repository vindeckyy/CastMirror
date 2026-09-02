#ifndef CASTCORE_LATENCY_HUD_H_
#define CASTCORE_LATENCY_HUD_H_

#include "castcore/types.h"
#include <cstdint>

namespace castcore {

class LatencyHud {
 public:
  // Composites a high-contrast millisecond stopwatch and frame counter
  // into the top-left corner of the captured frame buffer.
  static void Render(CapturedVideoFrame& frame);

 private:
  static void DrawChar(uint8_t* dst, int stride, int width, int height,
                       int x, int y, char c, uint32_t color);
  static void DrawString(uint8_t* dst, int stride, int width, int height,
                         int x, int y, const char* str, uint32_t color);
  static void DrawRect(uint8_t* dst, int stride, int width, int height,
                       int x, int y, int w, int h, uint32_t bg_color);
};

}  // namespace castcore

#endif  // CASTCORE_LATENCY_HUD_H_
