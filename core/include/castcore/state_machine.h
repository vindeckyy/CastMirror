#ifndef CASTCORE_STATE_MACHINE_H_
#define CASTCORE_STATE_MACHINE_H_

#include "castcore/types.h"
#include <mutex>
#include <vector>
#include <functional>

namespace castcore {

class StateMachine {
 public:
  using StateCallback = std::function<void(SessionState old_state, SessionState new_state, const std::string& message)>;

  StateMachine();
  ~StateMachine();

  SessionState GetState() const;
  std::string GetLastMessage() const;

  bool TransitionTo(SessionState new_state, const std::string& message = "");
  bool CanTransitionTo(SessionState new_state) const;

  void RegisterCallback(StateCallback callback);
  void Reset();

  bool IsActive() const {
    SessionState s = GetState();
    return s == SessionState::kConnecting || s == SessionState::kNegotiating ||
           s == SessionState::kStreaming || s == SessionState::kReconnecting;
  }

  bool IsStreaming() const {
    return GetState() == SessionState::kStreaming;
  }

  // Phase 0.5: assertion helper — verify IsActive() matches external capture state.
  // Call with display_capture->IsCapturing() (or synthetic equivalent).
  // Reports in Release as well as asserting in debug; see types.h.
  void AssertCaptureInvariant(bool capture_running,
                              const char* context = "StateMachine") const {
    ::castcore::CheckCaptureInvariant(IsActive(), capture_running, context);
  }

  // Convenience: check if transition would be valid without mutating state.
  bool CanTransitionVia(const SessionState from, SessionState to) const {
    return IsValidTransition(from, to);
  }

 private:
  bool IsValidTransition(SessionState from, SessionState to) const;

  mutable std::mutex mutex_;
  SessionState current_state_ = SessionState::kIdle;
  std::string last_message_;
  std::vector<StateCallback> callbacks_;
};

} // namespace castcore

#endif // CASTCORE_STATE_MACHINE_H_
