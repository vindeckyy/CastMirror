#include "castcore/gpu_processor.h"
#include "castcore/logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace castcore {

GpuProcessor::GpuProcessor() = default;

GpuProcessor::~GpuProcessor() {
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
}

bool GpuProcessor::Initialize(int src_width, int src_height, int dst_width, int dst_height) {
  if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
    LOG_ERROR << "Invalid dimensions for GpuProcessor::Initialize";
    return false;
  }

  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }

  src_width_ = src_width;
  src_height_ = src_height;
  dst_width_ = dst_width;
  dst_height_ = dst_height;

  sws_ctx_ = sws_getContext(
      src_width_, src_height_, AV_PIX_FMT_BGRA,
      dst_width_, dst_height_, AV_PIX_FMT_YUV420P,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

  if (!sws_ctx_) {
    LOG_ERROR << "Failed to allocate SwsContext";
    return false;
  }

  return true;
}

bool GpuProcessor::ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                                      std::vector<uint8_t>& dst_y,
                                      std::vector<uint8_t>& dst_u,
                                      std::vector<uint8_t>& dst_v,
                                      int& y_stride, int& u_stride, int& v_stride) {
  if (src.width != src_width_ || src.height != src_height_ || !sws_ctx_) {
    if (!Initialize(src.width, src.height, dst_width_ > 0 ? dst_width_ : src.width, dst_height_ > 0 ? dst_height_ : src.height)) {
      return false;
    }
  }

  y_stride = dst_width_;
  u_stride = dst_width_ / 2;
  v_stride = dst_width_ / 2;

  size_t y_size = static_cast<size_t>(y_stride * dst_height_);
  size_t uv_size = static_cast<size_t>(u_stride * (dst_height_ / 2));

  dst_y.resize(y_size);
  dst_u.resize(uv_size);
  dst_v.resize(uv_size);

  const uint8_t* src_slice[1] = { src.data.data() };
  int src_stride[1] = { src.stride > 0 ? src.stride : src.width * 4 };

  uint8_t* dst_slice[3] = { dst_y.data(), dst_u.data(), dst_v.data() };
  int dst_strides[3] = { y_stride, u_stride, v_stride };

  sws_scale(sws_ctx_, src_slice, src_stride, 0, src_height_, dst_slice, dst_strides);
  return true;
}

} // namespace castcore
