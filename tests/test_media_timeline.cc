#include <gtest/gtest.h>

#include "castcore/media_timeline.h"

#include <chrono>

namespace {

using Clock = castcore::MediaTimeline::Clock;
using castcore::MediaTimeline;
using ms = std::chrono::milliseconds;
using us = std::chrono::microseconds;

Clock::time_point T0() {
  return Clock::time_point{} + ms(1000);
}

}  // namespace

TEST(MediaTimelineTest, NextStepsExactlyByFrameDuration) {
  const auto t0 = T0();
  MediaTimeline timeline;
  timeline.Reset(t0, us(10000));

  for (int k = 0; k < 10000; ++k) {
    EXPECT_EQ(timeline.Next(), t0 + us(10000 * k));
    EXPECT_EQ(timeline.Index(), k + 1);
  }
}

TEST(MediaTimelineTest, DueCountsPassedFrames) {
  const auto t0 = T0();
  MediaTimeline timeline;
  timeline.Reset(t0, us(10000));

  EXPECT_EQ(timeline.Due(t0 - us(1)), 0);
  EXPECT_EQ(timeline.Due(t0), 1);
  EXPECT_EQ(timeline.Due(t0 + us(9999)), 1);
  EXPECT_EQ(timeline.Due(t0 + us(10000)), 2);
  EXPECT_EQ(timeline.Due(t0 + us(25000)), 3);
}

TEST(MediaTimelineTest, InvalidTimelineIsInertUntilReset) {
  MediaTimeline timeline;
  EXPECT_FALSE(timeline.valid());
  EXPECT_EQ(timeline.Next(), Clock::time_point{});
  EXPECT_EQ(timeline.Peek(), Clock::time_point{});
  EXPECT_EQ(timeline.Due(Clock::now()), 0);
  EXPECT_EQ(timeline.Index(), 0);

  timeline.Next();
  EXPECT_EQ(timeline.Index(), 0);

  const auto t0 = T0();
  timeline.Reset(t0, us(10000));
  timeline.Next();
  timeline.Invalidate();
  EXPECT_EQ(timeline.Index(), 0);
  EXPECT_EQ(timeline.Next(), Clock::time_point{});
}

// The Opus encoder derives RTP time as (timestamp - origin) * 48000 / 1e6.
// A 10 ms step lands exactly on 480 ticks, so an hour cannot accumulate a
// rounding remainder the way a 90 kHz / non-integer frame step could.
TEST(MediaTimelineTest, AudioRtpTicksAreExactForOneHour) {
  const auto t0 = T0();
  MediaTimeline timeline;
  timeline.Reset(t0, us(10000));

  const auto origin_us = std::chrono::duration_cast<us>(t0.time_since_epoch()).count();
  for (int64_t frame = 0; frame < 360000; ++frame) {
    const auto stamp_us =
        std::chrono::duration_cast<us>(timeline.Next().time_since_epoch()).count();
    const int64_t rtp = (stamp_us - origin_us) * 48000 / 1000000;
    EXPECT_EQ(rtp, frame * 480);
  }
}
