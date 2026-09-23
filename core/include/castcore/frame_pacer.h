#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "castcore/types.h"

namespace castcore {

// Turns an irregular stream of captured frames into a fixed-cadence output
// stream: each tick emits the newest frame captured since the previous tick,
// and re-sends the previous frame when nothing new arrived.
//
// Windows desktop duplication only signals when the desktop changes, so an idle
// screen would otherwise starve the encoder (and trip the session's video-stall
// detector). Re-sending the last frame keeps the stream steady at the requested
// fps regardless of desktop activity.
//
// The clock is injected through the call sites (no internal time reads) so the
// cadence is deterministic under test.
class FramePacer {
 public:
  using Clock = std::chrono::steady_clock;

  struct Decision {
    bool emit = false;
    // Valid when emit is true. Borrowed from the pacer, which keeps the frame
    // alive until the next Tick/Submit: a re-send hands back the same pixels
    // instead of copying them, so callers that do not modify the frame pay no
    // buffer copy per tick.
    const CapturedVideoFrame* frame = nullptr;
    int crop_x = 0;
    int crop_y = 0;
    bool fresh = false;
  };

  FramePacer() : next_emit_(Clock::now()) {}

  void SetTargetFps(int fps) {
    if (fps > 0) target_fps_ = fps;
  }

  int TargetFps() const { return target_fps_; }

  void Reset(Clock::time_point now) {
    next_emit_ = now;
    have_pending_ = false;
    have_last_ = false;
    have_last_emit_ts_ = false;
    pending_ = CapturedVideoFrame{};
    last_ = CapturedVideoFrame{};
  }

  // Stages a freshly captured frame. Only the newest one is kept: a frame that
  // was never emitted is superseded by the next capture, never queued.
  void Submit(CapturedVideoFrame frame, int crop_x, int crop_y) {
    pending_ = std::move(frame);
    pending_crop_x_ = crop_x;
    pending_crop_y_ = crop_y;
    have_pending_ = true;
  }

  // Milliseconds the caller may spend waiting for new content before the next
  // tick is due; 0 when a tick is already due. Capped so a long stall still
  // wakes the loop to re-check capture state.
  int WaitSliceMs(Clock::time_point now) const {
    if (now >= next_emit_) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next_emit_ - now).count();
    return static_cast<int>(std::min<int64_t>(std::max<int64_t>(ms, 0), kMaxWaitMs));
  }

  // Deadline of the next cadence tick, for the loop's tail sleep.
  Clock::time_point NextTick() const { return next_emit_; }

  // True once a frame has been staged or emitted. The capture loop uses this to
  // seed a placeholder frame so a desktop that never changes still streams.
  bool HasFrame() const { return have_last_ || have_pending_; }

  // Advances the cadence and returns what to emit for this tick (nothing while
  // the first frame is still pending).
  Decision Tick(Clock::time_point now) {
    Decision decision;
    const auto interval = Interval();

    // Resync when the loop fell behind (stall, duplication recreate, minimized
    // window) or when the target fps was raised so the previous, longer
    // interval does not delay the next tick.
    if (now > next_emit_ + 4 * interval || now + 4 * interval < next_emit_) {
      next_emit_ = now;
    }
    if (now < next_emit_) return decision;

    if (have_pending_) {
      last_ = std::move(pending_);
      last_crop_x_ = pending_crop_x_;
      last_crop_y_ = pending_crop_y_;
      have_pending_ = false;
      have_last_ = true;
      decision.fresh = true;
    }

    if (have_last_) {
      if (decision.fresh) {
        // A present-time that did not advance (or moved backwards) would break
        // the receiver's A/V sync, so force it forward by the smallest step.
        if (have_last_emit_ts_ && last_.timestamp <= last_emit_ts_) {
          last_.timestamp = last_emit_ts_ + std::chrono::milliseconds(1);
        }
      } else {
        // Re-send: stamp the tick, but never behind the previous emit. A fresh
        // frame's present-time can sit past this deadline when the loop fell
        // behind, and a backwards RTP timestamp would stall the receiver.
        const auto floor =
            have_last_emit_ts_ ? last_emit_ts_ + std::chrono::milliseconds(1) : next_emit_;
        last_.timestamp = std::max(next_emit_, floor);
      }
      last_emit_ts_ = last_.timestamp;
      have_last_emit_ts_ = true;
      decision.emit = true;
      decision.frame = &last_;
      decision.crop_x = last_crop_x_;
      decision.crop_y = last_crop_y_;
    }

    next_emit_ += interval;
    return decision;
  }

 private:
  static constexpr int kMaxWaitMs = 100;

  std::chrono::microseconds Interval() const {
    return std::chrono::microseconds(1000000 / (target_fps_ > 0 ? target_fps_ : 60));
  }

  int target_fps_ = 60;
  Clock::time_point next_emit_;
  Clock::time_point last_emit_ts_{};
  bool have_last_emit_ts_ = false;

  CapturedVideoFrame pending_;
  int pending_crop_x_ = 0;
  int pending_crop_y_ = 0;
  bool have_pending_ = false;

  CapturedVideoFrame last_;
  int last_crop_x_ = 0;
  int last_crop_y_ = 0;
  bool have_last_ = false;
};

}  // namespace castcore
