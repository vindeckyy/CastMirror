#include "tray.h"
#include "gui_app.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"

#if defined(CASTMIRROR_HAVE_TRAY)
#if __has_include(<libayatana-appindicator/app-indicator.h>)
#include <libayatana-appindicator/app-indicator.h>
#elif __has_include(<libappindicator/app-indicator.h>)
#include <libappindicator/app-indicator.h>
#endif
#endif

namespace castcore::gui {

TrayManager::TrayManager(GuiApp* app) : app_(app) {}

TrayManager::~TrayManager() {
  DestroyIndicator();
}

bool TrayManager::IsAvailable() const {
#if defined(CASTMIRROR_HAVE_TRAY)
  return true;
#else
  return false;
#endif
}

void TrayManager::CreateIndicator() {
#if defined(CASTMIRROR_HAVE_TRAY)
  if (created_) return;

  menu_ = gtk_menu_new();

  item_show_ = gtk_menu_item_new_with_label("Show CastMirror");
  g_signal_connect(item_show_, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer user_data) {
    auto* app = static_cast<GuiApp*>(user_data);
    gtk_window_present(app->GetWindow());
  }), app_);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_), item_show_);

  item_cast_last_ = gtk_menu_item_new_with_label("Cast last device");
  g_signal_connect(item_cast_last_, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
    std::thread([]() {
      CastEngine::Instance().StartCastingLastDevice();
    }).detach();
  }), nullptr);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_), item_cast_last_);

  item_stop_ = gtk_menu_item_new_with_label("Stop casting");
  gtk_widget_set_sensitive(item_stop_, FALSE);
  g_signal_connect(item_stop_, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer) {
    CastEngine::Instance().StopCasting();
  }), nullptr);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_), item_stop_);

  GtkWidget* sep = gtk_separator_menu_item_new();
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_), sep);

  GtkWidget* item_quit = gtk_menu_item_new_with_label("Quit");
  g_signal_connect(item_quit, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->Quit();
  }), app_);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu_), item_quit);

  gtk_widget_show_all(menu_);

  AppIndicator* ai = app_indicator_new("castmirror", "castmirror", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  if (ai) {
    app_indicator_set_status(ai, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(ai, GTK_MENU(menu_));
    indicator_ = ai;
    created_ = true;
    LOG_INFO << "[UI] System tray icon created";
  }
#endif
}

void TrayManager::DestroyIndicator() {
#if defined(CASTMIRROR_HAVE_TRAY)
  if (indicator_) {
    app_indicator_set_status(static_cast<AppIndicator*>(indicator_), APP_INDICATOR_STATUS_PASSIVE);
    g_object_unref(indicator_);
    indicator_ = nullptr;
  }
  if (menu_) {
    menu_ = nullptr;
  }
  created_ = false;
#endif
}

void TrayManager::UpdateState(SessionState state, const std::string& device_name) {
#if defined(CASTMIRROR_HAVE_TRAY)
  if (!created_ || !indicator_) return;

  bool is_active = (state == SessionState::kStreaming || state == SessionState::kConnecting ||
                    state == SessionState::kNegotiating || state == SessionState::kReconnecting);

  if (item_stop_) {
    gtk_widget_set_sensitive(item_stop_, is_active);
    if (is_active && !device_name.empty()) {
      std::string lbl = "Stop casting — " + device_name;
      gtk_menu_item_set_label(GTK_MENU_ITEM(item_stop_), lbl.c_str());
    } else {
      gtk_menu_item_set_label(GTK_MENU_ITEM(item_stop_), "Stop casting");
    }
  }

  if (item_cast_last_) {
    gtk_widget_set_sensitive(item_cast_last_, !is_active);
  }

  if (state == SessionState::kStreaming) {
    app_indicator_set_status(static_cast<AppIndicator*>(indicator_), APP_INDICATOR_STATUS_ATTENTION);
  } else {
    app_indicator_set_status(static_cast<AppIndicator*>(indicator_), APP_INDICATOR_STATUS_ACTIVE);
  }
#else
  (void)state;
  (void)device_name;
#endif
}

}  // namespace castcore::gui
