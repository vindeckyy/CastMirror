#include <gtest/gtest.h>
#include "castcore/session_recovery.h"
#include <thread>
#include <chrono>

using namespace castcore;

TEST(SessionRecoveryTest, InitialStateNotRecovering) {
  SessionRecovery rec(30);
  EXPECT_FALSE(rec.IsRecovering());
  EXPECT_FALSE(rec.HasTimedOut());
  EXPECT_EQ(rec.GetElapsedSeconds(), 0);
  EXPECT_EQ(rec.GetAttemptCount(), 0);
}

TEST(SessionRecoveryTest, TimesOutAfterLimit) {
  SessionRecovery rec(1);
  rec.StartRecovery("Network drop");
  EXPECT_TRUE(rec.IsRecovering());
  EXPECT_EQ(rec.GetAttemptCount(), 0);

  rec.IncrementAttempt();
  EXPECT_EQ(rec.GetAttemptCount(), 1);

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_TRUE(rec.HasTimedOut());

  rec.Reset();
  EXPECT_FALSE(rec.IsRecovering());
  EXPECT_EQ(rec.GetAttemptCount(), 0);
}
