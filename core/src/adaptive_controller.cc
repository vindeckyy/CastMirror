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

  consecutive_loss_events_ = 0;
  consecutive_clean_seconds_ = 0;
  LOG_INFO << "Initialized Adaptive Controller at Ladder Rung " << current_rung_idx_
           << " (" << ladder_[current_rung_idx_].resolution.width << "x"
           << ladder_[current_rung_idx_].resolution.height << " @ "
           << ladder_[current_rung_idx_].framerate << "fps, "
           << ladder_[current_rung_idx_].bitrate_kbps << " kbps)";
}

void AdaptiveController::OnFeedback(const RtcpFeedback& feedback) {
  recent_loss_fraction_ = feedback.fraction_lost;
  recent_rtt_ms_ = feedback.rtt_ms;
}

bool AdaptiveController::CheckAdaptation(StreamStats& out_updated_settings) {
  auto now = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_eval_time_).count() < 1000) {
    return false;
  }
  last_eval_time_ = now;

  bool changed = false;

  if (recent_loss_fraction_ > 0.03 || recent_rtt_ms_ > 120.0) {
    consecutive_loss_events_++;
    consecutive_clean_seconds_ = 0;

    // Downshift if packet loss persists
    if (consecutive_loss_events_ >= 2 && current_rung_idx_ < static_cast<int>(ladder_.size()) - 1) {
      current_rung_idx_++;
      current_bitrate_kbps_ = ladder_[current_rung_idx_].bitrate_kbps;
      consecutive_loss_events_ = 0;
      last_downshift_time_ = now;
      changed = true;

      LOG_WARN << "Adaptive Downshift -> Rung " << current_rung_idx_
               << " (" << ladder_[current_rung_idx_].resolution.width << "x"
               << ladder_[current_rung_idx_].resolution.height << " @ "
               << ladder_[current_rung_idx_].framerate << "fps, "
               << current_bitrate_kbps_ << " kbps) due to loss: "
               << (recent_loss_fraction_ * 100.0) << "%";
    }
  } else {
    consecutive_loss_events_ = 0;
    consecutive_clean_seconds_++;

    // Upshift after 8 seconds of clean link (if not restricted by preset)
    auto time_since_downshift = std::chrono::duration_cast<std::chrono::seconds>(now - last_downshift_time_).count();
    if (consecutive_clean_seconds_ >= 8 && time_since_downshift >= 12 && current_rung_idx_ > 0) {
      if (preset_ == QualityPreset::kAuto || preset_ == QualityPreset::kHigh) {
        int next = current_rung_idx_ - 1;
        if (ladder_[next].resolution.width > max_encode_width_ ||
            ladder_[next].resolution.height > max_encode_height_) {
          consecutive_clean_seconds_ = 0;
        } else {
          current_rung_idx_--;
          current_bitrate_kbps_ = ladder_[current_rung_idx_].bitrate_kbps;
          consecutive_clean_seconds_ = 0;
          changed = true;

          LOG_INFO << "Adaptive Upshift -> Rung " << current_rung_idx_
                   << " (" << ladder_[current_rung_idx_].resolution.width << "x"
                   << ladder_[current_rung_idx_].resolution.height << " @ "
                   << ladder_[current_rung_idx_].framerate << "fps, "
                   << current_bitrate_kbps_ << " kbps)";
        }
      }
    }
  }

  if (changed) {
    out_updated_settings.current_resolution = ladder_[current_rung_idx_].resolution;
    out_updated_settings.current_framerate = ladder_[current_rung_idx_].framerate;
    out_updated_settings.bitrate_kbps = current_bitrate_kbps_;
    out_updated_settings.target_delay_ms = current_target_delay_ms_;
  }

  return changed;
}

} // namespace castcore
