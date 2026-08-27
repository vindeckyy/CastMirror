#ifndef CASTCORE_ADAPTIVE_CONTROLLER_H_
#define CASTCORE_ADAPTIVE_CONTROLLER_H_

#include "castcore/types.h"
#include "castcore/rtcp_parser.h"
#include <vector>
#include <chrono>
#include <atomic>
#include <mutex>
#include <unordered_set>


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

  void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool IsEnabled() const { return enabled_; }

  // User slider cap: adaptive may downshift but must not upshift above this.
  void SetBitrateCapKbps(uint32_t kbps);

  // Called on each RTCP feedback packet
  void OnFeedback(const RtcpFeedback& feedback);
  // Clear transient RTCP pressure after a new transport/reconnect while
  // preserving the selected ladder rung and user bitrate cap.
  void ResetFeedbackWindow();


  // Periodic evaluation (e.g. every 1 second). Bitrate-only; resolution/fps stay put.
  bool CheckAdaptation(StreamStats& out_updated_settings);

  int GetCurrentTargetDelayMs() const { return current_target_delay_ms_; }
  uint32_t GetCurrentBitrateKbps() const { return current_bitrate_kbps_; }
  int GetCurrentLadderIndex() const { return current_rung_idx_; }
  const std::vector<LadderRung>& GetLadder() const { return ladder_; }
  Resolution GetCurrentResolution() const { return current_resolution_; }
  int GetCurrentFramerate() const { return current_framerate_; }

 private:
  void BuildLadder();

  QualityPreset preset_ = QualityPreset::kAuto;
  std::vector<LadderRung> ladder_;
  int current_rung_idx_ = 0;
  uint32_t current_bitrate_kbps_ = 6000;
  int current_target_delay_ms_ = 200;
  Resolution current_resolution_{1920, 1080};
  int current_framerate_ = 60;
  int max_encode_width_ = 1920;
  int max_encode_height_ = 1080;
  int initial_framerate_ = 60;
  bool enabled_ = true;
  uint32_t user_bitrate_cap_kbps_ = 0;
  uint32_t stability_cap_kbps_ = 0;

  std::atomic<double> recent_loss_fraction_{0.0};
  std::atomic<double> recent_rtt_ms_{0.0};
  mutable std::mutex feedback_mutex_;
  std::unordered_set<uint64_t> recent_nack_keys_;
  std::atomic<bool> recent_pli_{false};
  std::atomic<bool> recent_feedback_{false};

  int consecutive_loss_events_ = 0;
  int consecutive_clean_seconds_ = 0;

  std::chrono::steady_clock::time_point last_eval_time_;
  std::chrono::steady_clock::time_point last_downshift_time_;
  std::chrono::steady_clock::time_point last_floor_warning_time_{};
};

} // namespace castcore

#endif // CASTCORE_ADAPTIVE_CONTROLLER_H_
