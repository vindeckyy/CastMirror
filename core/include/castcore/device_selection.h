#ifndef CASTCORE_DEVICE_SELECTION_H_
#define CASTCORE_DEVICE_SELECTION_H_

#include "castcore/types.h"
#include <string>
#include <vector>

namespace castcore {

// Returns index of the preferred device, or -1 if not present.
inline int IndexOfPreferredDevice(const std::vector<CastDevice>& devices,
                                  const std::string& id,
                                  const std::string& ip) {
  if (!id.empty()) {
    for (size_t i = 0; i < devices.size(); ++i) {
      if (devices[i].id == id) {
        return static_cast<int>(i);
      }
    }
  }
  if (!ip.empty()) {
    for (size_t i = 0; i < devices.size(); ++i) {
      if (devices[i].ip_address == ip) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

inline int IndexOfPreferredDisplay(const std::vector<DisplayInfo>& displays, int display_id) {
  for (size_t i = 0; i < displays.size(); ++i) {
    if (displays[i].id == display_id) {
      return static_cast<int>(i);
    }
  }
  return displays.empty() ? -1 : 0;
}

}  // namespace castcore

#endif  // CASTCORE_DEVICE_SELECTION_H_
