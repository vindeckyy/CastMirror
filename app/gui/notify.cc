#include "notify.h"
#include "castcore/logger.h"

namespace castcore::gui {

GApplication* NotificationManager::app_ = nullptr;
SessionState NotificationManager::last_notified_state_ = SessionState::kIdle;

void NotificationManager::Initialize(GApplication* app) {
  app_ = app;
  last_notified_state_ = SessionState::kIdle;
}

void NotificationManager::Shutdown() {
  if (app_) {
    g_application_withdraw_notification(app_, "castmirror-session");
    app_ = nullptr;
  }
  last_notified_state_ = SessionState::kIdle;
}

void NotificationManager::NotifyStateChange(SessionState state,
                                            const std::string& device_name,
                                            const std::string& message) {
  if (!app_) return;
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

  GNotification* notif = g_notification_new(summary.c_str());
  if (notif) {
    g_notification_set_body(notif, body.c_str());
    g_notification_set_default_action(notif, "app.present");

    GIcon* icon = g_themed_icon_new("io.github.vindeckyy.CastMirror");
    if (icon) {
      g_notification_set_icon(notif, icon);
      g_object_unref(icon);
    }

    g_notification_set_priority(
        notif, (state == SessionState::kFailed) ? G_NOTIFICATION_PRIORITY_URGENT
                                               : G_NOTIFICATION_PRIORITY_NORMAL);

    g_application_send_notification(app_, "castmirror-session", notif);
    g_object_unref(notif);
  }
}

}  // namespace castcore::gui
