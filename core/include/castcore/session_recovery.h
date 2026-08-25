#ifndef CASTCORE_SESSION_RECOVERY_H_
#define CASTCORE_SESSION_RECOVERY_H_

#include "castcore/types.h"
#include <chrono>
#include <functional>

namespace castcore {

class SessionRecovery {
 public:
  using ReconnectAction = std::function<bool()>;

  SessionRecovery(int max_timeout_seconds = 30);
  ~SessionRecovery();

  void StartRecovery(const std::string& reason);
  void Reset();

  bool IsRecovering() const { return is_recovering_; }
  bool HasTimedOut() const;
  int GetElapsedSeconds() const;

  int GetAttemptCount() const { return attempt_count_; }
  void IncrementAttempt() { attempt_count_++; }

 private:
  bool is_recovering_ = false;
  int max_timeout_seconds_ = 30;
  int attempt_count_ = 0;
  std::string reason_;
  std::chrono::steady_clock::time_point recovery_start_time_;
};

} // namespace castcore

#endif // CASTCORE_SESSION_RECOVERY_H_
