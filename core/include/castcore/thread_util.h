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
  (void)timeout_ms;
  t.join();
}

}  // namespace castcore

#endif  // CASTCORE_THREAD_UTIL_H_
