#include <gtk/gtk.h>
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/logger.h"
#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <fstream>

using namespace castcore;

struct AppWidgets {
  GtkWidget* window;
  GtkWidget* status_badge;
  GtkWidget* status_label;
  GtkWidget* device_combo;
  GtkWidget* display_combo;
  GtkWidget* preset_combo;
  GtkWidget* bitrate_scale;
  GtkWidget* bitrate_value_label;
  GtkWidget* audio_switch;
  GtkWidget* cast_button;
  GtkWidget* cast_button_label;
  GtkWidget* spinner;
  GtkWidget* rescan_button;
  GtkWidget* add_ip_button;
  GtkWidget* stats_box;
  GtkWidget* fps_label;
  GtkWidget* bitrate_label;
  GtkWidget* rtt_label;
  GtkWidget* loss_label;
  GtkWidget* delay_label;
  GtkWidget* preview_area;

  std::vector<CastDevice> current_devices;
  std::vector<DisplayInfo> current_displays;
  std::string selected_device_id;
  int selected_display_id = 0;
  bool is_casting = false;
  bool updating_bitrate_slider = false;
  std::mutex data_mutex;
};

static AppWidgets g_app;

const char* kCustomCss = R"(
  window {
    background-color: #121417;
    color: #e1e3e6;
    font-family: 'Segoe UI', 'Ubuntu', 'Cantarell', sans-serif;
  }
  .header-box {
    background-color: #1a1d24;
    padding: 18px 24px;
    border-bottom: 1px solid #2a2e39;
  }
  .title-label {
    font-size: 20px;
    font-weight: 700;
    color: #ffffff;
  }
  .subtitle-label {
    font-size: 12px;
    color: #8c93a0;
  }
  .status-badge {
    padding: 4px 12px;
    border-radius: 12px;
    font-weight: 600;
    font-size: 12px;
  }
  .status-idle {
    background-color: #262c36;
    color: #9aa0a6;
  }
  .status-connecting {
    background-color: #5c4314;
    color: #fbc02d;
  }
  .status-live {
    background-color: #143d22;
    color: #4caf50;
  }
  .card-box {
    background-color: #1a1d24;
    border: 1px solid #2a2e39;
    border-radius: 10px;
    padding: 18px;
    margin: 8px 0px;
  }
  .card-title {
    font-size: 14px;
    font-weight: 600;
    color: #00d2ff;
    margin-bottom: 8px;
  }
  .btn-cast-start {
    background: linear-gradient(135deg, #0078d4, #005a9e);
    color: #ffffff;
    font-size: 16px;
    font-weight: 700;
    border-radius: 8px;
    padding: 14px 24px;
    border: none;
    box-shadow: 0 4px 12px rgba(0, 120, 212, 0.35);
  }
  .btn-cast-start:hover {
    background: linear-gradient(135deg, #1084d8, #0066b2);
    box-shadow: 0 6px 16px rgba(0, 120, 212, 0.5);
  }
  .btn-cast-stop {
    background: linear-gradient(135deg, #d83b01, #a80000);
    color: #ffffff;
    font-size: 16px;
    font-weight: 700;
    border-radius: 8px;
    padding: 14px 24px;
    border: none;
    box-shadow: 0 4px 12px rgba(216, 59, 1, 0.35);
  }
  .btn-cast-stop:hover {
    background: linear-gradient(135deg, #ea4335, #b71c1c);
    box-shadow: 0 6px 16px rgba(234, 67, 53, 0.5);
  }
  .stat-card {
    background-color: #121417;
    border: 1px solid #262c36;
    border-radius: 6px;
    padding: 10px;
  }
  .stat-value {
    font-size: 16px;
    font-weight: 700;
    color: #00e5ff;
  }
  .stat-title {
    font-size: 11px;
    color: #8c93a0;
  }
  combobox, button, entry {
    border-radius: 6px;
    padding: 4px;
  }
  scale highlight {
    background-color: #0078d4;
  }
  scale trough {
    background-color: #262c36;
  }
)";

static void ApplyCustomCss() {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, kCustomCss, -1, nullptr);
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static QualityPreset PresetFromCombo() {
  int preset_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_app.preset_combo));
  if (preset_idx == 1) return QualityPreset::kHigh;
  if (preset_idx == 2) return QualityPreset::kBalanced;
  if (preset_idx == 3) return QualityPreset::kSmooth;
  return QualityPreset::kAuto;
}

static int ComboIndexFromPreset(QualityPreset preset) {
  switch (preset) {
    case QualityPreset::kHigh: return 1;
    case QualityPreset::kBalanced: return 2;
    case QualityPreset::kSmooth: return 3;
    case QualityPreset::kAuto:
    default: return 0;
  }
}

static void UpdateBitrateValueLabel(uint32_t kbps) {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(1) << (kbps / 1000.0) << " Mbps";
  gtk_label_set_text(GTK_LABEL(g_app.bitrate_value_label), ss.str().c_str());
}

static void SyncBitrateSliderToPreset() {
  if (!g_app.bitrate_scale) return;
  QualityPreset preset = PresetFromCombo();
  uint32_t kbps = ConfigStore::Instance().Get().GetPresetBitrateKbps(preset);
  g_app.updating_bitrate_slider = true;
  gtk_range_set_value(GTK_RANGE(g_app.bitrate_scale), kbps / 1000.0);
  UpdateBitrateValueLabel(kbps);
  g_app.updating_bitrate_slider = false;
}

static void OnPresetChanged(GtkComboBox* combo, gpointer user_data) {
  (void)combo;
  (void)user_data;
  if (g_app.is_casting) return;
  SyncBitrateSliderToPreset();
}

static void OnBitrateSliderChanged(GtkRange* range, gpointer user_data) {
  (void)user_data;
  if (g_app.updating_bitrate_slider || g_app.is_casting) return;
  double mbps = gtk_range_get_value(range);
  uint32_t kbps = static_cast<uint32_t>(mbps * 1000.0 + 0.5);
  if (kbps < 1000) kbps = 1000;
  ConfigStore::Instance().Mutable().SetPresetBitrateKbps(PresetFromCombo(), kbps);
  UpdateBitrateValueLabel(kbps);
}

static void SetBitrateControlsLocked(bool locked) {
  if (g_app.bitrate_scale) {
    gtk_widget_set_sensitive(g_app.bitrate_scale, !locked);
  }
  if (g_app.preset_combo) {
    gtk_widget_set_sensitive(g_app.preset_combo, !locked);
  }
}

static void UpdateDeviceComboUI() {
  std::lock_guard<std::mutex> lock(g_app.data_mutex);
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_app.device_combo));

  auto& engine = CastEngine::Instance();
  g_app.current_devices = engine.GetDevices();

  if (g_app.current_devices.empty()) {
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.device_combo), "No Cast devices found (Click Rescan or Add IP)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_app.device_combo), 0);
  } else {
    for (size_t i = 0; i < g_app.current_devices.size(); ++i) {
      const auto& d = g_app.current_devices[i];
      std::string text = d.name + " (" + d.model_name + ") - " + d.ip_address;
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.device_combo), text.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_app.device_combo), 0);
  }
}

