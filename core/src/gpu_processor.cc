#include "castcore/gpu_processor.h"
#include "castcore/logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <libdrm/drm_fourcc.h>
#endif

namespace castcore {

GpuProcessor::GpuProcessor() = default;

GpuProcessor::~GpuProcessor() {
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
  if (sws_ctx_nv12_) {
    sws_freeContext(sws_ctx_nv12_);
    sws_ctx_nv12_ = nullptr;
  }
  if (sws_nv12_direct_) {
    sws_freeContext(sws_nv12_direct_);
    sws_nv12_direct_ = nullptr;
  }
}

bool GpuProcessor::Initialize(int src_width, int src_height, int dst_width, int dst_height) {
  if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) {
    LOG_ERROR << "Invalid dimensions for GpuProcessor::Initialize";
    return false;
  }

  // Fit-inside scale. Upscaling is allowed so window capture can fill a
  // 1920x1080 encoder frame even when the window is smaller (e.g. 1920x1044).
  // Everything stays even so 4:2:0 chroma offsets land on whole subsample boundaries.
  double scale = std::min(static_cast<double>(dst_width) / src_width,
                          static_cast<double>(dst_height) / src_height);
  int fit_w = std::max(2, static_cast<int>(src_width * scale) & ~1);
  int fit_h = std::max(2, static_cast<int>(src_height * scale) & ~1);
  fit_w = std::min(fit_w, dst_width & ~1);
  fit_h = std::min(fit_h, dst_height & ~1);

  int fit_x = ((dst_width - fit_w) / 2) & ~1;
  int fit_y = ((dst_height - fit_h) / 2) & ~1;

  ::SwsContext* ctx = sws_getContext(
      src_width, src_height, AV_PIX_FMT_BGRA,
      fit_w, fit_h, AV_PIX_FMT_YUV420P,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!ctx) {
    LOG_ERROR << "Failed to allocate SwsContext (YUV420P)";
    return false;
  }
  ::SwsContext* ctx_nv12 = sws_getContext(
      src_width, src_height, AV_PIX_FMT_BGRA,
      fit_w, fit_h, AV_PIX_FMT_NV12,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!ctx_nv12) {
    sws_freeContext(ctx);
    LOG_ERROR << "Failed to allocate SwsContext (NV12)";
    return false;
  }
  // Phase 1.2: NV12 DIRECT (DMA-BUF zero-copy) – when source is already NV12
  // from PipeWire DMA-BUF we avoid the BGRA shadow copy and scale NV12->NV12
  // with 0 extra GPU copies, letterboxing directly into the encoder's NV12 planes.
  ::SwsContext* ctx_direct = sws_getContext(
      src_width, src_height, AV_PIX_FMT_NV12,
      fit_w, fit_h, AV_PIX_FMT_NV12,
      SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!ctx_direct) {
    sws_freeContext(ctx);
    sws_freeContext(ctx_nv12);
    LOG_ERROR << "Failed to allocate SwsContext (NV12 direct)";
    return false;
  }

  if (sws_ctx_) sws_freeContext(sws_ctx_);
  if (sws_ctx_nv12_) sws_freeContext(sws_ctx_nv12_);
  if (sws_nv12_direct_) sws_freeContext(sws_nv12_direct_);
  sws_ctx_ = ctx;
  sws_ctx_nv12_ = ctx_nv12;
  sws_nv12_direct_ = ctx_direct;

  src_width_ = src_width;
  src_height_ = src_height;
  dst_width_ = dst_width;
  dst_height_ = dst_height;
  fit_x_ = fit_x;
  fit_y_ = fit_y;
  fit_w_ = fit_w;
  fit_h_ = fit_h;
  return true;
}

bool GpuProcessor::EnsureSource(const CapturedVideoFrame& src) {
  if (src.width == src_width_ && src.height == src_height_ && sws_ctx_ && sws_ctx_nv12_ && sws_nv12_direct_) {
    return true;
  }
  return Initialize(src.width, src.height,
                    dst_width_ > 0 ? dst_width_ : src.width,
                    dst_height_ > 0 ? dst_height_ : src.height);
}

bool GpuProcessor::ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                                        uint8_t* dst_y, int y_stride,
                                        uint8_t* dst_u, int u_stride,
                                        uint8_t* dst_v, int v_stride) {
  if (!dst_y || !dst_u || !dst_v || y_stride <= 0 || u_stride <= 0 || v_stride <= 0) {
    return false;
  }
  if (!EnsureSource(src) || !sws_ctx_) {
    return false;
  }

  // Black background for the letterbox bars, then scale into the fit rect.
  std::memset(dst_y, 16, static_cast<size_t>(y_stride) * dst_height_);
  std::memset(dst_u, 128, static_cast<size_t>(u_stride) * (dst_height_ / 2));
  std::memset(dst_v, 128, static_cast<size_t>(v_stride) * (dst_height_ / 2));

  const uint8_t* src_slice[4] = { src.data.data(), nullptr, nullptr, nullptr };
  int src_stride[4] = { src.stride > 0 ? src.stride : src.width * 4, 0, 0, 0 };

  uint8_t* dst_slice[4] = {
      dst_y + static_cast<size_t>(fit_y_) * y_stride + fit_x_,
      dst_u + static_cast<size_t>(fit_y_ / 2) * u_stride + fit_x_ / 2,
      dst_v + static_cast<size_t>(fit_y_ / 2) * v_stride + fit_x_ / 2,
      nullptr,
  };
  int dst_strides[4] = { y_stride, u_stride, v_stride, 0 };

  sws_scale(sws_ctx_, src_slice, src_stride, 0, src_height_, dst_slice, dst_strides);
  return true;
}

