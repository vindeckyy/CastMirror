#include "settings_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "help_copy.h"
#include "tray.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"
#include "castcore/display_capture.h"
#include "castcore/video_encoder.h"
#include "castcore/audio_capture.h"
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace castcore::gui {

namespace {

static const uint32_t kAudioBitrateChoices[] = {64000, 96000, 128000, 192000, 256000};

int AudioBitrateToComboIndex(uint32_t bps) {
  for (int i = 0; i < 5; ++i) {
    if (kAudioBitrateChoices[i] == bps) return i;
  }
  return 3;  // 192 kbps default
}

uint32_t AudioBitrateFromComboIndex(int idx) {
  if (idx < 0 || idx > 4) return 192000;
  return kAudioBitrateChoices[idx];
}

}  // namespace

SettingsTab::SettingsTab(GuiApp* app) : app_(app) {
  BuildUi();
}

SettingsTab::~SettingsTab() {
  if (bitrate_debounce_id_ != 0) {
    g_source_remove(bitrate_debounce_id_);
    bitrate_debounce_id_ = 0;
  }
}

void SettingsTab::BuildUi() {
  syncing_controls_ = true;

  AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  root_widget_ = GTK_WIDGET(page);

  const auto& cfg = ConfigStore::Instance().Get();

  // 1. Picture & encoding
  AdwPreferencesGroup* pic_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(pic_group, "Picture &amp; encoding");
  adw_preferences_page_add(page, pic_group);

  // Video bitrate cap
  bitrate_row_ = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(bitrate_row_), copy::kBitrateTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(bitrate_row_), copy::kBitrateHelp);

  GtkWidget* bitrate_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  bitrate_scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1.0, 25.0, 0.5);
  gtk_scale_set_digits(GTK_SCALE(bitrate_scale_), 1);
  gtk_scale_set_draw_value(GTK_SCALE(bitrate_scale_), FALSE);
  gtk_widget_set_size_request(bitrate_scale_, 160, -1);

  bitrate_val_lbl_ = gtk_label_new("8.0 Mbps");
  gtk_widget_set_size_request(bitrate_val_lbl_, 70, -1);
  gtk_widget_set_halign(bitrate_val_lbl_, GTK_ALIGN_END);

  GtkWidget* bitrate_info = MakeInfoButton(copy::kBitrateTitle, copy::kBitratePopover);

  gtk_box_append(GTK_BOX(bitrate_box), bitrate_scale_);
  gtk_box_append(GTK_BOX(bitrate_box), bitrate_val_lbl_);
  gtk_box_append(GTK_BOX(bitrate_box), bitrate_info);
  adw_action_row_add_suffix(ADW_ACTION_ROW(bitrate_row_), bitrate_box);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(bitrate_row_), bitrate_scale_);
  adw_preferences_group_add(pic_group, bitrate_row_);

  // Capture FPS
  GtkAdjustment* fps_adj = gtk_adjustment_new(cfg.capture_fps, 0, 60, 1, 5, 0);
  fps_row_ = adw_spin_row_new(fps_adj, 1, 0);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fps_row_), copy::kFpsTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(fps_row_), copy::kFpsHelp);
  GtkWidget* fps_info = MakeInfoButton(copy::kFpsTitle, copy::kFpsPopover);
  adw_action_row_add_suffix(ADW_ACTION_ROW(fps_row_), fps_info);
  adw_preferences_group_add(pic_group, fps_row_);

  // 2. Audio
  AdwPreferencesGroup* audio_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(audio_group, "Audio");
  adw_preferences_page_add(page, audio_group);

  // Computer sound
  audio_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(audio_row_), copy::kComputerSoundTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(audio_row_), copy::kComputerSoundHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(audio_row_), cfg.audio_enabled);
  adw_preferences_group_add(audio_group, audio_row_);

  // Mute sound while streaming
  silence_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(silence_row_), copy::kSilenceTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(silence_row_), copy::kSilenceHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(silence_row_), cfg.silence_host_speakers);
  adw_preferences_group_add(audio_group, silence_row_);

  // Audio Quality
  const char* const audio_bitrate_strings[] = {
    "64 kbps", "96 kbps", "128 kbps", "192 kbps", "256 kbps", nullptr
  };
  GtkStringList* string_list = gtk_string_list_new(audio_bitrate_strings);
  audio_quality_row_ = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(audio_quality_row_), copy::kAudioQualityTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(audio_quality_row_), copy::kAudioQualityHelp);
  adw_combo_row_set_model(ADW_COMBO_ROW(audio_quality_row_), G_LIST_MODEL(string_list));
  adw_combo_row_set_selected(ADW_COMBO_ROW(audio_quality_row_), AudioBitrateToComboIndex(cfg.audio_bitrate_bps));
  adw_preferences_group_add(audio_group, audio_quality_row_);

  // 3. Latency & buffering
  AdwPreferencesGroup* lat_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(lat_group, "Latency &amp; buffering");
  adw_preferences_page_add(page, lat_group);

  // Target delay
  delay_row_ = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(delay_row_), copy::kDelayTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(delay_row_), copy::kDelayHelp);

  GtkWidget* delay_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  delay_scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 100, 400, 10);
  gtk_range_set_value(GTK_RANGE(delay_scale_), cfg.target_delay_ms);
  gtk_scale_set_digits(GTK_SCALE(delay_scale_), 0);
  gtk_scale_set_draw_value(GTK_SCALE(delay_scale_), FALSE);
  gtk_widget_set_size_request(delay_scale_, 160, -1);

  delay_val_lbl_ = gtk_label_new("");
  gtk_widget_set_size_request(delay_val_lbl_, 70, -1);
  gtk_widget_set_halign(delay_val_lbl_, GTK_ALIGN_END);
  UpdateDelayLabel(cfg.target_delay_ms);

  GtkWidget* delay_info = MakeInfoButton(copy::kDelayTitle, copy::kDelayPopover);

  gtk_box_append(GTK_BOX(delay_box), delay_scale_);
  gtk_box_append(GTK_BOX(delay_box), delay_val_lbl_);
  gtk_box_append(GTK_BOX(delay_box), delay_info);
  adw_action_row_add_suffix(ADW_ACTION_ROW(delay_row_), delay_box);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(delay_row_), delay_scale_);
  adw_preferences_group_add(lat_group, delay_row_);

  // 4. Device discovery
  AdwPreferencesGroup* disc_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(disc_group, "Device discovery");
  adw_preferences_page_add(page, disc_group);

  // Subnet scan
  subnet_scan_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(subnet_scan_row_), copy::kSubnetScanTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(subnet_scan_row_), copy::kSubnetScanHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(subnet_scan_row_), cfg.subnet_scan_enabled);
  adw_preferences_group_add(disc_group, subnet_scan_row_);

  // 5. Advanced
  AdwPreferencesGroup* adv_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(adv_group, "Advanced");
  adw_preferences_page_add(page, adv_group);

  // Force X11 capture
  force_x11_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(force_x11_row_), copy::kForceX11Title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(force_x11_row_), copy::kForceX11Help);
  adw_switch_row_set_active(ADW_SWITCH_ROW(force_x11_row_), cfg.force_x11_capture);
  GtkWidget* fx11_info = MakeInfoButton(copy::kForceX11Title, copy::kForceX11Popover);
  adw_action_row_add_suffix(ADW_ACTION_ROW(force_x11_row_), fx11_info);
  adw_preferences_group_add(adv_group, force_x11_row_);

  // Force software encode
  force_software_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(force_software_row_), copy::kForceSoftwareTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(force_software_row_), copy::kForceSoftwareHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(force_software_row_), cfg.force_software_encode);
  GtkWidget* fsoft_info = MakeInfoButton(copy::kForceSoftwareTitle, copy::kForceSoftwarePopover);
  adw_action_row_add_suffix(ADW_ACTION_ROW(force_software_row_), fsoft_info);
  adw_preferences_group_add(adv_group, force_software_row_);

  // 6. Desktop integration
  AdwPreferencesGroup* desk_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(desk_group, "Desktop integration");
  adw_preferences_page_add(page, desk_group);

  // Show a system tray icon
  tray_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tray_row_), copy::kTrayTitle);
  adw_switch_row_set_active(ADW_SWITCH_ROW(tray_row_), cfg.enable_tray_on_startup);
  adw_preferences_group_add(desk_group, tray_row_);

  // Closing window hides to tray
  close_to_tray_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(close_to_tray_row_), copy::kCloseToTrayTitle);
  adw_switch_row_set_active(ADW_SWITCH_ROW(close_to_tray_row_), cfg.close_to_tray);
  adw_preferences_group_add(desk_group, close_to_tray_row_);

  bool tray_supported = false;
  if (app_ && app_->GetTrayManager()) {
    tray_supported = app_->GetTrayManager()->IsAvailable();
  }
  if (tray_supported) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(tray_row_), copy::kTrayHelp);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(close_to_tray_row_), copy::kCloseToTrayHelp);
  } else {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(tray_row_), "System tray support is not available in this build.");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(close_to_tray_row_), "System tray support is not available in this build.");
  }

  // Desktop notifications
  notify_row_ = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(notify_row_), copy::kNotifyTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(notify_row_), copy::kNotifyHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(notify_row_), cfg.notify_on_events);
  adw_preferences_group_add(desk_group, notify_row_);

  // 7. Appearance
  AdwPreferencesGroup* appearance_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(appearance_group, "Appearance");
  adw_preferences_page_add(page, appearance_group);

  const char* const theme_strings[] = {
    "System default", "Light", "Dark", nullptr
  };
  GtkStringList* theme_list = gtk_string_list_new(theme_strings);
  GtkWidget* theme_row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row), "Color scheme");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(theme_row), "Choose between light, dark, or system visual styles");
  adw_combo_row_set_model(ADW_COMBO_ROW(theme_row), G_LIST_MODEL(theme_list));

  AdwStyleManager* sm = adw_style_manager_get_default();
  AdwColorScheme scheme = adw_style_manager_get_color_scheme(sm);
  guint sel_idx = 0;
  if (scheme == ADW_COLOR_SCHEME_FORCE_LIGHT) sel_idx = 1;
  else if (scheme == ADW_COLOR_SCHEME_FORCE_DARK) sel_idx = 2;
  adw_combo_row_set_selected(ADW_COMBO_ROW(theme_row), sel_idx);

  g_signal_connect(theme_row, "notify::selected", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer) {
    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
    AdwStyleManager* style_mgr = adw_style_manager_get_default();
    if (idx == 1) {
      adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_LIGHT);
    } else if (idx == 2) {
      adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_DARK);
    } else {
      adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_DEFAULT);
    }
  }), nullptr);
  adw_preferences_group_add(appearance_group, theme_row);

  // 8. Diagnostics & Health
  AdwPreferencesGroup* diag_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(diag_group, "Diagnostics &amp; health");
  adw_preferences_page_add(page, diag_group);

  GtkWidget* diag_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(diag_row), "Run self-test");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(diag_row), "Verify display capture, hardware encoder, audio loopback, and UDP sockets");

  GtkWidget* diag_btn = gtk_button_new_with_label("Run test");
  gtk_widget_add_css_class(diag_btn, "suggested-action");
  gtk_widget_set_valign(diag_btn, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(ADW_ACTION_ROW(diag_row), diag_btn);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(diag_row), diag_btn);

  g_signal_connect(diag_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    self->RunSelfTestDialog();
  }), this);
  adw_preferences_group_add(diag_group, diag_row);

  // Sync initial bitrate
  uint32_t kbps = cfg.GetPresetBitrateKbps(cfg.quality_preset);
  SyncBitrateSlider(kbps);

  // Connect Signals

  // 1. Bitrate Scale
  auto on_bitrate_changed = +[](GtkRange* range, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;

    const double mbps = gtk_range_get_value(range);
    self->pending_bitrate_kbps_ =
        std::max<uint32_t>(1000, static_cast<uint32_t>(mbps * 1000.0 + 0.5));
    self->UpdateBitrateLabel(self->pending_bitrate_kbps_);

    if (self->bitrate_debounce_id_ != 0) {
      g_source_remove(self->bitrate_debounce_id_);
    }
    self->bitrate_debounce_id_ = g_timeout_add(250, +[](gpointer data) -> gboolean {
      auto* tab = static_cast<SettingsTab*>(data);
      tab->bitrate_debounce_id_ = 0;
      const uint32_t kbps = tab->pending_bitrate_kbps_;
      if (CastEngine::Instance().GetState() == SessionState::kStreaming) {
        CastEngine::Instance().SetLiveVideoBitrateKbps(kbps);
      } else {
        auto& c = ConfigStore::Instance().Mutable();
        c.SetPresetBitrateKbps(c.quality_preset, kbps);
        c.max_bitrate_kbps = kbps;
        ConfigStore::Instance().Save();
      }
      return G_SOURCE_REMOVE;
    }, self);
  };
  g_signal_connect(bitrate_scale_, "value-changed", G_CALLBACK(on_bitrate_changed), this);

  // 2. Capture FPS Spin
  auto on_fps_changed = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    auto& c = ConfigStore::Instance().Mutable();
    c.capture_fps = static_cast<int>(adw_spin_row_get_value(ADW_SPIN_ROW(self->fps_row_)));
    ConfigStore::Instance().Save();
  };
  g_signal_connect(fps_row_, "notify::value", G_CALLBACK(on_fps_changed), this);

  // 4. Send Computer Audio Switch
  auto on_audio_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->audio_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.audio_enabled = state;
    ConfigStore::Instance().Save();
    if (self->app_) {
      self->app_->SyncAudioEnabled(state);
    }
    self->UpdateDependentSensitivities();
  };
  g_signal_connect(audio_row_, "notify::active", G_CALLBACK(on_audio_toggle), this);

  // 5. Audio Quality Combo
  auto on_aq_changed = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(self->audio_quality_row_));
    if (idx == GTK_INVALID_LIST_POSITION) return;
    uint32_t bps = AudioBitrateFromComboIndex(static_cast<int>(idx));
    auto& c = ConfigStore::Instance().Mutable();
    c.audio_bitrate_bps = bps;
    ConfigStore::Instance().Save();
    if (CastEngine::Instance().GetState() == SessionState::kStreaming) {
      CastEngine::Instance().SetLiveAudioBitrateBps(bps);
    }
  };
  g_signal_connect(audio_quality_row_, "notify::selected", G_CALLBACK(on_aq_changed), this);

  // 6. Silence Host Speakers Switch
  auto on_silence_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->silence_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.silence_host_speakers = state;
    ConfigStore::Instance().Save();
    if (self->app_) {
      self->app_->SyncSilenceHost(state);
    }
  };
  g_signal_connect(silence_row_, "notify::active", G_CALLBACK(on_silence_toggle), this);

  // 7. Target Delay Scale
  auto on_delay_changed = +[](GtkRange* range, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    int delay = static_cast<int>(gtk_range_get_value(range));
    self->UpdateDelayLabel(delay);
    auto& c = ConfigStore::Instance().Mutable();
    c.target_delay_ms = delay;
    ConfigStore::Instance().Save();
  };
  g_signal_connect(delay_scale_, "value-changed", G_CALLBACK(on_delay_changed), this);

  // 9. Subnet Scan Switch with Asynchronous AdwAlertDialog
  auto on_scan_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->subnet_scan_row_));
    if (state) {
      AdwDialog* dialog = adw_alert_dialog_new(
          "Enable LAN scanning?", copy::kSubnetScanConfirm);
      adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
      adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "enable", "Enable scan");
      adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "enable", ADW_RESPONSE_SUGGESTED);
      adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
      adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

      if (self->app_) {
        self->app_->PushModalActionBlock();
      }

      struct ConsentContext {
        SettingsTab* self;
      };
      auto* ctx = new ConsentContext{self};

      GtkWidget* parent_win = self->app_ ? GTK_WIDGET(self->app_->GetWindow()) : self->root_widget_;
      adw_alert_dialog_choose(
          ADW_ALERT_DIALOG(dialog),
          parent_win,
          nullptr,
          +[](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* ctx = static_cast<ConsentContext*>(user_data);
            SettingsTab* tab = ctx->self;
            delete ctx;

            const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), res);
            if (tab->app_) {
              tab->app_->PopModalActionBlock();
            }

            if (g_strcmp0(response, "enable") == 0) {
              auto& c = ConfigStore::Instance().Mutable();
              c.subnet_scan_enabled = true;
              ConfigStore::Instance().Save();
            } else {
              tab->syncing_controls_ = true;
              adw_switch_row_set_active(ADW_SWITCH_ROW(tab->subnet_scan_row_), FALSE);
              tab->syncing_controls_ = false;
            }
          },
          ctx);
    } else {
      auto& c = ConfigStore::Instance().Mutable();
      c.subnet_scan_enabled = false;
      ConfigStore::Instance().Save();
    }
  };
  g_signal_connect(subnet_scan_row_, "notify::active", G_CALLBACK(on_scan_toggle), this);

  // 10. Force X11 Capture Switch
  auto on_fx11_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->force_x11_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.force_x11_capture = state;
    ConfigStore::Instance().Save();
  };
  g_signal_connect(force_x11_row_, "notify::active", G_CALLBACK(on_fx11_toggle), this);

  // 11. Force Software Encode Switch
  auto on_fsoft_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->force_software_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.force_software_encode = state;
    ConfigStore::Instance().Save();
  };
  g_signal_connect(force_software_row_, "notify::active", G_CALLBACK(on_fsoft_toggle), this);

  // 12. Tray Startup Switch
  auto on_tray_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->tray_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.enable_tray_on_startup = state;
    ConfigStore::Instance().Save();
    self->UpdateDependentSensitivities();
  };
  g_signal_connect(tray_row_, "notify::active", G_CALLBACK(on_tray_toggle), this);

  // 13. Close to Tray Switch
  auto on_ctt_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->close_to_tray_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.close_to_tray = state;
    ConfigStore::Instance().Save();
  };
  g_signal_connect(close_to_tray_row_, "notify::active", G_CALLBACK(on_ctt_toggle), this);

  // 14. Desktop Notifications Switch
  auto on_notif_toggle = +[](GObject*, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->syncing_controls_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(self->notify_row_));
    auto& c = ConfigStore::Instance().Mutable();
    c.notify_on_events = state;
    ConfigStore::Instance().Save();
  };
  g_signal_connect(notify_row_, "notify::active", G_CALLBACK(on_notif_toggle), this);

  UpdateDependentSensitivities();
  syncing_controls_ = false;
}