static void UpdateDisplayComboUI() {
  std::lock_guard<std::mutex> lock(g_app.data_mutex);
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_app.display_combo));

  auto& engine = CastEngine::Instance();
  g_app.current_displays = engine.GetDisplays();

  for (size_t i = 0; i < g_app.current_displays.size(); ++i) {
    const auto& d = g_app.current_displays[i];
    std::string text = "Display " + std::to_string(d.id) + ": " + d.name + " (" +
                       std::to_string(d.width) + "x" + std::to_string(d.height) + " @" +
                       std::to_string(d.refresh_rate) + "Hz)" + (d.is_primary ? " [Primary]" : "");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.display_combo), text.c_str());
  }
  if (!g_app.current_displays.empty()) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_app.display_combo), 0);
  }
}

static gboolean OnStatsTimer(gpointer user_data) {
  auto& engine = CastEngine::Instance();
  if (engine.GetState() != SessionState::kStreaming) {
    gtk_widget_set_visible(g_app.stats_box, FALSE);
    return TRUE;
  }

  gtk_widget_set_visible(g_app.stats_box, TRUE);
  StreamStats s = engine.GetStats();

  std::ostringstream ss_fps, ss_bitrate, ss_rtt, ss_loss, ss_delay;
  ss_fps << std::fixed << std::setprecision(1) << s.current_fps << " FPS";
  ss_bitrate << std::fixed << std::setprecision(2) << (s.bitrate_kbps / 1000.0) << " Mbps";
  ss_rtt << s.round_trip_time_ms << " ms";
  ss_loss << std::fixed << std::setprecision(1) << (s.packet_loss_fraction * 100.0) << " %";
  ss_delay << s.target_delay_ms << " ms";

  gtk_label_set_text(GTK_LABEL(g_app.fps_label), ss_fps.str().c_str());
  gtk_label_set_text(GTK_LABEL(g_app.bitrate_label), ss_bitrate.str().c_str());
  gtk_label_set_text(GTK_LABEL(g_app.rtt_label), ss_rtt.str().c_str());
  gtk_label_set_text(GTK_LABEL(g_app.loss_label), ss_loss.str().c_str());
  gtk_label_set_text(GTK_LABEL(g_app.delay_label), ss_delay.str().c_str());

  return TRUE;
}

