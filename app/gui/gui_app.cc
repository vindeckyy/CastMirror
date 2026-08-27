#include "gui_app.h"
#include "cast_tab.h"
#include "live_tab.h"
#include "settings_tab.h"
#include "logs_tab.h"
#include "tray.h"
#include "notify.h"
#include "first_run.h"
#include "widgets.h"
#include "help_copy.h"
#include "css_loader.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"
#include <gdk/gdkkeysyms.h>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

namespace castcore::gui {

namespace {

struct StatePayload {
  SessionState state;
  std::string message;
};

std::string GetConfigDir() {
  const char* home = std::getenv("HOME");
  std::string dir = (home ? std::string(home) : "/tmp") + "/.config/castmirror";
  return dir;
}

}  // namespace

GuiApp::GuiApp() {
  BuildUi();
  SetupAccelerators();
  SetupEngineCallbacks();
  SetupLoggerCallback();

  // Create tray if configured
  tray_manager_ = std::make_unique<TrayManager>(this);
  if (ConfigStore::Instance().Get().enable_tray_on_startup) {
    tray_manager_->CreateIndicator();
  }

  // 500ms Stats telemetry timer
  stats_timer_id_ = g_timeout_add(500, OnStatsTimer, this);
}

GuiApp::~GuiApp() {
  if (stats_timer_id_ != 0) {
    g_source_remove(stats_timer_id_);
    stats_timer_id_ = 0;
  }
  if (rescan_timer_id_ != 0) {
    g_source_remove(rescan_timer_id_);
    rescan_timer_id_ = 0;
  }
}


void GuiApp::BuildUi() {
  const auto& cfg = ConfigStore::Instance().Get();

  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window_), copy::kAppTitle);

  int init_w = std::clamp(cfg.window_width, 760, 1600);
  int init_h = std::clamp(cfg.window_height, 560, 1200);
  gtk_window_set_default_size(GTK_WINDOW(window_), init_w, init_h);
  gtk_window_set_position(GTK_WINDOW(window_), GTK_WIN_POS_CENTER);

  // Set window icon if available
  const char* icon_path = CASTMIRROR_SRC_DIR "/docs/assets/logo.svg";
  if (std::filesystem::exists(icon_path)) {
    gtk_window_set_icon_from_file(GTK_WINDOW(window_), icon_path, nullptr);
  }

  g_signal_connect(window_, "configure-event", G_CALLBACK(OnConfigureEvent), this);
  g_signal_connect(window_, "delete-event", G_CALLBACK(OnDeleteEvent), this);

  GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window_), main_vbox);

  // 1. Header Box
  header_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(header_box_), "header-box");

  // Logo Icon
  if (std::filesystem::exists(icon_path)) {
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_size(icon_path, 28, 28, &err);
    if (pb) {
      GtkWidget* logo_img = gtk_image_new_from_pixbuf(pb);
      g_object_unref(pb);
      gtk_box_pack_start(GTK_BOX(header_box_), logo_img, FALSE, FALSE, 0);
    }
  }

  // Titles
  GtkWidget* title_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget* title_lbl = gtk_label_new(copy::kAppTitle);
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "title-label");

  header_subtitle_ = gtk_label_new(copy::kAppSubtitleDefault);
  gtk_widget_set_halign(header_subtitle_, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(header_subtitle_), "subtitle-label");

  gtk_box_pack_start(GTK_BOX(title_vbox), title_lbl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(title_vbox), header_subtitle_, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header_box_), title_vbox, TRUE, TRUE, 0);

  // Status Badge Pill
  status_badge_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_style_context_add_class(gtk_widget_get_style_context(status_badge_), "status-badge");
  gtk_style_context_add_class(gtk_widget_get_style_context(status_badge_), "status-idle");
  status_badge_lbl_ = gtk_label_new("○ READY");
  gtk_box_pack_start(GTK_BOX(status_badge_), status_badge_lbl_, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(header_box_), status_badge_, FALSE, FALSE, 0);

  // Menu Button with Popover
  GtkWidget* menu_btn = gtk_menu_button_new();
  GtkWidget* menu_icon = gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_button_set_image(GTK_BUTTON(menu_btn), menu_icon);

  GtkWidget* popover = gtk_popover_new(menu_btn);
  GtkWidget* pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_set_border_width(GTK_CONTAINER(pop_box), 8);

  GtkWidget* item_about = gtk_button_new_with_label("About CastMirror");
  auto on_about = +[](GtkButton*, gpointer user_data) {
    auto* self = static_cast<GuiApp*>(user_data);
    const char* authors[] = {"CastMirror Maintainers", nullptr};
    gtk_show_about_dialog(
        self->GetWindow(),
        "program-name", copy::kAppTitle,
        "version", "1.0.0",
        "comments", copy::kAboutComments,
        "license", copy::kAboutLicense,
        "website", "https://github.com/vindeckyy/CastMirror",
        "authors", authors,
        nullptr);
  };
  g_signal_connect(item_about, "clicked", G_CALLBACK(on_about), this);
  gtk_box_pack_start(GTK_BOX(pop_box), item_about, FALSE, FALSE, 0);

  GtkWidget* item_cfg = gtk_button_new_with_label("Open config folder");
  auto on_cfg = +[](GtkButton*, gpointer) {
    std::string uri = "file://" + GetConfigDir();
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
  };
  g_signal_connect(item_cfg, "clicked", G_CALLBACK(on_cfg), nullptr);
  gtk_box_pack_start(GTK_BOX(pop_box), item_cfg, FALSE, FALSE, 0);

  GtkWidget* item_log = gtk_button_new_with_label("Open log folder");
  auto on_log = +[](GtkButton*, gpointer) {
    std::string uri = "file://" + GetConfigDir();
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
  };
  g_signal_connect(item_log, "clicked", G_CALLBACK(on_log), nullptr);
  gtk_box_pack_start(GTK_BOX(pop_box), item_log, FALSE, FALSE, 0);

  GtkWidget* item_quit = gtk_button_new_with_label("Quit");
  auto on_quit = +[](GtkButton*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->Quit();
  };
  g_signal_connect(item_quit, "clicked", G_CALLBACK(on_quit), this);
  gtk_box_pack_start(GTK_BOX(pop_box), item_quit, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(popover), pop_box);
  gtk_widget_show_all(pop_box);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu_btn), popover);
  gtk_box_pack_end(GTK_BOX(header_box_), menu_btn, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(main_vbox), header_box_, FALSE, FALSE, 0);

  // 2. Notebook with 4 Tabs
  notebook_ = gtk_notebook_new();
  gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook_), FALSE);
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook_), TRUE);

  cast_tab_ = std::make_unique<CastTab>(this);
  live_tab_ = std::make_unique<LiveTab>(this);
  settings_tab_ = std::make_unique<SettingsTab>(this);
  logs_tab_ = std::make_unique<LogsTab>(this);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), cast_tab_->GetRootWidget(), gtk_label_new("Cast"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), live_tab_->GetRootWidget(), gtk_label_new("Live session"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), settings_tab_->GetRootWidget(), gtk_label_new("Settings"));
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook_), logs_tab_->GetRootWidget(), gtk_label_new("Logs"));

  gtk_box_pack_start(GTK_BOX(main_vbox), notebook_, TRUE, TRUE, 0);

  // 3. Persistent Footer Box
  footer_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(footer_box_), "footer-box");

  spinner_ = gtk_spinner_new();
  gtk_widget_set_visible(spinner_, FALSE);
  gtk_box_pack_start(GTK_BOX(footer_box_), spinner_, FALSE, FALSE, 0);

  cast_button_ = gtk_button_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(cast_button_), "btn-cast-start");

  GtkWidget* cast_btn_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(cast_btn_inner, GTK_ALIGN_CENTER);
  cast_button_lbl_ = gtk_label_new("Cast display");
  gtk_box_pack_start(GTK_BOX(cast_btn_inner), cast_button_lbl_, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(cast_button_), cast_btn_inner);

  auto on_cast_click = +[](GtkButton*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->TriggerCastAction();
  };
  g_signal_connect(cast_button_, "clicked", G_CALLBACK(on_cast_click), this);
  gtk_box_pack_start(GTK_BOX(footer_box_), cast_button_, TRUE, TRUE, 0);

  view_live_btn_ = gtk_button_new_with_label("View live session →");
  gtk_style_context_add_class(gtk_widget_get_style_context(view_live_btn_), "btn-info");
  auto on_view_live = +[](GtkButton*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->SwitchToTab(1);
  };
  g_signal_connect(view_live_btn_, "clicked", G_CALLBACK(on_view_live), this);

  gtk_box_pack_start(GTK_BOX(main_vbox), footer_box_, FALSE, FALSE, 0);

  // Initial populate
  cast_tab_->RefreshDevices();
  cast_tab_->RefreshDisplays();
}

