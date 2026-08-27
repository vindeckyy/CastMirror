#include "castcore/session_recovery.h"
#include "castcore/logger.h"

namespace castcore {

SessionRecovery::SessionRecovery(int max_timeout_seconds)
    : max_timeout_seconds_(max_timeout_seconds) {}

SessionRecovery::~SessionRecovery() = default;

void SessionRecovery::StartRecovery(const std::string& reason) {
  is_recovering_ = true;
  reason_ = reason;
  attempt_count_ = 0;
  recovery_start_time_ = std::chrono::steady_clock::now();
  LOG_WARN << "Starting Session Recovery (Max Timeout: " << max_timeout_seconds_
           << "s). Reason: " << reason;
}

void SessionRecovery::Reset() {
  if (is_recovering_) {
    LOG_INFO << "Session Recovery Succeeded (resolved after " << GetElapsedSeconds() << "s)";
  }
  is_recovering_ = false;
  attempt_count_ = 0;
}

bool SessionRecovery::HasTimedOut() const {
  if (!is_recovering_) return false;
  return (std::chrono::steady_clock::now() - recovery_start_time_) >=
         std::chrono::seconds(max_timeout_seconds_);
}

int SessionRecovery::GetElapsedSeconds() const {
  if (!is_recovering_) return 0;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - recovery_start_time_).count());
}

} // namespace castcore
