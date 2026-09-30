#include <gtest/gtest.h>
#include "castcore/pixel_convert.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace castcore;

namespace {
uint16_t Half(float f) {
  // Encode a handful of exact values by hand; enough for these tests.
  if (f == 0.0f) return 0x0000;
  if (f == 1.0f) return 0x3C00;
  if (f == 0.5f) return 0x3800;
  if (f == 2.5f) return 0x4100;
  if (f == 8.0f) return 0x4800;
  if (f == -1.0f) return 0xBC00;
  ADD_FAILURE() << "no half encoding for " << f;
  return 0;
}
}  // namespace

TEST(PixelConvertTest, HalfToFloatKnownValues) {
  EXPECT_FLOAT_EQ(HalfToFloat(0x0000), 0.0f);
  EXPECT_FLOAT_EQ(HalfToFloat(0x3C00), 1.0f);
  EXPECT_FLOAT_EQ(HalfToFloat(0xC000), -2.0f);
  EXPECT_FLOAT_EQ(HalfToFloat(0x3800), 0.5f);
  EXPECT_TRUE(std::isinf(HalfToFloat(0x7C00)));
  EXPECT_TRUE(std::isnan(HalfToFloat(0x7E00)));
  EXPECT_GT(HalfToFloat(0x0001), 0.0f);  // smallest subnormal
}

TEST(PixelConvertTest, Bgra8IsCopiedRowByRowHonouringPitch) {
  // 2x2 source with 12-byte pitch (4 bytes of padding per row).
  const uint8_t src[24] = {1, 2, 3, 255, 4,  5,  6,  255, 9, 9, 9, 9,
                           7, 8, 9, 255, 10, 11, 12, 255, 9, 9, 9, 9};
  uint8_t dst[16] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 12, SourcePixelFormat::kBgra8, 2, 2, dst, 8));
  const uint8_t expect[16] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
  EXPECT_EQ(std::memcmp(dst, expect, sizeof(expect)), 0);
}

TEST(PixelConvertTest, Rgba8SwapsRedAndBlue) {
  const uint8_t src[4] = {10, 20, 30, 40};  // R G B A
  uint8_t dst[4] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 4, SourcePixelFormat::kRgba8, 1, 1, dst, 4));
  EXPECT_EQ(dst[0], 30);
  EXPECT_EQ(dst[1], 20);
  EXPECT_EQ(dst[2], 10);
  EXPECT_EQ(dst[3], 255);
}

TEST(PixelConvertTest, Rgb10A2UnpacksToEightBitBgra) {
  const uint32_t r = 1023, g = 512, b = 0;
  const uint32_t packed = r | (g << 10) | (b << 20) | (3u << 30);
  uint8_t src[4];
  std::memcpy(src, &packed, 4);
  uint8_t dst[4] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 4, SourcePixelFormat::kRgb10A2, 1, 1, dst, 4));
  EXPECT_EQ(dst[2], 255);  // R
  EXPECT_EQ(dst[1], 128);  // G
  EXPECT_EQ(dst[0], 0);  // B
  EXPECT_EQ(dst[3], 255);
}

TEST(PixelConvertTest, HdrSdrWhiteMapsToFullScaleAndBlackStaysBlack) {
  // scRGB with SDR white at 2.5 (200 nits): 2.5 -> 255, 0 -> 0.
  uint16_t px[4] = {Half(2.5f), Half(2.5f), Half(2.5f), Half(1.0f)};
  uint8_t src[8];
  std::memcpy(src, px, 8);
  uint8_t dst[4] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 8, SourcePixelFormat::kRgbaF16, 1, 1, dst, 4, 2.5f));
  EXPECT_EQ(dst[0], 255);
  EXPECT_EQ(dst[1], 255);
  EXPECT_EQ(dst[2], 255);

  uint16_t black[4] = {0, 0, 0, Half(1.0f)};
  std::memcpy(src, black, 8);
  ASSERT_TRUE(ConvertToBgra8(src, 8, SourcePixelFormat::kRgbaF16, 1, 1, dst, 4, 2.5f));
  EXPECT_EQ(dst[0], 0);
  EXPECT_EQ(dst[1], 0);
  EXPECT_EQ(dst[2], 0);
}

TEST(PixelConvertTest, HdrHighlightsClipAndNegativesGoToBlack) {
  uint16_t px[4] = {Half(8.0f), Half(-1.0f), Half(0.0f), Half(1.0f)};  // R bright, G negative
  uint8_t src[8];
  std::memcpy(src, px, 8);
  uint8_t dst[4] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 8, SourcePixelFormat::kRgbaF16, 1, 1, dst, 4));
  EXPECT_EQ(dst[2], 255);  // R clipped
  EXPECT_EQ(dst[1], 0);  // G negative
}

TEST(PixelConvertTest, HdrMidGreyIsSrgbEncodedNotLinear) {
  // Linear 0.5 of SDR white encodes to about 188 in sRGB (a linear copy would give 128).
  uint16_t px[4] = {Half(0.5f), Half(0.5f), Half(0.5f), Half(1.0f)};
  uint8_t src[8];
  std::memcpy(src, px, 8);
  uint8_t dst[4] = {};
  ASSERT_TRUE(ConvertToBgra8(src, 8, SourcePixelFormat::kRgbaF16, 1, 1, dst, 4, 1.0f));
  EXPECT_NEAR(dst[1], 188, 2);
}

TEST(PixelConvertTest, RejectsBadArguments) {
  uint8_t px[4] = {};
  uint8_t out[4] = {1, 2, 3, 4};
  EXPECT_FALSE(ConvertToBgra8(nullptr, 4, SourcePixelFormat::kBgra8, 1, 1, out, 4));
  EXPECT_FALSE(ConvertToBgra8(px, 4, SourcePixelFormat::kBgra8, 0, 1, out, 4));
  EXPECT_FALSE(ConvertToBgra8(px, 3, SourcePixelFormat::kBgra8, 1, 1, out, 4));  // pitch too small
  EXPECT_FALSE(ConvertToBgra8(px, 4, SourcePixelFormat::kRgbaF16, 1, 1, out, 4));  // needs 8 bytes
  EXPECT_EQ(out[0], 1);  // untouched on failure
}
