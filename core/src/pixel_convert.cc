#include "castcore/pixel_convert.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace castcore {

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 0x1u;
  const uint32_t exp = (h >> 10) & 0x1Fu;
  const uint32_t man = h & 0x3FFu;
  float value;
  if (exp == 0) {
    value = std::ldexp(static_cast<float>(man), -24);  // subnormal or zero
  } else if (exp == 31) {
    value = man == 0 ? INFINITY : NAN;
  } else {
    value = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
  }
  return sign ? -value : value;
}

namespace {

// Linear [0,1] to 8-bit sRGB through a table: cheaper than pow() per channel on a
// 1080p frame, and the 4096 steps keep the dark end (where banding shows) smooth.
constexpr int kEncodeSteps = 4096;

const std::vector<uint8_t>& SrgbEncodeTable() {
  static std::vector<uint8_t> table;
  static std::once_flag once;
  std::call_once(once, [] {
    table.resize(kEncodeSteps + 1);
    for (int i = 0; i <= kEncodeSteps; ++i) {
      const float linear = static_cast<float>(i) / kEncodeSteps;
      const float srgb = linear <= 0.0031308f
                             ? linear * 12.92f
                             : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
      table[i] = static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
    }
  });
  return table;
}

// All 65536 half values to float, built once (256 KB).
const std::vector<float>& HalfTable() {
  static std::vector<float> table;
  static std::once_flag once;
  std::call_once(once, [] {
    table.resize(65536);
    for (uint32_t i = 0; i < 65536; ++i) table[i] = HalfToFloat(static_cast<uint16_t>(i));
  });
  return table;
}

}  // namespace

bool ConvertToBgra8(const uint8_t* src, size_t src_pitch, SourcePixelFormat fmt,
                    int width, int height, uint8_t* dst, size_t dst_pitch,
                    float sdr_white_ratio) {
  if (!src || !dst || width <= 0 || height <= 0) return false;
  const size_t bytes_per_pixel = fmt == SourcePixelFormat::kRgbaF16 ? 8u : 4u;
  if (src_pitch < static_cast<size_t>(width) * bytes_per_pixel) return false;
  if (dst_pitch < static_cast<size_t>(width) * 4u) return false;

  switch (fmt) {
    case SourcePixelFormat::kBgra8:
      for (int y = 0; y < height; ++y) {
        std::memcpy(dst + y * dst_pitch, src + y * src_pitch, static_cast<size_t>(width) * 4u);
      }
      return true;

    case SourcePixelFormat::kRgba8:
      for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + y * src_pitch;
        uint8_t* d = dst + y * dst_pitch;
        for (int x = 0; x < width; ++x, s += 4, d += 4) {
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = 255;
        }
      }
      return true;

    case SourcePixelFormat::kRgb10A2:
      for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + y * src_pitch;
        uint8_t* d = dst + y * dst_pitch;
        for (int x = 0; x < width; ++x, s += 4, d += 4) {
          uint32_t v;
          std::memcpy(&v, s, sizeof(v));
          // R in bits 0-9, G in 10-19, B in 20-29. Drop the low two bits of each.
          d[2] = static_cast<uint8_t>((v & 0x3FFu) >> 2);
          d[1] = static_cast<uint8_t>(((v >> 10) & 0x3FFu) >> 2);
          d[0] = static_cast<uint8_t>(((v >> 20) & 0x3FFu) >> 2);
          d[3] = 255;
        }
      }
      return true;

    case SourcePixelFormat::kRgbaF16: {
      const std::vector<float>& half = HalfTable();
      const std::vector<uint8_t>& encode = SrgbEncodeTable();
      const float scale = sdr_white_ratio > 0.0f ? 1.0f / sdr_white_ratio : 1.0f / kDefaultSdrWhiteRatio;
      auto to8 = [&](uint16_t bits) {
        float v = half[bits] * scale;   // 1.0 = SDR white
        if (!(v > 0.0f)) return static_cast<uint8_t>(0);  // negatives and NaN
        if (v >= 1.0f) return static_cast<uint8_t>(255);
        return encode[static_cast<size_t>(v * kEncodeSteps + 0.5f)];
      };
      for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + y * src_pitch;
        uint8_t* d = dst + y * dst_pitch;
        for (int x = 0; x < width; ++x, s += 8, d += 4) {
          uint16_t px[3];
          std::memcpy(px, s, sizeof(px));  // R, G, B; alpha ignored
          d[2] = to8(px[0]);
          d[1] = to8(px[1]);
          d[0] = to8(px[2]);
          d[3] = 255;
        }
      }
      return true;
    }
  }
  return false;
}

}  // namespace castcore
