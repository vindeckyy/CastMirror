#include "gui_app.h"

#include "cast_tab.h"
#include "css_loader.h"
#include "first_run.h"
#include "help_copy.h"
#include "live_tab.h"
#include "logs_tab.h"
#include "notify.h"
#include "settings_tab.h"
#include "tray.h"
#include "widgets.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"

#include <algorithm>
#include <memory>
#include <cstdlib>
#include <thread>

#ifndef CASTMIRROR_VERSION
#define CASTMIRROR_VERSION "0.0.0"
#endif

namespace castcore::gui {

namespace {

struct StatePayload {
  GuiApp* app = nullptr;
  SessionState state = SessionState::kIdle;
  std::string message;
};

struct StartFailPayload {
  GuiApp* app = nullptr;
  std::string err;
};

bool IsGenericIdleStopMessage(const std::string& msg) {
  return msg.empty() || msg == "Cast Stopped";
}

std::string GetConfigDir() {
  const char* home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/.config/castmirror";
}

std::string TruncateStatus(const std::string& text) {
  if (text.size() <= 32) {
    return text;
  }
  return text.substr(0, 31) + "…";
}

bool SessionBlocksCastControls(SessionState state) {
  return state == SessionState::kConnecting ||
         state == SessionState::kNegotiating ||
         state == SessionState::kStreaming ||
         state == SessionState::kReconnecting ||
         state == SessionState::kStopping;
}

}  // namespace

GuiApp::GuiApp(AdwApplication* application) : application_(application) {
  tray_manager_ = std::make_unique<TrayManager>(this);
  BuildUi();
  SetupActions();
  SetupEngineCallbacks();
  SetupLoggerCallback();

  if (ConfigStore::Instance().Get().enable_tray_on_startup) {
    tray_manager_->CreateIndicator();
  }

  stats_timer_id_ = g_timeout_add(500, OnStatsTimer, this);

  g_object_set_data(G_OBJECT(window_), "castmirror-gui-app", this);

  scan_in_progress_ = true;
  if (cast_tab_) {
    cast_tab_->SetScanInProgress(true);
    cast_tab_->RefreshDevices();
    cast_tab_->RefreshDisplays();
  }
  OnDestinationSelectionChanged();
  TriggerRescan();
}

GuiApp::~GuiApp() {
  Shutdown();
}

void GuiApp::BuildUi() {
  const auto& cfg = ConfigStore::Instance().Get();

  window_ = adw_application_window_new(GTK_APPLICATION(application_));
  gtk_window_set_title(GTK_WINDOW(window_), copy::kAppTitle);
  gtk_window_set_icon_name(GTK_WINDOW(window_), "io.github.vindeckyy.CastMirror");
  gtk_widget_add_css_class(window_, "cm-window");

  int init_w = std::clamp(cfg.window_width, 760, 1600);
  int init_h = std::clamp(cfg.window_height, 560, 1200);
  gtk_window_set_default_size(GTK_WINDOW(window_), init_w, init_h);
  gtk_widget_set_size_request(GTK_WIDGET(window_), 760, 560);

  g_signal_connect(window_, "close-request", G_CALLBACK(OnCloseRequest), this);

  toast_overlay_ = adw_toast_overlay_new();
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window_), toast_overlay_);

  GtkWidget* toolbar_view = adw_toolbar_view_new();
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(toolbar_view), ADW_TOOLBAR_RAISED);
  adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar_view), ADW_TOOLBAR_RAISED);
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toast_overlay_), toolbar_view);

  GtkWidget* header_bar = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header_bar), FALSE);
  // Desktop gtk-decoration-layout includes "icon,menu", which would draw a 16px
  // window icon beside our packed 48px brand logo. Keep native window buttons only.
  adw_header_bar_set_decoration_layout(ADW_HEADER_BAR(header_bar), ":minimize,maximize,close");

  GtkWidget* brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_valign(brand, GTK_ALIGN_CENTER);
  GtkWidget* logo = gtk_image_new_from_resource("/io/github/vindeckyy/CastMirror/logo.svg");
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 48);
  gtk_widget_add_css_class(logo, "cm-header-logo");
  gtk_widget_set_valign(logo, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(brand), logo);

  header_title_ = adw_window_title_new(copy::kAppTitle, copy::kAppSubtitleDefault);
  gtk_widget_add_css_class(header_title_, "cm-header-title");
  gtk_widget_set_valign(header_title_, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(brand), header_title_);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar), brand);

  GtkWidget* menu_btn = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
  gtk_widget_set_tooltip_text(menu_btn, "Application menu");
  gtk_accessible_update_property(GTK_ACCESSIBLE(menu_btn),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Application menu",
                                 -1);

  GMenu* menu = g_menu_new();
  g_menu_append(menu, "About CastMirror", "app.about");
  g_menu_append(menu, "Open config folder", "app.open-config");
  g_menu_append(menu, "Open log folder", "app.open-logs");
  GMenu* quit_section = g_menu_new();
  g_menu_append(quit_section, "Quit", "app.quit");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(quit_section));
  g_object_unref(quit_section);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
  g_object_unref(menu);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar), menu_btn);

  status_badge_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(status_badge_, "cm-status-pill");
  gtk_widget_add_css_class(status_badge_, "is-idle");
  status_badge_dot_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(status_badge_dot_, "cm-status-dot");
  gtk_widget_add_css_class(status_badge_dot_, "is-idle");
  gtk_widget_set_valign(status_badge_dot_, GTK_ALIGN_CENTER);
  status_badge_lbl_ = gtk_label_new("Ready");
  gtk_box_append(GTK_BOX(status_badge_), status_badge_dot_);
  gtk_box_append(GTK_BOX(status_badge_), status_badge_lbl_);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar), status_badge_);

  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);

  GtkWidget* switcher_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(switcher_box, "toolbar");
  view_switcher_ = adw_view_switcher_new();
  gtk_widget_add_css_class(view_switcher_, "cm-view-switcher");
  adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(view_switcher_), ADW_VIEW_SWITCHER_POLICY_WIDE);
  gtk_widget_set_hexpand(view_switcher_, TRUE);
  gtk_widget_set_halign(view_switcher_, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(switcher_box), view_switcher_);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), switcher_box);

  view_stack_ = adw_view_stack_new();
  adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(view_switcher_), ADW_VIEW_STACK(view_stack_));
  g_signal_connect(view_stack_, "notify::visible-child", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->UpdateViewLiveVisibility();
  }), this);

  cast_tab_ = std::make_unique<CastTab>(this);
  live_tab_ = std::make_unique<LiveTab>(this);
  settings_tab_ = std::make_unique<SettingsTab>(this);
  logs_tab_ = std::make_unique<LogsTab>(this);

  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_),
                                      cast_tab_->GetRootWidget(),
                                      "cast",
                                      "Cast",
                                      "castmirror-signal-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_),
                                      live_tab_->GetRootWidget(),
                                      "live",
                                      "Live session",
                                      "media-record-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_),
                                      settings_tab_->GetRootWidget(),
                                      "settings",
                                      "Settings",
                                      "preferences-system-symbolic");
  adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(view_stack_),
                                      logs_tab_->GetRootWidget(),
                                      "logs",
                                      "Logs",
                                      "text-x-generic-symbolic");

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), view_stack_);

  GtkWidget* bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(bottom_bar, "toolbar");
  gtk_widget_add_css_class(bottom_bar, "cm-bottom-bar");
  gtk_widget_set_size_request(bottom_bar, -1, 64);

  spinner_ = gtk_spinner_new();
  gtk_widget_set_visible(spinner_, FALSE);
  gtk_box_append(GTK_BOX(bottom_bar), spinner_);

  footer_status_label_ = gtk_label_new("Select a Cast display to continue");
  gtk_widget_set_halign(footer_status_label_, GTK_ALIGN_START);
  gtk_widget_set_hexpand(footer_status_label_, TRUE);
  gtk_label_set_single_line_mode(GTK_LABEL(footer_status_label_), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(footer_status_label_), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(footer_status_label_), 32);
  gtk_box_append(GTK_BOX(bottom_bar), footer_status_label_);

  view_live_btn_ = gtk_button_new_with_label("View live session");
  gtk_widget_add_css_class(view_live_btn_, "flat");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(view_live_btn_), "win.page");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(view_live_btn_),
                                         g_variant_new_string("live"));
  gtk_widget_set_visible(view_live_btn_, FALSE);
  gtk_box_append(GTK_BOX(bottom_bar), view_live_btn_);

  cast_button_ = gtk_button_new();
  gtk_widget_add_css_class(cast_button_, "suggested-action");
  gtk_widget_add_css_class(cast_button_, "pill");
  gtk_widget_add_css_class(cast_button_, "cm-primary-action");
  gtk_widget_set_size_request(cast_button_, 220, 40);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(cast_button_), "win.cast");

  GtkWidget* cast_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(cast_inner, GTK_ALIGN_CENTER);
  cast_button_icon_ = gtk_image_new_from_icon_name("castmirror-signal-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(cast_button_icon_), 16);
  cast_button_lbl_ = gtk_label_new("Cast display");
  gtk_box_append(GTK_BOX(cast_inner), cast_button_icon_);
  gtk_box_append(GTK_BOX(cast_inner), cast_button_lbl_);
  gtk_button_set_child(GTK_BUTTON(cast_button_), cast_inner);
  gtk_box_append(GTK_BOX(bottom_bar), cast_button_);

  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar_view), bottom_bar);

  UpdateStatusBadge(status_badge_, status_badge_dot_, status_badge_lbl_, SessionState::kReady);
}

