#include "cast_tab.h"
#include "gui_app.h"
#include "widgets.h"
#include "help_copy.h"
#include "castcore/cast_engine.h"
#include "castcore/config.h"
#include "castcore/capability_model.h"
#include "castcore/device_selection.h"
#include "castcore/logger.h"
#include <arpa/inet.h>
#include <sstream>
#include <iomanip>
#include <thread>

namespace castcore::gui {

namespace {

GtkWidget* MakeDeviceRowWidget(const CastDevice& dev) {
  GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  // Line 1: Friendly name + Status Pill
  GtkWidget* top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* name_lbl = gtk_label_new(dev.name.c_str());
  gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(name_lbl), "device-name");
  gtk_box_pack_start(GTK_BOX(top_box), name_lbl, TRUE, TRUE, 0);

  GtkWidget* status_pill = gtk_label_new(DeviceStatusToString(dev.status));
  gtk_style_context_add_class(gtk_widget_get_style_context(status_pill), "status-badge");
  if (dev.status == DeviceStatus::kReady) {
    gtk_style_context_add_class(gtk_widget_get_style_context(status_pill), "status-live");
  } else if (dev.status == DeviceStatus::kBusy) {
    gtk_style_context_add_class(gtk_widget_get_style_context(status_pill), "status-connecting");
  } else {
    gtk_style_context_add_class(gtk_widget_get_style_context(status_pill), "status-idle");
  }
  gtk_box_pack_end(GTK_BOX(top_box), status_pill, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(row_box), top_box, FALSE, FALSE, 0);

  // Line 2: Model, IP, Capability Family
  DeviceCapabilities caps = CapabilityModel::Evaluate(dev);
  std::string meta = dev.model_name + "  •  " + dev.ip_address + "  •  " + caps.device_family;
  GtkWidget* meta_lbl = gtk_label_new(meta.c_str());
  gtk_widget_set_halign(meta_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(meta_lbl), "device-meta");
  gtk_box_pack_start(GTK_BOX(row_box), meta_lbl, FALSE, FALSE, 0);

  // Line 3: Capability limit explanation
  std::ostringstream ss;
  ss << "Supports up to " << caps.max_resolution.width << "x" << caps.max_resolution.height
     << " @" << caps.max_fps << "fps (~" << (caps.max_bitrate_kbps / 1000) << " Mbps)";
  GtkWidget* caps_lbl = gtk_label_new(ss.str().c_str());
  gtk_widget_set_halign(caps_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(caps_lbl), "device-caps");
  gtk_box_pack_start(GTK_BOX(row_box), caps_lbl, FALSE, FALSE, 0);

  return row_box;
}

GtkWidget* MakeDisplayRowWidget(const DisplayInfo& disp) {
  GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

  GtkWidget* top_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  std::string title = disp.name.empty() ? ("Display " + std::to_string(disp.id)) : disp.name;
  GtkWidget* title_lbl = gtk_label_new(title.c_str());
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(title_lbl), "device-name");
  gtk_box_pack_start(GTK_BOX(top_box), title_lbl, TRUE, TRUE, 0);

  if (disp.is_primary) {
    GtkWidget* pri_pill = gtk_label_new("Primary");
    gtk_style_context_add_class(gtk_widget_get_style_context(pri_pill), "status-badge");
    gtk_style_context_add_class(gtk_widget_get_style_context(pri_pill), "status-idle");
    gtk_box_pack_end(GTK_BOX(top_box), pri_pill, FALSE, FALSE, 0);
  }
  gtk_box_pack_start(GTK_BOX(row_box), top_box, FALSE, FALSE, 0);

  std::ostringstream ss;
  ss << disp.width << "x" << disp.height << " @" << disp.refresh_rate << " Hz  •  Display ID " << disp.id;
  GtkWidget* sub_lbl = gtk_label_new(ss.str().c_str());
  gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(sub_lbl), "device-meta");
  gtk_box_pack_start(GTK_BOX(row_box), sub_lbl, FALSE, FALSE, 0);

  return row_box;
}

bool DevicesEqual(const std::vector<CastDevice>& a, const std::vector<CastDevice>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id ||
        a[i].name != b[i].name ||
        a[i].model_name != b[i].model_name ||
        a[i].ip_address != b[i].ip_address ||
        a[i].status != b[i].status ||
        a[i].port != b[i].port) {
      return false;
    }
  }
  return true;
}