static gboolean UpdateStateUIIdle(gpointer data) {
  auto state = static_cast<SessionState>(GPOINTER_TO_INT(data));
  GtkStyleContext* ctx = gtk_widget_get_style_context(g_app.status_badge);

  gtk_style_context_remove_class(ctx, "status-idle");
  gtk_style_context_remove_class(ctx, "status-connecting");
  gtk_style_context_remove_class(ctx, "status-live");

  GtkStyleContext* btn_ctx = gtk_widget_get_style_context(g_app.cast_button);
  gtk_style_context_remove_class(btn_ctx, "btn-cast-start");
  gtk_style_context_remove_class(btn_ctx, "btn-cast-stop");

  switch (state) {
    case SessionState::kStreaming:
      gtk_style_context_add_class(ctx, "status-live");
      gtk_label_set_text(GTK_LABEL(g_app.status_label), "● LIVE STREAMING");
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(g_app.cast_button_label), "⏹  STOP CASTING");
      gtk_spinner_stop(GTK_SPINNER(g_app.spinner));
      gtk_widget_set_visible(g_app.spinner, FALSE);
      gtk_widget_set_sensitive(g_app.device_combo, FALSE);
      gtk_widget_set_sensitive(g_app.display_combo, FALSE);
      SetBitrateControlsLocked(true);
      g_app.is_casting = true;
      break;

    case SessionState::kConnecting:
    case SessionState::kNegotiating:
    case SessionState::kReconnecting:
      gtk_style_context_add_class(ctx, "status-connecting");
      gtk_label_set_text(GTK_LABEL(g_app.status_label), "◐ CONNECTING...");
      gtk_style_context_add_class(btn_ctx, "btn-cast-stop");
      gtk_label_set_text(GTK_LABEL(g_app.cast_button_label), "CANCEL");
      gtk_widget_set_visible(g_app.spinner, TRUE);
      gtk_spinner_start(GTK_SPINNER(g_app.spinner));
      gtk_widget_set_sensitive(g_app.device_combo, FALSE);
      gtk_widget_set_sensitive(g_app.display_combo, FALSE);
      SetBitrateControlsLocked(true);
      g_app.is_casting = true;
      break;

    case SessionState::kIdle:
    default:
      gtk_style_context_add_class(ctx, "status-idle");
      gtk_label_set_text(GTK_LABEL(g_app.status_label), "○ READY");
      gtk_style_context_add_class(btn_ctx, "btn-cast-start");
      gtk_label_set_text(GTK_LABEL(g_app.cast_button_label), "⚡  CAST DISPLAY");
      gtk_spinner_stop(GTK_SPINNER(g_app.spinner));
      gtk_widget_set_visible(g_app.spinner, FALSE);
      gtk_widget_set_sensitive(g_app.device_combo, TRUE);
      gtk_widget_set_sensitive(g_app.display_combo, TRUE);
      SetBitrateControlsLocked(false);
      gtk_widget_set_visible(g_app.stats_box, FALSE);
      g_app.is_casting = false;
      break;
  }
  return G_SOURCE_REMOVE;
}

