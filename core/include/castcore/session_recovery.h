#ifndef CASTCORE_SESSION_RECOVERY_H_
#define CASTCORE_SESSION_RECOVERY_H_

#include "castcore/types.h"
#include <chrono>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace castcore {

class SessionRecovery {
 public:
  using ReconnectAction = std::function<bool()>;

  SessionRecovery(int max_timeout_seconds = 30);
  ~SessionRecovery();

  // Called from the channel, encode and adaptation threads, so every method
  // is safe to call concurrently.
  //
  // Begins a recovery window. Returns false, changing nothing, if one is
  // already open: two threads noticing the same drop must not both start it.
  bool StartRecovery(const std::string& reason);
  // How long a dropped connection is retried before the session gives up.
  void SetTimeoutSeconds(int seconds);
  void Reset();

  bool IsRecovering() const { return is_recovering_.load(); }
  bool HasTimedOut() const;
  int GetElapsedSeconds() const;

  int GetAttemptCount() const { return attempt_count_.load(); }
  void IncrementAttempt() { attempt_count_++; }

 private:
  mutable std::mutex mutex_;
  std::atomic<bool> is_recovering_{false};
  std::atomic<int> max_timeout_seconds_{30};
  std::atomic<int> attempt_count_{0};
  std::string reason_;
  std::chrono::steady_clock::time_point recovery_start_time_;
};

}  // namespace castcore

#endif  // CASTCORE_SESSION_RECOVERY_H_
