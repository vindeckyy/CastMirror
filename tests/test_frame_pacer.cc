#include <gtest/gtest.h>

#include "castcore/frame_pacer.h"

#include <chrono>
#include <vector>

using castcore::CapturedVideoFrame;
using castcore::FramePacer;

namespace {

using Clock = FramePacer::Clock;
using ms = std::chrono::milliseconds;
using us = std::chrono::microseconds;

Clock::time_point T0() {
  return Clock::time_point{} + ms(1000);
}

CapturedVideoFrame MakeFrame(Clock::time_point ts) {
  CapturedVideoFrame frame;
  frame.width = 4;
  frame.height = 2;
  frame.stride = 16;
  frame.timestamp = ts;
  frame.data.assign(32, 0x42);
  return frame;
}

std::vector<Clock::time_point> EmittedTimestamps(FramePacer* pacer, Clock::time_point from,
                                                 int duration_ms, int* fresh_count = nullptr) {
  std::vector<Clock::time_point> stamps;
  int fresh = 0;
  auto now = from;
  for (int i = 0; i <= duration_ms; ++i, now += ms(1)) {
    auto decision = pacer->Tick(now);
    if (!decision.emit) continue;
    stamps.push_back(decision.frame->timestamp);
    if (decision.fresh) ++fresh;
  }
  if (fresh_count) *fresh_count = fresh;
  return stamps;
}

}  // namespace

// An idle source (no new captures) must still emit: Windows desktop duplication
// only signals on change, so re-sends are what keep the receiver fed.
TEST(FramePacerTest, IdleTicksReSendTheLastFrameAtExactCadence) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);

  int fresh = 0;
  auto stamps = EmittedTimestamps(&pacer, t0, 2000, &fresh);

  EXPECT_EQ(fresh, 1) << "only the staged frame was new content";
  EXPECT_EQ(stamps.size(), 61u) << "2 s at 30 fps (tick 0 plus 60 intervals)";
  // Re-sends step by the time that actually elapsed between ticks, so the video
  // clock runs at real-time rate rather than a fixed step that would drift away
  // from audio. The harness ticks on a 1 ms grid, so a 33.333 ms interval is
  // observed as 33.333..34 ms; that bound is what pins the property.
  for (size_t i = 1; i < stamps.size(); ++i) {
    const us step = std::chrono::duration_cast<us>(stamps[i] - stamps[i - 1]);
    EXPECT_GE(step, us(33000)) << "emit " << i << " did not advance by a cadence interval";
    EXPECT_LE(step, us(33333) + ms(1)) << "emit " << i << " advanced " << step.count()
                                       << " us; re-sends must track elapsed time, not a fixed step";
  }
}

TEST(FramePacerTest, OnlyTheNewestCapturedFrameIsEmitted) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);

  // Three captures land inside one interval: the older two are superseded, not
  // queued, so the stream never falls behind the desktop.
  pacer.Submit(MakeFrame(t0 + ms(1)), 11, 12);
  pacer.Submit(MakeFrame(t0 + ms(5)), 21, 22);

  auto first = pacer.Tick(t0);
  ASSERT_TRUE(first.emit);
  EXPECT_TRUE(first.fresh);
  EXPECT_EQ(first.frame->timestamp, t0 + ms(5));
  EXPECT_EQ(first.crop_x, 21);
  EXPECT_EQ(first.crop_y, 22);

  // Before the next interval elapses nothing goes out.
  EXPECT_FALSE(pacer.Tick(t0 + ms(1)).emit);

  // A present-time that did not advance cannot be used as-is: the timeline moves
  // on by the wall-clock time that has elapsed, so the video clock never stalls
  // behind audio waiting for the source to catch up.
  pacer.Submit(MakeFrame(t0 + ms(5)), 0, 0);
  auto second = pacer.Tick(t0 + ms(34));
  ASSERT_TRUE(second.emit);
  EXPECT_TRUE(second.fresh);
  EXPECT_EQ(second.frame->timestamp, t0 + ms(5) + ms(34));

  // Nothing staged for the following tick: the last frame is re-sent instead of
  // leaving a gap.
  auto third = pacer.Tick(t0 + ms(67));
  ASSERT_TRUE(third.emit);
  EXPECT_FALSE(third.fresh);
  // The re-send continues the timeline by the time that actually elapsed, not
  // from the wall-clock tick deadline, which is a different clock from the
  // source's present time.
  EXPECT_EQ(third.frame->timestamp, t0 + ms(5) + ms(34) + ms(33));
  EXPECT_EQ(third.frame->data.size(), 32u);
}

