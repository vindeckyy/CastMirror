#ifndef CASTCORE_GPU_PROCESSOR_H_
#define CASTCORE_GPU_PROCESSOR_H_

#include "castcore/types.h"
#include <vector>
#include <memory>

struct SwsContext;

namespace castcore {

struct ProcessedVideoFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> y_plane;
  std::vector<uint8_t> uv_plane; // NV12 or U+V for YUV420p
  int y_stride = 0;
  int uv_stride = 0;
  std::chrono::steady_clock::time_point timestamp;
};

// CPU color conversion (libswscale) from BGRA capture to encoder input.
// Despite the historical name this stays on libswscale until a real GPU
// backend exists. Conversion letterboxes the source inside the destination:
// the source is scaled down to fit, never stretched, never upscaled, and the
// remainder is padded with YUV black (16/128/128).
class GpuProcessor {
 public:
  GpuProcessor();
  ~GpuProcessor();

  bool Initialize(int src_width, int src_height, int dst_width, int dst_height);

  // Direct-write variants: the caller owns the destination planes (typical
  // case: an AVFrame's linesized planes) and must size them for
  // GetDstWidth()/GetDstHeight(). No temporary vectors, no extra copies.
  bool ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                            uint8_t* dst_y, int y_stride,
                            uint8_t* dst_u, int u_stride,
                            uint8_t* dst_v, int v_stride);

  bool ConvertBgraToNv12(const CapturedVideoFrame& src,
                         uint8_t* dst_y, int y_stride,
                         uint8_t* dst_uv, int uv_stride);

  // Convenience wrapper around the pointer variant (allocates/reuses the
  // vectors at the exact letterbox strides).
  bool ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                            std::vector<uint8_t>& dst_y,
                            std::vector<uint8_t>& dst_u,
                            std::vector<uint8_t>& dst_v,
                            int& y_stride, int& u_stride, int& v_stride);

  int GetDstWidth() const { return dst_width_; }
  int GetDstHeight() const { return dst_height_; }

 private:
  bool EnsureSource(const CapturedVideoFrame& src);

  int src_width_ = 0;
  int src_height_ = 0;
  int dst_width_ = 0;
  int dst_height_ = 0;

  // Fit-inside rectangle inside dst_width_ x dst_height_ (all values even).
  int fit_x_ = 0;
  int fit_y_ = 0;
  int fit_w_ = 0;
  int fit_h_ = 0;

  ::SwsContext* sws_ctx_ = nullptr;       // BGRA -> YUV420P at fit rect
  ::SwsContext* sws_ctx_nv12_ = nullptr;  // BGRA -> NV12 at fit rect
  ::SwsContext* sws_nv12_direct_ = nullptr; // NV12 -> NV12 direct (DMA-BUF zero-copy, 0 extra GPU copies)
};

} // namespace castcore

#endif // CASTCORE_GPU_PROCESSOR_H_
