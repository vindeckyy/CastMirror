#include "settings_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "help_copy.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"
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
  return 3; // 192 kbps default
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
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  root_widget_ = scroller;

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_container_set_border_width(GTK_CONTAINER(content_box), 18);
  gtk_container_add(GTK_CONTAINER(scroller), content_box);

  const auto& cfg = ConfigStore::Instance().Get();

  // 1. Picture Settings Section
  GtkWidget* pic_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(pic_section), "card-box");
  GtkWidget* pic_header = MakeSectionHeader("PICTURE & ENCODING", "Video resolution, framerate clamping, and bitrate caps");
  gtk_box_pack_start(GTK_BOX(pic_section), pic_header, FALSE, FALSE, 0);

  // Bitrate Scale Row
  GtkWidget* bitrate_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  bitrate_scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1.0, 25.0, 0.5);
  gtk_scale_set_digits(GTK_SCALE(bitrate_scale_), 1);
  gtk_scale_set_draw_value(GTK_SCALE(bitrate_scale_), FALSE);
  gtk_widget_set_size_request(bitrate_scale_, 160, -1);

  bitrate_val_lbl_ = gtk_label_new("8.0 Mbps");
  gtk_widget_set_size_request(bitrate_val_lbl_, 70, -1);
  gtk_widget_set_halign(bitrate_val_lbl_, GTK_ALIGN_END);

  gtk_box_pack_start(GTK_BOX(bitrate_row), bitrate_scale_, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bitrate_row), bitrate_val_lbl_, FALSE, FALSE, 0);

  auto on_bitrate_changed = +[](GtkRange* range, gpointer user_data) {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (self->updating_bitrate_) return;

    const double mbps = gtk_range_get_value(range);
    self->pending_bitrate_kbps_ =
        std::max<uint32_t>(1000, static_cast<uint32_t>(mbps * 1000.0 + 0.5));
    self->UpdateBitrateLabel(self->pending_bitrate_kbps_);

    // GtkScale emits for every pixel while dragging. Applying and persisting
    // every intermediate value repeatedly reconfigured VAAPI and generated
    // dozens of config writes per second. Commit only after the drag settles.
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
  GtkWidget* bitrate_setting = MakeSettingRow(copy::kBitrateTitle, copy::kBitrateHelp, copy::kBitratePopover, bitrate_row);
  gtk_box_pack_start(GTK_BOX(pic_section), bitrate_setting, FALSE, FALSE, 0);

  // Capture FPS Spin
  fps_spin_ = gtk_spin_button_new_with_range(0, 60, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(fps_spin_), cfg.capture_fps);
  auto on_fps_changed = +[](GtkSpinButton* spin, gpointer) {
    auto& c = ConfigStore::Instance().Mutable();
    c.capture_fps = static_cast<int>(gtk_spin_button_get_value(spin));
    ConfigStore::Instance().Save();
  };
  g_signal_connect(fps_spin_, "value-changed", G_CALLBACK(on_fps_changed), nullptr);

  GtkWidget* fps_setting = MakeSettingRow(copy::kFpsTitle, copy::kFpsHelp, copy::kFpsPopover, fps_spin_);
  gtk_box_pack_start(GTK_BOX(pic_section), fps_setting, FALSE, FALSE, 0);

  // Adaptive Quality Switch
  adaptive_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(adaptive_switch_), cfg.adaptive_enabled);
  auto on_adapt_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.adaptive_enabled = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(adaptive_switch_, "state-set", G_CALLBACK(on_adapt_toggle), nullptr);

  GtkWidget* adapt_setting = MakeSettingRow(copy::kAdaptiveTitle, copy::kAdaptiveHelp, copy::kAdaptivePopover, adaptive_switch_);
  gtk_box_pack_start(GTK_BOX(pic_section), adapt_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), pic_section, FALSE, FALSE, 0);

  // 2. Sound Settings Section
  GtkWidget* sound_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(sound_section), "card-box");
  GtkWidget* sound_header = MakeSectionHeader("AUDIO CONFIGURATION", "Opus encoding, monitor sink capture, and speaker muting");
  gtk_box_pack_start(GTK_BOX(sound_section), sound_header, FALSE, FALSE, 0);

  // Audio Enable Switch
  audio_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(audio_switch_), cfg.audio_enabled);
  auto on_audio_toggle = +[](GtkSwitch*, gboolean state, gpointer user_data) -> gboolean {
    auto* self = static_cast<SettingsTab*>(user_data);
    auto& c = ConfigStore::Instance().Mutable();
    c.audio_enabled = state;
    ConfigStore::Instance().Save();
    self->app_->SyncAudioEnabled(state);
    return FALSE;
  };
  g_signal_connect(audio_switch_, "state-set", G_CALLBACK(on_audio_toggle), this);

  GtkWidget* audio_setting = MakeSettingRow("Send PC sound to TV", "Audio mirroring toggle", nullptr, audio_switch_);
  gtk_box_pack_start(GTK_BOX(sound_section), audio_setting, FALSE, FALSE, 0);

  // Audio Quality Combo
  audio_quality_combo_ = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(audio_quality_combo_), "64 kbps");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(audio_quality_combo_), "96 kbps");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(audio_quality_combo_), "128 kbps");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(audio_quality_combo_), "192 kbps");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(audio_quality_combo_), "256 kbps");
  gtk_combo_box_set_active(GTK_COMBO_BOX(audio_quality_combo_), AudioBitrateToComboIndex(cfg.audio_bitrate_bps));

  auto on_aq_changed = +[](GtkComboBox* combo, gpointer) {
    int idx = gtk_combo_box_get_active(combo);
    uint32_t bps = AudioBitrateFromComboIndex(idx);
    auto& c = ConfigStore::Instance().Mutable();
    c.audio_bitrate_bps = bps;
    ConfigStore::Instance().Save();
    if (CastEngine::Instance().GetState() == SessionState::kStreaming) {
      CastEngine::Instance().SetLiveAudioBitrateBps(bps);
    }
  };
  g_signal_connect(audio_quality_combo_, "changed", G_CALLBACK(on_aq_changed), nullptr);

  GtkWidget* aq_setting = MakeSettingRow(copy::kAudioQualityTitle, copy::kAudioQualityHelp, nullptr, audio_quality_combo_);
  gtk_box_pack_start(GTK_BOX(sound_section), aq_setting, FALSE, FALSE, 0);

  // Silence Host Speakers Switch
  silence_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(silence_switch_), cfg.silence_host_speakers);
  auto on_silence_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.silence_host_speakers = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(silence_switch_, "state-set", G_CALLBACK(on_silence_toggle), nullptr);

  GtkWidget* sil_setting = MakeSettingRow(copy::kSilenceTitle, copy::kSilenceHelp, nullptr, silence_switch_);
  gtk_box_pack_start(GTK_BOX(sound_section), sil_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), sound_section, FALSE, FALSE, 0);

  // 3. Latency & Buffering Section
  GtkWidget* lat_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(lat_section), "card-box");
  GtkWidget* lat_header = MakeSectionHeader("LATENCY & BUFFERING", "Playout delay buffer control");
  gtk_box_pack_start(GTK_BOX(lat_section), lat_header, FALSE, FALSE, 0);

  delay_scale_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 100, 400, 10);
  gtk_range_set_value(GTK_RANGE(delay_scale_), cfg.target_delay_ms);
  gtk_scale_set_digits(GTK_SCALE(delay_scale_), 0);
  gtk_widget_set_size_request(delay_scale_, 160, -1);

  auto on_delay_changed = +[](GtkRange* range, gpointer) {
    auto& c = ConfigStore::Instance().Mutable();
    c.target_delay_ms = static_cast<int>(gtk_range_get_value(range));
    ConfigStore::Instance().Save();
  };
  g_signal_connect(delay_scale_, "value-changed", G_CALLBACK(on_delay_changed), nullptr);

  GtkWidget* delay_setting = MakeSettingRow(copy::kDelayTitle, copy::kDelayHelp, copy::kDelayPopover, delay_scale_);
  gtk_box_pack_start(GTK_BOX(lat_section), delay_setting, FALSE, FALSE, 0);

  low_latency_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(low_latency_switch_), cfg.low_latency_mode);
  auto on_ll_toggle = +[](GtkSwitch*, gboolean state, gpointer user_data) -> gboolean {
    auto* self = static_cast<SettingsTab*>(user_data);
    auto& c = ConfigStore::Instance().Mutable();
    c.low_latency_mode = state;
    if (state) {
      c.target_delay_ms = 200;
      gtk_range_set_value(GTK_RANGE(self->delay_scale_), 200);
    }
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(low_latency_switch_, "state-set", G_CALLBACK(on_ll_toggle), this);

  GtkWidget* ll_setting = MakeSettingRow(copy::kLowLatencyTitle, copy::kLowLatencyHelp, nullptr, low_latency_switch_);
  gtk_box_pack_start(GTK_BOX(lat_section), ll_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), lat_section, FALSE, FALSE, 0);

  // 4. Discovery Section
  GtkWidget* disc_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(disc_section), "card-box");
  GtkWidget* disc_header = MakeSectionHeader("DEVICE DISCOVERY", "Local network mDNS and subnet scanning");
  gtk_box_pack_start(GTK_BOX(disc_section), disc_header, FALSE, FALSE, 0);

  subnet_scan_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(subnet_scan_switch_), cfg.subnet_scan_enabled);
  auto on_scan_toggle = +[](GtkSwitch* sw, gboolean state, gpointer user_data) -> gboolean {
    auto* self = static_cast<SettingsTab*>(user_data);
    if (state) {
      GtkWidget* dialog = gtk_message_dialog_new(
          self->app_->GetWindow(), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
          "%s", copy::kSubnetScanConfirm);
      gint res = gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);

      if (res != GTK_RESPONSE_OK) {
        gtk_switch_set_active(sw, FALSE);
        return TRUE;
      }
    }
    auto& c = ConfigStore::Instance().Mutable();
    c.subnet_scan_enabled = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(subnet_scan_switch_, "state-set", G_CALLBACK(on_scan_toggle), this);

  GtkWidget* scan_setting = MakeSettingRow(copy::kSubnetScanTitle, copy::kSubnetScanHelp, nullptr, subnet_scan_switch_);
  gtk_box_pack_start(GTK_BOX(disc_section), scan_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), disc_section, FALSE, FALSE, 0);

  // 5. Advanced Capture & Encoding Section
  GtkWidget* adv_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(adv_section), "card-box");
  GtkWidget* adv_header = MakeSectionHeader("ADVANCED CAPTURE & ENCODING", "Backend overrides and fallback flags");
  gtk_box_pack_start(GTK_BOX(adv_section), adv_header, FALSE, FALSE, 0);

  force_x11_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(force_x11_switch_), cfg.force_x11_capture);
  auto on_fx11_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.force_x11_capture = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(force_x11_switch_, "state-set", G_CALLBACK(on_fx11_toggle), nullptr);

  GtkWidget* fx11_setting = MakeSettingRow(copy::kForceX11Title, copy::kForceX11Help, copy::kForceX11Popover, force_x11_switch_);
  gtk_box_pack_start(GTK_BOX(adv_section), fx11_setting, FALSE, FALSE, 0);

  force_software_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(force_software_switch_), cfg.force_software_encode);
  auto on_fsoft_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.force_software_encode = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(force_software_switch_, "state-set", G_CALLBACK(on_fsoft_toggle), nullptr);

  GtkWidget* fsoft_setting = MakeSettingRow(copy::kForceSoftwareTitle, copy::kForceSoftwareHelp, copy::kForceSoftwarePopover, force_software_switch_);
  gtk_box_pack_start(GTK_BOX(adv_section), fsoft_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), adv_section, FALSE, FALSE, 0);

  // 6. Desktop Integration Section
  GtkWidget* desk_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(desk_section), "card-box");
  GtkWidget* desk_header = MakeSectionHeader("DESKTOP INTEGRATION", "System tray icon and desktop notifications");
  gtk_box_pack_start(GTK_BOX(desk_section), desk_header, FALSE, FALSE, 0);

  tray_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(tray_switch_), cfg.enable_tray_on_startup);
  auto on_tray_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.enable_tray_on_startup = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(tray_switch_, "state-set", G_CALLBACK(on_tray_toggle), nullptr);

  GtkWidget* tray_setting = MakeSettingRow(copy::kTrayTitle, copy::kTrayHelp, nullptr, tray_switch_);
  gtk_box_pack_start(GTK_BOX(desk_section), tray_setting, FALSE, FALSE, 0);

  close_to_tray_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(close_to_tray_switch_), cfg.close_to_tray);
  auto on_ctt_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.close_to_tray = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(close_to_tray_switch_, "state-set", G_CALLBACK(on_ctt_toggle), nullptr);

  GtkWidget* ctt_setting = MakeSettingRow(copy::kCloseToTrayTitle, copy::kCloseToTrayHelp, nullptr, close_to_tray_switch_);
  gtk_box_pack_start(GTK_BOX(desk_section), ctt_setting, FALSE, FALSE, 0);

  notify_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(notify_switch_), cfg.notify_on_events);
  auto on_notif_toggle = +[](GtkSwitch*, gboolean state, gpointer) -> gboolean {
    auto& c = ConfigStore::Instance().Mutable();
    c.notify_on_events = state;
    ConfigStore::Instance().Save();
    return FALSE;
  };
  g_signal_connect(notify_switch_, "state-set", G_CALLBACK(on_notif_toggle), nullptr);

  GtkWidget* notif_setting = MakeSettingRow(copy::kNotifyTitle, copy::kNotifyHelp, nullptr, notify_switch_);
  gtk_box_pack_start(GTK_BOX(desk_section), notif_setting, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), desk_section, FALSE, FALSE, 0);

  // Sync initial bitrate
  uint32_t kbps = cfg.GetPresetBitrateKbps(cfg.quality_preset);
  SyncBitrateSlider(kbps);
}

