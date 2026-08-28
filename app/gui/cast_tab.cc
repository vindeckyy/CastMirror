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
#include <unordered_set>

namespace castcore::gui {

namespace {

inline constexpr const char* kEmptyDevicesDetail =
    "CastMirror uses mDNS to find video-capable Cast devices. If your router blocks mDNS, use "
    "Add by IP or enable LAN scanning in Settings. Audio-only speakers are hidden.";

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

GtkWidget* MakeCircularGlyph(const char* icon_name, int box_px, int icon_px) {
  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "cm-device-glyph");
  gtk_widget_set_size_request(box, box_px, box_px);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(box, FALSE);
  gtk_widget_set_vexpand(box, FALSE);
  gtk_widget_set_overflow(box, GTK_OVERFLOW_HIDDEN);

  GtkWidget* icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), icon_px);
  gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(icon, TRUE);
  gtk_widget_set_vexpand(icon, TRUE);
  gtk_box_append(GTK_BOX(box), icon);
  return box;
}

}  // namespace

CastTab::CastTab(GuiApp* app) : app_(app) {
  BuildUi();
}

CastTab::~CastTab() = default;

void CastTab::BuildUi() {
  GtkWidget* scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  root_widget_ = scroller;

  GtkWidget* clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 960);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 720);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), clamp);

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
  gtk_widget_add_css_class(content_box, "cm-page-content");
  adw_clamp_set_child(ADW_CLAMP(clamp), content_box);

  GtkWidget* heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget* title_lbl = gtk_label_new("Cast your screen");
  gtk_widget_add_css_class(title_lbl, "cm-page-title");
  gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(heading), title_lbl);

  GtkWidget* desc_lbl = gtk_label_new("Choose a nearby display, a screen, and the quality you want.");
  gtk_widget_add_css_class(desc_lbl, "cm-page-description");
  gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(desc_lbl), TRUE);
  gtk_box_append(GTK_BOX(heading), desc_lbl);
  gtk_box_append(GTK_BOX(content_box), heading);

  // 1. Where to cast Section
  dev_section_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(dev_section_, "cm-section-card");

  GtkWidget* dev_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* dev_header = MakeSectionHeader("Where to cast", copy::kSectionTelevisionHelp);
  gtk_widget_set_hexpand(dev_header, TRUE);
  gtk_box_append(GTK_BOX(dev_header_box), dev_header);

  GtkWidget* dev_info_btn = MakeInfoButton("Where to cast", kEmptyDevicesDetail);
  gtk_box_append(GTK_BOX(dev_header_box), dev_info_btn);

  dev_header_actions_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  rescan_btn_ = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_add_css_class(rescan_btn_, "flat");
  gtk_widget_set_tooltip_text(rescan_btn_, copy::kRescanTooltip);
  gtk_accessible_update_property(GTK_ACCESSIBLE(rescan_btn_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, copy::kRescanTooltip, -1);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(rescan_btn_), "win.rescan");
  gtk_box_append(GTK_BOX(dev_header_actions_), rescan_btn_);

  add_ip_btn_ = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(add_ip_btn_, "flat");
  gtk_widget_set_tooltip_text(add_ip_btn_, copy::kAddIpTooltip);
  gtk_accessible_update_property(GTK_ACCESSIBLE(add_ip_btn_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, copy::kAddIpTooltip, -1);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(add_ip_btn_), "win.add-ip");
  gtk_box_append(GTK_BOX(dev_header_actions_), add_ip_btn_);

  remove_btn_ = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(remove_btn_, "flat");
  gtk_widget_set_tooltip_text(remove_btn_, copy::kRemoveDeviceTooltip);
  gtk_accessible_update_property(GTK_ACCESSIBLE(remove_btn_),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, copy::kRemoveDeviceTooltip, -1);
  gtk_widget_set_sensitive(remove_btn_, FALSE);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(remove_btn_), "win.remove-device");
  gtk_box_append(GTK_BOX(dev_header_actions_), remove_btn_);

  gtk_box_append(GTK_BOX(dev_header_box), dev_header_actions_);
  gtk_box_append(GTK_BOX(dev_section_), dev_header_box);

  // Loading box
  dev_loading_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(dev_loading_box_, "cm-empty-state");
  gtk_widget_set_visible(dev_loading_box_, FALSE);

  GtkWidget* spinner = gtk_spinner_new();
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(dev_loading_box_), spinner);

  GtkWidget* loading_title = gtk_label_new("Finding Cast displays…");
  gtk_widget_add_css_class(loading_title, "cm-section-title");
  gtk_widget_set_halign(loading_title, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(dev_loading_box_), loading_title);

  GtkWidget* loading_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(loading_btn_box, GTK_ALIGN_CENTER);

  GtkWidget* load_scan_btn = gtk_button_new_with_label("Scan again");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(load_scan_btn), "win.rescan");
  gtk_widget_set_sensitive(load_scan_btn, FALSE);
  gtk_box_append(GTK_BOX(loading_btn_box), load_scan_btn);

  GtkWidget* load_add_btn = gtk_button_new_with_label("Add by IP");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(load_add_btn), "win.add-ip");
  gtk_box_append(GTK_BOX(loading_btn_box), load_add_btn);

  gtk_box_append(GTK_BOX(dev_loading_box_), loading_btn_box);
  gtk_box_append(GTK_BOX(dev_section_), dev_loading_box_);

  // Empty box
  dev_empty_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(dev_empty_box_, "cm-empty-state");
  gtk_widget_set_visible(dev_empty_box_, FALSE);

  GtkWidget* empty_title = gtk_label_new("No Cast displays found");
  gtk_widget_add_css_class(empty_title, "cm-section-title");
  gtk_widget_set_halign(empty_title, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(dev_empty_box_), empty_title);

  GtkWidget* empty_body = gtk_label_new("Make sure the TV and this computer are on the same network, then scan again.");
  gtk_widget_add_css_class(empty_body, "cm-section-description");
  gtk_widget_set_halign(empty_body, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(empty_body), TRUE);
  gtk_box_append(GTK_BOX(dev_empty_box_), empty_body);

  GtkWidget* empty_btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(empty_btn_box, GTK_ALIGN_CENTER);

  GtkWidget* empty_scan_btn = gtk_button_new_with_label("Scan again");
  gtk_widget_add_css_class(empty_scan_btn, "suggested-action");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(empty_scan_btn), "win.rescan");
  gtk_box_append(GTK_BOX(empty_btn_box), empty_scan_btn);

  GtkWidget* empty_add_btn = gtk_button_new_with_label("Add by IP");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(empty_add_btn), "win.add-ip");
  gtk_box_append(GTK_BOX(empty_btn_box), empty_add_btn);

  gtk_box_append(GTK_BOX(dev_empty_box_), empty_btn_box);
  gtk_box_append(GTK_BOX(dev_section_), dev_empty_box_);

  // Device ListBox
  device_list_box_ = gtk_list_box_new();
  gtk_widget_add_css_class(device_list_box_, "cm-choice-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(device_list_box_), GTK_SELECTION_SINGLE);
  gtk_list_box_set_show_separators(GTK_LIST_BOX(device_list_box_), FALSE);
  gtk_list_box_set_sort_func(
      GTK_LIST_BOX(device_list_box_),
      +[](GtkListBoxRow* row1, GtkListBoxRow* row2, gpointer user_data) -> gint {
        auto* self = static_cast<CastTab*>(user_data);
        const char* id1 = static_cast<const char*>(g_object_get_data(G_OBJECT(row1), "cm_id"));
        const char* id2 = static_cast<const char*>(g_object_get_data(G_OBJECT(row2), "cm_id"));
        int r1 = id1 ? self->GetDeviceRank(id1) : 0;
        int r2 = id2 ? self->GetDeviceRank(id2) : 0;
        return (r1 < r2) ? -1 : ((r1 > r2) ? 1 : 0);
      },
      this, nullptr);
  g_signal_connect(device_list_box_, "row-selected", G_CALLBACK(+[](GtkListBox* box, GtkListBoxRow* row, gpointer user_data) {
    static_cast<CastTab*>(user_data)->OnDeviceRowSelected(box, row);
  }), this);
  gtk_box_append(GTK_BOX(dev_section_), device_list_box_);
  gtk_box_append(GTK_BOX(content_box), dev_section_);

  // 2. Screen to share Section
  disp_section_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(disp_section_, "cm-section-card");

  GtkWidget* disp_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* disp_header = MakeSectionHeader("Screen to share", copy::kSectionDisplayHelp);
  gtk_widget_set_hexpand(disp_header, TRUE);
  gtk_box_append(GTK_BOX(disp_header_box), disp_header);
  gtk_box_append(GTK_BOX(disp_section_), disp_header_box);

  // Empty screens box
  disp_empty_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class(disp_empty_box_, "cm-empty-state");
  gtk_widget_set_visible(disp_empty_box_, FALSE);

  GtkWidget* disp_empty_title = gtk_label_new("No screens available");
  gtk_widget_add_css_class(disp_empty_title, "cm-section-title");
  gtk_widget_set_halign(disp_empty_title, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(disp_empty_box_), disp_empty_title);

  GtkWidget* disp_empty_body = gtk_label_new("CastMirror could not find a screen to share. Check the capture settings, then try again.");
  gtk_widget_add_css_class(disp_empty_body, "cm-section-description");
  gtk_widget_set_halign(disp_empty_body, GTK_ALIGN_CENTER);
  gtk_label_set_wrap(GTK_LABEL(disp_empty_body), TRUE);
  gtk_box_append(GTK_BOX(disp_empty_box_), disp_empty_body);

  GtkWidget* disp_try_btn = gtk_button_new_with_label("Try again");
  gtk_widget_add_css_class(disp_try_btn, "suggested-action");
  gtk_widget_set_halign(disp_try_btn, GTK_ALIGN_CENTER);
  g_signal_connect(disp_try_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    static_cast<CastTab*>(user_data)->RefreshDisplays();
  }), this);
  gtk_box_append(GTK_BOX(disp_empty_box_), disp_try_btn);
  gtk_box_append(GTK_BOX(disp_section_), disp_empty_box_);

  // Display ListBox
  display_list_box_ = gtk_list_box_new();
  gtk_widget_add_css_class(display_list_box_, "cm-choice-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(display_list_box_), GTK_SELECTION_SINGLE);
  gtk_list_box_set_show_separators(GTK_LIST_BOX(display_list_box_), FALSE);
  gtk_list_box_set_sort_func(
      GTK_LIST_BOX(display_list_box_),
      +[](GtkListBoxRow* row1, GtkListBoxRow* row2, gpointer user_data) -> gint {
        auto* self = static_cast<CastTab*>(user_data);
        gpointer p1 = g_object_get_data(G_OBJECT(row1), "cm_disp_id");
        gpointer p2 = g_object_get_data(G_OBJECT(row2), "cm_disp_id");
        int id1 = GPOINTER_TO_INT(p1);
        int id2 = GPOINTER_TO_INT(p2);
        int r1 = self->GetDisplayRank(id1);
        int r2 = self->GetDisplayRank(id2);
        return (r1 < r2) ? -1 : ((r1 > r2) ? 1 : 0);
      },
      this, nullptr);
  g_signal_connect(display_list_box_, "row-selected", G_CALLBACK(+[](GtkListBox* box, GtkListBoxRow* row, gpointer user_data) {
    static_cast<CastTab*>(user_data)->OnDisplayRowSelected(box, row);
  }), this);
  gtk_box_append(GTK_BOX(disp_section_), display_list_box_);

  // Wayland info banner
  const char* wayland_env = std::getenv("WAYLAND_DISPLAY");
  bool force_x11 = ConfigStore::Instance().Get().force_x11_capture;
  if (wayland_env && wayland_env[0] != '\0' && !force_x11) {
    wayland_banner_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(wayland_banner_, "cm-info-banner");
    GtkWidget* info_icon = gtk_image_new_from_icon_name("dialog-information-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(info_icon), 18);
    gtk_box_append(GTK_BOX(wayland_banner_), info_icon);

    GtkWidget* wayland_lbl = gtk_label_new(copy::kWaylandPortalNote);
    gtk_widget_set_halign(wayland_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(wayland_lbl), TRUE);
    gtk_box_append(GTK_BOX(wayland_banner_), wayland_lbl);

    gtk_box_append(GTK_BOX(disp_section_), wayland_banner_);
  }
  gtk_box_append(GTK_BOX(content_box), disp_section_);

  // 3. Picture quality Section
  GtkWidget* quality_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(quality_section, "cm-section-card");

  GtkWidget* q_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* q_header = MakeSectionHeader("Picture quality", copy::kSectionQualityHelp);
  gtk_widget_set_hexpand(q_header, TRUE);
  gtk_box_append(GTK_BOX(q_header_box), q_header);
  GtkWidget* q_info_btn = MakeInfoButton("Picture quality", copy::kQualityPopover);
  gtk_box_append(GTK_BOX(q_header_box), q_info_btn);
  gtk_box_append(GTK_BOX(quality_section), q_header_box);

  // FlowBox for Presets
  preset_flow_box_ = gtk_flow_box_new();
  gtk_widget_add_css_class(preset_flow_box_, "cm-preset-grid");
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(preset_flow_box_), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(preset_flow_box_), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(preset_flow_box_), 2);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(preset_flow_box_), 12);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(preset_flow_box_), 12);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(preset_flow_box_), TRUE);

  auto make_preset_card = [this](const char* title, const char* desc, QualityPreset preset, GtkWidget* group_leader) -> GtkWidget* {
    GtkWidget* check = gtk_check_button_new();
    if (group_leader) {
      gtk_check_button_set_group(GTK_CHECK_BUTTON(check), GTK_CHECK_BUTTON(group_leader));
    }
    gtk_widget_add_css_class(check, "cm-preset-card");
    gtk_widget_set_size_request(check, 260, -1);

    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* tlbl = gtk_label_new(title);
    gtk_widget_set_halign(tlbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(tlbl, "heading");
    gtk_box_append(GTK_BOX(card), tlbl);

    GtkWidget* dlbl = gtk_label_new(desc);
    gtk_widget_set_halign(dlbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(dlbl), TRUE);
    gtk_widget_add_css_class(dlbl, "dim-label");
    gtk_box_append(GTK_BOX(card), dlbl);

    gtk_check_button_set_child(GTK_CHECK_BUTTON(check), card);

    g_object_set_data(G_OBJECT(check), "preset_enum", GINT_TO_POINTER(static_cast<int>(preset)));
    g_signal_connect(check, "toggled", G_CALLBACK(+[](GtkCheckButton* btn, gpointer user_data) {
      if (gtk_check_button_get_active(btn)) {
        auto* self = static_cast<CastTab*>(user_data);
        int p_int = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "preset_enum"));
        self->OnPresetChanged(static_cast<QualityPreset>(p_int));
      }
    }), this);

    return check;
  };

  preset_auto_btn_ = make_preset_card(copy::kPresetAutoTitle, "Recommended · adapts to the TV and Wi‑Fi", QualityPreset::kAuto, nullptr);
  preset_high_btn_ = make_preset_card(copy::kPresetHighTitle, "Best detail · up to 1080p60 or 4K30", QualityPreset::kHigh, preset_auto_btn_);
  preset_balanced_btn_ = make_preset_card(copy::kPresetBalancedTitle, "Sharp picture · moderate network use", QualityPreset::kBalanced, preset_auto_btn_);
  preset_smooth_btn_ = make_preset_card(copy::kPresetSmoothTitle, "Steadier motion on busy Wi‑Fi", QualityPreset::kSmooth, preset_auto_btn_);

  gtk_flow_box_append(GTK_FLOW_BOX(preset_flow_box_), preset_auto_btn_);
  gtk_flow_box_append(GTK_FLOW_BOX(preset_flow_box_), preset_high_btn_);
  gtk_flow_box_append(GTK_FLOW_BOX(preset_flow_box_), preset_balanced_btn_);
  gtk_flow_box_append(GTK_FLOW_BOX(preset_flow_box_), preset_smooth_btn_);
  gtk_box_append(GTK_BOX(quality_section), preset_flow_box_);

  bitrate_note_lbl_ = gtk_label_new("");
  gtk_widget_set_halign(bitrate_note_lbl_, GTK_ALIGN_START);
  gtk_widget_add_css_class(bitrate_note_lbl_, "cm-section-description");
  gtk_box_append(GTK_BOX(quality_section), bitrate_note_lbl_);
  gtk_box_append(GTK_BOX(content_box), quality_section);

  // Set initial preset active
  selected_preset_ = ConfigStore::Instance().Get().quality_preset;
  updating_ui_ = true;
  if (selected_preset_ == QualityPreset::kHigh) gtk_check_button_set_active(GTK_CHECK_BUTTON(preset_high_btn_), TRUE);
  else if (selected_preset_ == QualityPreset::kBalanced) gtk_check_button_set_active(GTK_CHECK_BUTTON(preset_balanced_btn_), TRUE);
  else if (selected_preset_ == QualityPreset::kSmooth) gtk_check_button_set_active(GTK_CHECK_BUTTON(preset_smooth_btn_), TRUE);
  else gtk_check_button_set_active(GTK_CHECK_BUTTON(preset_auto_btn_), TRUE);
  updating_ui_ = false;
  UpdateBitrateNote();

  // 4. Sound Section
  sound_section_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(sound_section_, "cm-section-card");

  GtkWidget* s_header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget* s_header = MakeSectionHeader("Sound", copy::kSectionSoundHelp);
  gtk_widget_set_hexpand(s_header, TRUE);
  gtk_box_append(GTK_BOX(s_header_box), s_header);
  GtkWidget* s_info_btn = MakeInfoButton("Sound", copy::kSoundPopover);
  gtk_box_append(GTK_BOX(s_header_box), s_info_btn);
  gtk_box_append(GTK_BOX(sound_section_), s_header_box);

  GtkWidget* sound_list = gtk_list_box_new();
  gtk_widget_add_css_class(sound_list, "cm-choice-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(sound_list), GTK_SELECTION_NONE);
  gtk_list_box_set_show_separators(GTK_LIST_BOX(sound_list), FALSE);

  const auto& sound_cfg = ConfigStore::Instance().Get();

  audio_switch_row_ = adw_switch_row_new();
  gtk_widget_add_css_class(audio_switch_row_, "cm-sound-row");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(audio_switch_row_), copy::kComputerSoundTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(audio_switch_row_), copy::kComputerSoundHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(audio_switch_row_), sound_cfg.audio_enabled);

  g_signal_connect(audio_switch_row_, "notify::active", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    if (self->syncing_audio_ || self->updating_ui_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.audio_enabled = (state != FALSE);
    ConfigStore::Instance().Save();
    self->app_->SyncAudioEnabled(state != FALSE);
    self->UpdateSoundRowSensitivity();
  }), this);

  silence_switch_row_ = adw_switch_row_new();
  gtk_widget_add_css_class(silence_switch_row_, "cm-sound-row");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(silence_switch_row_), copy::kSilenceTitle);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(silence_switch_row_), copy::kSilenceHelp);
  adw_switch_row_set_active(ADW_SWITCH_ROW(silence_switch_row_), sound_cfg.silence_host_speakers);

  g_signal_connect(silence_switch_row_, "notify::active", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    if (self->syncing_audio_ || self->updating_ui_) return;
    gboolean state = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.silence_host_speakers = (state != FALSE);
    ConfigStore::Instance().Save();
    self->app_->SyncSilenceHost(state != FALSE);
  }), this);

  gtk_list_box_append(GTK_LIST_BOX(sound_list), audio_switch_row_);
  gtk_list_box_append(GTK_LIST_BOX(sound_list), silence_switch_row_);
  gtk_box_append(GTK_BOX(sound_section_), sound_list);
  gtk_box_append(GTK_BOX(content_box), sound_section_);
  UpdateSoundRowSensitivity();
}