void GuiApp::SetupActions() {
  auto add_app_action = [this](const char* name, GCallback callback) {
    GSimpleAction* action = g_simple_action_new(name, nullptr);
    g_signal_connect(action, "activate", callback, this);
    g_action_map_add_action(G_ACTION_MAP(application_), G_ACTION(action));
    g_object_unref(action);
  };

  add_app_action("present", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->Present();
  }));
  add_app_action("about", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->ShowAboutDialog();
  }));
  add_app_action("open-config", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->OpenConfigFolder();
  }));
  add_app_action("open-logs", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->OpenLogsFolder();
  }));
  add_app_action("quit", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->Quit();
  }));

  action_cast_ = g_simple_action_new("cast", nullptr);
  g_signal_connect(action_cast_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->TriggerCastAction();
  }), this);
  g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action_cast_));

  action_rescan_ = g_simple_action_new("rescan", nullptr);
  g_signal_connect(action_rescan_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->TriggerRescan();
  }), this);
  g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action_rescan_));

  action_add_ip_ = g_simple_action_new("add-ip", nullptr);
  g_signal_connect(action_add_ip_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    auto* self = static_cast<GuiApp*>(user_data);
    if (self->cast_tab_) {
      self->cast_tab_->OnAddIpClicked();
    }
  }), this);
  g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action_add_ip_));

  action_remove_device_ = g_simple_action_new("remove-device", nullptr);
  g_signal_connect(action_remove_device_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer user_data) {
    auto* self = static_cast<GuiApp*>(user_data);
    if (self->cast_tab_) {
      self->cast_tab_->RemoveSelectedDevice();
    }
  }), this);
  g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action_remove_device_));

  action_page_ = g_simple_action_new("page", G_VARIANT_TYPE_STRING);
  g_signal_connect(action_page_, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant* parameter, gpointer user_data) {
    if (!parameter) {
      return;
    }
    static_cast<GuiApp*>(user_data)->OnPageAction(g_variant_get_string(parameter, nullptr));
  }), this);
  g_action_map_add_action(G_ACTION_MAP(window_), G_ACTION(action_page_));

  GtkApplication* gtk_app = GTK_APPLICATION(application_);
  const char* cast_accels[] = {"<Control>Return", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.cast", cast_accels);
  const char* rescan_accels[] = {"<Control>r", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.rescan", rescan_accels);
  const char* page_cast[] = {"<Control>1", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.page::cast", page_cast);
  const char* page_live[] = {"<Control>2", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.page::live", page_live);
  const char* page_settings[] = {"<Control>3", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.page::settings", page_settings);
  const char* page_logs[] = {"<Control>4", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "win.page::logs", page_logs);
  const char* quit_accels[] = {"<Control>q", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "app.quit", quit_accels);
  const char* about_accels[] = {"F1", nullptr};
  gtk_application_set_accels_for_action(gtk_app, "app.about", about_accels);

  RefreshWindowActionSensitivity();
}

void GuiApp::SetupEngineCallbacks() {
  auto& engine = CastEngine::Instance();

  engine.SetOnStateChanged([this](SessionState, SessionState new_s, const std::string& msg) {
    auto* payload = new StatePayload{this, new_s, msg};
    g_main_context_invoke(nullptr, +[](gpointer p) -> gboolean {
      std::unique_ptr<StatePayload> pl(static_cast<StatePayload*>(p));
      if (pl->app && !pl->app->is_quitting_) {
        pl->app->UpdateStateUi(pl->state, pl->message);
      }
      return G_SOURCE_REMOVE;
    }, payload);
  });

  engine.GetDiscovery().SetCallback([this](const std::vector<CastDevice>&) {
    g_main_context_invoke(nullptr, +[](gpointer user_data) -> gboolean {
      auto* self = static_cast<GuiApp*>(user_data);
      if (!self->is_quitting_ && self->cast_tab_) {
        self->cast_tab_->RefreshDevices();
      }
      return G_SOURCE_REMOVE;
    }, this);
  });
}

void GuiApp::SetupLoggerCallback() {
  Logger::Instance().SetCallback([this](LogLevel level, const std::string& formatted_line) {
    if (logs_tab_ && !is_quitting_) {
      logs_tab_->OnLogMessage(level, formatted_line);
    }
  });
}

void GuiApp::Present() {
  if (!window_ || is_quitting_) {
    return;
  }
  gtk_widget_set_visible(window_, TRUE);
  gtk_window_present(GTK_WINDOW(window_));

  if (!first_presented_) {
    first_presented_ = true;
    if (!ConfigStore::Instance().Get().first_run_complete) {
      ShowFirstRunDialogIfNeeded(ADW_APPLICATION_WINDOW(window_));
    }
  }
}

void GuiApp::Quit() {
  if (is_quitting_) {
    return;
  }
  SaveWindowGeometry();
  if (application_) {
    g_application_quit(G_APPLICATION(application_));
  }
}

void GuiApp::Shutdown() {
  if (shutdown_done_) {
    return;
  }
  shutdown_done_ = true;
  is_quitting_ = true;

  SaveWindowGeometry();
  LOG_INFO << "[UI] Shutting down CastMirror application...";
  Logger::Instance().SetCallback(nullptr);

  if (stats_timer_id_ != 0) {
    g_source_remove(stats_timer_id_);
    stats_timer_id_ = 0;
  }
  if (rescan_timer_id_ != 0) {
    g_source_remove(rescan_timer_id_);
    rescan_timer_id_ = 0;
  }

  CastEngine::Instance().StopCasting();
  CastEngine::Instance().Shutdown();

  if (tray_manager_) {
    tray_manager_->DestroyIndicator();
  }
  NotificationManager::Shutdown();
}

void GuiApp::SwitchToPage(const char* page_name) {
  if (!view_stack_ || !page_name) {
    return;
  }
  adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(view_stack_), page_name);
  UpdateViewLiveVisibility();
}

void GuiApp::OnPageAction(const char* page_name) {
  SwitchToPage(page_name);
}

void GuiApp::SyncAudioEnabled(bool enabled) {
  if (settings_tab_) {
    settings_tab_->SyncAudioSwitch(enabled);
  }
}

void GuiApp::SyncSilenceHost(bool enabled) {
  if (settings_tab_) {
    settings_tab_->SyncSilenceSwitch(enabled);
  }
}

void GuiApp::SyncBitrateSlider(uint32_t kbps) {
  if (settings_tab_) {
    settings_tab_->SyncBitrateSlider(kbps);
  }
}

void GuiApp::ShowToast(const std::string& title) {
  if (!toast_overlay_ || title.empty()) {
    return;
  }
  AdwToast* toast = adw_toast_new(title.c_str());
  adw_toast_set_timeout(toast, 3);
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(toast_overlay_), toast);
}

void GuiApp::PushModalActionBlock() {
  ++modal_action_block_count_;
  RefreshWindowActionSensitivity();
}

void GuiApp::PopModalActionBlock() {
  if (modal_action_block_count_ > 0) {
    --modal_action_block_count_;
  }
  RefreshWindowActionSensitivity();
}

void GuiApp::OnDestinationSelectionChanged() {
  if (!IsSessionActive()) {
    std::string footer;
    if (!cast_tab_ || cast_tab_->GetSelectedDeviceId().empty()) {
      footer = "Select a Cast display to continue";
    } else if (!cast_tab_->HasSelectedDisplay()) {
      footer = "Select a screen to continue";
    } else {
      footer = cast_tab_->GetSelectedDeviceName() + " · " + cast_tab_->GetSelectedDisplayName();
    }
    SetFooterStatus(footer);
  }
  RefreshWindowActionSensitivity();
}

void GuiApp::RefreshWindowActionSensitivity() {
  const bool blocked = modal_action_block_count_ > 0;
  const bool session_active = IsSessionActive();
  const bool stopping = current_state_ == SessionState::kStopping;
  const bool has_device = cast_tab_ && !cast_tab_->GetSelectedDeviceId().empty();
  const bool has_display = cast_tab_ && cast_tab_->HasSelectedDisplay();
  const bool can_cast_idle = has_device && has_display;

  bool cast_enabled = false;
  if (!blocked && !stopping) {
    if (session_active) {
      cast_enabled = true;
    } else {
      cast_enabled = can_cast_idle;
    }
  }

  if (action_cast_) {
    g_simple_action_set_enabled(action_cast_, cast_enabled);
  }
  if (action_rescan_) {
    g_simple_action_set_enabled(action_rescan_, !blocked && rescan_timer_id_ == 0);
  }
  if (action_add_ip_) {
    g_simple_action_set_enabled(action_add_ip_, !blocked);
  }
  if (action_remove_device_) {
    g_simple_action_set_enabled(action_remove_device_,
                                !blocked && cast_tab_ && cast_tab_->IsSelectedDeviceRemovable());
  }
  if (action_page_) {
    g_simple_action_set_enabled(action_page_, !blocked);
  }
}

void GuiApp::TriggerRescan() {
  if (rescan_timer_id_ != 0) {
    return;
  }
  scan_in_progress_ = true;
  if (cast_tab_) {
    cast_tab_->SetScanInProgress(true);
  }
  ShowToast("Scanning for Cast displays…");
  LOG_INFO << "[UI] Rescanning for Cast devices via mDNS...";
  CastEngine::Instance().GetDiscovery().TriggerScan();
  if (!IsSessionActive()) {
    UpdateStateUi(current_state_, last_state_message_);
  }
  RefreshWindowActionSensitivity();

  rescan_timer_id_ = g_timeout_add(1500, +[](gpointer user_data) -> gboolean {
    auto* app = static_cast<GuiApp*>(user_data);
    app->rescan_timer_id_ = 0;
    app->scan_in_progress_ = false;
    if (!app->is_quitting_ && app->cast_tab_) {
      app->cast_tab_->SetScanInProgress(false);
      app->cast_tab_->RefreshDevices();
    }
    if (!app->is_quitting_ && !app->IsSessionActive()) {
      app->UpdateStateUi(app->current_state_, app->last_state_message_);
    }
    app->RefreshWindowActionSensitivity();
    return G_SOURCE_REMOVE;
  }, this);
}

void GuiApp::TriggerCastAction() {
  if (modal_action_block_count_ > 0) {
    return;
  }
  auto& engine = CastEngine::Instance();

  if (current_state_ == SessionState::kStreaming || current_state_ == SessionState::kConnecting ||
      current_state_ == SessionState::kNegotiating || current_state_ == SessionState::kReconnecting) {
    LOG_INFO << "[UI] User requested Stop Casting";
    engine.StopCasting();
    return;
  }
  if (current_state_ == SessionState::kStopping) {
    return;
  }

  std::string device_id = cast_tab_ ? cast_tab_->GetSelectedDeviceId() : std::string();
  if (device_id.empty()) {
    PresentAlert("Choose a Cast display", "Select a TV or add one by IP before casting.");
    return;
  }
  if (!cast_tab_ || !cast_tab_->HasSelectedSource()) {
    PresentAlert("Choose what to share", "Select a screen or window before casting.");
    return;
  }

  CaptureSource source = cast_tab_->GetSelectedSource();
  QualityPreset preset = cast_tab_->GetSelectedPreset();
  bool audio_on = cast_tab_->GetAudioEnabled();
  last_device_name_ = cast_tab_->GetSelectedDeviceName();
  active_session_audio_enabled_ = audio_on;

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

  LOG_INFO << "[UI] Starting Cast Session to device id " << device_id
           << " (source: " << CaptureSourceKindToString(source.kind) << " id=" << source.id << ")...";

  std::thread([this, device_id, source, opts]() {
    bool ok = CastEngine::Instance().StartCasting(device_id, source, opts);
    if (ok) {
      return;
    }
    auto* payload = new StartFailPayload();
    payload->app = this;
    payload->err = CastEngine::Instance().GetLastError();
    if (payload->err.empty()) {
      payload->err = CastEngine::Instance().GetStateMachine().GetLastMessage();
    }
    g_main_context_invoke(nullptr, +[](gpointer user_data) -> gboolean {
      std::unique_ptr<StartFailPayload> pl(static_cast<StartFailPayload*>(user_data));
      auto* self = pl->app;
      if (!self || self->is_quitting_) {
        return G_SOURCE_REMOVE;
      }
      std::string last_err = self->last_failed_message_;
      if (IsGenericIdleStopMessage(last_err)) {
        last_err = pl->err;
      }
      if (IsGenericIdleStopMessage(last_err)) {
        last_err = "The TV could not be reached. Ensure it is powered on and on the same Wi-Fi.";
      }
      if (self->live_tab_) {
        self->live_tab_->UpdateSessionState(SessionState::kFailed, last_err);
      }
      self->PresentAlert("Could not start casting", last_err.c_str());
      return G_SOURCE_REMOVE;
    }, payload);
  }).detach();
}

void GuiApp::UpdateStateUi(SessionState new_state, const std::string& message) {
  current_state_ = new_state;
  last_state_message_ = message;

  std::string display_msg = message;
  if (display_msg.empty()) {
    display_msg = CastEngine::Instance().GetStateMachine().GetLastMessage();
  }
  if (new_state == SessionState::kFailed && !IsGenericIdleStopMessage(display_msg)) {
    last_failed_message_ = display_msg;
  } else if (new_state == SessionState::kConnecting) {
    last_failed_message_.clear();
  }

  const bool session_active = IsSessionActive();
  const bool scanning_idle = scan_in_progress_ && !session_active;

  if (scanning_idle) {
    gtk_widget_remove_css_class(status_badge_, "is-idle");
    gtk_widget_remove_css_class(status_badge_, "is-progress");
    gtk_widget_remove_css_class(status_badge_, "is-live");
    gtk_widget_remove_css_class(status_badge_, "is-warning");
    gtk_widget_remove_css_class(status_badge_, "is-error");
    gtk_widget_add_css_class(status_badge_, "is-progress");
    gtk_widget_remove_css_class(status_badge_dot_, "is-idle");
    gtk_widget_remove_css_class(status_badge_dot_, "is-progress");
    gtk_widget_remove_css_class(status_badge_dot_, "is-live");
    gtk_widget_remove_css_class(status_badge_dot_, "is-warning");
    gtk_widget_remove_css_class(status_badge_dot_, "is-error");
    gtk_widget_add_css_class(status_badge_dot_, "is-progress");
    gtk_label_set_text(GTK_LABEL(status_badge_lbl_), "Finding devices");
  } else {
    UpdateStatusBadge(status_badge_, status_badge_dot_, status_badge_lbl_, new_state);
  }

  const char* subtitle = copy::kAppSubtitleDefault;
  if (new_state == SessionState::kIdle || new_state == SessionState::kReady ||
      new_state == SessionState::kDiscovering) {
    subtitle = display_msg.empty() ? copy::kReadySubtitle : display_msg.c_str();
  } else if (!display_msg.empty()) {
    subtitle = display_msg.c_str();
  }
  adw_window_title_set_subtitle(ADW_WINDOW_TITLE(header_title_), subtitle);

  switch (new_state) {
    case SessionState::kStreaming:
      ApplyPrimaryAction("Stop casting", "media-playback-stop-symbolic", true, true);
      gtk_spinner_stop(GTK_SPINNER(spinner_));
      gtk_widget_set_visible(spinner_, FALSE);
      SwitchToPage("live");
      SetFooterStatus(last_device_name_.empty() ? "Live" : ("Casting to " + last_device_name_));
      break;
    case SessionState::kConnecting:
      ApplyPrimaryAction("Cancel", "process-stop-symbolic", true, true);
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      SwitchToPage("live");
      SetFooterStatus(last_device_name_.empty() ? "Connecting" : ("Connecting to " + last_device_name_));
      break;
    case SessionState::kNegotiating:
      ApplyPrimaryAction("Cancel", "process-stop-symbolic", true, true);
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      SetFooterStatus(display_msg.empty() ? "Starting stream" : display_msg);
      break;
    case SessionState::kReconnecting:
      ApplyPrimaryAction("Stop casting", "media-playback-stop-symbolic", true, true);
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      SwitchToPage("live");
      SetFooterStatus(display_msg.empty() ? "Reconnecting" : display_msg);
      break;
    case SessionState::kStopping:
      ApplyPrimaryAction("Stopping…", "media-playback-stop-symbolic", true, false);
      gtk_widget_set_visible(spinner_, TRUE);
      gtk_spinner_start(GTK_SPINNER(spinner_));
      SetFooterStatus("Stopping");
      break;
    case SessionState::kFailed:
    case SessionState::kIdle:
    case SessionState::kReady:
    case SessionState::kDiscovering:
    default:
      ApplyPrimaryAction("Cast display", "castmirror-signal-symbolic", false, true);
      gtk_spinner_stop(GTK_SPINNER(spinner_));
      gtk_widget_set_visible(spinner_, FALSE);
      active_session_audio_enabled_ = false;
      OnDestinationSelectionChanged();
      break;
  }

  if (cast_tab_) {
    cast_tab_->UpdateSessionState(new_state, display_msg);
  }
  if (live_tab_) {
    live_tab_->UpdateSessionState(new_state, display_msg);
  }
  if (settings_tab_) {
    settings_tab_->UpdateSessionState(new_state);
  }

  if (ConfigStore::Instance().Get().notify_on_events) {
    NotificationManager::NotifyStateChange(new_state, last_device_name_, display_msg);
  }
  if (tray_manager_) {
    tray_manager_->UpdateState(new_state, last_device_name_);
  }

  UpdateViewLiveVisibility();
  RefreshWindowActionSensitivity();
}

gboolean GuiApp::OnStatsTimer(gpointer user_data) {
  auto* self = static_cast<GuiApp*>(user_data);
  if (!self || self->is_quitting_) {
    return G_SOURCE_REMOVE;
  }
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

gboolean GuiApp::OnCloseRequest(GtkWindow* /*window*/, gpointer user_data) {
  auto* self = static_cast<GuiApp*>(user_data);
  self->SaveWindowGeometry();
  const auto& cfg = ConfigStore::Instance().Get();
  if (cfg.enable_tray_on_startup && cfg.close_to_tray && self->tray_manager_ &&
      self->tray_manager_->CanHideToTray()) {
    gtk_widget_set_visible(self->window_, FALSE);
    return TRUE;
  }
  self->Quit();
  return TRUE;
}

void GuiApp::SaveWindowGeometry() {
  if (!window_) {
    return;
  }
  int width = 0;
  int height = 0;
  gtk_window_get_default_size(GTK_WINDOW(window_), &width, &height);
  int allocated_w = gtk_widget_get_width(window_);
  int allocated_h = gtk_widget_get_height(window_);
  if (allocated_w >= 760) {
    width = allocated_w;
  }
  if (allocated_h >= 560) {
    height = allocated_h;
  }
  width = std::clamp(width, 760, 1600);
  height = std::clamp(height, 560, 1200);

  auto& cfg = ConfigStore::Instance().Mutable();
  if (cfg.window_width != width || cfg.window_height != height) {
    cfg.window_width = width;
    cfg.window_height = height;
    ConfigStore::Instance().Save();
  }
}

void GuiApp::UpdateViewLiveVisibility() {
  const bool show = IsSessionActive() && g_strcmp0(VisiblePageName(), "live") != 0;
  if (view_live_btn_) {
    gtk_widget_set_visible(view_live_btn_, show);
  }
}

void GuiApp::SetFooterStatus(const std::string& text) {
  if (!footer_status_label_) {
    return;
  }
  gtk_label_set_text(GTK_LABEL(footer_status_label_), TruncateStatus(text).c_str());
  gtk_widget_set_tooltip_text(footer_status_label_, text.c_str());
}

void GuiApp::ApplyPrimaryAction(const char* label,
                                const char* icon_name,
                                bool destructive,
                                bool sensitive) {
  if (cast_button_lbl_) {
    gtk_label_set_text(GTK_LABEL(cast_button_lbl_), label);
  }
  if (cast_button_icon_ && icon_name) {
    gtk_image_set_from_icon_name(GTK_IMAGE(cast_button_icon_), icon_name);
  }
  if (cast_button_) {
    gtk_widget_remove_css_class(cast_button_, "suggested-action");
    gtk_widget_remove_css_class(cast_button_, "destructive-action");
    gtk_widget_add_css_class(cast_button_, destructive ? "destructive-action" : "suggested-action");
    gtk_widget_set_sensitive(cast_button_, sensitive);
  }
}

void GuiApp::PresentAlert(const char* heading, const char* body) {
  AdwDialog* dialog = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "OK");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");
  PushModalActionBlock();
  g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->PopModalActionBlock();
  }), this);
  adw_dialog_present(dialog, window_);
}