static void OnCastButtonClicked(GtkButton* button, gpointer user_data) {
  auto& engine = CastEngine::Instance();

  if (g_app.is_casting || engine.GetState() == SessionState::kStreaming ||
      engine.GetState() == SessionState::kConnecting) {
    LOG_INFO << "[UI] User clicked Stop Casting";
    engine.StopCasting();
    return;
  }

  std::lock_guard<std::mutex> lock(g_app.data_mutex);
  int active_dev = gtk_combo_box_get_active(GTK_COMBO_BOX(g_app.device_combo));
  if (active_dev < 0 || static_cast<size_t>(active_dev) >= g_app.current_devices.size()) {
    GtkWidget* dialog = gtk_message_dialog_new(
        GTK_WINDOW(g_app.window), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
        "Please select a valid Google Cast device or enter an IP manually.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  const auto& dev = g_app.current_devices[active_dev];
  int active_disp = gtk_combo_box_get_active(GTK_COMBO_BOX(g_app.display_combo));
  int display_id = (active_disp >= 0 && static_cast<size_t>(active_disp) < g_app.current_displays.size())
                       ? g_app.current_displays[active_disp].id
                       : 0;

  QualityPreset preset = PresetFromCombo();
  gboolean audio_on = gtk_switch_get_active(GTK_SWITCH(g_app.audio_switch));
  uint32_t bitrate_kbps = ConfigStore::Instance().Get().GetPresetBitrateKbps(preset);

  LOG_INFO << "[UI] Starting Cast to " << dev.name << " (" << dev.ip_address << ")...";

  // Run connect in background thread so GTK main loop stays completely responsive
  std::thread([dev_id = dev.id, display_id, preset, audio_on, bitrate_kbps]() {
    bool ok = CastEngine::Instance().StartCasting(dev_id, display_id, preset, audio_on, bitrate_kbps);
    if (!ok) {
      g_idle_add([](gpointer) -> gboolean {
        GtkWidget* dialog = gtk_message_dialog_new(
            GTK_WINDOW(g_app.window), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to connect to Cast device. Ensure device is powered on and on the same Wi-Fi / network.");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return G_SOURCE_REMOVE;
      }, nullptr);
    }
  }).detach();
}

static void OnRescanClicked(GtkButton* button, gpointer user_data) {
  LOG_INFO << "[UI] Rescanning for Cast devices...";
  CastEngine::Instance().GetDiscovery().TriggerScan();
  g_timeout_add(1500, [](gpointer) -> gboolean {
    UpdateDeviceComboUI();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

static void OnAddIpClicked(GtkButton* button, gpointer user_data) {
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Add Cast Device by IP", GTK_WINDOW(g_app.window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL, "_Add Device", GTK_RESPONSE_ACCEPT, nullptr);

  GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 16);

  GtkWidget* label = gtk_label_new("Enter IP Address of Google Cast / TV:");
  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), "192.168.1.");

  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(content_area), box);
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    std::string ip = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!ip.empty()) {
      CastDevice d;
      d.id = ip;
      d.name = "Cast Device (" + ip + ")";
      d.model_name = "Custom Chromecast";
      d.ip_address = ip;
      d.port = 8009;
      d.capabilities = kCapVideoOut | kCapAudioOut;
      CastEngine::Instance().GetDiscovery().AddOrUpdateDevice(d);
      UpdateDeviceComboUI();
    }
  }
  gtk_widget_destroy(dialog);
}

