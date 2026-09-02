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
  static constexpr int kMinPlayoutDelayMs = 150;
  static constexpr int kMaxPlayoutDelayMs = 400;
  static constexpr int kDefaultPlayoutDelayMs = 200;

  AdaptiveController();
  ~AdaptiveController();

  void Initialize(const StreamStats& initial_target, QualityPreset preset);

  void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool IsEnabled() const { return enabled_; }
  void SetAllowResolutionChange(bool allow) { allow_resolution_change_ = allow; }
  bool AllowResolutionChange() const { return allow_resolution_change_; }

  // User slider cap: adaptive may downshift but must not upshift above this.
  // Also sets the custom hold target — the bitrate the controller holds on
  // a clean link and aggressively ramps back to after a congestion drop.
  void SetBitrateCapKbps(uint32_t kbps);

  // Reset the ladder rung to match a user-specified bitrate. Called when the
  // user manually changes the bitrate cap — the emergency downshift rung
  // should track the user's intent, not stay at whatever the last emergency
  // set it to.
  void ResetRungForBitrate(uint32_t bitrate_kbps);

  // Dynamic playout delay target
  void SetPlayoutDelayMs(int delay_ms);
  void SetTargetDelayMs(int delay_ms) { SetPlayoutDelayMs(delay_ms); }
  int GetPlayoutDelayMs() const { return current_target_delay_ms_; }
  int GetCurrentTargetDelayMs() const { return current_target_delay_ms_; }

  // Called on each RTCP feedback packet
  void OnFeedback(const RtcpFeedback& feedback);
  // Clear transient RTCP pressure after a new transport/reconnect while
  // preserving the selected ladder rung and user bitrate cap.
  void ResetFeedbackWindow();


  // Periodic evaluation (e.g. every 1 second). Bitrate-only; resolution/fps stay put.
  bool CheckAdaptation(StreamStats& out_updated_settings);

  uint32_t GetCurrentBitrateKbps() const { return current_bitrate_kbps_; }
  int GetCurrentLadderIndex() const { return current_rung_idx_; }
  const std::vector<LadderRung>& GetLadder() const { return ladder_; }
  Resolution GetCurrentResolution() const { return current_resolution_; }
  int GetCurrentFramerate() const { return current_framerate_; }
  double GetEwmaRttMs() const { return ewma_rtt_ms_; }
  double GetEwmaJitterMs() const { return ewma_jitter_ms_; }

  // Test helpers for fast simulation without long sleep
  void SetEvaluationIntervalMsForTest(int ms) { eval_interval_ms_ = ms; }
  void ResetEvalTimeForTest() {
    last_eval_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    last_downshift_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  }

 private:
  void BuildLadder();
  void StepUpPlayoutDelay();
  void StepDownPlayoutDelay();

  QualityPreset preset_ = QualityPreset::kAuto;
  std::vector<LadderRung> ladder_;
  int current_rung_idx_ = 0;
  uint32_t current_bitrate_kbps_ = 6000;
  int current_target_delay_ms_ = kDefaultPlayoutDelayMs;
  Resolution current_resolution_{1920, 1080};
  int current_framerate_ = 60;
  int max_encode_width_ = 1920;
  int max_encode_height_ = 1080;
  int initial_framerate_ = 60;
  bool enabled_ = true;
  bool allow_resolution_change_ = true;
  uint32_t user_bitrate_cap_kbps_ = 0;
  uint32_t custom_target_kbps_ = 0;   // user-selected bitrate held & ramped back to
  uint32_t stability_cap_kbps_ = 0;

  // Phase 2 EWMA smoothing: rtt 0.8/0.2 jitter 0.9/0.1
  static constexpr double kRttAlpha = 0.2;
  static constexpr double kRttKeep = 0.8;
  static constexpr double kJitterAlpha = 0.1;
  static constexpr double kJitterKeep = 0.9;
  std::atomic<double> recent_loss_fraction_{0.0};
  std::atomic<double> recent_rtt_ms_{0.0};
  std::atomic<double> recent_jitter_ms_{0.0};
  double ewma_rtt_ms_ = 0.0;
  double ewma_jitter_ms_ = 0.0;
  bool ewma_initialized_ = false;
  mutable std::mutex feedback_mutex_;
  std::unordered_set<uint64_t> recent_nack_keys_;
  std::atomic<bool> recent_pli_{false};
  std::atomic<bool> recent_feedback_{false};

  int consecutive_loss_events_ = 0;
  int consecutive_clean_seconds_ = 0;
  int consecutive_clean_delay_intervals_ = 0;
  int eval_interval_ms_ = 1000;

  std::chrono::steady_clock::time_point last_eval_time_;
  std::chrono::steady_clock::time_point last_downshift_time_;
  std::chrono::steady_clock::time_point last_floor_warning_time_{};
};

} // namespace castcore

#endif // CASTCORE_ADAPTIVE_CONTROLLER_H_
