#include "castcore/state_machine.h"
#include "castcore/logger.h"

namespace castcore {

StateMachine::StateMachine() = default;
StateMachine::~StateMachine() = default;

SessionState StateMachine::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_state_;
}

std::string StateMachine::GetLastMessage() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_message_;
}

bool StateMachine::IsValidTransition(SessionState from, SessionState to) const {
  if (from == to) return true;

  // Anything can transition to Failed or Stopping (except Idle/Failed to Stopping)
  if (to == SessionState::kFailed) return true;
  if (to == SessionState::kStopping) {
    return from != SessionState::kIdle && from != SessionState::kFailed;
  }

  switch (from) {
    case SessionState::kIdle:
      return to == SessionState::kDiscovering || to == SessionState::kReady || to == SessionState::kConnecting;

    case SessionState::kDiscovering:
      return to == SessionState::kReady || to == SessionState::kIdle || to == SessionState::kConnecting;

    case SessionState::kReady:
      return to == SessionState::kConnecting || to == SessionState::kDiscovering || to == SessionState::kIdle;

    case SessionState::kConnecting:
      return to == SessionState::kNegotiating || to == SessionState::kStopping || to == SessionState::kIdle;

    case SessionState::kNegotiating:
      return to == SessionState::kStreaming || to == SessionState::kStopping || to == SessionState::kIdle;

    case SessionState::kStreaming:
      return to == SessionState::kReconnecting || to == SessionState::kStopping || to == SessionState::kIdle;

    case SessionState::kReconnecting:
      return to == SessionState::kStreaming || to == SessionState::kStopping || to == SessionState::kIdle;

    case SessionState::kStopping:
      return to == SessionState::kIdle;

    case SessionState::kFailed:
      return to == SessionState::kIdle || to == SessionState::kDiscovering || to == SessionState::kReady;
  }
  return false;
}

bool StateMachine::CanTransitionTo(SessionState new_state) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsValidTransition(current_state_, new_state);
}

bool StateMachine::TransitionTo(SessionState new_state, const std::string& message) {
  std::vector<StateCallback> callbacks_copy;
  SessionState old_state;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsValidTransition(current_state_, new_state)) {
      LOG_WARN << "Invalid state transition attempted: "
               << SessionStateToString(current_state_) << " -> "
               << SessionStateToString(new_state);
      return false;
    }

    old_state = current_state_;
    current_state_ = new_state;
    last_message_ = message;
    callbacks_copy = callbacks_;
  }

  LOG_INFO << "State Transition: " << SessionStateToString(old_state)
           << " -> " << SessionStateToString(new_state)
           << (message.empty() ? "" : (" (" + message + ")"));

  for (const auto& cb : callbacks_copy) {
    if (cb) {
      cb(old_state, new_state, message);
    }
  }

  return true;
}

void StateMachine::RegisterCallback(StateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.push_back(std::move(callback));
}

void StateMachine::Reset() {
  TransitionTo(SessionState::kIdle, "Reset");
}

} // namespace castcore
