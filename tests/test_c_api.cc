#include <gtest/gtest.h>
#include "castcore/c_api.h"

TEST(CApiTest, LifecycleAndStateTransitions) {
  EXPECT_TRUE(castmirror_init());
  EXPECT_EQ(castmirror_get_state(), CASTMIRROR_STATE_IDLE);

  // Stats retrieval in idle state
  CastMirrorStreamStats stats{};
  EXPECT_TRUE(castmirror_get_stats(&stats));

  // Bitrate and delay runtime adjustments
  castmirror_set_bitrate(5000);
  castmirror_set_playout_delay(150);
  castmirror_set_freeze(true);
  castmirror_set_freeze(false);
  castmirror_set_muted(true);
  castmirror_set_muted(false);

  // Discovery controls
  castmirror_start_discovery();
  int dev_count = castmirror_get_device_count();
  EXPECT_GE(dev_count, 0);

  CastMirrorDeviceInfo dev_info{};
  EXPECT_FALSE(castmirror_get_device_info(-1, &dev_info));
  EXPECT_FALSE(castmirror_get_device_info(999, &dev_info));

  castmirror_stop_discovery();
  castmirror_shutdown();
}
