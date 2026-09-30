// Coverage for GpuProcessor's letterbox geometry and BGRA -> YUV conversion.
//
// Initialize() computes a fit-inside rectangle (core/src/gpu_processor.cc:44-55)
// and both conversions pad the remainder with YUV black. Together those decide
// what the TV actually shows, so the tests below compare the *observable*
// geometry of the written planes against an independent restatement of the
// formula, and check the exact byte values of the padding.

#include <gtest/gtest.h>
#include "castcore/gpu_processor.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace castcore;

namespace {

struct FitRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// Independent restatement of the fit-inside scale in GpuProcessor::Initialize
// (core/src/gpu_processor.cc:44-55). The rectangle observed in the converted
// frame must equal this.
FitRect ExpectedFit(int sw, int sh, int dw, int dh) {
  double scale = std::min(static_cast<double>(dw) / sw, static_cast<double>(dh) / sh);
  int fit_w = std::max(2, static_cast<int>(sw * scale) & ~1);
  int fit_h = std::max(2, static_cast<int>(sh * scale) & ~1);
  fit_w = std::min(fit_w, dw & ~1);
  fit_h = std::min(fit_h, dh & ~1);
  FitRect r;
  r.x = ((dw - fit_w) / 2) & ~1;
  r.y = ((dh - fit_h) / 2) & ~1;
  r.w = fit_w;
  r.h = fit_h;
  return r;
}

CapturedVideoFrame MakeSolidFrame(int w, int h, uint8_t b, uint8_t g, uint8_t r) {
  CapturedVideoFrame f;
  f.width = w;
  f.height = h;
  f.stride = w * 4;
  f.data.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  for (size_t i = 0; i + 3 < f.data.size(); i += 4) {
    f.data[i + 0] = b;
    f.data[i + 1] = g;
    f.data[i + 2] = r;
    f.data[i + 3] = 0xFF;
  }
  return f;
}

// Bounding box of Y samples that are not letterbox padding (Y == 16).
bool ObserveContentRect(const std::vector<uint8_t>& y, int stride, int w, int h, FitRect* out) {
  int min_x = w, min_y = h, max_x = -1, max_y = -1;
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      if (y[static_cast<size_t>(row) * static_cast<size_t>(stride) + col] != 16) {
        min_y = std::min(min_y, row);
        max_y = std::max(max_y, row);
        min_x = std::min(min_x, col);
        max_x = std::max(max_x, col);
      }
    }
  }
  if (max_x < 0) return false;
  out->x = min_x;
  out->y = min_y;
  out->w = max_x - min_x + 1;
  out->h = max_y - min_y + 1;
  return true;
}

}  // namespace

// Odd source dimensions must still land on even fit_w/fit_h (4:2:0 chroma needs
// whole subsample boundaries) and the bars must sit exactly where the formula
// predicts.
TEST(GpuProcessorTest, LetterboxRectMatchesFormulaForOddSourceDimensions) {
  struct Case {
    int sw, sh, dw, dh;
  };
  const Case cases[] = {
      {1920, 1044, 1920, 1080},  // window slightly shorter than a 1080p frame
      {65, 37, 64, 48},          // odd source, downscale
      {63, 35, 48, 64},          // odd source, portrait destination
      {101, 99, 64, 48},         // both dimensions odd
  };

  for (const Case& c : cases) {
    SCOPED_TRACE(testing::Message() << c.sw << "x" << c.sh << " -> " << c.dw << "x" << c.dh);

    GpuProcessor gp;
    ASSERT_TRUE(gp.Initialize(c.sw, c.sh, c.dw, c.dh));
    EXPECT_EQ(gp.GetDstWidth(), c.dw);
    EXPECT_EQ(gp.GetDstHeight(), c.dh);

    // A bright source so every content sample differs from the 16 padding.
    CapturedVideoFrame src = MakeSolidFrame(c.sw, c.sh, 255, 255, 255);
    std::vector<uint8_t> y, u, v;
    int y_stride = 0, u_stride = 0, v_stride = 0;
    ASSERT_TRUE(gp.ConvertBgraToYuv420p(src, y, u, v, y_stride, u_stride, v_stride));

    const FitRect expected = ExpectedFit(c.sw, c.sh, c.dw, c.dh);
    EXPECT_EQ(expected.w % 2, 0);
    EXPECT_EQ(expected.h % 2, 0);

    FitRect observed;
    ASSERT_TRUE(ObserveContentRect(y, y_stride, c.dw, c.dh, &observed));
    EXPECT_EQ(observed.x, expected.x);
    EXPECT_EQ(observed.y, expected.y);
    EXPECT_EQ(observed.w, expected.w);
    EXPECT_EQ(observed.h, expected.h);

    // The invariants themselves, read off the produced frame.
    EXPECT_EQ(observed.w % 2, 0);
    EXPECT_EQ(observed.h % 2, 0);
    EXPECT_EQ(observed.x % 2, 0);
    EXPECT_EQ(observed.y % 2, 0);
  }
}

