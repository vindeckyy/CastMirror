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

// Several threads noticing the same drop must open exactly one recovery window.
TEST(SessionRecoveryTest, ConcurrentStartRecoveryOpensExactlyOneWindow) {
  SessionRecovery rec(30);
  std::atomic<int> winners{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      if (rec.StartRecovery("drop")) ++winners;
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_TRUE(rec.IsRecovering());
  rec.Reset();
  EXPECT_FALSE(rec.IsRecovering());
  EXPECT_TRUE(rec.StartRecovery("again"));
}