bool DisplaysEqual(const std::vector<DisplayInfo>& a, const std::vector<DisplayInfo>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].id != b[i].id ||
        a[i].name != b[i].name ||
        a[i].width != b[i].width ||
        a[i].height != b[i].height ||
        a[i].refresh_rate != b[i].refresh_rate ||
        a[i].is_primary != b[i].is_primary) {
      return false;
    }
  }
  return true;
}

}  // namespace

CastTab::CastTab(GuiApp* app) : app_(app) {
  BuildUi();
}

CastTab::~CastTab() = default;

void CastTab::BuildUi() {
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  root_widget_ = scroller;

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
  gtk_container_set_border_width(GTK_CONTAINER(content_box), 18);
  gtk_container_add(GTK_CONTAINER(scroller), content_box);

  // 1. Television Section
  GtkWidget* dev_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(dev_section), "card-box");
  GtkWidget* dev_header = MakeSectionHeader(copy::kSectionTelevision, copy::kSectionTelevisionHelp);
  gtk_box_pack_start(GTK_BOX(dev_section), dev_header, FALSE, FALSE, 0);

  // Empty state card
  empty_device_card_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_device_card_), "card-box");
  GtkWidget* empty_title = gtk_label_new(copy::kEmptyDevicesTitle);
  gtk_widget_set_halign(empty_title, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_title), "setting-title");
  gtk_box_pack_start(GTK_BOX(empty_device_card_), empty_title, FALSE, FALSE, 0);

  GtkWidget* empty_body = gtk_label_new(copy::kEmptyDevicesBody);
  gtk_widget_set_halign(empty_body, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(empty_body), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(empty_body), "setting-help");
  gtk_box_pack_start(GTK_BOX(empty_device_card_), empty_body, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(dev_section), empty_device_card_, FALSE, FALSE, 0);

  // Device ListBox
  device_list_box_ = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(device_list_box_), GTK_SELECTION_SINGLE);
  g_signal_connect(device_list_box_, "row-selected", G_CALLBACK(+[](GtkListBox* box, GtkListBoxRow* row, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    self->OnDeviceRowSelected(box, row);
  }), this);
  gtk_box_pack_start(GTK_BOX(dev_section), device_list_box_, FALSE, FALSE, 0);

  // Device Toolbar
  GtkWidget* dev_tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  rescan_btn_ = gtk_button_new_from_icon_name("view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(rescan_btn_, copy::kRescanTooltip);
  g_signal_connect(rescan_btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    static_cast<CastTab*>(user_data)->app_->TriggerRescan();
  }), this);
  gtk_box_pack_start(GTK_BOX(dev_tb), rescan_btn_, FALSE, FALSE, 0);

  add_ip_btn_ = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(add_ip_btn_, copy::kAddIpTooltip);
  g_signal_connect(add_ip_btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    static_cast<CastTab*>(user_data)->OnAddIpClicked();
  }), this);
  gtk_box_pack_start(GTK_BOX(dev_tb), add_ip_btn_, FALSE, FALSE, 0);

  remove_btn_ = gtk_button_new_from_icon_name("user-trash-symbolic", GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(remove_btn_, copy::kRemoveDeviceTooltip);
  gtk_widget_set_sensitive(remove_btn_, FALSE);
  g_signal_connect(remove_btn_, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    if (!self->selected_device_id_.empty()) {
      CastEngine::Instance().GetDiscovery().RemoveDevice(self->selected_device_id_);
      self->RefreshDevices();
    }
  }), this);
  gtk_box_pack_start(GTK_BOX(dev_tb), remove_btn_, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(dev_section), dev_tb, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), dev_section, FALSE, FALSE, 0);

  // 2. Display Source Section
  GtkWidget* disp_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(disp_section), "card-box");
  GtkWidget* disp_header = MakeSectionHeader(copy::kSectionDisplay, copy::kSectionDisplayHelp);
  gtk_box_pack_start(GTK_BOX(disp_section), disp_header, FALSE, FALSE, 0);

  display_list_box_ = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(display_list_box_), GTK_SELECTION_SINGLE);
  g_signal_connect(display_list_box_, "row-selected", G_CALLBACK(+[](GtkListBox* box, GtkListBoxRow* row, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    self->OnDisplayRowSelected(box, row);
  }), this);
  gtk_box_pack_start(GTK_BOX(disp_section), display_list_box_, FALSE, FALSE, 0);

  const char* wayland_env = std::getenv("WAYLAND_DISPLAY");
  bool force_x11 = ConfigStore::Instance().Get().force_x11_capture;
  if (wayland_env && wayland_env[0] != '\0' && !force_x11) {
    wayland_note_lbl_ = gtk_label_new(copy::kWaylandPortalNote);
    gtk_widget_set_halign(wayland_note_lbl_, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(wayland_note_lbl_), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(wayland_note_lbl_), "card-desc");
    gtk_box_pack_start(GTK_BOX(disp_section), wayland_note_lbl_, FALSE, FALSE, 0);
  }

  gtk_box_pack_start(GTK_BOX(content_box), disp_section, FALSE, FALSE, 0);

  // 3. Picture Quality Section
  GtkWidget* quality_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(quality_section), "card-box");

  GtkWidget* q_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget* q_header = MakeSectionHeader(copy::kSectionQuality, copy::kSectionQualityHelp);
  gtk_box_pack_start(GTK_BOX(q_header_box), q_header, TRUE, TRUE, 0);
  GtkWidget* q_info_btn = MakeInfoButton(copy::kQualityPopover);
  gtk_box_pack_end(GTK_BOX(q_header_box), q_info_btn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(quality_section), q_header_box, FALSE, FALSE, 0);

  // 4 Radio Cards Grid
  GtkWidget* preset_grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(preset_grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(preset_grid), 8);
  gtk_grid_set_column_homogeneous(GTK_GRID(preset_grid), TRUE);

  auto make_preset_card = [this](const char* title, const char* desc, QualityPreset preset, GtkWidget* group_leader) -> GtkWidget* {
    GtkWidget* radio = gtk_radio_button_new(group_leader ? gtk_radio_button_get_group(GTK_RADIO_BUTTON(group_leader)) : nullptr);
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "preset-card");

    GtkWidget* tlbl = gtk_label_new(title);
    gtk_widget_set_halign(tlbl, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(tlbl), "preset-title");
    gtk_box_pack_start(GTK_BOX(card), tlbl, FALSE, FALSE, 0);

    GtkWidget* dlbl = gtk_label_new(desc);
    gtk_widget_set_halign(dlbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(dlbl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dlbl), "preset-desc");
    gtk_box_pack_start(GTK_BOX(card), dlbl, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(radio), card);

    g_object_set_data(G_OBJECT(radio), "preset_enum", GINT_TO_POINTER(static_cast<int>(preset)));
    auto on_toggle = +[](GtkToggleButton* btn, gpointer user_data) {
      if (gtk_toggle_button_get_active(btn)) {
        auto* self = static_cast<CastTab*>(user_data);
        int p_int = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "preset_enum"));
        self->OnPresetChanged(static_cast<QualityPreset>(p_int));
      }
    };
    g_signal_connect(radio, "toggled", G_CALLBACK(on_toggle), this);

    return radio;
  };

  preset_auto_btn_ = make_preset_card(copy::kPresetAutoTitle, copy::kPresetAutoDesc, QualityPreset::kAuto, nullptr);
  preset_high_btn_ = make_preset_card(copy::kPresetHighTitle, copy::kPresetHighDesc, QualityPreset::kHigh, preset_auto_btn_);
  preset_balanced_btn_ = make_preset_card(copy::kPresetBalancedTitle, copy::kPresetBalancedDesc, QualityPreset::kBalanced, preset_auto_btn_);
  preset_smooth_btn_ = make_preset_card(copy::kPresetSmoothTitle, copy::kPresetSmoothDesc, QualityPreset::kSmooth, preset_auto_btn_);

  gtk_grid_attach(GTK_GRID(preset_grid), preset_auto_btn_, 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(preset_grid), preset_high_btn_, 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(preset_grid), preset_balanced_btn_, 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(preset_grid), preset_smooth_btn_, 1, 1, 1, 1);
  gtk_box_pack_start(GTK_BOX(quality_section), preset_grid, FALSE, FALSE, 0);

  bitrate_note_lbl_ = gtk_label_new("");
  gtk_widget_set_halign(bitrate_note_lbl_, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(bitrate_note_lbl_), "card-desc");
  gtk_box_pack_start(GTK_BOX(quality_section), bitrate_note_lbl_, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), quality_section, FALSE, FALSE, 0);

  // Set initial preset active
  selected_preset_ = ConfigStore::Instance().Get().quality_preset;
  if (selected_preset_ == QualityPreset::kHigh) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preset_high_btn_), TRUE);
  else if (selected_preset_ == QualityPreset::kBalanced) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preset_balanced_btn_), TRUE);
  else if (selected_preset_ == QualityPreset::kSmooth) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preset_smooth_btn_), TRUE);
  else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(preset_auto_btn_), TRUE);
  UpdateBitrateNote();

  // 4. Sound Section
  GtkWidget* sound_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_style_context_add_class(gtk_widget_get_style_context(sound_section), "card-box");

  audio_switch_ = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(audio_switch_), ConfigStore::Instance().Get().audio_enabled);
  g_signal_connect(audio_switch_, "state-set", G_CALLBACK(+[](GtkSwitch*, gboolean state, gpointer user_data) -> gboolean {
    auto* self = static_cast<CastTab*>(user_data);
    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.audio_enabled = state;
    ConfigStore::Instance().Save();
    self->app_->SyncAudioEnabled(state);
    return FALSE;
  }), this);

  GtkWidget* sound_row = MakeSettingRow("Send PC sound to the TV", copy::kSectionSoundHelp, copy::kSoundPopover, audio_switch_);
  gtk_box_pack_start(GTK_BOX(sound_section), sound_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(content_box), sound_section, FALSE, FALSE, 0);
}