void GuiApp::ShowAboutDialog() {
  AdwDialog* dialog = adw_about_dialog_new();
  adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), copy::kAppTitle);
  adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(dialog), "io.github.vindeckyy.CastMirror");
  adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), CASTMIRROR_VERSION);
  adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "CastMirror Maintainers");
  adw_about_dialog_set_website(ADW_ABOUT_DIALOG(dialog), "https://github.com/vindeckyy/CastMirror");
  adw_about_dialog_set_license(ADW_ABOUT_DIALOG(dialog), copy::kAboutLicense);
  adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(dialog), copy::kAboutComments);
  PushModalActionBlock();
  g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer user_data) {
    static_cast<GuiApp*>(user_data)->PopModalActionBlock();
  }), this);
  adw_dialog_present(dialog, window_);
}

void GuiApp::OpenConfigFolder() {
  std::string uri = "file://" + GetConfigDir();
  g_app_info_launch_default_for_uri(uri.c_str(), nullptr, nullptr);
}

void GuiApp::OpenLogsFolder() {
  OpenConfigFolder();
}

bool GuiApp::IsSessionActive() const {
  return SessionBlocksCastControls(current_state_);
}

const char* GuiApp::VisiblePageName() const {
  if (!view_stack_) {
    return "cast";
  }
  const char* name = adw_view_stack_get_visible_child_name(ADW_VIEW_STACK(view_stack_));
  return name ? name : "cast";
}

}  // namespace castcore::gui