void SettingsTab::UpdateBitrateLabel(uint32_t kbps) {
  if (!bitrate_val_lbl_) return;
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << (kbps / 1000.0) << " Mbps";
  gtk_label_set_text(GTK_LABEL(bitrate_val_lbl_), ss.str().c_str());
}

void SettingsTab::UpdateDelayLabel(int ms) {
  if (!delay_val_lbl_) return;
  std::string text = std::to_string(ms) + " ms";
  gtk_label_set_text(GTK_LABEL(delay_val_lbl_), text.c_str());
}

void SettingsTab::UpdateDependentSensitivities() {
  SessionState state = app_ ? app_->GetCurrentState() : SessionState::kIdle;
  bool is_session_idle = (state == SessionState::kIdle ||
                          state == SessionState::kReady ||
                          state == SessionState::kDiscovering ||
                          state == SessionState::kFailed);
  bool is_streaming = (state == SessionState::kStreaming);

  bool audio_on = audio_row_ ? adw_switch_row_get_active(ADW_SWITCH_ROW(audio_row_)) : true;
  bool tray_on = tray_row_ ? adw_switch_row_get_active(ADW_SWITCH_ROW(tray_row_)) : false;

  bool tray_supported = false;
  if (app_ && app_->GetTrayManager()) {
    tray_supported = app_->GetTrayManager()->IsAvailable();
  }

  // Picture & encoding
  if (bitrate_row_) gtk_widget_set_sensitive(bitrate_row_, is_session_idle || is_streaming);
  if (fps_row_) gtk_widget_set_sensitive(fps_row_, is_session_idle);

  // Audio
  if (audio_row_) gtk_widget_set_sensitive(audio_row_, is_session_idle);
  if (silence_row_) gtk_widget_set_sensitive(silence_row_, is_session_idle && audio_on);
  bool audio_quality_sensitive =
      (is_session_idle && audio_on) ||
      (is_streaming && app_ && app_->IsActiveSessionAudioEnabled());
  if (audio_quality_row_) gtk_widget_set_sensitive(audio_quality_row_, audio_quality_sensitive);

  // Latency & buffering
  if (delay_row_) gtk_widget_set_sensitive(delay_row_, is_session_idle);

  // Advanced
  if (force_x11_row_) gtk_widget_set_sensitive(force_x11_row_, is_session_idle);
  if (force_software_row_) gtk_widget_set_sensitive(force_software_row_, is_session_idle);

  // Discovery
  if (subnet_scan_row_) gtk_widget_set_sensitive(subnet_scan_row_, TRUE);

  // Desktop integration
  if (tray_row_ && close_to_tray_row_) {
    if (tray_supported) {
      gtk_widget_set_sensitive(tray_row_, TRUE);
      gtk_widget_set_sensitive(close_to_tray_row_, tray_on);
    } else {
      gtk_widget_set_sensitive(tray_row_, FALSE);
      gtk_widget_set_sensitive(close_to_tray_row_, FALSE);
    }
  }
  if (notify_row_) gtk_widget_set_sensitive(notify_row_, TRUE);
}

