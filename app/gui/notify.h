#ifndef CASTMIRROR_GUI_NOTIFY_H_
#define CASTMIRROR_GUI_NOTIFY_H_

#include <string>
#include "castcore/types.h"

namespace castcore::gui {

class NotificationManager {
 public:
  static void Initialize();
  static void Shutdown();

  static bool IsAvailable();
  static void NotifyStateChange(SessionState state,
                                const std::string& device_name,
                                const std::string& message);

 private:
  static SessionState last_notified_state_;
};

}  // namespace castcore::gui

#endif  // CASTMIRROR_GUI_NOTIFY_H_
