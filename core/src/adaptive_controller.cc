#include "castcore/adaptive_controller.h"
#include "castcore/logger.h"
#include <algorithm>
#include <cmath>

namespace castcore {

AdaptiveController::AdaptiveController() {
  BuildLadder();
  last_eval_time_ = std::chrono::steady_clock::now();
  last_downshift_time_ = std::chrono::steady_clock::now();
}

AdaptiveController::~AdaptiveController() = default;

void AdaptiveController::BuildLadder() {
  ladder_ = {
    {{3840, 2160}, 60, 25000}, // Rung 0: 4K60
    {{3840, 2160}, 30, 16000}, // Rung 1: 4K30
    {{2560, 1440}, 60, 12000}, // Rung 2: 1440p60
    {{1920, 1080}, 60, 8000},  // Rung 3: 1080p60 (High)
    {{1920, 1080}, 30, 5000},  // Rung 4: 1080p30 (Balanced)
    {{1280, 720},  60, 3500},  // Rung 5: 720p60 (Smooth)
    {{1280, 720},  30, 2000},  // Rung 6: 720p30
    {{960,  540},  30, 1200}   // Rung 7: 540p30 (Floor)
  };
}

void AdaptiveController::Initialize(const StreamStats& initial_target, QualityPreset preset) {
  preset_ = preset;
  current_target_delay_ms_ = initial_target.target_delay_ms > 0
                                 ? std::clamp(initial_target.target_delay_ms, kMinPlayoutDelayMs, kMaxPlayoutDelayMs)
                                 : kDefaultPlayoutDelayMs;
  current_bitrate_kbps_ = initial_target.bitrate_kbps;
  max_encode_width_ = initial_target.current_resolution.width;
  max_encode_height_ = initial_target.current_resolution.height;
  initial_framerate_ = initial_target.current_framerate > 0 ? initial_target.current_framerate : 60;
  user_bitrate_cap_kbps_ = initial_target.bitrate_kbps;
  custom_target_kbps_ = initial_target.bitrate_kbps;
  stability_cap_kbps_ = 0;
  last_eval_time_ = std::chrono::steady_clock::now();
  last_downshift_time_ = last_eval_time_ - std::chrono::seconds(10);
  last_floor_warning_time_ = {};
  ewma_rtt_ms_ = 0.0;
  ewma_jitter_ms_ = 0.0;
  ewma_initialized_ = false;
  recent_jitter_ms_.store(0.0);
  ResetFeedbackWindow();


  // Find closest rung in ladder
  current_rung_idx_ = 3; // Default 1080p60
  for (size_t i = 0; i < ladder_.size(); ++i) {
    if (ladder_[i].resolution.width <= initial_target.current_resolution.width &&
        ladder_[i].framerate <= initial_target.current_framerate &&
        ladder_[i].bitrate_kbps <= initial_target.bitrate_kbps) {
      current_rung_idx_ = static_cast<int>(i);
      break;
    }
  }

  current_resolution_ = {
      std::min(ladder_[current_rung_idx_].resolution.width, max_encode_width_) & ~1,
      std::min(ladder_[current_rung_idx_].resolution.height, max_encode_height_) & ~1};
  current_framerate_ = std::min(ladder_[current_rung_idx_].framerate, initial_framerate_);

  consecutive_loss_events_ = 0;
  consecutive_clean_seconds_ = 0;
  consecutive_clean_delay_intervals_ = 0;
  LOG_INFO << "Initialized Adaptive Controller at Ladder Rung " << current_rung_idx_
           << " (" << current_resolution_.width << "x"
           << current_resolution_.height << " @ "
           << current_framerate_ << "fps, "
           << ladder_[current_rung_idx_].bitrate_kbps << " kbps, target_delay="
           << current_target_delay_ms_ << "ms)";
}

void AdaptiveController::SetBitrateCapKbps(uint32_t kbps) {
  user_bitrate_cap_kbps_ = kbps;
  // The user's bitrate cap is also the hold target: the controller holds
  // this value on a clean link and ramps back to it after a congestion drop.
  custom_target_kbps_ = kbps;
  if (kbps > 0 && current_bitrate_kbps_ > kbps) {
    current_bitrate_kbps_ = kbps;
  }
}

void AdaptiveController::ResetRungForBitrate(uint32_t bitrate_kbps) {
  // Find the highest rung whose bitrate does not exceed the user's target.
  // This ensures the emergency downshift starts from the right place when
  // the user manually changes the bitrate cap.
  int best = static_cast<int>(ladder_.size()) - 1;
  for (int i = 0; i < static_cast<int>(ladder_.size()); ++i) {
    if (ladder_[i].bitrate_kbps <= bitrate_kbps &&
        ladder_[i].resolution.width <= max_encode_width_ &&
        ladder_[i].resolution.height <= max_encode_height_ &&
        ladder_[i].framerate <= initial_framerate_) {
      best = i;
      break;
    }
  }
  current_rung_idx_ = best;
  current_bitrate_kbps_ = ladder_[best].bitrate_kbps;
  stability_cap_kbps_ = 0;
  consecutive_loss_events_ = 0;
  consecutive_clean_seconds_ = 0;
  last_downshift_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(10);
}

void AdaptiveController::SetPlayoutDelayMs(int delay_ms) {
  current_target_delay_ms_ = std::clamp(delay_ms, kMinPlayoutDelayMs, kMaxPlayoutDelayMs);
}

void AdaptiveController::StepUpPlayoutDelay() {
  if (current_target_delay_ms_ < 200) {
    current_target_delay_ms_ = 200;
  } else if (current_target_delay_ms_ < 300) {
    current_target_delay_ms_ = 300;
  } else if (current_target_delay_ms_ < 400) {
    current_target_delay_ms_ = 400;
  } else {
    current_target_delay_ms_ = 400;
  }
}

void AdaptiveController::StepDownPlayoutDelay() {
  if (current_target_delay_ms_ > 300) {
    current_target_delay_ms_ = 300;
  } else if (current_target_delay_ms_ > 200) {
    current_target_delay_ms_ = 200;
  } else if (current_target_delay_ms_ > 150) {
    current_target_delay_ms_ = 150;
  } else {
    current_target_delay_ms_ = 150;
  }
}

void AdaptiveController::OnFeedback(const RtcpFeedback& feedback) {
  // Phase 2 EWMA smoothing: rtt 0.8/0.2 jitter 0.9/0.1
  double sample_rtt = feedback.rtt_ms;
  double sample_jitter = static_cast<double>(feedback.jitter);
  // If RTCP jitter field is zero, estimate jitter as |sample - ewma|
  double prev_rtt = ewma_rtt_ms_;
  if (!ewma_initialized_) {
    ewma_rtt_ms_ = sample_rtt;
    ewma_jitter_ms_ = sample_jitter;
    ewma_initialized_ = true;
  } else {
    ewma_rtt_ms_ = kRttKeep * ewma_rtt_ms_ + kRttAlpha * sample_rtt;
    double rtt_diff = std::abs(sample_rtt - prev_rtt);
    // Prefer reported jitter if present, else use rtt_diff
    double jitter_sample = sample_jitter > 0 ? sample_jitter : rtt_diff;
    ewma_jitter_ms_ = kJitterKeep * ewma_jitter_ms_ + kJitterAlpha * jitter_sample;
  }
  recent_loss_fraction_.store(feedback.fraction_lost);
  recent_rtt_ms_.store(ewma_rtt_ms_);
  recent_jitter_ms_.store(ewma_jitter_ms_);
  if (!feedback.nacks.empty()) {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    for (const auto& nack : feedback.nacks) {
      const uint64_t key = (static_cast<uint64_t>(nack.frame_id) << 16) |
                           static_cast<uint64_t>(nack.packet_id);
      recent_nack_keys_.insert(key);
    }
  }
  if (feedback.picture_loss_indicator) {
    recent_pli_.store(true);
  }
  recent_feedback_.store(true);
}

void AdaptiveController::ResetFeedbackWindow() {
  recent_loss_fraction_.store(0.0);
  recent_rtt_ms_.store(0.0);
  recent_jitter_ms_.store(0.0);
  ewma_rtt_ms_ = 0.0;
  ewma_jitter_ms_ = 0.0;
  ewma_initialized_ = false;
  recent_pli_.store(false);
  recent_feedback_.store(false);
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    recent_nack_keys_.clear();
  }
  consecutive_loss_events_ = 0;
  consecutive_clean_seconds_ = 0;
  consecutive_clean_delay_intervals_ = 0;
  last_eval_time_ = std::chrono::steady_clock::now();
}

