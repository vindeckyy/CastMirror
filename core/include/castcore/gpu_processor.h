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

class GpuProcessor {
 public:
  GpuProcessor();
  ~GpuProcessor();

  bool Initialize(int src_width, int src_height, int dst_width, int dst_height);

  bool ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                           std::vector<uint8_t>& dst_y,
                           std::vector<uint8_t>& dst_u,
                           std::vector<uint8_t>& dst_v,
                           int& y_stride, int& u_stride, int& v_stride);

  int GetDstWidth() const { return dst_width_; }
  int GetDstHeight() const { return dst_height_; }

 private:
  int src_width_ = 0;
  int src_height_ = 0;
  int dst_width_ = 0;
  int dst_height_ = 0;

  ::SwsContext* sws_ctx_ = nullptr;
};

} // namespace castcore

#endif // CASTCORE_GPU_PROCESSOR_H_