void GuiApp::SetupAccelerators() {
  GtkAccelGroup* accel = gtk_accel_group_new();

  // Ctrl+Return -> Cast/Stop
  auto accel_cast = +[](GtkAccelGroup*, GObject*, guint, GdkModifierType, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->TriggerCastAction();
  };
  gtk_accel_group_connect(
      accel, GDK_KEY_Return, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(accel_cast), this, nullptr));

  // Ctrl+R -> Rescan
  auto accel_rescan = +[](GtkAccelGroup*, GObject*, guint, GdkModifierType, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->TriggerRescan();
  };
  gtk_accel_group_connect(
      accel, GDK_KEY_r, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(accel_rescan), this, nullptr));

  // Ctrl+1..4 -> Tabs
  auto accel_tab = +[](GtkAccelGroup*, GObject*, guint keyval, GdkModifierType, gpointer user_data) {
    int idx = 0;
    if (keyval == GDK_KEY_2) idx = 1;
    else if (keyval == GDK_KEY_3) idx = 2;
    else if (keyval == GDK_KEY_4) idx = 3;
    static_cast<GuiApp*>(user_data)->SwitchToTab(idx);
  };
  const guint tab_keys[] = {GDK_KEY_1, GDK_KEY_2, GDK_KEY_3, GDK_KEY_4};
  for (int i = 0; i < 4; ++i) {
    gtk_accel_group_connect(
        accel, tab_keys[i], GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
        g_cclosure_new(G_CALLBACK(accel_tab), this, nullptr));
  }

  // Ctrl+Q -> Quit
  auto accel_quit = +[](GtkAccelGroup*, GObject*, guint, GdkModifierType, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->Quit();
  };
  gtk_accel_group_connect(
      accel, GDK_KEY_q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(accel_quit), this, nullptr));

  // F1 -> About
  auto accel_about = +[](GtkAccelGroup*, GObject*, guint, GdkModifierType, gpointer user_data) {
    auto* self = static_cast<GuiApp*>(user_data);
    const char* authors[] = {"CastMirror Maintainers", nullptr};
    gtk_show_about_dialog(
        self->GetWindow(),
        "program-name", copy::kAppTitle,
        "version", "1.0.0",
        "comments", copy::kAboutComments,
        "license", copy::kAboutLicense,
        "website", "https://github.com/vindeckyy/CastMirror",
        "authors", authors,
        nullptr);
  };
  gtk_accel_group_connect(
      accel, GDK_KEY_F1, static_cast<GdkModifierType>(0), GTK_ACCEL_VISIBLE,
      g_cclosure_new(G_CALLBACK(accel_about), this, nullptr));

  gtk_window_add_accel_group(GTK_WINDOW(window_), accel);
}