void CastTab::UpdateBitrateNote() {
  uint32_t kbps = ConfigStore::Instance().Get().GetPresetBitrateKbps(selected_preset_);
  std::ostringstream ss;
  ss << "Video budget: " << std::fixed << std::setprecision(1) << (kbps / 1000.0)
     << " Mbps (adjust in Settings)";
  gtk_label_set_text(GTK_LABEL(bitrate_note_lbl_), ss.str().c_str());
}

void CastTab::RefreshDevices() {
  auto new_devices = CastEngine::Instance().GetDevices();
  const auto& cfg = ConfigStore::Instance().Get();

  std::string prefer_id = !selected_device_id_.empty() ? selected_device_id_ : cfg.last_device_id;
  int preferred_idx = IndexOfPreferredDevice(new_devices, prefer_id, cfg.last_device_ip);

  // If previous saved device not discovered, insert sticky entry in the UI list
  if (preferred_idx < 0 && !cfg.last_device_ip.empty()) {
    CastDevice sticky;
    sticky.id = cfg.last_device_id.empty() ? cfg.last_device_ip : cfg.last_device_id;
    sticky.name = cfg.last_device_name.empty() ? cfg.last_device_ip : cfg.last_device_name;
    sticky.model_name = "saved";
    sticky.ip_address = cfg.last_device_ip;
    sticky.port = 8009;
    sticky.capabilities = kCapVideoOut | kCapAudioOut;
    new_devices.insert(new_devices.begin(), sticky);
    preferred_idx = 0;
  }

  // If device list has not changed at all, avoid touching widgets to prevent flicker
  if (DevicesEqual(devices_, new_devices)) {
    return;
  }

  updating_ui_ = true;
  devices_ = std::move(new_devices);

  // Clear existing rows
  GList* children = gtk_container_get_children(GTK_CONTAINER(device_list_box_));
  for (GList* iter = children; iter != nullptr; iter = g_list_next(iter)) {
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  }
  g_list_free(children);

  if (devices_.empty()) {
    gtk_widget_set_visible(empty_device_card_, TRUE);
    gtk_widget_set_visible(device_list_box_, FALSE);
    selected_device_id_.clear();
    gtk_widget_set_sensitive(remove_btn_, FALSE);
  } else {
    gtk_widget_set_visible(empty_device_card_, FALSE);
    gtk_widget_set_visible(device_list_box_, TRUE);

    for (const auto& d : devices_) {
      GtkWidget* row_widget = MakeDeviceRowWidget(d);
      gtk_list_box_insert(GTK_LIST_BOX(device_list_box_), row_widget, -1);
    }
    gtk_widget_show_all(device_list_box_);

    if (preferred_idx < 0 || static_cast<size_t>(preferred_idx) >= devices_.size()) {
      preferred_idx = 0;
    }
    GtkListBoxRow* target_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(device_list_box_), preferred_idx);
    if (target_row) {
      gtk_list_box_select_row(GTK_LIST_BOX(device_list_box_), target_row);
      selected_device_id_ = devices_[static_cast<size_t>(preferred_idx)].id;
      bool is_custom = (devices_[static_cast<size_t>(preferred_idx)].model_name == "Custom Chromecast" ||
                        devices_[static_cast<size_t>(preferred_idx)].model_name == "saved");
      gtk_widget_set_sensitive(remove_btn_, is_custom);
    }
  }

  updating_ui_ = false;
}

