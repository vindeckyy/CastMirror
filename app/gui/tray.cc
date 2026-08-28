#include "tray.h"
#include "gui_app.h"
#include "castcore/cast_engine.h"
#include "castcore/logger.h"

#include <thread>

#if defined(CASTMIRROR_HAVE_TRAY)
#include <gtk/gtk.h>
#if __has_include(<libayatana-appindicator/app-indicator.h>)
#include <libayatana-appindicator/app-indicator.h>
#elif __has_include(<libayatana-appindicator-glib/app-indicator.h>)
#include <libayatana-appindicator-glib/app-indicator.h>
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

  action_group_ = g_simple_action_group_new();

  action_show_ = g_simple_action_new("show", nullptr);
  g_signal_connect(action_show_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    auto* self = static_cast<TrayManager*>(user_data);
    if (self && self->app_) {
      self->app_->Present();
    }
  }), this);
  g_action_map_add_action(G_ACTION_MAP(action_group_), G_ACTION(action_show_));

  action_cast_last_ = g_simple_action_new("cast-last", nullptr);
  g_signal_connect(action_cast_last_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer) {
    std::thread([]() {
      CastEngine::Instance().StartCastingLastDevice();
    }).detach();
  }), nullptr);
  g_action_map_add_action(G_ACTION_MAP(action_group_), G_ACTION(action_cast_last_));

  action_stop_ = g_simple_action_new("stop", nullptr);
  g_signal_connect(action_stop_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer) {
    CastEngine::Instance().StopCasting();
  }), nullptr);
  g_simple_action_set_enabled(action_stop_, FALSE);
  g_action_map_add_action(G_ACTION_MAP(action_group_), G_ACTION(action_stop_));

  action_quit_ = g_simple_action_new("quit", nullptr);
  g_signal_connect(action_quit_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    auto* self = static_cast<TrayManager*>(user_data);
    if (self && self->app_) {
      self->app_->Quit();
    }
  }), this);
  g_action_map_add_action(G_ACTION_MAP(action_group_), G_ACTION(action_quit_));

  menu_ = g_menu_new();
  g_menu_append(menu_, "Show CastMirror", "indicator.show");
  g_menu_append(menu_, "Cast last display", "indicator.cast-last");
  g_menu_append(menu_, "Stop casting", "indicator.stop");
  current_stop_label_ = "Stop casting";

  GMenu* quit_section = g_menu_new();
  g_menu_append(quit_section, "Quit", "indicator.quit");
  g_menu_append_section(menu_, nullptr, G_MENU_MODEL(quit_section));
  g_object_unref(quit_section);

  AppIndicator* ai = app_indicator_new("io.github.vindeckyy.CastMirror",
                                       "io.github.vindeckyy.CastMirror",
                                       APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
  if (ai) {
    indicator_ = ai;
    created_ = true;

    app_indicator_set_attention_icon_full(ai,
                                          "io.github.vindeckyy.CastMirror-attention-symbolic",
                                          "CastMirror is casting");

    app_indicator_set_actions(ai, G_ACTION_GROUP(action_group_));
    app_indicator_set_menu(ai, G_MENU_MODEL(menu_));

    app_indicator_set_status(ai, APP_INDICATOR_STATUS_ACTIVE);

    gboolean connected = FALSE;
    g_object_get(ai, "connected", &connected, NULL);
    connected_ = (connected != FALSE);

    g_signal_connect(ai, "connection-changed", G_CALLBACK(+[](AppIndicator*, gboolean connected, gpointer user_data) {
      auto* self = static_cast<TrayManager*>(user_data);
      if (self) {
        self->OnConnectionChanged(connected != FALSE);
      }
    }), this);

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
    g_object_unref(menu_);
    menu_ = nullptr;
  }
  if (action_group_) {
    g_object_unref(action_group_);
    action_group_ = nullptr;
  }
  action_show_ = nullptr;
  action_cast_last_ = nullptr;
  action_stop_ = nullptr;
  action_quit_ = nullptr;
  created_ = false;
  connected_ = false;
  current_stop_label_.clear();
#endif
}

void TrayManager::UpdateState(SessionState state, const std::string& device_name) {
#if defined(CASTMIRROR_HAVE_TRAY)
  if (!created_ || !indicator_) return;

  bool is_active = (state == SessionState::kStreaming || state == SessionState::kConnecting ||
                    state == SessionState::kNegotiating || state == SessionState::kReconnecting);

  if (action_stop_) {
    g_simple_action_set_enabled(action_stop_, is_active ? TRUE : FALSE);
  }

  if (action_cast_last_) {
    g_simple_action_set_enabled(action_cast_last_, is_active ? FALSE : TRUE);
  }

  std::string stop_label = (is_active && !device_name.empty())
                               ? ("Stop casting — " + device_name)
                               : "Stop casting";

  if (menu_ && stop_label != current_stop_label_) {
    g_menu_remove(menu_, 2);
    g_menu_insert(menu_, 2, stop_label.c_str(), "indicator.stop");
    current_stop_label_ = stop_label;
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

void TrayManager::OnConnectionChanged(bool connected) {
#if defined(CASTMIRROR_HAVE_TRAY)
  connected_ = connected;
  if (!connected_) {
    if (app_) {
      GtkWindow* win = app_->GetWindow();
      if (win && !gtk_widget_get_visible(GTK_WIDGET(win))) {
        app_->Present();
      }
    }
  }
#else
  (void)connected;
#endif
}

}  // namespace castcore::gui