void GuiApp::SetupEngineCallbacks() {
  auto& engine = CastEngine::Instance();

  engine.SetOnStateChanged([this](SessionState old_s, SessionState new_s, const std::string& msg) {
    (void)old_s;
    auto* payload = new StatePayload{new_s, msg};
    g_idle_add(+[](gpointer p) -> gboolean {
      auto* pl = static_cast<StatePayload*>(p);
      if (pl) {
        // Find GuiApp instance from context
        auto* app = static_cast<GuiApp*>(g_object_get_data(G_OBJECT(gdk_screen_get_default()), "castmirror_app"));
        if (app) {
          app->UpdateStateUi(pl->state, pl->message);
        }
        delete pl;
      }
      return G_SOURCE_REMOVE;
    }, payload);
  });

  engine.GetDiscovery().SetCallback([this](const std::vector<CastDevice>&) {
    g_idle_add(+[](gpointer user_data) -> gboolean {
      auto* self = static_cast<GuiApp*>(user_data);
      if (self->cast_tab_) {
        self->cast_tab_->RefreshDevices();
      }
      return G_SOURCE_REMOVE;
    }, this);
  });

  g_object_set_data(G_OBJECT(gdk_screen_get_default()), "castmirror_app", this);
}

void GuiApp::SetupLoggerCallback() {
  Logger::Instance().SetCallback([this](LogLevel level, const std::string& formatted_line) {
    if (logs_tab_) {
      logs_tab_->OnLogMessage(level, formatted_line);
    }
  });
}

void GuiApp::SwitchToTab(int tab_index) {
  if (notebook_) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), tab_index);
  }
}

void GuiApp::SyncAudioEnabled(bool enabled) {
  if (cast_tab_) cast_tab_->SyncAudioSwitch(enabled);
  if (settings_tab_) settings_tab_->SyncAudioSwitch(enabled);
}