// The re-sent frame is borrowed, not copied: the pacer hands out its own buffer
// and only advances the timestamp, so a steady stream costs no pixel copies.
TEST(FramePacerTest, ReSendBorrowsTheSameBufferAndKeepsTimeForward) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  // The loop fell behind by more than one interval, so the fresh frame it picks
  // up carries a present-time past the tick it is emitted on.
  pacer.Submit(MakeFrame(t0 + ms(80)), 0, 0);
  auto late = pacer.Tick(t0 + ms(80));
  ASSERT_TRUE(late.emit);
  ASSERT_TRUE(late.fresh);
  const void* pixels = late.frame->data.data();
  const auto late_ts = late.frame->timestamp;
  EXPECT_EQ(late_ts, t0 + ms(80));

  // Re-sending must not stamp the already-passed tick deadline: that would move
  // the video timeline backwards relative to the frame just emitted.
  auto resend = pacer.Tick(t0 + ms(80));
  ASSERT_TRUE(resend.emit);
  EXPECT_FALSE(resend.fresh);
  EXPECT_GT(resend.frame->timestamp, late_ts);
  EXPECT_EQ(resend.frame->data.data(), pixels) << "re-sends must not copy the pixel buffer";
}

TEST(FramePacerTest, ResumesImmediatelyAfterAStall) {
  FramePacer pacer;
  pacer.SetTargetFps(60);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  // The loop was blocked for half a second (duplication recreate, minimized
  // window): it must resume at the current time, not replay the backlog.
  const auto t1 = t0 + ms(500);
  auto resumed = pacer.Tick(t1);
  ASSERT_TRUE(resumed.emit);
  // Resume at the current time: the frame emitted at t0 hit the display at t0
  // and does not cover the 500 ms that followed, during which the audio clock
  // kept running. The video timeline must advance by the elapsed time or it
  // trails audio by the whole stall.
  EXPECT_EQ(resumed.frame->timestamp, t1);
  EXPECT_EQ(pacer.NextTick() - t1, us(16666)) << "one interval, not a catch-up burst";
}

TEST(FramePacerTest, RaisedTargetFpsAppliesAfterThePendingTick) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  pacer.SetTargetFps(60);
  auto stamps = EmittedTimestamps(&pacer, t0 + ms(1), 1000);
  ASSERT_GE(stamps.size(), 30u);
  // The tick already scheduled at 30 fps is not pulled in: the first stamp still
  // lands a whole old interval on, then the new cadence takes over.
  const us old = std::chrono::duration_cast<us>(stamps[0] - t0);
  EXPECT_GE(old, us(33000)) << "the already-scheduled tick was pulled in";
  EXPECT_LE(old, us(33333) + ms(1)) << "expected one old-cadence interval, got " << old.count();
  for (size_t i = 1; i < stamps.size(); ++i) {
    const us step = std::chrono::duration_cast<us>(stamps[i] - stamps[i - 1]);
    EXPECT_GE(step, us(16000)) << "emit " << i << " is faster than the new cadence";
    EXPECT_LE(step, us(16666) + ms(1))
        << "emit " << i << " stepped " << step.count() << " us, not the 60 fps cadence";
  }
}

TEST(FramePacerTest, LongIntervalsArePolledInSlices) {
  FramePacer pacer;
  pacer.SetTargetFps(5);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  EXPECT_EQ(pacer.WaitSliceMs(t0), 100) << "a 200 ms interval is waited out in slices";
  EXPECT_EQ(pacer.WaitSliceMs(t0 + ms(150)), 50);
  EXPECT_EQ(pacer.WaitSliceMs(t0 + ms(200)), 0) << "a due tick must not block";
}

// The capture loop recycles the buffer of a frame the pacer has already emitted
// instead of allocating and zero-filling a full BGRA frame per capture (8.3 MB
// at 1080p, entirely overwritten by the row copy straight after).
TEST(FramePacerTest, SupersededBufferIsHandedBackForReuse) {
  FramePacer pacer;
  const auto t0 = T0();
  pacer.Reset(t0);

  // Nothing has been emitted yet, so there is nothing to recycle and the caller
  // falls back to its own allocation.
  EXPECT_TRUE(pacer.TakeSupersededBuffer().empty());

  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);
  EXPECT_TRUE(pacer.TakeSupersededBuffer().empty())
      << "the frame just emitted is still current, not superseded";

  // Emitting a second frame retires the first, whose pixels become reusable.
  pacer.Submit(MakeFrame(t0 + ms(40)), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0 + ms(40)).emit);
  EXPECT_EQ(pacer.TakeSupersededBuffer().size(), 32u);
  EXPECT_TRUE(pacer.TakeSupersededBuffer().empty()) << "a buffer is handed out only once";
}