// The padding bytes are memset, so they are exactly hand-computable: limited
// range black is Y=16 / U=V=128. Also verifies no write escapes the declared
// plane sizes (a stride or dimension bug would clobber the guard bytes).
TEST(GpuProcessorTest, Yuv420pBarsAreNeutralBlackAndStridesAreRespected) {
  const int sw = 64, sh = 36, dw = 64, dh = 48;
  const FitRect fit = ExpectedFit(sw, sh, dw, dh);
  ASSERT_EQ(fit.y, 6);   // (48 - 36) / 2
  ASSERT_EQ(fit.h, 36);
  ASSERT_EQ(fit.w, 64);

  GpuProcessor gp;
  ASSERT_TRUE(gp.Initialize(sw, sh, dw, dh));

  CapturedVideoFrame src = MakeSolidFrame(sw, sh, 128, 128, 128);  // neutral gray

  const int y_stride = dw;
  const int c_stride = dw / 2;
  const size_t guard = 64;
  std::vector<uint8_t> y(static_cast<size_t>(y_stride) * dh + guard, 0xAB);
  std::vector<uint8_t> u(static_cast<size_t>(c_stride) * (dh / 2) + guard, 0xAB);
  std::vector<uint8_t> v(static_cast<size_t>(c_stride) * (dh / 2) + guard, 0xAB);

  ASSERT_TRUE(gp.ConvertBgraToYuv420p(src, y.data(), y_stride, u.data(), c_stride,
                                      v.data(), c_stride));

  // Top letterbox bar: rows [0, fit.y).
  for (int row = 0; row < fit.y; ++row) {
    for (int col = 0; col < dw; ++col) {
      EXPECT_EQ(y[static_cast<size_t>(row) * y_stride + col], 16) << "row " << row;
    }
  }
  // Bottom letterbox bar: rows [fit.y + fit.h, dh).
  for (int row = fit.y + fit.h; row < dh; ++row) {
    for (int col = 0; col < dw; ++col) {
      EXPECT_EQ(y[static_cast<size_t>(row) * y_stride + col], 16) << "row " << row;
    }
  }

  // Neutral gray has Cb == Cr == 128, and the memset padding is 128 too, so the
  // whole chroma planes must be exactly 128.
  const size_t u_bytes = static_cast<size_t>(c_stride) * (dh / 2);
  for (size_t i = 0; i < u_bytes; ++i) {
    ASSERT_EQ(u[i], 128) << "u[" << i << "]";
    ASSERT_EQ(v[i], 128) << "v[" << i << "]";
  }

  // Content rows are definitely not padding and sit near limited-range mid gray.
  const int mid_row = fit.y + fit.h / 2;
  for (int col = 0; col < dw; ++col) {
    const int sample = y[static_cast<size_t>(mid_row) * y_stride + col];
    EXPECT_NE(sample, 16);
    EXPECT_GE(sample, 118);
    EXPECT_LE(sample, 134);
  }

  // Nothing may be written past the plane sizes the caller declared.
  for (size_t i = static_cast<size_t>(y_stride) * dh; i < y.size(); ++i) {
    EXPECT_EQ(y[i], 0xAB);
  }
  for (size_t i = u_bytes; i < u.size(); ++i) {
    EXPECT_EQ(u[i], 0xAB);
  }
  for (size_t i = u_bytes; i < v.size(); ++i) {
    EXPECT_EQ(v[i], 0xAB);
  }
}