void GuiApp::SyncBitrateSlider(uint32_t kbps) {
  if (settings_tab_) settings_tab_->SyncBitrateSlider(kbps);
}

void GuiApp::TriggerRescan() {
  if (rescan_timer_id_ != 0) {
    return;
  }
  LOG_INFO << "[UI] Rescanning for Cast devices via mDNS...";
  CastEngine::Instance().GetDiscovery().TriggerScan();
  rescan_timer_id_ = g_timeout_add(1500, +[](gpointer user_data) -> gboolean {
    auto* app = static_cast<GuiApp*>(user_data);
    app->rescan_timer_id_ = 0;
    app->GetCastTab()->RefreshDevices();
    return G_SOURCE_REMOVE;
  }, this);
}

void GuiApp::TriggerCastAction() {
  auto& engine = CastEngine::Instance();

  if (current_state_ == SessionState::kStreaming || current_state_ == SessionState::kConnecting ||
      current_state_ == SessionState::kNegotiating || current_state_ == SessionState::kReconnecting) {
    LOG_INFO << "[UI] User requested Stop Casting";
    engine.StopCasting();
    return;
  }

  std::string device_id = cast_tab_->GetSelectedDeviceId();
  if (device_id.empty()) {
    GtkWidget* dialog = gtk_message_dialog_new(
        GetWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
        "Please select a target TV or add one by IP first.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  int display_id = cast_tab_->GetSelectedDisplayId();
  QualityPreset preset = cast_tab_->GetSelectedPreset();
  bool audio_on = cast_tab_->GetAudioEnabled();

  const auto& cfg = ConfigStore::Instance().Get();
  uint32_t bitrate_kbps = cfg.GetPresetBitrateKbps(preset);

  SessionOptions opts;
  opts.preset = preset;
  opts.enable_audio = audio_on;
  opts.video_bitrate_kbps = bitrate_kbps;
  opts.audio_bitrate_bps = cfg.audio_bitrate_bps;
  opts.capture_fps = cfg.capture_fps;
  opts.target_delay_ms = cfg.target_delay_ms;
  opts.silence_host_speakers = cfg.silence_host_speakers;
  opts.adaptive_enabled = cfg.adaptive_enabled;

  LOG_INFO << "[UI] Starting Cast Session to device id " << device_id << "...";

  std::thread([this, device_id, display_id, opts]() {
    bool ok = CastEngine::Instance().StartCasting(device_id, display_id, opts);
    if (!ok) {
      g_idle_add(+[](gpointer user_data) -> gboolean {
        auto* self = static_cast<GuiApp*>(user_data);
        std::string last_err = CastEngine::Instance().GetLastError();
        if (last_err.empty()) {
          last_err = CastEngine::Instance().GetStateMachine().GetLastMessage();
        }
        if (last_err.empty()) {
          last_err = "The TV could not be reached. Ensure it is powered on and on the same Wi-Fi.";
        }

        GtkWidget* dialog = gtk_message_dialog_new(
            self->GetWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Could not start Cast.\n\n%s", last_err.c_str());
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return G_SOURCE_REMOVE;
      }, this);
    }
  }).detach();
}

void GuiApp::UpdateStateUi(SessionState new_state, const std::string& message) {
  current_state_ = new_state;
  last_state_message_ = message;

  // Fallback to StateMachine last message if empty
  std::string display_msg = message;
  if (display_msg.empty()) {
    display_msg = CastEngine::Instance().GetStateMachine().GetLastMessage();
  }

  // Update Status Badge
  UpdateStatusBadge(status_badge_, status_badge_lbl_, new_state);

  // Update Header Subtitle
  if (new_state == SessionState::kIdle || new_state == SessionState::kReady || new_state == SessionState::kDiscovering) {
    gtk_label_set_text(GTK_LABEL(header_subtitle_), display_msg.empty() ? copy::kReadySubtitle : display_msg.c_str());
  } else {
    gtk_label_set_text(GTK_LABEL(header_subtitle_), display_msg.c_str());
  }

  // Update Footer Button and Spinner
  GtkStyleContext* btn_ctx = gtk_widget_get_style_context(cast_button_);
  gtk_style_context_remove_class(btn_ctx, "btn-cast-start");
  gtk_style_context_remove_class(btn_ctx, "btn-cast-stop");

  switch (new_state) {
    case SessionState::kStreaming:
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(cast_button_lbl_), "Stop casting");
      gtk_spinner_stop(GTK_SPINNER(spinner_));
      gtk_widget_set_visible(spinner_, FALSE);
      gtk_widget_set_sensitive(cast_button_, TRUE);
      SwitchToTab(1); // Auto switch to Live tab
      break;

    case SessionState::kConnecting:
    case SessionState::kNegotiating:
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(cast_button_lbl_), "Cancel");
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      gtk_widget_set_sensitive(cast_button_, TRUE);
      SwitchToTab(1); // Auto switch to Live tab
      break;

    case SessionState::kReconnecting:
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(cast_button_lbl_), "Stop casting");
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      gtk_widget_set_sensitive(cast_button_, TRUE);
      SwitchToTab(1); // Auto switch to Live tab
      break;

    case SessionState::kStopping:
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(cast_button_lbl_), "Stopping...");
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      gtk_widget_set_sensitive(cast_button_, FALSE);
      break;

    case SessionState::kFailed:
    case SessionState::kIdle:
    case SessionState::kReady:
    case SessionState::kDiscovering:
    default:
      gtk_style_context_add_class(btn_ctx, "btn-cast-start");
      gtk_label_set_text(GTK_LABEL(cast_button_lbl_), "Cast display");
      gtk_spinner_stop(GTK_SPINNER(spinner_));
      gtk_widget_set_visible(spinner_, FALSE);
      gtk_widget_set_sensitive(cast_button_, TRUE);
      break;
  }

  // Update tabs
  if (cast_tab_) cast_tab_->UpdateSessionState(new_state, display_msg);
  if (live_tab_) live_tab_->UpdateSessionState(new_state, display_msg);
  if (settings_tab_) settings_tab_->UpdateSessionState(new_state);

  // Desktop Notifications
  if (ConfigStore::Instance().Get().notify_on_events) {
    NotificationManager::NotifyStateChange(new_state, last_device_name_, display_msg);
  }

  // Tray Icon update
  if (tray_manager_) {
    tray_manager_->UpdateState(new_state, last_device_name_);
  }
}