bool GpuProcessor::ConvertBgraToNv12(const CapturedVideoFrame& src,
                                     uint8_t* dst_y, int y_stride,
                                     uint8_t* dst_uv, int uv_stride) {
  if (!dst_y || !dst_uv || y_stride <= 0 || uv_stride <= 0) {
    return false;
  }
  if (!EnsureSource(src) || !sws_ctx_nv12_) {
    return false;
  }

  // Phase 1.2: If source is already DMA-BUF NV12 (PipeWire zero-copy), avoid
  // the BGRA shadow copy entirely. Keep SW fallback when is_dmabuf but the
  // encoder cannot import the fd (handled in video_encoder.cc via hw_frames_ctx_).
  // Here we handle the case where the DMA-BUF was mapped to CPU NV12 (fallback
  // memcpy) or where the PipeWire MemFd path gave us BGRA.
  if (src.is_dmabuf) {
    // is_dmabuf with data empty means hardware import – caller should have used
    // the VAAPI hw_frames_ctx_ path (0 extra GPU copies). Fall back to black.
    if (src.data.empty() && src.dmabuf_fd >= 0) {
      // No BGRA shadow copy; this conversion is not needed – signal caller to use HW path.
      // For SW fallback we could mmap and convert, but we return false to let
      // video_encoder try SW BGRA path or direct DMA-BUF mapping.
      return false;
    }
    // If is_dmabuf but data contains already-NV12 CPU copy (via dmabuf mmap
    // fallback), use NV12->NV12 direct scaling with 0 extra copies.
    if (!src.data.empty() && src.dmabuf_stride > 0) {
      std::memset(dst_y, 16, static_cast<size_t>(y_stride) * dst_height_);
      std::memset(dst_uv, 128, static_cast<size_t>(uv_stride) * (dst_height_ / 2));
      const uint8_t* src_y = src.data.data() + src.dmabuf_offset_y;
      const uint8_t* src_uv = src.data.data() + src.dmabuf_offset_uv;
      const uint8_t* src_slice_nv12[4] = { src_y, src_uv, nullptr, nullptr };
      int src_stride_nv12[4] = { src.dmabuf_stride, src.dmabuf_stride, 0, 0 };
      uint8_t* dst_slice[4] = {
          dst_y + static_cast<size_t>(fit_y_) * y_stride + fit_x_,
          dst_uv + static_cast<size_t>(fit_y_ / 2) * uv_stride + fit_x_,
          nullptr,
          nullptr,
      };
      int dst_strides[4] = { y_stride, uv_stride, 0, 0 };
      if (sws_nv12_direct_) {
        sws_scale(sws_nv12_direct_, src_slice_nv12, src_stride_nv12, 0, src_height_, dst_slice, dst_strides);
        return true;
      }
    }
    // Fall through to BGRA path if dmabuf handling not applicable
  }

  std::memset(dst_y, 16, static_cast<size_t>(y_stride) * dst_height_);
  // Interleaved UV: both chroma components are neutral at 128, so one memset
  // covers the whole plane.
  std::memset(dst_uv, 128, static_cast<size_t>(uv_stride) * (dst_height_ / 2));

  const uint8_t* src_slice[4] = { src.data.data(), nullptr, nullptr, nullptr };
  int src_stride[4] = { src.stride > 0 ? src.stride : src.width * 4, 0, 0, 0 };

  // NV12 UV plane is one byte per chroma sample horizontally, two per pixel
  // pair: the byte offset equals the pixel offset x.
  uint8_t* dst_slice[4] = {
      dst_y + static_cast<size_t>(fit_y_) * y_stride + fit_x_,
      dst_uv + static_cast<size_t>(fit_y_ / 2) * uv_stride + fit_x_,
      nullptr,
      nullptr,
  };
  int dst_strides[4] = { y_stride, uv_stride, 0, 0 };

  sws_scale(sws_ctx_nv12_, src_slice, src_stride, 0, src_height_, dst_slice, dst_strides);
  return true;
}

bool GpuProcessor::ConvertBgraToYuv420p(const CapturedVideoFrame& src,
                                        std::vector<uint8_t>& dst_y,
                                        std::vector<uint8_t>& dst_u,
                                        std::vector<uint8_t>& dst_v,
                                        int& y_stride, int& u_stride, int& v_stride) {
  if (!EnsureSource(src)) {
    return false;
  }
  y_stride = dst_width_;
  u_stride = dst_width_ / 2;
  v_stride = dst_width_ / 2;

  dst_y.resize(static_cast<size_t>(y_stride) * dst_height_);
  dst_u.resize(static_cast<size_t>(u_stride) * (dst_height_ / 2));
  dst_v.resize(static_cast<size_t>(v_stride) * (dst_height_ / 2));

  return ConvertBgraToYuv420p(src, dst_y.data(), y_stride,
                              dst_u.data(), u_stride, dst_v.data(), v_stride);
}

} // namespace castcore