void CastTab::UpdateBitrateNote() {
  uint32_t kbps = ConfigStore::Instance().Get().GetPresetBitrateKbps(selected_preset_);
  std::ostringstream ss;
  ss << "Video budget: " << std::fixed << std::setprecision(1) << (kbps / 1000.0)
     << " Mbps · Adjust in Settings";
  if (bitrate_note_lbl_) {
    gtk_label_set_text(GTK_LABEL(bitrate_note_lbl_), ss.str().c_str());
  }
}

int CastTab::GetDeviceRank(const std::string& id) const {
  auto it = device_row_widgets_.find(id);
  if (it != device_row_widgets_.end()) {
    return it->second.rank;
  }
  return 999999;
}

int CastTab::GetDisplayRank(int id) const {
  auto it = display_row_widgets_.find(id);
  if (it != display_row_widgets_.end()) {
    return it->second.rank;
  }
  return 999999;
}

void CastTab::UpdateDeviceSectionState() {
  if (devices_.empty()) {
    gtk_widget_set_visible(device_list_box_, FALSE);
    gtk_widget_set_visible(dev_header_actions_, FALSE);
    if (scan_in_progress_) {
      gtk_widget_set_visible(dev_loading_box_, TRUE);
      gtk_widget_set_visible(dev_empty_box_, FALSE);
    } else {
      gtk_widget_set_visible(dev_loading_box_, FALSE);
      gtk_widget_set_visible(dev_empty_box_, TRUE);
    }
  } else {
    gtk_widget_set_visible(dev_loading_box_, FALSE);
    gtk_widget_set_visible(dev_empty_box_, FALSE);
    gtk_widget_set_visible(device_list_box_, TRUE);
    gtk_widget_set_visible(dev_header_actions_, TRUE);
  }
}

