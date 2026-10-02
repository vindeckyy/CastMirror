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
    last_emit_now_ = now;
    last_emit_ts_ = Clock::time_point{};
    have_pending_ = false;
    have_last_ = false;
    pending_ = CapturedVideoFrame{};
    last_ = CapturedVideoFrame{};
    // A reset invalidates every resolution the spare was sized for; keeping it
    // would hand the producer a buffer that no longer matches the new mode.
    spare_.clear();
  }

  // Stages a freshly captured frame. Only the newest one is kept: a frame that
  // was never emitted is superseded by the next capture, never queued.
  void Submit(CapturedVideoFrame frame, int crop_x, int crop_y) {
    pending_ = std::move(frame);
    pending_crop_x_ = crop_x;
    pending_crop_y_ = crop_y;
    have_pending_ = true;
  }

  // Hands back a pixel buffer from a frame that has already been emitted, so
  // the producer can refill it instead of allocating and zero-filling a full
  // frame on every capture. Empty until a frame has been superseded, and empty
  // again once handed out, so the caller falls back to its own allocation.
  //
  // The steady state is two buffers alternating: the capture loop takes the
  // retired buffer, refills it, and the next tick retires the frame it replaces.
  std::vector<uint8_t> TakeSupersededBuffer() { return std::move(spare_); }

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
      // The frame this tick replaces has already been emitted, so its pixels are
      // dead and become the producer's next reusable buffer. Retiring it here
      // (not in Submit) is what makes the steady state work: Tick consumes
      // pending_ before the next capture arrives, so by the time Submit runs
      // there is nothing left staged to reuse.
      if (have_last_ && spare_.size() < last_.data.size()) {
        spare_.swap(last_.data);
      }
      last_ = std::move(pending_);
      last_crop_x_ = pending_crop_x_;
      last_crop_y_ = pending_crop_y_;
      have_pending_ = false;
      have_last_ = true;
      decision.fresh = true;
    }

    if (have_last_) {
      // The stamp is taken from the source's own present time when it leads the
      // timeline, and otherwise stepped by the time that actually elapsed since
      // the last emit. Both branches step by elapsed time, never by a fixed
      // amount, so the video clock keeps real-time rate on every path.
      //
      // Fresh frames carry the present time DXGI reports, which is behind wall
      // clock by the present lag; a re-send carries no new present time at all.
      // Stamping either from the tick deadline (wall clock) mixed the two
      // timelines and stepped video ahead of audio by that lag, which the
      // receiver — playing by RTP timestamp — showed late for the whole session.
      const bool ahead_of_source = last_.timestamp > last_emit_ts_;
      if (!decision.fresh || !ahead_of_source) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(now - last_emit_now_);
        // Floor at 1 ms (strictly increasing), not at the nominal interval:
        // ticks land late then early, and flooring at the interval counts every
        // early tick at full length, so the timeline ratchets ahead of wall clock.
        last_.timestamp = last_emit_ts_ + std::max(elapsed, std::chrono::microseconds(1000));
      }
      last_emit_ts_ = last_.timestamp;
      last_emit_now_ = now;
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
  // Default-constructed, and Reset() restores it: a fresh capture timestamp is
  // always past the epoch, so the first frame takes the source branch with no
  // separate "have we emitted yet" flag to fall out of sync with this member.
  Clock::time_point last_emit_ts_{};
  // Wall clock of the last emit, so a re-send can step the source timeline by
  // the time that actually elapsed rather than by a nominal interval.
  Clock::time_point last_emit_now_{};

  CapturedVideoFrame pending_;
  int pending_crop_x_ = 0;
  int pending_crop_y_ = 0;
  bool have_pending_ = false;
  // A retired buffer kept for the producer to refill. Deliberately not a pool:
  // one spare is enough to reach a steady state of two alternating buffers,
  // because Submit() always retires the frame it replaces.
  std::vector<uint8_t> spare_;

  CapturedVideoFrame last_;
  int last_crop_x_ = 0;
  int last_crop_y_ = 0;
  bool have_last_ = false;
};

}  // namespace castcore