void CastTab::RefreshDisplays() {
  auto new_displays = CastEngine::Instance().GetDisplays();
  if (DisplaysEqual(displays_, new_displays)) {
    return;
  }

  updating_ui_ = true;
  displays_ = std::move(new_displays);

  GList* children = gtk_container_get_children(GTK_CONTAINER(display_list_box_));
  for (GList* iter = children; iter != nullptr; iter = g_list_next(iter)) {
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  }
  g_list_free(children);

  int prefer = selected_display_id_ != 0 ? selected_display_id_ : ConfigStore::Instance().Get().last_display_id;
  int preferred_idx = IndexOfPreferredDisplay(displays_, prefer);

  for (const auto& d : displays_) {
    GtkWidget* row_widget = MakeDisplayRowWidget(d);
    gtk_list_box_insert(GTK_LIST_BOX(display_list_box_), row_widget, -1);
  }
  gtk_widget_show_all(display_list_box_);

  if (!displays_.empty()) {
    if (preferred_idx < 0 || static_cast<size_t>(preferred_idx) >= displays_.size()) {
      preferred_idx = 0;
    }
    GtkListBoxRow* target_row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(display_list_box_), preferred_idx);
    if (target_row) {
      gtk_list_box_select_row(GTK_LIST_BOX(display_list_box_), target_row);
      selected_display_id_ = displays_[static_cast<size_t>(preferred_idx)].id;
    }
  }

  updating_ui_ = false;
}

