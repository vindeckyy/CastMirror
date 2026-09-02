#include "castcore/latency_hud.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <atomic>

namespace castcore {

namespace {

// Simple 5x7 bitmap font for ASCII 32..90 (space through Z)
// Each character is 5 columns wide, encoded as 5 bytes (each byte is 7 bits tall).
static const uint8_t kFont5x7[][5] = {
  {0x00, 0x00, 0x00, 0x00, 0x00}, // 32 ' '
  {0x00, 0x00, 0x5F, 0x00, 0x00}, // 33 '!'
  {0x00, 0x07, 0x00, 0x07, 0x00}, // 34 '"'
  {0x14, 0x7F, 0x14, 0x7F, 0x14}, // 35 '#'
  {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // 36 '$'
  {0x23, 0x13, 0x08, 0x64, 0x62}, // 37 '%'
  {0x36, 0x49, 0x55, 0x22, 0x50}, // 38 '&'
  {0x00, 0x05, 0x03, 0x00, 0x00}, // 39 '\''
  {0x00, 0x1C, 0x22, 0x41, 0x00}, // 40 '('
  {0x00, 0x41, 0x22, 0x1C, 0x00}, // 41 ')'
  {0x14, 0x08, 0x3E, 0x08, 0x14}, // 42 '*'
  {0x08, 0x08, 0x3E, 0x08, 0x08}, // 43 '+'
  {0x00, 0x50, 0x30, 0x00, 0x00}, // 44 ','
  {0x08, 0x08, 0x08, 0x08, 0x08}, // 45 '-'
  {0x00, 0x60, 0x60, 0x00, 0x00}, // 46 '.'
  {0x20, 0x10, 0x08, 0x04, 0x02}, // 47 '/'
  {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 48 '0'
  {0x00, 0x42, 0x7F, 0x40, 0x00}, // 49 '1'
  {0x42, 0x61, 0x51, 0x49, 0x46}, // 50 '2'
  {0x21, 0x41, 0x45, 0x4B, 0x31}, // 51 '3'
  {0x18, 0x14, 0x12, 0x7F, 0x10}, // 52 '4'
  {0x27, 0x45, 0x45, 0x45, 0x39}, // 53 '5'
  {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 54 '6'
  {0x01, 0x71, 0x09, 0x05, 0x03}, // 55 '7'
  {0x36, 0x49, 0x49, 0x49, 0x36}, // 56 '8'
  {0x06, 0x49, 0x49, 0x29, 0x1E}, // 57 '9'
  {0x00, 0x36, 0x36, 0x00, 0x00}, // 58 ':'
  {0x00, 0x56, 0x36, 0x00, 0x00}, // 59 ';'
  {0x08, 0x14, 0x22, 0x41, 0x00}, // 60 '<'
  {0x14, 0x14, 0x14, 0x14, 0x14}, // 61 '='
  {0x00, 0x41, 0x22, 0x14, 0x08}, // 62 '>'
  {0x02, 0x01, 0x51, 0x09, 0x06}, // 63 '?'
  {0x32, 0x49, 0x79, 0x41, 0x3E}, // 64 '@'
  {0x7E, 0x11, 0x11, 0x11, 0x7E}, // 65 'A'
  {0x7F, 0x49, 0x49, 0x49, 0x36}, // 66 'B'
  {0x3E, 0x41, 0x41, 0x41, 0x22}, // 67 'C'
  {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 68 'D'
  {0x7F, 0x49, 0x49, 0x49, 0x41}, // 69 'E'
  {0x7F, 0x09, 0x09, 0x09, 0x01}, // 70 'F'
  {0x3E, 0x41, 0x49, 0x49, 0x7A}, // 71 'G'
  {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 72 'H'
  {0x00, 0x41, 0x7F, 0x41, 0x00}, // 73 'I'
  {0x20, 0x40, 0x41, 0x3F, 0x01}, // 74 'J'
  {0x7F, 0x08, 0x14, 0x22, 0x41}, // 75 'K'
  {0x7F, 0x40, 0x40, 0x40, 0x40}, // 76 'L'
  {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 77 'M'
  {0x7F, 0x04, 0x08, 0x10, 0x7F}, // 78 'N'
  {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 79 'O'
  {0x7F, 0x09, 0x09, 0x09, 0x06}, // 80 'P'
  {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 81 'Q'
  {0x7F, 0x09, 0x19, 0x29, 0x46}, // 82 'R'
  {0x46, 0x49, 0x49, 0x49, 0x31}, // 83 'S'
  {0x01, 0x01, 0x7F, 0x01, 0x01}, // 84 'T'
  {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 85 'U'
  {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 86 'V'
  {0x7F, 0x20, 0x18, 0x20, 0x7F}, // 87 'W'
  {0x63, 0x14, 0x08, 0x14, 0x63}, // 88 'X'
  {0x07, 0x08, 0x70, 0x08, 0x07}, // 89 'Y'
  {0x61, 0x51, 0x49, 0x45, 0x43}  // 90 'Z'
};

std::atomic<uint64_t> s_frame_counter{0};
std::chrono::steady_clock::time_point s_origin_time = std::chrono::steady_clock::now();

}  // namespace

void LatencyHud::DrawRect(uint8_t* dst, int stride, int width, int height,
                          int x, int y, int w, int h, uint32_t bg_color) {
  uint8_t b = bg_color & 0xFF;
  uint8_t g = (bg_color >> 8) & 0xFF;
  uint8_t r = (bg_color >> 16) & 0xFF;
  uint8_t a = (bg_color >> 24) & 0xFF;

  for (int dy = 0; dy < h; ++dy) {
    int py = y + dy;
    if (py < 0 || py >= height) continue;
    uint8_t* row = dst + static_cast<size_t>(py) * stride;
    for (int dx = 0; dx < w; ++dx) {
      int px = x + dx;
      if (px < 0 || px >= width) continue;
      uint8_t* p = row + px * 4;
      if (a == 255) {
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
      } else {
        uint32_t inv = 255 - a;
        p[0] = static_cast<uint8_t>((p[0] * inv + b * a) / 255);
        p[1] = static_cast<uint8_t>((p[1] * inv + g * a) / 255);
        p[2] = static_cast<uint8_t>((p[2] * inv + r * a) / 255);
        p[3] = 255;
      }
    }
  }
}

void LatencyHud::DrawChar(uint8_t* dst, int stride, int width, int height,
                          int x, int y, char c, uint32_t color) {
  if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
  if (c < 32 || c > 90) c = ' ';
  const uint8_t* glyph = kFont5x7[c - 32];

  uint8_t b = color & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t r = (color >> 16) & 0xFF;

  // Scale 2x for high-DPI TV readability (each 5x7 glyph becomes 10x14)
  const int scale = 2;
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 7; ++row) {
      if ((bits >> row) & 1) {
        for (int sy = 0; sy < scale; ++sy) {
          int py = y + row * scale + sy;
          if (py < 0 || py >= height) continue;
          uint8_t* prow = dst + static_cast<size_t>(py) * stride;
          for (int sx = 0; sx < scale; ++sx) {
            int px = x + col * scale + sx;
            if (px < 0 || px >= width) continue;
            uint8_t* p = prow + px * 4;
            p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
          }
        }
      }
    }
  }
}

void LatencyHud::DrawString(uint8_t* dst, int stride, int width, int height,
                           int x, int y, const char* str, uint32_t color) {
  int cur_x = x;
  while (*str) {
    DrawChar(dst, stride, width, height, cur_x, y, *str, color);
    cur_x += (5 * 2) + 2;  // 10px width + 2px inter-char spacing
    ++str;
  }
}

void LatencyHud::Render(CapturedVideoFrame& frame) {
  if (frame.data.empty() || frame.width < 320 || frame.height < 180) return;

  uint64_t fid = ++s_frame_counter;
  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_origin_time).count();

  uint64_t secs = elapsed_ms / 1000;
  uint64_t ms = elapsed_ms % 1000;
  uint64_t mins = secs / 60;
  secs = secs % 60;
  mins = mins % 60;

  char time_buf[64];
  std::snprintf(time_buf, sizeof(time_buf), "TIME: %02lu:%02lu.%03lu",
                static_cast<unsigned long>(mins),
                static_cast<unsigned long>(secs),
                static_cast<unsigned long>(ms));

  char frame_buf[64];
  std::snprintf(frame_buf, sizeof(frame_buf), "FRAME: #%lu", static_cast<unsigned long>(fid));

  // Render background badge: 240x48 at (20, 20)
  const int badge_x = 20;
  const int badge_y = 20;
  const int badge_w = 260;
  const int badge_h = 44;

  // Dark semi-transparent background: 0xE0181818 (BGRA format)
  DrawRect(frame.data.data(), frame.stride, frame.width, frame.height,
           badge_x, badge_y, badge_w, badge_h, 0xEE181818);

  // Border: 1px accent cyan/green outline
  DrawRect(frame.data.data(), frame.stride, frame.width, frame.height,
           badge_x, badge_y, badge_w, 2, 0xFF00D4AA);

  // Text lines: yellow/green high contrast colors
  DrawString(frame.data.data(), frame.stride, frame.width, frame.height,
             badge_x + 10, badge_y + 8, time_buf, 0xFF00FFFF);  // Bright yellow
  DrawString(frame.data.data(), frame.stride, frame.width, frame.height,
             badge_x + 10, badge_y + 24, frame_buf, 0xFFFFFFFF); // Bright white
}

}  // namespace castcore