static void OnViewLogsClicked(GtkButton* button, gpointer user_data) {
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "CastMirror Debug Logs", GTK_WINDOW(g_app.window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Close", GTK_RESPONSE_CLOSE, nullptr);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 750, 480);

  GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  GtkWidget* text_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);

  const char* home = std::getenv("HOME");
  std::string log_file = (home ? std::string(home) : "/tmp") + "/.config/castmirror/castmirror.log";
  std::ifstream f(log_file);
  std::string log_contents;
  if (f.is_open()) {
    std::stringstream ss;
    ss << f.rdbuf();
    log_contents = ss.str();
  } else {
    log_contents = "No log file found at " + log_file;
  }

  GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
  gtk_text_buffer_set_text(buffer, log_contents.c_str(), -1);

  GtkTextIter end_iter;
  gtk_text_buffer_get_end_iter(buffer, &end_iter);
  gtk_text_buffer_place_cursor(buffer, &end_iter);

  gtk_container_add(GTK_CONTAINER(scrolled), text_view);
  gtk_box_pack_start(GTK_BOX(content_area), scrolled, TRUE, TRUE, 0);
  gtk_widget_show_all(dialog);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

int main(int argc, char** argv) {
  gtk_init(&argc, &argv);
  ApplyCustomCss();

  auto& engine = CastEngine::Instance();
  engine.Initialize();
  engine.StartDiscovery();

  // Create main window
  g_app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(g_app.window), "CastMirror - Display Casting");
  gtk_window_set_default_size(GTK_WINDOW(g_app.window), 540, 680);
  gtk_window_set_position(GTK_WINDOW(g_app.window), GTK_WIN_POS_CENTER);
  g_signal_connect(g_app.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

  GtkWidget* main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(g_app.window), main_vbox);

  // 1. Header Box
  GtkWidget* header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(header_box), "header-box");

  GtkWidget* title_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget* title_label = gtk_label_new("CastMirror");
  gtk_widget_set_halign(title_label, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_label), "title-label");

  GtkWidget* sub_label = gtk_label_new("Native Low-Latency Chromecast Mirroring");
  gtk_widget_set_halign(sub_label, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(sub_label), "subtitle-label");

  gtk_box_pack_start(GTK_BOX(title_vbox), title_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(title_vbox), sub_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header_box), title_vbox, TRUE, TRUE, 0);

  // Status Badge
  g_app.status_badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app.status_badge), "status-badge");
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app.status_badge), "status-idle");
  g_app.status_label = gtk_label_new("○ READY");
  gtk_box_pack_start(GTK_BOX(g_app.status_badge), g_app.status_label, TRUE, TRUE, 0);

  GtkWidget* logs_btn = gtk_button_new_with_label("📋 Logs");
  gtk_widget_set_tooltip_text(logs_btn, "View live CastMirror debug logs");
  g_signal_connect(logs_btn, "clicked", G_CALLBACK(OnViewLogsClicked), nullptr);

  gtk_box_pack_end(GTK_BOX(header_box), logs_btn, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(header_box), g_app.status_badge, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);

  // Content Area
  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(content_box), 20);
  gtk_box_pack_start(GTK_BOX(main_vbox), content_box, TRUE, TRUE, 0);

  // 2. Target Device Card
  GtkWidget* dev_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(dev_card), "card-box");
  GtkWidget* dev_title = gtk_label_new("TARGET CAST TV / DEVICE");
  gtk_widget_set_halign(dev_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(dev_title), "card-title");
  gtk_box_pack_start(GTK_BOX(dev_card), dev_title, FALSE, FALSE, 0);

  GtkWidget* dev_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_app.device_combo = gtk_combo_box_text_new();
  gtk_box_pack_start(GTK_BOX(dev_row), g_app.device_combo, TRUE, TRUE, 0);

  g_app.rescan_button = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(g_app.rescan_button, "Rescan LAN for Google Cast Devices");
  g_signal_connect(g_app.rescan_button, "clicked", G_CALLBACK(OnRescanClicked), nullptr);
  gtk_box_pack_start(GTK_BOX(dev_row), g_app.rescan_button, FALSE, FALSE, 0);

  g_app.add_ip_button = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(g_app.add_ip_button, "Add Custom Device IP");
  g_signal_connect(g_app.add_ip_button, "clicked", G_CALLBACK(OnAddIpClicked), nullptr);
  gtk_box_pack_start(GTK_BOX(dev_row), g_app.add_ip_button, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(dev_card), dev_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), dev_card, FALSE, FALSE, 0);

  // 3. Settings Card (Display, Quality, Audio)
  GtkWidget* set_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_style_context_add_class(gtk_widget_get_style_context(set_card), "card-box");
  GtkWidget* set_title = gtk_label_new("MIRRORING CONFIGURATION");
  gtk_widget_set_halign(set_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(set_title), "card-title");
  gtk_box_pack_start(GTK_BOX(set_card), set_title, FALSE, FALSE, 0);

  // Display Row
  GtkWidget* grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_widget_set_hexpand(grid, TRUE);

  GtkWidget* lbl_disp = gtk_label_new("Display Source:");
  gtk_widget_set_halign(lbl_disp, GTK_ALIGN_START);
  g_app.display_combo = gtk_combo_box_text_new();
  gtk_grid_attach(GTK_GRID(grid), lbl_disp, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), g_app.display_combo, 1, 0, 1, 1);

  // Preset Row
  GtkWidget* lbl_preset = gtk_label_new("Quality Preset:");
  gtk_widget_set_halign(lbl_preset, GTK_ALIGN_START);
  g_app.preset_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.preset_combo), "Auto (Adaptive)");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.preset_combo), "High (1080p60 / 4K)");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.preset_combo), "Balanced (1080p30)");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_app.preset_combo), "Smooth (720p60 Low-Latency)");
  gtk_combo_box_set_active(GTK_COMBO_BOX(g_app.preset_combo),
                          ComboIndexFromPreset(ConfigStore::Instance().Get().quality_preset));
  gtk_grid_attach(GTK_GRID(grid), lbl_preset, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), g_app.preset_combo, 1, 1, 1, 1);

  GtkWidget* lbl_bitrate = gtk_label_new("Video bitrate:");
  gtk_widget_set_halign(lbl_bitrate, GTK_ALIGN_START);
  GtkWidget* bitrate_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_app.bitrate_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1.0, 25.0, 0.5);
  gtk_scale_set_digits(GTK_SCALE(g_app.bitrate_scale), 1);
  gtk_scale_set_draw_value(GTK_SCALE(g_app.bitrate_scale), FALSE);
  gtk_widget_set_hexpand(g_app.bitrate_scale, TRUE);
  gtk_widget_set_tooltip_text(g_app.bitrate_scale,
      "Video bitrate for the selected quality preset. Defaults to that profile's normal bitrate until you change it. Locked while casting.");
  g_app.bitrate_value_label = gtk_label_new("8.0 Mbps");
  gtk_widget_set_size_request(g_app.bitrate_value_label, 84, -1);
  gtk_widget_set_halign(g_app.bitrate_value_label, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(bitrate_row), g_app.bitrate_scale, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(bitrate_row), g_app.bitrate_value_label, FALSE, FALSE, 0);
  gtk_grid_attach(GTK_GRID(grid), lbl_bitrate, 0, 2, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), bitrate_row, 1, 2, 1, 1);

  // Audio Row
  GtkWidget* lbl_audio = gtk_label_new("Audio Mirroring:");
  gtk_widget_set_halign(lbl_audio, GTK_ALIGN_START);
  g_app.audio_switch = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(g_app.audio_switch), TRUE);
  gtk_widget_set_halign(g_app.audio_switch, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), lbl_audio, 0, 3, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), g_app.audio_switch, 1, 3, 1, 1);

  g_signal_connect(g_app.preset_combo, "changed", G_CALLBACK(OnPresetChanged), nullptr);
  g_signal_connect(g_app.bitrate_scale, "value-changed", G_CALLBACK(OnBitrateSliderChanged), nullptr);
  SyncBitrateSliderToPreset();

  gtk_box_pack_start(GTK_BOX(set_card), grid, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), set_card, FALSE, FALSE, 0);

  // 4. Primary Cast Action Button
  GtkWidget* btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  g_app.cast_button = gtk_button_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app.cast_button), "btn-cast-start");

  GtkWidget* btn_inner_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(btn_inner_box, GTK_ALIGN_CENTER);

  g_app.spinner = gtk_spinner_new();
  gtk_widget_set_visible(g_app.spinner, FALSE);
  gtk_box_pack_start(GTK_BOX(btn_inner_box), g_app.spinner, FALSE, FALSE, 0);

  g_app.cast_button_label = gtk_label_new("⚡  CAST DISPLAY");
  gtk_box_pack_start(GTK_BOX(btn_inner_box), g_app.cast_button_label, FALSE, FALSE, 0);

  gtk_container_add(GTK_CONTAINER(g_app.cast_button), btn_inner_box);
  g_signal_connect(g_app.cast_button, "clicked", G_CALLBACK(OnCastButtonClicked), nullptr);

  gtk_box_pack_start(GTK_BOX(btn_box), g_app.cast_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), btn_box, FALSE, FALSE, 8);

  // 5. Telemetry Live Stats Box (hidden when idle)
  g_app.stats_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(g_app.stats_box), "card-box");
  GtkWidget* stats_title = gtk_label_new("LIVE STREAM TELEMETRY");
  gtk_widget_set_halign(stats_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(stats_title), "card-title");
  gtk_box_pack_start(GTK_BOX(g_app.stats_box), stats_title, FALSE, FALSE, 0);

  GtkWidget* sgrid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(sgrid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(sgrid), 12);
  gtk_grid_set_column_homogeneous(GTK_GRID(sgrid), TRUE);

  auto make_stat = [](const char* title, GtkWidget** out_val) -> GtkWidget* {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "stat-card");
    *out_val = gtk_label_new("--");
    gtk_style_context_add_class(gtk_widget_get_style_context(*out_val), "stat-value");
    GtkWidget* tlbl = gtk_label_new(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(tlbl), "stat-title");
    gtk_box_pack_start(GTK_BOX(card), *out_val, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), tlbl, FALSE, FALSE, 0);
    return card;
  };

  gtk_grid_attach(GTK_GRID(sgrid), make_stat("FRAME RATE", &g_app.fps_label), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), make_stat("BITRATE", &g_app.bitrate_label), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), make_stat("ROUND TRIP (RTT)", &g_app.rtt_label), 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), make_stat("PACKET LOSS", &g_app.loss_label), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(sgrid), make_stat("TARGET DELAY", &g_app.delay_label), 1, 1, 2, 1);

  gtk_box_pack_start(GTK_BOX(g_app.stats_box), sgrid, FALSE, FALSE, 0);
  gtk_widget_set_visible(g_app.stats_box, FALSE);
  gtk_box_pack_start(GTK_BOX(content_box), g_app.stats_box, FALSE, FALSE, 0);

  // Initialize UI data
  UpdateDeviceComboUI();
  UpdateDisplayComboUI();

  // Engine callbacks
  engine.SetOnStateChanged([](SessionState old_s, SessionState new_s, const std::string& msg) {
    g_idle_add(UpdateStateUIIdle, GINT_TO_POINTER(static_cast<int>(new_s)));
  });

  engine.GetDiscovery().SetCallback([](const std::vector<CastDevice>&) {
    g_idle_add([](gpointer) -> gboolean {
      UpdateDeviceComboUI();
      return G_SOURCE_REMOVE;
    }, nullptr);
  });

  // Telemetry Timer (every 500ms)
  g_timeout_add(500, OnStatsTimer, nullptr);

  gtk_widget_show_all(g_app.window);
  gtk_main();

  engine.Shutdown();
  return 0;
}