CastTab::DeviceRowWidgets CastTab::CreateDeviceRow(const CastDevice& dev, int rank) {
  DeviceRowWidgets w;
  w.rank = rank;
  w.device = dev;

  GtkWidget* row = gtk_list_box_row_new();
  w.row = row;
  gtk_widget_set_size_request(row, -1, 84);

  g_object_set_data_full(G_OBJECT(row), "cm_id", g_strdup(dev.id.c_str()), g_free);

  GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(row_box, "cm-device-row");
  GtkWidget* device_glyph = MakeCircularGlyph("video-display-symbolic", 48, 22);
  gtk_widget_set_valign(device_glyph, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row_box), device_glyph);

  // Text column
  GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

  // Title
  w.title_lbl = gtk_label_new(dev.name.c_str());
  gtk_widget_set_halign(w.title_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(w.title_lbl), TRUE);
  gtk_label_set_lines(GTK_LABEL(w.title_lbl), 2);
  gtk_label_set_ellipsize(GTK_LABEL(w.title_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(w.title_lbl, "heading");
  gtk_box_append(GTK_BOX(text_box), w.title_lbl);

  // Subtitle 1
  std::string sub1 = (dev.model_name == "saved" || dev.model_name == "Custom Chromecast")
                         ? ("Saved display · " + dev.ip_address)
                         : (dev.model_name + " · " + dev.ip_address);
  w.sub1_lbl = gtk_label_new(sub1.c_str());
  gtk_widget_set_halign(w.sub1_lbl, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(w.sub1_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(w.sub1_lbl, "dim-label");
  gtk_box_append(GTK_BOX(text_box), w.sub1_lbl);

  // Subtitle 2
  DeviceCapabilities caps = CapabilityModel::Evaluate(dev);
  std::ostringstream ss2;
  ss2 << "Up to " << caps.max_resolution.width << " × " << caps.max_resolution.height
      << " at " << caps.max_fps << " fps · " << (caps.max_bitrate_kbps / 1000) << " Mbps";
  w.sub2_lbl = gtk_label_new(ss2.str().c_str());
  gtk_widget_set_halign(w.sub2_lbl, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(w.sub2_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(w.sub2_lbl, "dim-label");
  gtk_box_append(GTK_BOX(text_box), w.sub2_lbl);

  gtk_box_append(GTK_BOX(row_box), text_box);

  // Right box: status pill + select icon
  GtkWidget* right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(right_box, GTK_ALIGN_CENTER);

  std::string status_text;
  const char* status_state_class = "is-idle";
  if (dev.model_name == "saved") {
    status_text = "Saved";
    status_state_class = "is-idle";
  } else if (dev.status == DeviceStatus::kReady) {
    status_text = "Available";
    status_state_class = "is-live";
  } else if (dev.status == DeviceStatus::kBusy) {
    status_text = "In use";
    status_state_class = "is-warning";
  } else {
    status_text = "Offline";
    status_state_class = "is-error";
  }

  w.status_pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class(w.status_pill, "cm-status-pill");
  gtk_widget_add_css_class(w.status_pill, status_state_class);

  w.status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(w.status_dot, "cm-status-dot");
  gtk_box_append(GTK_BOX(w.status_pill), w.status_dot);

  w.status_lbl = gtk_label_new(status_text.c_str());
  gtk_box_append(GTK_BOX(w.status_pill), w.status_lbl);
  gtk_box_append(GTK_BOX(right_box), w.status_pill);

  w.select_icon = gtk_image_new_from_icon_name("object-select-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(w.select_icon), 18);
  gtk_widget_set_visible(w.select_icon, FALSE);
  gtk_box_append(GTK_BOX(right_box), w.select_icon);

  gtk_box_append(GTK_BOX(row_box), right_box);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
  return w;
}

CastTab::DisplayRowWidgets CastTab::CreateDisplayRow(const DisplayInfo& disp, int rank) {
  DisplayRowWidgets w;
  w.rank = rank;
  w.display = disp;

  GtkWidget* row = gtk_list_box_row_new();
  w.row = row;
  gtk_widget_set_size_request(row, -1, 72);

  g_object_set_data(G_OBJECT(row), "cm_disp_id", GINT_TO_POINTER(disp.id));

  GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_add_css_class(row_box, "cm-display-row");
  GtkWidget* display_glyph = MakeCircularGlyph("video-single-display-symbolic", 40, 18);
  gtk_widget_set_valign(display_glyph, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row_box), display_glyph);

  // Text column
  GtkWidget* text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

  bool is_wayland_placeholder = (disp.name == "Wayland (pick in system dialog)");

  std::string title;
  std::string subtitle;
  if (is_wayland_placeholder) {
    title = "Choose in system dialog";
    subtitle = "The desktop portal will ask what to share when casting.";
  } else {
    title = disp.name.empty() ? ("Display " + std::to_string(disp.id)) : disp.name;
    std::ostringstream ss;
    ss << disp.width << " × " << disp.height << " · " << disp.refresh_rate << " Hz";
    subtitle = ss.str();
  }

  w.title_lbl = gtk_label_new(title.c_str());
  gtk_widget_set_halign(w.title_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(w.title_lbl), TRUE);
  gtk_label_set_lines(GTK_LABEL(w.title_lbl), 2);
  gtk_widget_add_css_class(w.title_lbl, "heading");
  gtk_box_append(GTK_BOX(text_box), w.title_lbl);

  w.sub_lbl = gtk_label_new(subtitle.c_str());
  gtk_widget_set_halign(w.sub_lbl, GTK_ALIGN_START);
  gtk_label_set_ellipsize(GTK_LABEL(w.sub_lbl), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(w.sub_lbl, "dim-label");
  gtk_box_append(GTK_BOX(text_box), w.sub_lbl);

  gtk_box_append(GTK_BOX(row_box), text_box);

  // Right box: Primary pill + select icon
  GtkWidget* right_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(right_box, GTK_ALIGN_CENTER);

  if (!is_wayland_placeholder && disp.is_primary) {
    w.primary_pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(w.primary_pill, "cm-status-pill");
    gtk_widget_add_css_class(w.primary_pill, "is-idle");
    GtkWidget* pri_lbl = gtk_label_new("Primary");
    gtk_box_append(GTK_BOX(w.primary_pill), pri_lbl);
    gtk_box_append(GTK_BOX(right_box), w.primary_pill);
  }

  w.select_icon = gtk_image_new_from_icon_name("object-select-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(w.select_icon), 18);
  gtk_widget_set_visible(w.select_icon, FALSE);
  gtk_box_append(GTK_BOX(right_box), w.select_icon);

  gtk_box_append(GTK_BOX(row_box), right_box);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
  return w;
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
    sticky.status = DeviceStatus::kOffline;
    new_devices.insert(new_devices.begin(), sticky);
    preferred_idx = 0;
  }

  bool equal = DevicesEqual(devices_, new_devices);
  if (equal && !devices_.empty() && !device_row_widgets_.empty()) {
    UpdateDeviceSectionState();
    return;
  }

  updating_ui_ = true;
  devices_ = std::move(new_devices);

  std::unordered_set<std::string> current_ids;
  for (size_t i = 0; i < devices_.size(); ++i) {
    const auto& d = devices_[i];
    current_ids.insert(d.id);
    auto it = device_row_widgets_.find(d.id);
    if (it != device_row_widgets_.end()) {
      auto& w = it->second;
      w.rank = static_cast<int>(i);
      w.device = d;

      gtk_label_set_text(GTK_LABEL(w.title_lbl), d.name.c_str());

      std::string sub1 = (d.model_name == "saved" || d.model_name == "Custom Chromecast")
                             ? ("Saved display · " + d.ip_address)
                             : (d.model_name + " · " + d.ip_address);
      gtk_label_set_text(GTK_LABEL(w.sub1_lbl), sub1.c_str());

      DeviceCapabilities caps = CapabilityModel::Evaluate(d);
      std::ostringstream ss2;
      ss2 << "Up to " << caps.max_resolution.width << " × " << caps.max_resolution.height
          << " at " << caps.max_fps << " fps · " << (caps.max_bitrate_kbps / 1000) << " Mbps";
      gtk_label_set_text(GTK_LABEL(w.sub2_lbl), ss2.str().c_str());

      std::string status_text;
      const char* status_state_class = "is-idle";
      if (d.model_name == "saved") {
        status_text = "Saved";
        status_state_class = "is-idle";
      } else if (d.status == DeviceStatus::kReady) {
        status_text = "Available";
        status_state_class = "is-live";
      } else if (d.status == DeviceStatus::kBusy) {
        status_text = "In use";
        status_state_class = "is-warning";
      } else {
        status_text = "Offline";
        status_state_class = "is-error";
      }

      gtk_widget_remove_css_class(w.status_pill, "is-idle");
      gtk_widget_remove_css_class(w.status_pill, "is-live");
      gtk_widget_remove_css_class(w.status_pill, "is-warning");
      gtk_widget_remove_css_class(w.status_pill, "is-error");
      gtk_widget_add_css_class(w.status_pill, status_state_class);
      gtk_label_set_text(GTK_LABEL(w.status_lbl), status_text.c_str());
    } else {
      DeviceRowWidgets w = CreateDeviceRow(d, static_cast<int>(i));
      gtk_list_box_append(GTK_LIST_BOX(device_list_box_), w.row);
      device_row_widgets_[d.id] = w;
    }
  }

  for (auto it = device_row_widgets_.begin(); it != device_row_widgets_.end();) {
    if (current_ids.find(it->first) == current_ids.end()) {
      gtk_list_box_remove(GTK_LIST_BOX(device_list_box_), it->second.row);
      it = device_row_widgets_.erase(it);
    } else {
      ++it;
    }
  }

  gtk_list_box_invalidate_sort(GTK_LIST_BOX(device_list_box_));

  if (devices_.empty()) {
    selected_device_id_.clear();
  } else {
    bool found_selected = false;
    for (const auto& d : devices_) {
      if (d.id == selected_device_id_) {
        found_selected = true;
        break;
      }
    }
    if (!found_selected) {
      if (preferred_idx >= 0 && static_cast<size_t>(preferred_idx) < devices_.size()) {
        selected_device_id_ = devices_[static_cast<size_t>(preferred_idx)].id;
      } else {
        selected_device_id_ = devices_[0].id;
      }
    }

    auto it = device_row_widgets_.find(selected_device_id_);
    if (it != device_row_widgets_.end()) {
      gtk_list_box_select_row(GTK_LIST_BOX(device_list_box_), GTK_LIST_BOX_ROW(it->second.row));
    }
  }

  for (auto& [id, w] : device_row_widgets_) {
    bool is_selected = (id == selected_device_id_);
    if (w.select_icon) {
      gtk_widget_set_visible(w.select_icon, is_selected);
    }
    if (is_selected) {
      gtk_widget_add_css_class(w.row, "is-selected");
    } else {
      gtk_widget_remove_css_class(w.row, "is-selected");
    }
  }

  if (remove_btn_) {
    gtk_widget_set_sensitive(remove_btn_, IsSelectedDeviceRemovable());
  }

  UpdateDeviceSectionState();
  updating_ui_ = false;
  app_->OnDestinationSelectionChanged();
}

void CastTab::RefreshDisplays() {
  auto new_displays = CastEngine::Instance().GetDisplays();
  bool equal = DisplaysEqual(displays_, new_displays);
  if (equal && !displays_.empty() && !display_row_widgets_.empty()) {
    return;
  }

  updating_ui_ = true;
  displays_ = std::move(new_displays);

  if (displays_.empty()) {
    has_selected_display_ = false;
    selected_display_id_ = 0;
    gtk_widget_set_visible(display_list_box_, FALSE);
    gtk_widget_set_visible(disp_empty_box_, TRUE);
    if (wayland_banner_) gtk_widget_set_visible(wayland_banner_, FALSE);

    for (auto it = display_row_widgets_.begin(); it != display_row_widgets_.end();) {
      gtk_list_box_remove(GTK_LIST_BOX(display_list_box_), it->second.row);
      it = display_row_widgets_.erase(it);
    }
  } else {
    gtk_widget_set_visible(disp_empty_box_, FALSE);
    gtk_widget_set_visible(display_list_box_, TRUE);
    if (wayland_banner_) gtk_widget_set_visible(wayland_banner_, TRUE);

    std::unordered_set<int> current_ids;
    for (size_t i = 0; i < displays_.size(); ++i) {
      const auto& d = displays_[i];
      current_ids.insert(d.id);
      auto it = display_row_widgets_.find(d.id);
      if (it != display_row_widgets_.end()) {
        auto& w = it->second;
        w.rank = static_cast<int>(i);
        w.display = d;

        bool is_wayland_placeholder = (d.name == "Wayland (pick in system dialog)");
        if (is_wayland_placeholder) {
          gtk_label_set_text(GTK_LABEL(w.title_lbl), "Choose in system dialog");
          gtk_label_set_text(GTK_LABEL(w.sub_lbl), "The desktop portal will ask what to share when casting.");
          if (w.primary_pill) gtk_widget_set_visible(w.primary_pill, FALSE);
        } else {
          std::string t = d.name.empty() ? ("Display " + std::to_string(d.id)) : d.name;
          gtk_label_set_text(GTK_LABEL(w.title_lbl), t.c_str());
          std::ostringstream ss;
          ss << d.width << " × " << d.height << " · " << d.refresh_rate << " Hz";
          gtk_label_set_text(GTK_LABEL(w.sub_lbl), ss.str().c_str());
          if (w.primary_pill) gtk_widget_set_visible(w.primary_pill, d.is_primary);
        }
      } else {
        DisplayRowWidgets w = CreateDisplayRow(d, static_cast<int>(i));
        gtk_list_box_append(GTK_LIST_BOX(display_list_box_), w.row);
        display_row_widgets_[d.id] = w;
      }
    }

    for (auto it = display_row_widgets_.begin(); it != display_row_widgets_.end();) {
      if (current_ids.find(it->first) == current_ids.end()) {
        gtk_list_box_remove(GTK_LIST_BOX(display_list_box_), it->second.row);
        it = display_row_widgets_.erase(it);
      } else {
        ++it;
      }
    }

    gtk_list_box_invalidate_sort(GTK_LIST_BOX(display_list_box_));

    int prefer = has_selected_display_
                     ? selected_display_id_
                     : ConfigStore::Instance().Get().last_display_id;
    int preferred_idx = IndexOfPreferredDisplay(displays_, prefer);
    if (preferred_idx < 0 || static_cast<size_t>(preferred_idx) >= displays_.size()) {
      preferred_idx = 0;
    }
    selected_display_id_ = displays_[static_cast<size_t>(preferred_idx)].id;
    has_selected_display_ = true;

    auto it = display_row_widgets_.find(selected_display_id_);
    if (it != display_row_widgets_.end()) {
      gtk_list_box_select_row(GTK_LIST_BOX(display_list_box_), GTK_LIST_BOX_ROW(it->second.row));
    }

    for (auto& [id, w] : display_row_widgets_) {
      bool is_selected = (id == selected_display_id_);
      if (w.select_icon) {
        gtk_widget_set_visible(w.select_icon, is_selected);
      }
      if (is_selected) {
        gtk_widget_add_css_class(w.row, "is-selected");
      } else {
        gtk_widget_remove_css_class(w.row, "is-selected");
      }
    }
  }

  updating_ui_ = false;
  app_->OnDestinationSelectionChanged();
}

void CastTab::OnDeviceRowSelected(GtkListBox*, GtkListBoxRow* row) {
  if (updating_ui_ || !row) return;
  const char* id_str = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "cm_id"));
  if (!id_str || selected_device_id_ == id_str) return;

  selected_device_id_ = id_str;

  for (const auto& dev : devices_) {
    if (dev.id == selected_device_id_) {
      auto& cfg = ConfigStore::Instance().Mutable();
      cfg.last_device_id = dev.id;
      cfg.last_device_name = dev.name;
      cfg.last_device_ip = dev.ip_address;
      ConfigStore::Instance().Save();
      break;
    }
  }

  for (auto& [id, w] : device_row_widgets_) {
    bool is_selected = (id == selected_device_id_);
    if (w.select_icon) {
      gtk_widget_set_visible(w.select_icon, is_selected);
    }
    if (is_selected) {
      gtk_widget_add_css_class(w.row, "is-selected");
    } else {
      gtk_widget_remove_css_class(w.row, "is-selected");
    }
  }

  if (remove_btn_) {
    gtk_widget_set_sensitive(remove_btn_, IsSelectedDeviceRemovable());
  }

  app_->OnDestinationSelectionChanged();
}

void CastTab::OnDisplayRowSelected(GtkListBox*, GtkListBoxRow* row) {
  if (updating_ui_ || !row) return;
  gpointer p = g_object_get_data(G_OBJECT(row), "cm_disp_id");
  int id = GPOINTER_TO_INT(p);
  if (has_selected_display_ && selected_display_id_ == id) return;

  selected_display_id_ = id;
  has_selected_display_ = true;

  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.last_display_id = selected_display_id_;
  ConfigStore::Instance().Save();

  for (auto& [disp_id, w] : display_row_widgets_) {
    bool is_selected = (disp_id == selected_display_id_);
    if (w.select_icon) {
      gtk_widget_set_visible(w.select_icon, is_selected);
    }
    if (is_selected) {
      gtk_widget_add_css_class(w.row, "is-selected");
    } else {
      gtk_widget_remove_css_class(w.row, "is-selected");
    }
  }

  app_->OnDestinationSelectionChanged();
}

void CastTab::OnPresetChanged(QualityPreset preset) {
  if (updating_ui_) return;
  selected_preset_ = preset;
  auto& cfg = ConfigStore::Instance().Mutable();
  cfg.quality_preset = preset;
  ConfigStore::Instance().Save();
  UpdateBitrateNote();
  app_->SyncBitrateSlider(cfg.GetPresetBitrateKbps(preset));
}

void CastTab::SyncAudioSwitch(bool active) {
  if (!audio_switch_row_) return;
  syncing_audio_ = true;
  if (adw_switch_row_get_active(ADW_SWITCH_ROW(audio_switch_row_)) != static_cast<gboolean>(active)) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(audio_switch_row_), active);
  }
  syncing_audio_ = false;
  UpdateSoundRowSensitivity();
}

void CastTab::SyncSilenceSwitch(bool active) {
  if (!silence_switch_row_) return;
  syncing_audio_ = true;
  if (adw_switch_row_get_active(ADW_SWITCH_ROW(silence_switch_row_)) != static_cast<gboolean>(active)) {
    adw_switch_row_set_active(ADW_SWITCH_ROW(silence_switch_row_), active);
  }
  syncing_audio_ = false;
}

bool CastTab::GetAudioEnabled() const {
  return audio_switch_row_ ? (adw_switch_row_get_active(ADW_SWITCH_ROW(audio_switch_row_)) != FALSE) : true;
}

void CastTab::UpdateSoundRowSensitivity() {
  const bool audio_on = GetAudioEnabled();
  if (audio_switch_row_) {
    gtk_widget_set_sensitive(audio_switch_row_, session_controls_sensitive_);
  }
  if (silence_switch_row_) {
    gtk_widget_set_sensitive(silence_switch_row_, session_controls_sensitive_ && audio_on);
  }
}

void CastTab::SetControlsSensitive(bool sensitive) {
  session_controls_sensitive_ = sensitive;
  gtk_widget_set_sensitive(device_list_box_, sensitive);
  gtk_widget_set_sensitive(display_list_box_, sensitive);
  if (preset_auto_btn_) gtk_widget_set_sensitive(preset_auto_btn_, sensitive);
  if (preset_high_btn_) gtk_widget_set_sensitive(preset_high_btn_, sensitive);
  if (preset_balanced_btn_) gtk_widget_set_sensitive(preset_balanced_btn_, sensitive);
  if (preset_smooth_btn_) gtk_widget_set_sensitive(preset_smooth_btn_, sensitive);
  UpdateSoundRowSensitivity();
  if (rescan_btn_) gtk_widget_set_sensitive(rescan_btn_, sensitive);
  if (add_ip_btn_) gtk_widget_set_sensitive(add_ip_btn_, sensitive);
  if (remove_btn_) gtk_widget_set_sensitive(remove_btn_, sensitive && IsSelectedDeviceRemovable());
}

void CastTab::SetScanInProgress(bool scanning) {
  scan_in_progress_ = scanning;
  UpdateDeviceSectionState();
}

std::string CastTab::GetSelectedDeviceName() const {
  if (selected_device_id_.empty()) return "";
  for (const auto& dev : devices_) {
    if (dev.id == selected_device_id_) {
      return dev.name;
    }
  }
  return ConfigStore::Instance().Get().last_device_name;
}

std::string CastTab::GetSelectedDisplayName() const {
  if (!has_selected_display_ || displays_.empty()) return "";
  for (const auto& disp : displays_) {
    if (disp.id == selected_display_id_) {
      if (disp.name == "Wayland (pick in system dialog)") {
        return "Choose in system dialog";
      }
      return disp.name.empty() ? ("Display " + std::to_string(disp.id)) : disp.name;
    }
  }
  return "";
}

bool CastTab::IsSelectedDeviceRemovable() const {
  if (selected_device_id_.empty()) return false;
  for (const auto& dev : devices_) {
    if (dev.id == selected_device_id_) {
      return (dev.model_name == "Custom Chromecast" || dev.model_name == "saved");
    }
  }
  const auto& cfg = ConfigStore::Instance().Get();
  if (selected_device_id_ == cfg.last_device_id || selected_device_id_ == cfg.last_device_ip) {
    return true;
  }
  return false;
}

void CastTab::RemoveSelectedDevice() {
  if (selected_device_id_.empty() || !IsSelectedDeviceRemovable()) {
    return;
  }
  std::string id_to_remove = selected_device_id_;
  std::string ip_to_remove;
  for (const auto& d : devices_) {
    if (d.id == id_to_remove) {
      ip_to_remove = d.ip_address;
      break;
    }
  }

  CastEngine::Instance().GetDiscovery().RemoveDevice(id_to_remove);

  auto& cfg = ConfigStore::Instance().Mutable();
  if (cfg.last_device_id == id_to_remove || (!ip_to_remove.empty() && cfg.last_device_ip == ip_to_remove) || cfg.last_device_ip == id_to_remove) {
    cfg.last_device_id.clear();
    cfg.last_device_name.clear();
    cfg.last_device_ip.clear();
    ConfigStore::Instance().Save();
  }

  selected_device_id_.clear();
  RefreshDevices();
  app_->OnDestinationSelectionChanged();
}

void CastTab::UpdateSessionState(SessionState state, const std::string& message) {
  (void)message;
  bool is_idle = (state == SessionState::kIdle || state == SessionState::kReady ||
                  state == SessionState::kDiscovering || state == SessionState::kFailed);
  SetControlsSensitive(is_idle);
}

void CastTab::OnAddIpClicked() {
  app_->PushModalActionBlock();

  AdwDialog* dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, "Add Cast display by IP");
  adw_dialog_set_content_width(dialog, 560);

  AdwToolbarView* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  AdwHeaderBar* header_bar = ADW_HEADER_BAR(adw_header_bar_new());
  adw_header_bar_set_show_start_title_buttons(header_bar, FALSE);
  adw_header_bar_set_show_end_title_buttons(header_bar, FALSE);

  GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
  adw_header_bar_pack_start(header_bar, cancel_btn);

  GtkWidget* add_btn = gtk_button_new_with_label("Add display");
  gtk_widget_add_css_class(add_btn, "suggested-action");
  adw_header_bar_pack_end(header_bar, add_btn);

  adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header_bar));

  GtkWidget* content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_add_css_class(content_box, "cm-dialog-content");
  gtk_widget_set_margin_start(content_box, 16);
  gtk_widget_set_margin_end(content_box, 16);
  gtk_widget_set_margin_top(content_box, 16);
  gtk_widget_set_margin_bottom(content_box, 16);

  AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

  GtkWidget* name_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name_row), "Friendly name (optional)");
  adw_preferences_group_add(group, name_row);

  GtkWidget* ip_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ip_row), "IPv4 address");
  adw_preferences_group_add(group, ip_row);

  GtkWidget* port_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(port_row), "Port");
  GtkWidget* port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(port_spin), TRUE);
  gtk_spin_button_set_increments(GTK_SPIN_BUTTON(port_spin), 1, 100);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(port_spin), 8009);
  gtk_widget_set_valign(port_spin, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(port_spin, 96, -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(port_spin),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Port",
                                 -1);
  adw_action_row_add_suffix(ADW_ACTION_ROW(port_row), port_spin);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(port_row), port_spin);
  adw_preferences_group_add(group, port_row);

  gtk_box_append(GTK_BOX(content_box), GTK_WIDGET(group));

  GtkWidget* err_lbl = gtk_label_new("");
  gtk_widget_add_css_class(err_lbl, "cm-section-description");
  gtk_widget_add_css_class(err_lbl, "is-error");
  gtk_widget_set_halign(err_lbl, GTK_ALIGN_START);
  gtk_label_set_wrap(GTK_LABEL(err_lbl), TRUE);
  gtk_widget_set_visible(err_lbl, FALSE);
  gtk_box_append(GTK_BOX(content_box), err_lbl);

  gtk_accessible_update_relation(GTK_ACCESSIBLE(ip_row),
                                 GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, err_lbl, nullptr,
                                 -1);

  adw_toolbar_view_set_content(toolbar_view, content_box);
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar_view));
  adw_dialog_set_default_widget(dialog, add_btn);

  // Connect text changed on IP row to clear error
  g_signal_connect(ip_row, "changed", G_CALLBACK(+[](GtkEditable* ed, gpointer user_data) {
    auto* err_lbl = GTK_WIDGET(user_data);
    gtk_widget_remove_css_class(GTK_WIDGET(ed), "error");
    gtk_label_set_text(GTK_LABEL(err_lbl), "");
    gtk_widget_set_visible(err_lbl, FALSE);
  }), err_lbl);

  // Dialog closed
  g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer user_data) {
    auto* self = static_cast<CastTab*>(user_data);
    self->app_->PopModalActionBlock();
    if (self->add_ip_btn_) {
      gtk_widget_grab_focus(self->add_ip_btn_);
    }
  }), this);

  // Cancel clicked
  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(+[](GtkButton*, gpointer user_data) {
    auto* dlg = static_cast<AdwDialog*>(user_data);
    adw_dialog_close(dlg);
  }), dialog);

  struct AddIpContext {
    CastTab* tab;
    AdwDialog* dialog;
    GtkWidget* name_row;
    GtkWidget* ip_row;
    GtkWidget* port_spin;
    GtkWidget* err_lbl;
  };

  auto* ctx = new AddIpContext{this, dialog, name_row, ip_row, port_spin, err_lbl};

  auto on_add_submit = +[](gpointer, gpointer user_data) {
    auto* ctx = static_cast<AddIpContext*>(user_data);
    std::string ip_str = gtk_editable_get_text(GTK_EDITABLE(ctx->ip_row));
    struct sockaddr_in sa{};
    if (inet_pton(AF_INET, ip_str.c_str(), &(sa.sin_addr)) <= 0) {
      gtk_label_set_text(GTK_LABEL(ctx->err_lbl), "Enter a valid IPv4 address, for example 192.168.1.150.");
      gtk_widget_set_visible(ctx->err_lbl, TRUE);
      gtk_widget_add_css_class(ctx->ip_row, "error");
      gtk_widget_grab_focus(ctx->ip_row);
      return;
    }

    std::string name_str = gtk_editable_get_text(GTK_EDITABLE(ctx->name_row));
    if (name_str.empty()) {
      name_str = "Cast device (" + ip_str + ")";
    }
    gtk_spin_button_update(GTK_SPIN_BUTTON(ctx->port_spin));
    uint16_t port = static_cast<uint16_t>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->port_spin)));

    CastDevice d;
    d.id = ip_str;
    d.name = name_str;
    d.model_name = "Custom Chromecast";
    d.ip_address = ip_str;
    d.port = port;
    d.capabilities = kCapVideoOut | kCapAudioOut;
    d.status = DeviceStatus::kReady;

    CastEngine::Instance().GetDiscovery().AddOrUpdateDevice(d);
    ctx->tab->selected_device_id_ = d.id;

    auto& cfg = ConfigStore::Instance().Mutable();
    cfg.last_device_id = d.id;
    cfg.last_device_name = d.name;
    cfg.last_device_ip = d.ip_address;
    ConfigStore::Instance().Save();

    ctx->tab->RefreshDevices();
    ctx->tab->app_->OnDestinationSelectionChanged();
    ctx->tab->app_->ShowToast("Cast display added");

    adw_dialog_close(ctx->dialog);
  };

  g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_submit), ctx);
  g_signal_connect(name_row, "entry-activated", G_CALLBACK(on_add_submit), ctx);
  g_signal_connect(ip_row, "entry-activated", G_CALLBACK(on_add_submit), ctx);

  g_object_set_data_full(G_OBJECT(dialog), "cm_add_ip_ctx", ctx, +[](gpointer data) {
    delete static_cast<AddIpContext*>(data);
  });

  adw_dialog_present(dialog, GTK_WIDGET(app_->GetWindow()));
  gtk_widget_grab_focus(ip_row);
}

}  // namespace castcore::gui