// NV12 keeps chroma interleaved in one plane; the byte order is U then V, and
// the letterbox padding is the same 16 / 128 as the planar path.
TEST(GpuProcessorTest, Nv12BarsAreNeutralAndChromaIsInterleavedUFirst) {
  const int sw = 64, sh = 36, dw = 64, dh = 48;
  const FitRect fit = ExpectedFit(sw, sh, dw, dh);
  ASSERT_EQ(fit.y, 6);
  ASSERT_EQ(fit.h, 36);

  GpuProcessor gp;
  ASSERT_TRUE(gp.Initialize(sw, sh, dw, dh));

  const int y_stride = dw;
  const int uv_stride = dw;  // one byte per chroma sample, two per pixel pair
  std::vector<uint8_t> y(static_cast<size_t>(y_stride) * dh + 64, 0xAB);
  std::vector<uint8_t> uv(static_cast<size_t>(uv_stride) * (dh / 2) + 64, 0xAB);

  // Red in BGRA (B=0, G=0, R=255): Cb falls below 128 and Cr rises above it,
  // which also pins the U-first byte order of the interleaved plane.
  CapturedVideoFrame src = MakeSolidFrame(sw, sh, 0, 0, 255);
  ASSERT_TRUE(gp.ConvertBgraToNv12(src, y.data(), y_stride, uv.data(), uv_stride));

  for (int row = 0; row < fit.y; ++row) {
    for (int col = 0; col < dw; ++col) {
      EXPECT_EQ(y[static_cast<size_t>(row) * y_stride + col], 16) << "row " << row;
    }
  }
  for (int row = fit.y + fit.h; row < dh; ++row) {
    for (int col = 0; col < dw; ++col) {
      EXPECT_EQ(y[static_cast<size_t>(row) * y_stride + col], 16) << "row " << row;
    }
  }

  // Chroma row 0 is entirely inside the top bar -> neutral 128,128.
  EXPECT_EQ(uv[0], 128);
  EXPECT_EQ(uv[1], 128);

  // A chroma row inside the fit rectangle: U (Cb) < 128 < V (Cr) for red.
  const int content_chroma_row = fit.y / 2 + fit.h / 4;
  const int u_byte = uv[static_cast<size_t>(content_chroma_row) * uv_stride + 0];
  const int v_byte = uv[static_cast<size_t>(content_chroma_row) * uv_stride + 1];
  EXPECT_LT(u_byte, 128) << "U must precede V in the interleaved plane";
  EXPECT_GT(v_byte, 128);

  for (size_t i = static_cast<size_t>(y_stride) * dh; i < y.size(); ++i) {
    EXPECT_EQ(y[i], 0xAB);
  }
  for (size_t i = static_cast<size_t>(uv_stride) * (dh / 2); i < uv.size(); ++i) {
    EXPECT_EQ(uv[i], 0xAB);
  }
}

TEST(GpuProcessorTest, InitializeAndConversionsRejectInvalidArguments) {
  GpuProcessor gp;
  EXPECT_FALSE(gp.Initialize(0, 0, 0, 0));
  EXPECT_FALSE(gp.Initialize(-1, 10, 10, 10));
  EXPECT_FALSE(gp.Initialize(10, 10, 0, 10));
  ASSERT_TRUE(gp.Initialize(64, 36, 64, 48));

  CapturedVideoFrame src = MakeSolidFrame(64, 36, 0, 0, 0);
  std::vector<uint8_t> y(64 * 48, 0), u(32 * 24, 0), v(32 * 24, 0);

  EXPECT_FALSE(gp.ConvertBgraToYuv420p(src, nullptr, 64, u.data(), 32, v.data(), 32));
  EXPECT_FALSE(gp.ConvertBgraToYuv420p(src, y.data(), 64, nullptr, 32, v.data(), 32));
  EXPECT_FALSE(gp.ConvertBgraToYuv420p(src, y.data(), 64, u.data(), 32, nullptr, 32));
  EXPECT_FALSE(gp.ConvertBgraToYuv420p(src, y.data(), 0, u.data(), 32, v.data(), 32));

  EXPECT_FALSE(gp.ConvertBgraToNv12(src, nullptr, 64, u.data(), 64));
  EXPECT_FALSE(gp.ConvertBgraToNv12(src, y.data(), 64, nullptr, 64));
  EXPECT_FALSE(gp.ConvertBgraToNv12(src, y.data(), 64, u.data(), 0));
}
