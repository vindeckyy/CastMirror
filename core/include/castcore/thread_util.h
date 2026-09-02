#ifndef CASTCORE_THREAD_UTIL_H_
#define CASTCORE_THREAD_UTIL_H_

#include "castcore/logger.h"
#include <thread>
#include <chrono>

namespace castcore {

// Join a worker after Stop() has already interrupted it (CV notify, Pulse
// wakeup, UDP shutdown). If this is called from the worker itself, detach.
inline void JoinOrDetach(std::thread& t, int timeout_ms, const char* name) {
  if (!t.joinable()) {
    return;
  }
  if (std::this_thread::get_id() == t.get_id()) {
    LOG_ERROR << "Refusing to join " << name << " from itself; detaching";
    t.detach();
    return;
  }
  auto start = std::chrono::steady_clock::now();
  t.join();
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  if (elapsed_ms > static_cast<long>(timeout_ms)) {
    LOG_WARN << "JoinOrDetach \"" << name << "\" exceeded " << timeout_ms
             << "ms budget: " << elapsed_ms << "ms";
  } else if (elapsed_ms > 100) {
    LOG_INFO << "JoinOrDetach \"" << name << "\" took " << elapsed_ms << "ms";
  }
  (void)timeout_ms;
}

// Phase 0.5: measure elapsed time of a scope and warn if exceeds budget.
// Used to verify Stop <= 500ms contract (StopMediaPipeline >400ms triggers WARN).
struct StopBudgetTimer {
  explicit StopBudgetTimer(const char* label, long warn_threshold_ms = 400)
      : label_(label), warn_ms_(warn_threshold_ms), start_(std::chrono::steady_clock::now()) {}
  ~StopBudgetTimer() {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start_)
                       .count();
    if (elapsed > warn_ms_) {
      LOG_WARN << label_ << " exceeded " << warn_ms_ << "ms budget: " << elapsed << "ms (Stop contract <=500ms)";
    } else {
      LOG_INFO << label_ << " completed in " << elapsed << "ms";
    }
  }
  const char* label_;
  long warn_ms_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace castcore

#endif  // CASTCORE_THREAD_UTIL_H_