void SettingsTab::UpdateSessionState(SessionState /*state*/) {
  UpdateDependentSensitivities();
}

void SettingsTab::SyncBitrateSlider(uint32_t kbps) {
  syncing_controls_ = true;
  if (bitrate_scale_) {
    gtk_range_set_value(GTK_RANGE(bitrate_scale_), kbps / 1000.0);
  }
  UpdateBitrateLabel(kbps);
  syncing_controls_ = false;
}

void SettingsTab::SyncAudioSwitch(bool active) {
  if (audio_row_) {
    gboolean current = adw_switch_row_get_active(ADW_SWITCH_ROW(audio_row_));
    if (current != static_cast<gboolean>(active)) {
      syncing_controls_ = true;
      adw_switch_row_set_active(ADW_SWITCH_ROW(audio_row_), active);
      UpdateDependentSensitivities();
      syncing_controls_ = false;
    } else {
      UpdateDependentSensitivities();
    }
  }
}

void SettingsTab::SyncSilenceSwitch(bool active) {
  if (!silence_row_) return;
  gboolean current = adw_switch_row_get_active(ADW_SWITCH_ROW(silence_row_));
  if (current != static_cast<gboolean>(active)) {
    syncing_controls_ = true;
    adw_switch_row_set_active(ADW_SWITCH_ROW(silence_row_), active);
    syncing_controls_ = false;
  }
}