gboolean GuiApp::OnStatsTimer(gpointer user_data) {
  auto* self = static_cast<GuiApp*>(user_data);
  if (self->current_state_ == SessionState::kStreaming) {
    StreamStats stats = CastEngine::Instance().GetStats();
    if (!stats.device_name.empty()) {
      self->last_device_name_ = stats.device_name;
    }
    if (self->live_tab_) {
      self->live_tab_->UpdateStats(stats);
    }
  }
  return G_SOURCE_CONTINUE;
}

gboolean GuiApp::OnConfigureEvent(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data) {
  (void)widget;
  auto* self = static_cast<GuiApp*>(user_data);
  if (self->resize_debounce_timer_ != 0) {
    g_source_remove(self->resize_debounce_timer_);
  }

  int w = event->width;
  int h = event->height;

  self->resize_debounce_timer_ = g_timeout_add(400, +[](gpointer data) -> gboolean {
    auto* pair = static_cast<std::pair<int, int>*>(data);
    auto& c = ConfigStore::Instance().Mutable();
    c.window_width = pair->first;
    c.window_height = pair->second;
    ConfigStore::Instance().Save();
    delete pair;
    return G_SOURCE_REMOVE;
  }, new std::pair<int, int>(w, h));

  return FALSE;
}

gboolean GuiApp::OnDeleteEvent(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
  (void)widget;
  (void)event;
  auto* self = static_cast<GuiApp*>(user_data);
  const auto& cfg = ConfigStore::Instance().Get();

  if (cfg.enable_tray_on_startup && cfg.close_to_tray && self->tray_manager_ && self->tray_manager_->IsCreated()) {
    gtk_widget_hide(self->window_);
    return TRUE; // Do not destroy window
  }

  self->Quit();
  return TRUE;
}

void GuiApp::Run() {
  gtk_widget_show_all(window_);
  ShowFirstRunAssistantIfNeeded(GTK_WINDOW(window_));
  gtk_main();
}

void GuiApp::Quit() {
  if (is_quitting_) return;
  is_quitting_ = true;

  LOG_INFO << "[UI] Shutting down CastMirror application...";
  Logger::Instance().SetCallback(nullptr);
  CastEngine::Instance().StopCasting();
  CastEngine::Instance().Shutdown();
  NotificationManager::Shutdown();

  if (tray_manager_) {
    tray_manager_->DestroyIndicator();
  }

  gtk_main_quit();
}

}  // namespace castcore::gui