// The recycled buffer must never be the frame still in flight: handing that one
// back would make the pacer emit a blank picture.
TEST(FramePacerTest, RecyclingNeverEmptiesTheFrameThatGetsEmitted) {
  FramePacer pacer;
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  // Force a spare to exist, then take it before staging the next capture.
  pacer.Submit(MakeFrame(t0 + ms(20)), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0 + ms(20)).emit);
  std::vector<uint8_t> recycled = pacer.TakeSupersededBuffer();
  ASSERT_EQ(recycled.size(), 32u);
  recycled.assign(32, 0x7f);  // the producer refills the retired buffer

  pacer.Submit(MakeFrame(t0 + ms(40)), 0, 0);
  auto decision = pacer.Tick(t0 + ms(40));
  ASSERT_TRUE(decision.emit);
  ASSERT_TRUE(decision.fresh);
  ASSERT_EQ(decision.frame->data.size(), 32u);
  EXPECT_EQ(decision.frame->data[0], 0x42u)
      << "the emitted frame is the staged one, never the recycled buffer";
}

// The steady state the capture loop relies on: two buffers alternate forever and
// each tick still emits the newest capture.
TEST(FramePacerTest, RecycledBuffersAlternateWithoutStallingTheStream) {
  FramePacer pacer;
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  int allocations = 0;
  uint8_t marker = 0x10;
  for (int i = 1; i <= 20; ++i) {
    // The capture loop: take a retired buffer if one exists, else allocate.
    std::vector<uint8_t> buffer = pacer.TakeSupersededBuffer();
    if (buffer.empty()) ++allocations;
    CapturedVideoFrame next = MakeFrame(t0 + ms(20 * i));
    marker = static_cast<uint8_t>(marker + 1);
    next.data = std::move(buffer);
    next.data.assign(32, marker);
    pacer.Submit(std::move(next), 0, 0);

    auto decision = pacer.Tick(t0 + ms(20 * i));
    ASSERT_TRUE(decision.emit);
    ASSERT_EQ(decision.frame->data.size(), 32u);
    EXPECT_EQ(decision.frame->data[0], marker) << "frame " << i << " must carry its own capture";
  }

  EXPECT_EQ(allocations, 1) << "after the first capture the spare keeps recycling";
}

// An idle source re-sends the same frame, so nothing is retired and the spare
// stays empty: a re-send must not recycle the buffer it is still handing out.
TEST(FramePacerTest, ResendsDoNotRetireTheFrameTheyReuse) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0), 0, 0);
  ASSERT_TRUE(pacer.Tick(t0).emit);

  for (int i = 1; i <= 5; ++i) {
    auto decision = pacer.Tick(t0 + ms(34 * i));
    ASSERT_TRUE(decision.emit);
    EXPECT_FALSE(decision.fresh) << "tick " << i << " is a re-send";
    ASSERT_EQ(decision.frame->data.size(), 32u) << "re-sends keep their pixels";
    EXPECT_TRUE(pacer.TakeSupersededBuffer().empty())
        << "the frame being re-sent is not available for recycling";
  }
}

// Fresh frames carry the source's present time, which DXGI reports behind wall
// clock by the present lag. A re-send stamped from the tick deadline (wall
// clock) therefore stepped the video timeline forward by that lag, and the
// receiver Ã¢â‚¬â€ which plays by RTP timestamp Ã¢â‚¬â€ showed every subsequent frame late
// against audio. The whole stream must stay on the present-time timeline.
TEST(FramePacerTest, ReSendsStayOnThePresentTimeTimeline) {
  constexpr int kPresentLagMs = 20;  // how far behind now DXGI reports a present
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);

  // The desktop only changes every other tick, so the stream alternates fresh
  // captures with re-sends Ã¢â‚¬â€ the case that mixed the two timelines.
  const us interval = us(33333);
  auto now = t0;
  for (int i = 0; i < 12; ++i) {
    if (i % 2 == 0) {
      pacer.Submit(MakeFrame(now - ms(kPresentLagMs)), 0, 0);
    }
    auto decision = pacer.Tick(now);
    ASSERT_TRUE(decision.emit) << "tick " << i;
    // With a capture every other tick both paths land on the same line: the
    // fresh frame carries now - lag, and the re-send adds exactly the one
    // interval that elapsed since the last emit. So the stamp is exactly the
    // present-time timeline on every tick, with no tolerance to tune. A
    // wall-clock stamp diverges at tick 1, where it stamps the tick deadline
    // instead of the source's present time.
    EXPECT_EQ(decision.frame->timestamp, now - ms(kPresentLagMs))
        << "tick " << i << " stamped off the source timeline";
    now += interval;
  }
}

