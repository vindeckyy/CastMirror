#ifndef CASTCORE_MEDIA_TIMELINE_H_
#define CASTCORE_MEDIA_TIMELINE_H_

#include <algorithm>
#include <chrono>

namespace castcore {

// Produces one timestamp per frame from a single measured anchor and a fixed
// step. Capture jitter, silence insertion and scheduling delays cannot move the
// resulting timeline backwards or change its rate. The caller owns the clock
// reads; this class never calls now().
class MediaTimeline {
 public:
  using Clock = std::chrono::steady_clock;

  void Reset(Clock::time_point anchor, std::chrono::microseconds step) {
    anchor_ = anchor;
    step_ = step.count() > 0 ? step : std::chrono::microseconds(10000);
    next_index_ = 0;
    valid_ = true;
  }

  void Invalidate() {
    anchor_ = Clock::time_point{};
    step_ = std::chrono::microseconds(10000);
    next_index_ = 0;
    valid_ = false;
  }

  bool valid() const { return valid_; }

  Clock::time_point Next() {
    if (!valid_) return Clock::time_point{};
    const auto ts = anchor_ + std::chrono::microseconds(next_index_ * step_.count());
    ++next_index_;
    return ts;
  }

  Clock::time_point Peek() const {
    if (!valid_) return Clock::time_point{};
    return anchor_ + std::chrono::microseconds(next_index_ * step_.count());
  }

  int64_t Index() const { return next_index_; }

  // Number of frame slots whose content instant is at or before now. Index 0
  // is due at the anchor; a time before the anchor has nothing due.
  int64_t Due(Clock::time_point now) const {
    if (!valid_) return 0;
    const int64_t delta_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - anchor_).count();
    if (delta_us < 0) return 0;
    return delta_us / step_.count() + 1;
  }

  // Emission policy shared with the PulseAudio capture loop. Keeping this here
  // lets the long-run drift test exercise the exact production decision.
  int64_t FramesToEmit(Clock::time_point now, int64_t max_catchup) const {
    return std::min<int64_t>(std::max<int64_t>(Due(now) - Index(), 0), max_catchup);
  }

  bool NeedsReanchor(Clock::time_point now, int64_t threshold_frames) const {
    return Due(now) - Index() > threshold_frames;
  }

 private:
  Clock::time_point anchor_{};
  std::chrono::microseconds step_{10000};
  int64_t next_index_ = 0;
  bool valid_ = false;
};

}  // namespace castcore

#endif  // CASTCORE_MEDIA_TIMELINE_H_
