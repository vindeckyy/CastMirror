#include <gtest/gtest.h>
#include "castcore/state_machine.h"

using namespace castcore;

TEST(StateMachineTest, InitialStateIsIdle) {
  StateMachine sm;
  EXPECT_EQ(sm.GetState(), SessionState::kIdle);
  EXPECT_FALSE(sm.IsActive());
  EXPECT_FALSE(sm.IsStreaming());
}

TEST(StateMachineTest, ValidTransitions) {
  StateMachine sm;
  EXPECT_TRUE(sm.TransitionTo(SessionState::kDiscovering));
  EXPECT_EQ(sm.GetState(), SessionState::kDiscovering);

  EXPECT_TRUE(sm.TransitionTo(SessionState::kReady));
  EXPECT_EQ(sm.GetState(), SessionState::kReady);

  EXPECT_TRUE(sm.TransitionTo(SessionState::kConnecting));
  EXPECT_EQ(sm.GetState(), SessionState::kConnecting);
  EXPECT_TRUE(sm.IsActive());

  EXPECT_TRUE(sm.TransitionTo(SessionState::kNegotiating));
  EXPECT_EQ(sm.GetState(), SessionState::kNegotiating);
  EXPECT_TRUE(sm.IsActive());

  EXPECT_TRUE(sm.TransitionTo(SessionState::kStreaming));
  EXPECT_EQ(sm.GetState(), SessionState::kStreaming);
  EXPECT_TRUE(sm.IsStreaming());

  EXPECT_TRUE(sm.TransitionTo(SessionState::kReconnecting));
  EXPECT_EQ(sm.GetState(), SessionState::kReconnecting);
  EXPECT_TRUE(sm.IsActive());

  EXPECT_TRUE(sm.TransitionTo(SessionState::kStreaming));
  EXPECT_EQ(sm.GetState(), SessionState::kStreaming);

  EXPECT_TRUE(sm.TransitionTo(SessionState::kStopping));
  EXPECT_EQ(sm.GetState(), SessionState::kStopping);

  EXPECT_TRUE(sm.TransitionTo(SessionState::kIdle));
  EXPECT_EQ(sm.GetState(), SessionState::kIdle);
}

TEST(StateMachineTest, RejectsInvalidTransitions) {
  StateMachine sm;
  // Cannot jump directly from Idle to Streaming
  EXPECT_FALSE(sm.TransitionTo(SessionState::kStreaming));
  EXPECT_EQ(sm.GetState(), SessionState::kIdle);

  // Cannot jump from Idle to Negotiating
  EXPECT_FALSE(sm.TransitionTo(SessionState::kNegotiating));
  EXPECT_EQ(sm.GetState(), SessionState::kIdle);
}

TEST(StateMachineTest, CanTransitionToFailedFromAnyState) {
  StateMachine sm;
  sm.TransitionTo(SessionState::kConnecting);
  EXPECT_TRUE(sm.TransitionTo(SessionState::kFailed, "Connection timed out"));
  EXPECT_EQ(sm.GetState(), SessionState::kFailed);
  EXPECT_EQ(sm.GetLastMessage(), "Connection timed out");

  // Can recover from Failed to Idle or Discovering
  EXPECT_TRUE(sm.TransitionTo(SessionState::kIdle));
  EXPECT_EQ(sm.GetState(), SessionState::kIdle);
}

TEST(StateMachineTest, FailedCanReconnect) {
  StateMachine sm;
  sm.TransitionTo(SessionState::kConnecting);
  EXPECT_TRUE(sm.TransitionTo(SessionState::kFailed, "lost"));
  EXPECT_TRUE(sm.TransitionTo(SessionState::kConnecting, "retry"));
  EXPECT_EQ(sm.GetState(), SessionState::kConnecting);
}

TEST(StateMachineTest, CallbackNotification) {
  StateMachine sm;
  SessionState observed_old = SessionState::kIdle;
  SessionState observed_new = SessionState::kIdle;
  std::string observed_msg;

  sm.RegisterCallback([&](SessionState old_s, SessionState new_s, const std::string& msg) {
    observed_old = old_s;
    observed_new = new_s;
    observed_msg = msg;
  });

  sm.TransitionTo(SessionState::kDiscovering, "Browsing LAN");
  EXPECT_EQ(observed_old, SessionState::kIdle);
  EXPECT_EQ(observed_new, SessionState::kDiscovering);
  EXPECT_EQ(observed_msg, "Browsing LAN");
}