// A re-send steps the timeline by the time that actually elapsed, so the video
// clock keeps real-time rate across a stall instead of crawling a nominal
// interval per tick while audio runs on. Stamps must still advance by at least
// one interval; the ceiling allows the 1 ms granularity of the tick harness.
TEST(FramePacerTest, ReSendsAdvanceByTheElapsedTime) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);
  pacer.Submit(MakeFrame(t0 - ms(20)), 0, 0);

  std::vector<Clock::time_point> stamps;
  auto now = t0;
  for (int i = 0; i < 10; ++i) {
    if (i == 5) pacer.Submit(MakeFrame(now - ms(20)), 0, 0);
    auto decision = pacer.Tick(now);
    ASSERT_TRUE(decision.emit);
    stamps.push_back(decision.frame->timestamp);
    now += us(33333);
  }

  for (size_t i = 1; i < stamps.size(); ++i) {
    const us step = std::chrono::duration_cast<us>(stamps[i] - stamps[i - 1]);
    EXPECT_GE(step, us(33000)) << "emit " << i << " did not advance by one interval";
    EXPECT_LE(step, us(33333) + ms(1)) << "emit " << i << " advanced " << step.count()
                                       << " us, more than one interval plus the tick granularity";
  }
}

// A fresh frame whose present time does not advance takes the clamp branch, which
// only ever raises last_emit_ts_. If that raise is not bounded by the source,
// every subsequent frame takes the same branch and the video timeline ratchets
// forward by 1 ms per frame while audio stays put.
TEST(FramePacerTest, PresentingLagJitterDoesNotRatchetTheTimelineForward) {
  constexpr int kPresentLagMs = 20;
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);

  const us interval = us(33333);
  auto now = t0;
  Clock::time_point last_stamp = t0;
  for (int i = 0; i < 120; ++i) {
    // The present lag jitters by a full vsync, which is enough to push the
    // present time behind the free-running re-send timeline.
    const int lag = kPresentLagMs + ((i / 4) % 2 ? 16 : 0);
    pacer.Submit(MakeFrame(now - ms(lag)), 0, 0);
    auto decision = pacer.Tick(now);
    ASSERT_TRUE(decision.emit) << "tick " << i;
    last_stamp = decision.frame->timestamp;

    // The stamp may not run ahead of where the source would present content
    // now. One interval of slack covers the re-send gap the design fills.
    const us ahead = std::chrono::duration_cast<us>(last_stamp - (now - ms(kPresentLagMs)));
    EXPECT_LE(ahead, interval)
        << "tick " << i << ": stamp ran " << ahead.count()
        << " us ahead of the source timeline; the clamp is ratcheting forward";
    now += interval;
  }
}

// When the source stops presenting (a frozen present time) the timeline has to
// run on by wall clock, or the receiver stalls waiting for content that never
// comes. The rate is the contract: it must advance by the elapsed time, not by a
// nominal interval. A 1 ms-per-frame step would cover 120 ms of a 4 s stream.
TEST(FramePacerTest, FrozenPresentTimeAdvancesAtWallClockRate) {
  FramePacer pacer;
  pacer.SetTargetFps(30);
  const auto t0 = T0();
  pacer.Reset(t0);

  const us interval = us(33333);
  const auto frozen_present = t0 - ms(40);
  auto now = t0;
  std::vector<Clock::time_point> stamps;
  for (int i = 0; i < 120; ++i) {
    pacer.Submit(MakeFrame(frozen_present), 0, 0);
    auto decision = pacer.Tick(now);
    ASSERT_TRUE(decision.emit) << "tick " << i;
    stamps.push_back(decision.frame->timestamp);
    now += interval;
  }

  for (size_t i = 1; i < stamps.size(); ++i) {
    const us step = std::chrono::duration_cast<us>(stamps[i] - stamps[i - 1]);
    EXPECT_GE(step, interval) << "emit " << i << " stepped " << step.count()
                              << " us; a frozen source must still advance at wall-clock rate";
  }
  // 119 intervals of wall clock have passed, so the stamp must cover nearly all
  // of it. A fixed 1 ms step would leave it ~3.9 s short.
  const us covered = std::chrono::duration_cast<us>(stamps.back() - stamps.front());
  EXPECT_GE(covered, 119 * us(33333) - us(1000)) << "the timeline only covered " << covered.count()
                                                 << " us of the 3966327 us that actually elapsed";
}