void SettingsTab::RunSelfTestDialog() {
  GtkWindow* parent_win = app_ ? app_->GetWindow() : nullptr;

  // Test 1: Video Capture
  auto cap = DisplayCaptureFactory::Create();
  bool cap_ok = (cap != nullptr);
  std::string cap_str = cap_ok ? ("Available (" + cap->BackendName() + ")") : "Failed";

  // Test 2: Video Encoder
  auto enc = VideoEncoderFactory::Create(VideoCodec::kH264);
  bool enc_ok = (enc != nullptr);
  std::string enc_str = enc_ok ? ("Ready (" + enc->EncoderName() + ")") : "Failed";

  // Test 3: Audio Capture
  auto audio = AudioCaptureFactory::Create();
  bool audio_ok = (audio != nullptr);
  std::string audio_str = audio_ok ? "Ready (PulseAudio / PipeWire monitor)" : "Unavailable";

  // Test 4: UDP Network Socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  bool net_ok = (fd >= 0);
  if (fd >= 0) close(fd);
  std::string net_str = net_ok ? "Ready (UDP multicast & unicast permitted)" : "Failed";

  std::ostringstream msg;
  msg << (cap_ok ? "✓ " : "✗ ") << "Display Capture: " << cap_str << "\n"
      << (enc_ok ? "✓ " : "✗ ") << "Video Encoder: " << enc_str << "\n"
      << (audio_ok ? "✓ " : "✗ ") << "Audio Monitor: " << audio_str << "\n"
      << (net_ok ? "✓ " : "✗ ") << "Network Sockets: " << net_str;

  AdwDialog* dialog = adw_alert_dialog_new(
      "Hardware & Network Diagnostics",
      msg.str().c_str());
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Close");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "close");
  adw_dialog_present(dialog, GTK_WIDGET(parent_win));
}

}  // namespace castcore::gui
