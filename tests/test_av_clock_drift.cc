#include <gtest/gtest.h>

#include "castcore/frame_pacer.h"
#include "castcore/media_timeline.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace {

using castcore::CapturedVideoFrame;
using castcore::FramePacer;
using castcore::MediaTimeline;

using Clock = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;
using us = std::chrono::microseconds;

constexpr int64_t kHours = 2;
constexpr int64_t kCheckpointUs = 600000000;  // 10 minutes

int64_t Us(Clock::time_point tp) {
  return std::chrono::duration_cast<us>(tp.time_since_epoch()).count();
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

// The audio capture loop's fixed-step emission policy, driven by a synthetic
// jittery clock. It uses the production MediaTimeline decisions, not a copy.
std::vector<int64_t> AudioTimestampsAtCheckpoints(Clock::time_point base) {
  MediaTimeline timeline;
  timeline.Reset(base, us(10000));

  std::vector<int64_t> samples;
  int64_t next_checkpoint = Us(base) + kCheckpointUs;
  auto now = base;
  const int64_t frames = kHours * 360000;
  int64_t last = Us(base);
  for (int64_t i = 0; i < frames; ++i) {
    const int64_t jitter = (i % 3 == 0) ? 9700 : ((i % 3 == 1) ? 10300 : 10000);
    if (i == frames / 2) now += ms(2000);  // suspend / dead stream
    now += us(jitter);

    const int64_t emit = timeline.FramesToEmit(now, 5);
    for (int64_t k = 0; k < emit; ++k)
      last = Us(timeline.Next());

    if (timeline.NeedsReanchor(now, 50)) {
      timeline.Reset(now - ms(10), us(10000));
    }
    while (Us(now) >= next_checkpoint) {
      samples.push_back(last);
      next_checkpoint += kCheckpointUs;
    }
  }
  return samples;
}

// The X11 capture loop's paced video timeline, driven by the same synthetic
// clock shape. Source timestamps are the post-read instants; idle re-sends use
// the pacer's elapsed-time step.
std::vector<int64_t> VideoTimestampsAtCheckpoints(Clock::time_point base) {
  FramePacer pacer;
  pacer.Reset(base);
  pacer.SetTargetFps(60);

  std::vector<int64_t> samples;
  int64_t next_checkpoint = Us(base) + kCheckpointUs;
  auto now = base;
  const int64_t frames = kHours * 216000;
  int64_t last = Us(base);
  for (int64_t i = 0; i < frames; ++i) {
    const int64_t jitter = (i % 3 == 0) ? 16600 : 16700;
    if (i == frames / 2) now += ms(2000);  // same suspend as the audio loop
    now += us(jitter);

    pacer.Submit(MakeFrame(now), 0, 0);
    const auto decision = pacer.Tick(now);
    if (decision.emit && decision.frame != nullptr) last = Us(decision.frame->timestamp);

    while (Us(now) >= next_checkpoint) {
      samples.push_back(last);
      next_checkpoint += kCheckpointUs;
    }
  }
  return samples;
}

}  // namespace

// Two hours of synthetic jitter and a 2-second suspend. Both streams derive
// from one clock; if either timestamp path ratcheted, the A/V epoch difference
// at the 10-minute checkpoints would grow instead of staying bounded.
TEST(AvClockDriftTest, LongRunSkewStaysBoundedWithJitterAndSuspend) {
  const auto base = Clock::time_point{} + ms(1000);
  const auto audio = AudioTimestampsAtCheckpoints(base);
  const auto video = VideoTimestampsAtCheckpoints(base);

  ASSERT_EQ(audio.size(), 12u);
  ASSERT_EQ(video.size(), 12u);

  // The 2-second suspend causes at most one bounded re-anchor offset. Drift
  // would keep growing checkpoint after checkpoint, so check both the absolute
  // bound and the flatness of the second half of the run.
  int64_t first_skew = 0;
  int64_t min_skew = INT64_MAX;
  int64_t max_skew = INT64_MIN;
  for (size_t i = 0; i < audio.size(); ++i) {
    const int64_t skew = audio[i] - video[i];
    if (i == 0) first_skew = skew;
    min_skew = std::min(min_skew, skew);
    max_skew = std::max(max_skew, skew);
    EXPECT_LE(skew - first_skew, 15000) << "skew grew at checkpoint " << i + 1;
    EXPECT_GE(skew - first_skew, -15000) << "skew shrank at checkpoint " << i + 1;
  }
  EXPECT_LE(max_skew - min_skew, 15000);

  int64_t late_min = INT64_MAX;
  int64_t late_max = INT64_MIN;
  for (size_t i = audio.size() / 2; i < audio.size(); ++i) {
    const int64_t skew = audio[i] - video[i];
    late_min = std::min(late_min, skew);
    late_max = std::max(late_max, skew);
  }
  EXPECT_LE(late_max - late_min, 1000) << "skew kept drifting after the suspend";

  // Each 10-minute interval must carry exactly the negotiated tick count.
  // The one interval containing the 2-second suspend is skipped: both streams
  // jump there by design, and the checks above already bound that offset.
  for (size_t i = 1; i < audio.size(); ++i) {
    const int64_t audio_dt = audio[i] - audio[i - 1];
    const int64_t video_dt = video[i] - video[i - 1];
    if (audio_dt > 601000000 || video_dt > 601000000) continue;
    EXPECT_NEAR(audio_dt * 48000 / 1000000, 28800000, 1000)
        << "audio RTP rate wrong in interval " << i + 1;
    EXPECT_NEAR(video_dt * 90 / 1000, 54000000, 1000)
        << "video RTP rate wrong in interval " << i + 1;
  }
}
