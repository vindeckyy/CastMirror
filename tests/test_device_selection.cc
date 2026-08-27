#include <gtest/gtest.h>
#include "castcore/device_selection.h"

using namespace castcore;

TEST(DeviceSelectionTest, PrefersIdThenIp) {
  std::vector<CastDevice> devices(2);
  devices[0].id = "aaa";
  devices[0].ip_address = "192.168.0.50";
  devices[0].name = "Family Room";
  devices[1].id = "bbb";
  devices[1].ip_address = "192.168.0.164";
  devices[1].name = "Living Room";

  EXPECT_EQ(IndexOfPreferredDevice(devices, "bbb", ""), 1);
  EXPECT_EQ(IndexOfPreferredDevice(devices, "", "192.168.0.164"), 1);
  EXPECT_EQ(IndexOfPreferredDevice(devices, "aaa", "192.168.0.164"), 0);
  EXPECT_EQ(IndexOfPreferredDevice(devices, "missing", "10.0.0.1"), -1);
}

TEST(DeviceSelectionTest, DisplayIndex) {
  std::vector<DisplayInfo> displays(2);
  displays[0].id = 0;
  displays[1].id = 1;
  EXPECT_EQ(IndexOfPreferredDisplay(displays, 1), 1);
  EXPECT_EQ(IndexOfPreferredDisplay(displays, 9), 0);
}
