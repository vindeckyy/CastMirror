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

Clock::time_point T0() { return Clock::time_point{} + ms(1000); }

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
  for (size_t i = 1; i < stamps.size(); ++i) {
    EXPECT_EQ(stamps[i] - stamps[i - 1], us(33333))
        << "re-sent frames must advance by exactly one cadence interval";
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

  // A present-time that did not advance is forced forward so the receiver never
  // sees the video timeline move backwards.
  pacer.Submit(MakeFrame(t0 + ms(5)), 0, 0);
  auto second = pacer.Tick(t0 + ms(34));
  ASSERT_TRUE(second.emit);
  EXPECT_TRUE(second.fresh);
  EXPECT_EQ(second.frame->timestamp, t0 + ms(6));

  // Nothing staged for the following tick: the last frame is re-sent instead of
  // leaving a gap.
  auto third = pacer.Tick(t0 + ms(67));
  ASSERT_TRUE(third.emit);
  EXPECT_FALSE(third.fresh);
  EXPECT_EQ(third.frame->timestamp, t0 + us(66666));
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
  EXPECT_EQ(stamps[0] - t0, us(33333)) << "the already-scheduled tick is not pulled in";
  for (size_t i = 1; i < stamps.size(); ++i) {
    EXPECT_EQ(stamps[i] - stamps[i - 1], us(16666)) << "the new cadence takes over";
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
