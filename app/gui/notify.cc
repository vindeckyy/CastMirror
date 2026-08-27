#include "notify.h"
#include "castcore/logger.h"

#if defined(CASTMIRROR_HAVE_NOTIFY)
#include <libnotify/notify.h>
#endif

namespace castcore::gui {

SessionState NotificationManager::last_notified_state_ = SessionState::kIdle;

void NotificationManager::Initialize() {
#if defined(CASTMIRROR_HAVE_NOTIFY)
  if (!notify_is_initted()) {
    notify_init("CastMirror");
  }
#endif
}

void NotificationManager::Shutdown() {
#if defined(CASTMIRROR_HAVE_NOTIFY)
  if (notify_is_initted()) {
    notify_uninit();
  }
#endif
}

bool NotificationManager::IsAvailable() {
#if defined(CASTMIRROR_HAVE_NOTIFY)
  return true;
#else
  return false;
#endif
}

void NotificationManager::NotifyStateChange(SessionState state,
                                           const std::string& device_name,
                                           const std::string& message) {
#if defined(CASTMIRROR_HAVE_NOTIFY)
  if (!notify_is_initted()) return;
  if (state == last_notified_state_) return;

  std::string summary;
  std::string body;

  if (state == SessionState::kStreaming) {
    summary = "Casting to " + device_name;
    body = "Your display is now streaming to " + device_name;
  } else if (state == SessionState::kFailed) {
    summary = "Cast Ended";
    body = message.empty() ? "The streaming session disconnected." : message;
  } else if (state == SessionState::kReconnecting) {
    summary = "Reconnecting to " + device_name;
    body = message.empty() ? "Attempting automatic session recovery..." : message;
  } else {
    last_notified_state_ = state;
    return;
  }

  last_notified_state_ = state;

  NotifyNotification* notif = notify_notification_new(summary.c_str(), body.c_str(), "castmirror");
  if (notif) {
    notify_notification_set_urgency(notif, (state == SessionState::kFailed) ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL);
    notify_notification_set_timeout(notif, 4000);
    GError* err = nullptr;
    notify_notification_show(notif, &err);
    if (err) {
      g_error_free(err);
    }
    g_object_unref(notif);
  }
#else
  (void)state;
  (void)device_name;
  (void)message;
#endif
}

}  // namespace castcore::gui
