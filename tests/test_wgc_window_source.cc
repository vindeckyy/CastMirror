#include <gtest/gtest.h>

#include "castcore/wgc_window_source.h"
#include "castcore/win_time.h"

#include <chrono>

using namespace castcore;
using Clock = std::chrono::steady_clock::time_point;

namespace {

// Ticks on the QPC timeline, so the expectation is stated in source time rather
// than as a wall-clock read.
Clock FromQpc(int64_t ticks) { return QpcTicksToSteadyClock(static_cast<uint64_t>(ticks)); }

}  // namespace

// The rule under test: a window frame is stamped with its own content instant,
// and a frame that reports none repeats the previous content's instant rather
// than falling back to the wall clock. The wall clock runs about 20 ms ahead of
// the audio timeline, so a single fallback would skew every frame of the session.

TEST(WgcContentInstantTest, AFrameWithAnInstantUpdatesTheCarriedValue) {
  Clock prev{};
  bool have_prev = false;
  const Clock stamped = ResolveContentInstant(1000, &prev, &have_prev);

  EXPECT_TRUE(have_prev);
  EXPECT_EQ(stamped, FromQpc(1000));
  EXPECT_EQ(prev, FromQpc(1000));
}

TEST(WgcContentInstantTest, AFrameWithoutAnInstantReusesThePreviousOne) {
  Clock prev{};
  bool have_prev = false;
  ResolveContentInstant(1000, &prev, &have_prev);

  // 0 is what the frame reports when it has no content instant.
  const Clock stamped = ResolveContentInstant(0, &prev, &have_prev);

  EXPECT_EQ(stamped, FromQpc(1000)) << "a frame with no instant repeats the previous content";
  EXPECT_EQ(prev, FromQpc(1000)) << "the carried instant must not move";
}

TEST(WgcContentInstantTest, TheFirstFrameWithNoInstantFallsBackToNow) {
  Clock prev{};
  bool have_prev = false;

  const auto before = std::chrono::steady_clock::now();
  const Clock stamped = ResolveContentInstant(0, &prev, &have_prev);
  const auto after = std::chrono::steady_clock::now();

  EXPECT_FALSE(have_prev) << "nothing has been stamped from the source yet";
  EXPECT_GE(stamped, before);
  EXPECT_LE(stamped, after);
}

TEST(WgcContentInstantTest, ANegativeInstantIsTreatedAsAbsent) {
  Clock prev{};
  bool have_prev = false;
  ResolveContentInstant(1000, &prev, &have_prev);

  const Clock stamped = ResolveContentInstant(-1, &prev, &have_prev);

  EXPECT_EQ(stamped, FromQpc(1000));
}

// A run of frames that report no instant must not drift: the timeline holds the
// content instant rather than free-running on the wall clock.
TEST(WgcContentInstantTest, RepeatedFramesWithNoInstantDoNotDrift) {
  Clock prev{};
  bool have_prev = false;
  ResolveContentInstant(1000, &prev, &have_prev);

  for (int i = 0; i < 500; ++i) {
    EXPECT_EQ(ResolveContentInstant(0, &prev, &have_prev), FromQpc(1000)) << "frame " << i;
  }
}