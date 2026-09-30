#ifndef CASTCORE_PIXEL_CONVERT_H_
#define CASTCORE_PIXEL_CONVERT_H_

#include <cstddef>
#include <cstdint>

namespace castcore {

// Pixel layouts a desktop duplication surface can arrive in. An SDR desktop is
// Bgra8. With HDR turned on Windows hands out 16-bit float scRGB, and some
// drivers use 10-bit; reading those as 8-bit BGRA (what the capture path used to
// do) produces solid noise.
enum class SourcePixelFormat {
  kBgra8,     // B8G8R8A8: copied as is
  kRgba8,     // R8G8B8A8: red and blue swapped
  kRgb10A2,   // R10G10B10A2, unsigned normalized
  kRgbaF16,   // R16G16B16A16 float, scRGB: linear, 1.0 = 80 nits
};

// SDR white is where "100% white" sits inside an HDR desktop, as a multiple of
// 80 nits. Windows' default slider position is about 200 nits.
constexpr float kDefaultSdrWhiteRatio = 2.5f;

// Converts a w x h block of `fmt` pixels to 8-bit BGRA with alpha 255. HDR input
// is mapped so SDR white becomes 255 and anything brighter clips (a highlight
// that clips is still a picture; noise is not). Returns false for unsupported
// arguments and leaves dst untouched.
bool ConvertToBgra8(const uint8_t* src, size_t src_pitch, SourcePixelFormat fmt,
                    int width, int height, uint8_t* dst, size_t dst_pitch,
                    float sdr_white_ratio = kDefaultSdrWhiteRatio);

// IEEE 754 half to single precision. Exposed for tests.
float HalfToFloat(uint16_t h);

}  // namespace castcore

#endif  // CASTCORE_PIXEL_CONVERT_H_
