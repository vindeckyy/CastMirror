#include "castcore/adaptive_controller.h"
#include "castcore/logger.h"
#include <algorithm>

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
  current_target_delay_ms_ = initial_target.target_delay_ms;
  current_bitrate_kbps_ = initial_target.bitrate_kbps;
  max_encode_width_ = initial_target.current_resolution.width;
  max_encode_height_ = initial_target.current_resolution.height;
  initial_framerate_ = initial_target.current_framerate > 0 ? initial_target.current_framerate : 60;
  user_bitrate_cap_kbps_ = initial_target.bitrate_kbps;
  stability_cap_kbps_ = 0;
  last_eval_time_ = std::chrono::steady_clock::now();
  last_downshift_time_ = last_eval_time_ - std::chrono::seconds(10);
  last_floor_warning_time_ = {};
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
  LOG_INFO << "Initialized Adaptive Controller at Ladder Rung " << current_rung_idx_
           << " (" << current_resolution_.width << "x"
           << current_resolution_.height << " @ "
           << current_framerate_ << "fps, "
           << ladder_[current_rung_idx_].bitrate_kbps << " kbps)";
}

void AdaptiveController::SetBitrateCapKbps(uint32_t kbps) {
  user_bitrate_cap_kbps_ = kbps;
  if (kbps > 0 && current_bitrate_kbps_ > kbps) {
    current_bitrate_kbps_ = kbps;
  }
}

void AdaptiveController::OnFeedback(const RtcpFeedback& feedback) {
  recent_loss_fraction_.store(feedback.fraction_lost);
  recent_rtt_ms_.store(feedback.rtt_ms);
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
  recent_pli_.store(false);
  recent_feedback_.store(false);
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    recent_nack_keys_.clear();
  }
  consecutive_loss_events_ = 0;
  consecutive_clean_seconds_ = 0;
  last_eval_time_ = std::chrono::steady_clock::now();
}

bool AdaptiveController::CheckAdaptation(StreamStats& out_updated_settings) {
  if (!enabled_) {
    return false;
  }

  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_eval_time_).count() < 1000) {
    return false;
  }
  last_eval_time_ = now;

  const bool received_feedback = recent_feedback_.exchange(false);
  if (!received_feedback) {
    return false;
  }
  const double recent_loss = recent_loss_fraction_.exchange(0.0);
  const double recent_rtt = recent_rtt_ms_.exchange(0.0);
  uint32_t recent_nacks = 0;
  {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    recent_nacks = static_cast<uint32_t>(recent_nack_keys_.size());
    recent_nack_keys_.clear();
  }
  const bool recent_pli = recent_pli_.exchange(false);


  uint32_t prev_bitrate = current_bitrate_kbps_;
  Resolution prev_res = current_resolution_;
  int prev_fps = current_framerate_;

  auto apply_caps = [this]() {
    uint32_t cap = user_bitrate_cap_kbps_;
    if (stability_cap_kbps_ > 0) {
      cap = cap > 0 ? std::min(cap, stability_cap_kbps_) : stability_cap_kbps_;
    }
    if (cap > 0 && current_bitrate_kbps_ > cap) {
      current_bitrate_kbps_ = cap;
    }
  };

  const bool nack_pressure = recent_nacks >= 4;
  const bool severe_feedback = recent_nacks >= 8 || recent_pli;
  if (recent_loss > 0.03 || recent_rtt > 120.0 || nack_pressure || recent_pli) {
    consecutive_loss_events_ = std::min(2, consecutive_loss_events_ + (severe_feedback ? 2 : 1));
    consecutive_clean_seconds_ = 0;

    const auto since_downshift =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_downshift_time_).count();
    const bool can_downshift = since_downshift >= 4;
    if (consecutive_loss_events_ >= 2 && can_downshift) {
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

    if (consecutive_clean_seconds_ >= 20) {
      stability_cap_kbps_ = 0;
    }

    auto time_since_downshift = std::chrono::duration_cast<std::chrono::seconds>(now - last_downshift_time_).count();
    if (consecutive_clean_seconds_ >= 20 && time_since_downshift >= 30 && current_rung_idx_ > 0) {
      if (preset_ == QualityPreset::kAuto || preset_ == QualityPreset::kHigh) {
        int next = current_rung_idx_ - 1;
        // Do not upshift past initial max dimensions
        if (ladder_[next].resolution.width <= max_encode_width_ &&
            ladder_[next].resolution.height <= max_encode_height_) {
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
  }

  // Derive target resolution and framerate from the active rung, clamped to initial max
  int target_w = std::min(ladder_[current_rung_idx_].resolution.width, max_encode_width_) & ~1;
  int target_h = std::min(ladder_[current_rung_idx_].resolution.height, max_encode_height_) & ~1;
  int target_fps = std::min(ladder_[current_rung_idx_].framerate, initial_framerate_);

  current_resolution_ = {target_w, target_h};
  current_framerate_ = target_fps;

  bool changed = (current_bitrate_kbps_ != prev_bitrate) ||
                 (current_resolution_ != prev_res) ||
                 (current_framerate_ != prev_fps);

  if (changed) {
    out_updated_settings.current_resolution = current_resolution_;
    out_updated_settings.current_framerate = current_framerate_;
    out_updated_settings.bitrate_kbps = current_bitrate_kbps_;
    out_updated_settings.target_delay_ms = current_target_delay_ms_;
  }

  return changed;
}
} // namespace castcore
