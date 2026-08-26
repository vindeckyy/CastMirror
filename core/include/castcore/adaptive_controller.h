#ifndef CASTCORE_ADAPTIVE_CONTROLLER_H_
#define CASTCORE_ADAPTIVE_CONTROLLER_H_

#include "castcore/types.h"
#include "castcore/rtcp_parser.h"
#include <vector>
#include <chrono>

namespace castcore {

struct LadderRung {
  Resolution resolution;
  int framerate = 60;
  uint32_t bitrate_kbps = 6000;
};

class AdaptiveController {
 public:
  AdaptiveController();
  ~AdaptiveController();

  void Initialize(const StreamStats& initial_target, QualityPreset preset);

  // Called on each RTCP feedback packet
  void OnFeedback(const RtcpFeedback& feedback);

  // Periodic evaluation (e.g. every 1 second)
  bool CheckAdaptation(StreamStats& out_updated_settings);

  int GetCurrentTargetDelayMs() const { return current_target_delay_ms_; }
  uint32_t GetCurrentBitrateKbps() const { return current_bitrate_kbps_; }
  int GetCurrentLadderIndex() const { return current_rung_idx_; }

 private:
  void BuildLadder();

  QualityPreset preset_ = QualityPreset::kAuto;
  std::vector<LadderRung> ladder_;
  int current_rung_idx_ = 0;
  uint32_t current_bitrate_kbps_ = 6000;
  int current_target_delay_ms_ = 200;
  int max_encode_width_ = 1920;
  int max_encode_height_ = 1080;

  double recent_loss_fraction_ = 0.0;
  double recent_rtt_ms_ = 0.0;
  int consecutive_loss_events_ = 0;
  int consecutive_clean_seconds_ = 0;

  std::chrono::steady_clock::time_point last_eval_time_;
  std::chrono::steady_clock::time_point last_downshift_time_;
};

} // namespace castcore

#endif // CASTCORE_ADAPTIVE_CONTROLLER_H_