void CastTab::OnDeviceRowSelected(GtkListBox*, GtkListBoxRow* row) {
  if (updating_ui_ || !row) return;
  int idx = gtk_list_box_row_get_index(row);
  if (idx >= 0 && static_cast<size_t>(idx) < devices_.size()) {
    const auto& dev = devices_[static_cast<size_t>(idx)];
    if (selected_device_id_ == dev.id) {
      return; // Already selected, no-op
    }
    selected_device_id_ = dev.id;

    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.last_device_id = dev.id;
    cfg.last_device_name = dev.name;
    cfg.last_device_ip = dev.ip_address;
    ConfigStore::Instance().Save();

    bool is_custom = (dev.model_name == "Custom Chromecast" || dev.model_name == "saved");
    gtk_widget_set_sensitive(remove_btn_, is_custom);
  }
}

void CastTab::OnDisplayRowSelected(GtkListBox*, GtkListBoxRow* row) {
  if (updating_ui_ || !row) return;
  int idx = gtk_list_box_row_get_index(row);
  if (idx >= 0 && static_cast<size_t>(idx) < displays_.size()) {
    if (selected_display_id_ == displays_[static_cast<size_t>(idx)].id) {
      return; // Already selected, no-op
    }
    selected_display_id_ = displays_[static_cast<size_t>(idx)].id;
    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.last_display_id = selected_display_id_;
    ConfigStore::Instance().Save();
  }
}
void CastTab::OnPresetChanged(QualityPreset preset) {
  selected_preset_ = preset;
  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.quality_preset = preset;
  ConfigStore::Instance().Save();
  UpdateBitrateNote();
  app_->SyncBitrateSlider(cfg.GetPresetBitrateKbps(preset));
}