bool AdaptiveController::CheckAdaptation(StreamStats& out_updated_settings) {
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_eval_time_).count() < eval_interval_ms_) {
    return false;
  }
  last_eval_time_ = now;

  // Even when adaptive is disabled, we must detect severe congestion and
  // emergency-downshift to prevent the receiver from closing the connection.
  // The user's "adaptive disabled" intent is "don't change my quality", but
  // nobody wants the session to crash from congestion.
  const bool received_feedback = recent_feedback_.exchange(false);
  if (!received_feedback) {
    return false;
  }
  const double recent_loss = recent_loss_fraction_.exchange(0.0);
  const double recent_rtt = recent_rtt_ms_.exchange(0.0);
  const double recent_jitter = recent_jitter_ms_.exchange(0.0);
  uint32_t recent_nacks = 0;
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    recent_nacks = static_cast<uint32_t>(recent_nack_keys_.size());
    recent_nack_keys_.clear();
  }
  const bool recent_pli = recent_pli_.exchange(false);

  // Emergency downshift: severe congestion, even with adaptive disabled.
  // This prevents receiver-side disconnects when the network can't handle the
  // configured bitrate. Require actual packet loss OR a very high NACK burst
  // — NACKs alone with 0% loss means retransmissions are working and the
  // network is recovering, so don't panic.
  if (!enabled_) {
    // Cooldown: don't cascade more than 1 rung per 3 seconds
    const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_downshift_time_).count();
    const bool emergency_cooldown = since_last >= 3000;

    const bool severe = (recent_loss > 0.10) || recent_pli ||
                        (recent_nacks >= 50 && recent_loss > 0.03);
    if (severe && emergency_cooldown && current_rung_idx_ < static_cast<int>(ladder_.size()) - 1) {
      current_rung_idx_++;
      current_bitrate_kbps_ = ladder_[current_rung_idx_].bitrate_kbps;
      last_downshift_time_ = now;
      LOG_WARN << "Emergency bitrate downshift -> " << current_bitrate_kbps_
               << " kbps (rung " << current_rung_idx_
               << ") — adaptive disabled but severe congestion detected"
               << " (unique_nacks=" << recent_nacks
               << ", loss=" << (recent_loss * 100.0) << "%)";
      out_updated_settings.bitrate_kbps = current_bitrate_kbps_;
      out_updated_settings.current_resolution = current_resolution_;
      out_updated_settings.current_framerate = current_framerate_;
      out_updated_settings.target_delay_ms = current_target_delay_ms_;
      return true;
    }
    return false;
  }

  uint32_t prev_bitrate = current_bitrate_kbps_;
  Resolution prev_res = current_resolution_;
  int prev_fps = current_framerate_;
  int prev_target_delay = current_target_delay_ms_;

  auto apply_caps = [this]() {
    uint32_t cap = user_bitrate_cap_kbps_;
    if (stability_cap_kbps_ > 0) {
      cap = cap > 0 ? std::min(cap, stability_cap_kbps_) : stability_cap_kbps_;
    }
    if (cap > 0 && current_bitrate_kbps_ > cap) {
      current_bitrate_kbps_ = cap;
    }
  };

  // Phase 2: jitter-aware thresholds (if jitter >30ms, be less sensitive to NACK bursts due to reordering)
  // NACKs alone with 0% loss are normal — retransmissions are working.
  // Only treat as pressure when there's actual loss OR a very large NACK burst
  // (50+ unique NACKs in one window indicates severe congestion even if
  // retransmissions are currently succeeding).
  int nack_pressure_thresh = recent_jitter > 30.0 ? 6 : 4;
  int severe_thresh = recent_jitter > 30.0 ? 10 : 8;
  const bool nack_pressure = (recent_loss > 0.01 && recent_nacks >= static_cast<uint32_t>(nack_pressure_thresh)) ||
                             recent_nacks >= 50;
  const bool severe_feedback = (recent_loss > 0.03 && recent_nacks >= static_cast<uint32_t>(severe_thresh)) ||
                               recent_nacks >= 50 || recent_pli;

  const auto since_downshift =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_downshift_time_).count();
  const bool can_downshift = since_downshift >= (4 * eval_interval_ms_);

  // Playout delay adaptation:
  // Step up delay: 150ms -> 200ms -> 300ms -> 400ms when EWMA_Jitter > 30ms or loss_fraction > 0.03 (or NACK burst).
  const bool delay_degraded = (ewma_jitter_ms_ > 30.0 || recent_loss > 0.03 || nack_pressure || recent_pli);
  if (delay_degraded) {
    if (can_downshift) {
      StepUpPlayoutDelay();
    }
    consecutive_clean_delay_intervals_ = 0;
  } else {
    // Step down delay: 400ms -> 300ms -> 200ms -> 150ms when network is clean for >= 15 consecutive evaluation intervals (loss == 0, RTT < 25ms, Jitter < 8ms).
    const bool clean_for_delay = (recent_loss <= 0.0001 && recent_nacks == 0 && !recent_pli &&
                                  ewma_rtt_ms_ < 25.0 && ewma_jitter_ms_ < 8.0);
    if (clean_for_delay) {
      consecutive_clean_delay_intervals_++;
      if (consecutive_clean_delay_intervals_ >= 15) {
        StepDownPlayoutDelay();
        consecutive_clean_delay_intervals_ = 0;
      }
    } else {
      consecutive_clean_delay_intervals_ = 0;
    }
  }

  if (recent_loss > 0.03 || recent_rtt > 120.0 || nack_pressure || recent_pli) {
    consecutive_loss_events_ = std::min(2, consecutive_loss_events_ + (severe_feedback ? 2 : 1));
    consecutive_clean_seconds_ = 0;

    // Immediate emergency downshift on severe congestion — don't wait for
    // 2 consecutive loss events. But require actual loss OR very high NACKs
    // with some loss, and respect a 3s cooldown to prevent cascade.
    const auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_downshift_time_).count();
    const bool emergency = (recent_loss > 0.15) ||
                           (recent_nacks >= 50 && recent_loss > 0.03);
    if (emergency && since_last >= 3000 &&
        current_rung_idx_ < static_cast<int>(ladder_.size()) - 1) {
      current_rung_idx_++;
      current_bitrate_kbps_ = ladder_[current_rung_idx_].bitrate_kbps;
      last_downshift_time_ = now;
      apply_caps();
      consecutive_loss_events_ = 0;
      LOG_WARN << "Emergency bitrate downshift -> " << current_bitrate_kbps_
               << " kbps (rung " << current_rung_idx_
               << ") due to severe feedback: unique_nacks=" << recent_nacks
               << ", loss=" << (recent_loss * 100.0) << "%";
    } else if (consecutive_loss_events_ >= 2 && can_downshift) {
      if (max_encode_width_ <= 1920 && max_encode_height_ <= 1080 &&
          (stability_cap_kbps_ == 0 || stability_cap_kbps_ > 8000)) {
        stability_cap_kbps_ = 8000;
      }

      if (current_rung_idx_ < static_cast<int>(ladder_.size()) - 1) {
        current_rung_idx_++;
        current_bitrate_kbps_ = ladder_[current_rung_idx_].bitrate_kbps;
        last_downshift_time_ = now;
        apply_caps();
        consecutive_loss_events_ = 0;

        LOG_WARN << "Adaptive bitrate downshift -> " << current_bitrate_kbps_
                 << " kbps (rung " << current_rung_idx_ << ") due to feedback: loss="
                 << (recent_loss * 100.0) << "%, rtt=" << recent_rtt
                 << "ms, unique_nacks=" << recent_nacks
                 << ", pli=" << (recent_pli ? "yes" : "no");
      } else if (last_floor_warning_time_.time_since_epoch().count() == 0 ||
                 now - last_floor_warning_time_ >= std::chrono::seconds(10)) {
        last_floor_warning_time_ = now;
        consecutive_loss_events_ = 0;
        LOG_WARN << "Adaptive quality is already at the floor; feedback remains degraded"
                 << " (unique_nacks=" << recent_nacks
                 << ", loss=" << (recent_loss * 100.0) << "%)";
      }
    }

  } else {
    consecutive_loss_events_ = 0;
    consecutive_clean_seconds_++;

    if (consecutive_clean_seconds_ >= 10) {
      stability_cap_kbps_ = 0;
    }

    // Aggressive bitrate ramp-back to the user's custom target.
    // After 3 clean seconds, step bitrate up by ~50% of the remaining gap
    // to custom_target_kbps_ each evaluation (1 s). This reaches an 8 Mbps
    // target from 4 Mbps in ~5-8 s without waiting for the slow resolution
    // upshift cadence. Bitrate-only recovery avoids keyframe storms.
    if (custom_target_kbps_ > 0 && consecutive_clean_seconds_ >= 3 &&
        current_bitrate_kbps_ < custom_target_kbps_) {
      uint32_t gap = custom_target_kbps_ - current_bitrate_kbps_;
      uint32_t step = std::max<uint32_t>(500, gap / 2);
      uint32_t before = current_bitrate_kbps_;
      current_bitrate_kbps_ = std::min(custom_target_kbps_, current_bitrate_kbps_ + step);
      apply_caps();
      consecutive_clean_seconds_ = 0;
      LOG_INFO << "Aggressive bitrate ramp-up -> " << current_bitrate_kbps_
               << " kbps (from " << before << ", target " << custom_target_kbps_ << ")";
    }

    // Resolution/fps upshift stays on the slow cadence (10 s clean + 15 s
    // since last downshift) and only fires once bitrate has ramped all the
    // way back to the user's target. This prevents keyframe storms while
    // bitrate is still recovering.
    auto time_since_downshift = std::chrono::duration_cast<std::chrono::seconds>(now - last_downshift_time_).count();
    const bool bitrate_recovered = (custom_target_kbps_ == 0) ||
                                   (current_bitrate_kbps_ >= custom_target_kbps_);
    if (bitrate_recovered && consecutive_clean_seconds_ >= 10 &&
        time_since_downshift >= 15 && current_rung_idx_ > 0) {
      int next = current_rung_idx_ - 1;
      // Do not upshift past initial max dimensions
      if (ladder_[next].resolution.width <= max_encode_width_ &&
          ladder_[next].resolution.height <= max_encode_height_ &&
          ladder_[next].framerate <= initial_framerate_) {
        uint32_t next_br = ladder_[next].bitrate_kbps;
        if (user_bitrate_cap_kbps_ > 0 && next_br > user_bitrate_cap_kbps_) {
          consecutive_clean_seconds_ = 0;
        } else {
          current_rung_idx_ = next;
          current_bitrate_kbps_ = next_br;
          apply_caps();
          consecutive_clean_seconds_ = 0;

          LOG_INFO << "Adaptive bitrate upshift -> " << current_bitrate_kbps_
                   << " kbps (rung " << current_rung_idx_ << ")";
        }
      } else {
        // Rung is larger than initial maximum encode size; stop upshifting here.
        consecutive_clean_seconds_ = 0;
      }
    }
  }

  // Derive target resolution and framerate from the active rung, clamped to initial max
  // Phase 3.2: when resolution steps disabled, keep initial resolution/fps and only vary bitrate
  int target_w, target_h, target_fps;
  if (allow_resolution_change_) {
    target_w = std::min(ladder_[current_rung_idx_].resolution.width, max_encode_width_) & ~1;
    target_h = std::min(ladder_[current_rung_idx_].resolution.height, max_encode_height_) & ~1;
    target_fps = std::min(ladder_[current_rung_idx_].framerate, initial_framerate_);
  } else {
    target_w = prev_res.width;
    target_h = prev_res.height;
    target_fps = prev_fps;
  }

  current_resolution_ = {target_w, target_h};
  current_framerate_ = target_fps;

  bool changed = (current_bitrate_kbps_ != prev_bitrate) ||
                 (allow_resolution_change_ && (current_resolution_ != prev_res)) ||
                 (allow_resolution_change_ && (current_framerate_ != prev_fps)) ||
                 (current_target_delay_ms_ != prev_target_delay);

  if (changed) {
    out_updated_settings.current_resolution = current_resolution_;
    out_updated_settings.current_framerate = current_framerate_;
    out_updated_settings.bitrate_kbps = current_bitrate_kbps_;
    out_updated_settings.target_delay_ms = current_target_delay_ms_;
  }

  return changed;
}
} // namespace castcore