void SettingsTab::UpdateBitrateLabel(uint32_t kbps) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << (kbps / 1000.0) << " Mbps";
  gtk_label_set_text(GTK_LABEL(bitrate_val_lbl_), ss.str().c_str());
}

void SettingsTab::SyncBitrateSlider(uint32_t kbps) {
  updating_bitrate_ = true;
  if (bitrate_scale_) {
    gtk_range_set_value(GTK_RANGE(bitrate_scale_), kbps / 1000.0);
  }
  UpdateBitrateLabel(kbps);
  updating_bitrate_ = false;
}

void SettingsTab::SyncAudioSwitch(bool active) {
  if (audio_switch_ && gtk_switch_get_active(GTK_SWITCH(audio_switch_)) != static_cast<gboolean>(active)) {
    gtk_switch_set_active(GTK_SWITCH(audio_switch_), active);
  }
}

void SettingsTab::UpdateSessionState(SessionState state) {
  bool is_idle = (state == SessionState::kIdle || state == SessionState::kReady ||
                  state == SessionState::kDiscovering || state == SessionState::kFailed);

  // During active streaming, bitrate scale stays sensitive (live cap); others locked
  gtk_widget_set_sensitive(bitrate_scale_, is_idle || state == SessionState::kStreaming);
  gtk_widget_set_sensitive(fps_spin_, is_idle);
  gtk_widget_set_sensitive(adaptive_switch_, is_idle);
  gtk_widget_set_sensitive(delay_scale_, is_idle);
  gtk_widget_set_sensitive(low_latency_switch_, is_idle);
  gtk_widget_set_sensitive(force_x11_switch_, is_idle);
  gtk_widget_set_sensitive(force_software_switch_, is_idle);
}

}  // namespace castcore::gui