void CastTab::SyncAudioSwitch(bool active) {
  if (audio_switch_ && gtk_switch_get_active(GTK_SWITCH(audio_switch_)) != static_cast<gboolean>(active)) {
    gtk_switch_set_active(GTK_SWITCH(audio_switch_), active);
  }
}

bool CastTab::GetAudioEnabled() const {
  return audio_switch_ ? (gtk_switch_get_active(GTK_SWITCH(audio_switch_)) != FALSE) : true;
}

void CastTab::SetControlsSensitive(bool sensitive) {
  gtk_widget_set_sensitive(device_list_box_, sensitive);
  gtk_widget_set_sensitive(display_list_box_, sensitive);
  gtk_widget_set_sensitive(preset_auto_btn_, sensitive);
  gtk_widget_set_sensitive(preset_high_btn_, sensitive);
  gtk_widget_set_sensitive(preset_balanced_btn_, sensitive);
  gtk_widget_set_sensitive(preset_smooth_btn_, sensitive);
  gtk_widget_set_sensitive(rescan_btn_, sensitive);
  gtk_widget_set_sensitive(add_ip_btn_, sensitive);
}

void CastTab::UpdateSessionState(SessionState state, const std::string& message) {
  (void)message;
  bool is_idle = (state == SessionState::kIdle || state == SessionState::kReady ||
                  state == SessionState::kDiscovering || state == SessionState::kFailed);
  SetControlsSensitive(is_idle);
}

void CastTab::OnAddIpClicked() {
  GtkWidget* dialog = gtk_dialog_new_with_buttons(
      "Add Custom Cast Device", app_->GetWindow(),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      "_Cancel", GTK_RESPONSE_CANCEL, "_Add TV", GTK_RESPONSE_ACCEPT, nullptr);

  GtkWidget* content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(box), 16);

  GtkWidget* name_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(name_entry), "Living Room TV (Optional)");
  GtkWidget* name_row = MakeSettingRow("Friendly name", "Optional name for this device card", nullptr, name_entry);
  gtk_box_pack_start(GTK_BOX(box), name_row, FALSE, FALSE, 0);

  GtkWidget* ip_entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(ip_entry), "192.168.1.");
  GtkWidget* ip_row = MakeSettingRow("IPv4 Address", "Local IP address of the Chromecast or Cast TV", nullptr, ip_entry);
  gtk_box_pack_start(GTK_BOX(box), ip_row, FALSE, FALSE, 0);

  GtkWidget* port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_spin), 8009);
  GtkWidget* port_row = MakeSettingRow("Port", "Cast V2 control port (standard is 8009)", nullptr, port_spin);
  gtk_box_pack_start(GTK_BOX(box), port_row, FALSE, FALSE, 0);

  GtkWidget* err_lbl = gtk_label_new("");
  gtk_style_context_add_class(gtk_widget_get_style_context(err_lbl), "card-desc");
  gtk_widget_set_halign(err_lbl, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), err_lbl, FALSE, FALSE, 0);

  gtk_container_add(GTK_CONTAINER(content_area), box);
  gtk_widget_show_all(dialog);

  while (true) {
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response != GTK_RESPONSE_ACCEPT) {
      break;
    }

    std::string ip_str = gtk_entry_get_text(GTK_ENTRY(ip_entry));
    struct sockaddr_in sa{};
    if (inet_pton(AF_INET, ip_str.c_str(), &(sa.sin_addr)) <= 0) {
      gtk_label_set_text(GTK_LABEL(err_lbl), "That is not a valid IPv4 address. Example: 192.168.1.150");
      continue;
    }

    std::string name_str = gtk_entry_get_text(GTK_ENTRY(name_entry));
    if (name_str.empty()) {
      name_str = "Cast device (" + ip_str + ")";
    }
    uint16_t port = static_cast<uint16_t>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(port_spin)));

    CastDevice d;
    d.id = ip_str;
    d.name = name_str;
    d.model_name = "Custom Chromecast";
    d.ip_address = ip_str;
    d.port = port;
    d.capabilities = kCapVideoOut | kCapAudioOut;

    CastEngine::Instance().GetDiscovery().AddOrUpdateDevice(d);
    selected_device_id_ = d.id;
    RefreshDevices();
    break;
  }

  gtk_widget_destroy(dialog);
}

}  // namespace castcore::gui
